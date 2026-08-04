# FPGA blitter offload

An alternative render path for MiSTer: instead of Qt rasterizing the whole
scene on the Cortex-A9 and copying the result to the FPGA, Qt emits a
per-frame **display list** that the fabric composites. The A9 stops touching
pixels.

This is the production implementation of the prototype in
[mister-fpga-blitter `prototypes/qt`](https://github.com/gmcnaught/mister-fpga-blitter/tree/claude/fpga-gpu-offload-example-zw4p3y).

> **Status: not yet run on hardware.** Everything below is implemented and
> the display-list layer is tested against the golden reference model, but no
> part of *this* has executed on a DE10-Nano. The wire ABI it emits is the
> canonical one; read "Opcode ABI" for which fabrics that does and does not
> match today, and "Before trusting this" before drawing conclusions.

## Why there is a seam at all

On MiSTer there is no GPU, so Qt Quick runs its **Software** adaptation and
composites the entire scene through `QPainter`. That means exactly one
`QPaintEngine` sees every draw — no widget/QML split to bridge. Redirecting
it is a public, documented operation:
`QQuickRenderTarget::fromPaintDevice()` (Qt 6.4+, software backend only).

## How a frame flows

```
QML scene
   │  Qt Quick Software adaptation
   ▼
BlitterPaintEngine          QPainter calls -> blitter commands
   │                          fillRect        -> FILL
   │                          rounded rect    -> 3 FILLs + 4 PALPHA corner blits
   │                          image 1:1       -> BLIT
   │                          image scaled    -> TRILIST quad (fabric resamples)
   │                          glyph run       -> PAL8 coverage blits, SPRITELIST-batched
   │                          anything else   -> rasterized on the A9, and COUNTED
   ▼
BlitterSurface              owns the emitter, glyph atlas, CLUT ramps
   ▼
BlitterTransport            ring + heap in DDR at 0x3B000000, submit/done doorbell
   ▼
fabric                      composites into WORK, burst-writes scanout, drives video
```

The A9 builds frame N+1 while the fabric composites N; `beginFrame()` blocks
until the previous list is done, so the wait costs nothing that was not going
to be spent anyway.

## Turning it on

Two switches, because compiling it in is not the same as trusting it:

```sh
cmake -DZAPAROO_FPGA=ON ...          # build the path
ZAPAROO_FPGA_OFFLOAD=1 ./frontend    # use it
```

`startOffload()` is all-or-nothing. Every failure — fabric not mapped, arenas
too small, entry point wrong — logs the reason, returns false, and the app
carries on with the existing software path completely unchanged. There is no
half-enabled state.

| Variable | Default | Meaning |
|---|---|---|
| `ZAPAROO_FPGA_OFFLOAD` | off | Master switch. |
| `ZAPAROO_FPGA_TARGET_FPS` | 60 | Upper bound on frame rate. Frames are only built when the scene changes. |
| `ZAPAROO_FPGA_GLYPH_PHASES` | 1 | Subpixel phases for text. See "Text" below. |
| `ZAPAROO_FPGA_GLYPH_ATLAS` | 512 | Glyph atlas edge, in texels. |
| `ZAPAROO_FPGA_STATS_INTERVAL` | 0 | Log a frame accounting line every N frames. |

### It replaces the native video writer, it does not layer on it

A blitter core owns scanout itself, and the blitter's DDR contract and the
Menu fork's native-video contract both claim `0x3A000000`. They are
alternative fabrics, never both loaded. So when the offload starts,
`initNativeVideoWriter()` is skipped and there is no `/dev/fb0` copy at all.

## Memory map

From mister-fpga-blitter `docs/blitter-protocol.md` §2, mirrored in
`blitter_transport.h`:

| Offset from `0x3B000000` | Size | Use |
|---|---|---|
| `0x000000` | 64 B | Control block: `submit_seq`, `cmd_count`, `clear_color`, `flags`, `done_seq`, `status`. One u32 per **qword** slot. |
| `0x000040` | 512 KiB | Command ring, walked until `END`. |
| `0x080000` | ~15.4 MiB | Source heap: uploaded images, the glyph atlas, and the sprite/vertex arenas. |
| `0xFC3000` | 64 KiB | `CLUT_BUF`, the palette DMA source. |

Two details are easy to get wrong and are covered by tests:

- **SPRITELIST and TRILIST entry offsets resolve against the source heap
  base**, not against the v4 map's standalone `SP_BUF`/`TL_BUF` regions. The
  arenas are therefore carved from the heap (`uio_init` puts the sprite arena
  at heap offset 0, which is a requirement, not a convenience) and those
  standalone regions go unused.
- **The CLUT has two layouts.** The host mirror the reference model reads is
  a packed `u32` array; the fabric's DMA source carries one `u32` per qword.
  `expandClut()` converts, and the fabric's CLUT FSM reads a *fixed* address,
  so the upload command's offset field is ignored.

## Text

Text is the biggest win and the most constrained part.

The fabric never rasterizes a glyph. The A9 rasterizes each one **once**, at
the size it is first seen, into a colour-free PAL8 coverage atlas; colour is
a 16-entry CLUT ramp, so one atlas serves every colour, and a whole screen of
mixed-colour text flushes as a single `SPRITELIST` command. Steady state is
pixels being moved, not computed.

**Positioning comes from Qt, never re-derived.** The cache is asked for one
glyph at a time at exactly the position Qt shaped it to. Letting the vendored
`uio_text_run` walk a pen of its own would re-implement shaping and break
kerning, ligatures, and every complex and right-to-left script the frontend
ships translations for.

### The one real gap: `ZAPAROO_FPGA_QT_PRIVATE`

Qt Quick paints QML text via `QPainter::drawGlyphRun`. For a plain
`QPaintEngine` that arrives as `drawTextItem` with a `QTextItemInt` whose
glyph layout is populated but whose **`text()` is empty** — the source string
does not travel with it. There is no public API that recovers those glyphs.

So:

- **`ZAPAROO_FPGA_QT_PRIVATE=OFF` (default)** — only text carrying a real
  string reaches the fabric. That excludes every QML `Text` item, so QML text
  rasterizes on the A9. It is still *correct*: `drawTextItem` hands the run to
  `QPaintEngine`'s own implementation, which converts it to outlines and
  re-enters through `drawPath`, where it is rasterized, blitted, and counted.
- **`ZAPAROO_FPGA_QT_PRIVATE=ON`** — reads the shaped glyphs through Qt's
  private text headers (`Qt6::GuiPrivate`) and QML text reaches the atlas.
  This is off by default because Qt private headers carry no compatibility
  guarantee: turning it on pins the build to a Qt patch release. **That is a
  dependency decision, not a build detail.**

Glyphs from a fallback font engine are refused whole rather than served, since
a glyph index only means anything to the engine that produced it and drawing
index 42 of the wrong face is silently wrong output.

`ZAPAROO_FPGA_GLYPH_PHASES` defaults to 1, which keeps every glyph
pixel-aligned and rasterized by Qt's own font engine — so text looks exactly
as it does today. Above 1, glyphs are rasterized from outlines at fractional
offsets instead, which buys smooth marquee scrolling at the cost of hinting.

## What still runs on the A9

Nothing is dropped silently. `FallbackStats` counts destination **area**, not
just calls, because the number that decides whether this was worth building is
what fraction of A9 *time* still rasterizes.

| Case | Why |
|---|---|
| Rotated or sheared draws | The blitter's only source transforms are HFLIP/VFLIP. Whole-scene tate rotation is not this layer's job — it belongs at MiSTer's `sys/screen_rotate` output stage. |
| Scaled images with per-pixel alpha | `TRILIST` samples 16bpp colour with no alpha channel, so this is refused rather than approximated. |
| Non-rounded-rect paths | No general path rasterizer in the fabric. |
| Rounded rects cut by a clip | No longer four whole arcs. |
| QML text without `ZAPAROO_FPGA_QT_PRIVATE` | See above. |

A non-rectangular clip is intersected conservatively with its bounding rect:
content is never drawn outside the clip, only — at worst — inside a corner the
clip would have cut.

## Tests

```sh
cmake -DZAPAROO_FPGA=ON ... && ctest -R fpga_offload
```

Qt-free and hardware-free: they build real display lists and execute them
against the same golden reference model the RTL is diffed against, so a
regression is a regression in what the fabric would composite. They cover the
glyph-key codec (which has to be the exact inverse of the vendored decoder, or
glyphs silently collide), the CLUT layout conversion, that batched glyph output
is pixel-identical to unbatched, that a fully covered glyph texel composites
identically to a `FILL` of the same colour, the command shape of a real frame,
and that an animated zoom stays at one command per frame without leaking the
vertex arena.

What they do **not** cover is the Qt-facing translation layer, which needs a
running scene graph.

## Before trusting this

1. **Measure first.** The prototype's own advice stands: confirm the A9
   actually drops frames before concluding this fixed anything. The counters
   are emit-side accounting, not frame times.
2. **No fabric currently carries everything this emits** — the canonical
   lineage has the text path but has not deployed `TRILIST`; the Maldita core
   has proven triangles but needs the opcode renumber and still lacks
   PAL8/CLUT. See "Opcode ABI".
3. The QML entry point is Item-rooted now, so the render loop can start —
   but it has never actually started against a fabric.

## Opcode ABI: `mister-fpga-blitter` `master` is canonical

The wire numbering this offload emits comes from `master`, which is the
canonical map:

| Opcode | Meaning |
|---|---|
| 10 | `SPRITELIST` |
| 11 | `TILEMAP` |
| 12 | `TRILIST` |
| 13 | `SET_TARGET` |

A second numbering exists in the field. [`maldita.castilla-mister`](https://github.com/gmcnaught/maldita.castilla-mister)
decodes `TRILIST` at 10 and `SET_TARGET` at 11, from the era when
`blitter_ref.h` really did put `TRILIST` at 10 (`mister-fpga-blitter` branch
`trilist-opcode-10`; its `blitter_defs.vh` still carries the comment *"10 is
free and matches the host ABI"*). `master` then moved `TRILIST` to 12 to make
room for `SPRITELIST`/`TILEMAP`, and that core did not follow.

**That divergence is resolved in `master`'s favour — the Maldita core is the
one that moves.** Nothing in this repository changes as a result: what is
vendored here is already the canonical map. Until that core is updated,
though, it is not a fabric this offload can run against, and the failure mode
is silent rather than loud — its decoder ends in a catch-all arm rather than
ignoring unknown opcodes, so a `TRILIST` at 12 would be fed to the generic
blit path instead of being skipped. Host and bitstream have to move together.

### What that core still lacks for this offload

Even renumbered, the Maldita fabric is a deliberately slimmed build and does
not yet carry the text path:

| Feature | `master` lineage | Maldita core |
|---|---|---|
| `SPRITELIST` / `TILEMAP` | production, HW-validated | not implemented |
| `PAL8` + CLUT + `CLUT_UPLOAD` | production, HW-validated | retired — `blitter_top.sv`: *"gmloader never uses PAL8"*, CLUT_UPLOAD FSM deleted |
| textured triangles | sim + model validated | **deployed, HW-validated** (bench logs measure `fabric_ms[tri=…]` per frame on device) |
| region | 18 MiB @ `0x3B000000` | fixed 16 MiB window, heap ~14.75 MiB |

The removal looks shallow rather than structural: `comp_pipeline`'s CLUT
ports are still there, just tied to constants — only the CLUT BRAM and the
upload FSM were deleted — and the fit reports 423/553 M10K against the ~26
blocks a 32×256×32-bit CLUT needs.

Without PAL8/CLUT the text path still has a route: an ARGB4444 white coverage
atlas blitted under `BLT_BLEND_PALPHA` with `BLT_F_COLORMOD` for colour,
which is exactly what the vendored `uio_text()` already does for the 6×8
face. It costs one command per glyph instead of one per screen, and two bytes
per texel instead of one.

## The QML entry point

`QQuickRenderControl` requires a `QQuickWindow` it constructed, so a
QML-declared `Window` can never be redirected to a render target. The tree
was rooted at `ApplicationWindow`, which is why the render loop originally
had nothing to host.

That is resolved. The visual root is now an `Item`:

- **`MainLayout.qml`** — an `Item`. Sized by its host through
  `implicitWidth`/`implicitHeight`, so a bare `Main {}` still gets the
  1280×720 design canvas while a host that anchors or resizes it wins with
  no binding conflict.
- **`AppWindow.qml`** — a thin `ApplicationWindow` owning geometry,
  visibility, title and the min/max constraints (all Window properties), and
  mounting `Main` filling it. This is what `main.cpp` loads on the software
  path.
- **The offload** mounts `Main` directly into its own window
  (`BlitterRenderLoop::Config::type` defaults to `Main`).

Two things that used to come from being a Window now come from the
`Window` attached property, so they work identically on both paths: the
first-frame gate listens to `Window.window`'s `frameSwapped`, and the
stuck-repeat cancel reads `Window.active`.

`applyCrtPreviewScale` resizes the host window rather than the item, since
writing the item's own width would fight `anchors.fill`. That path only runs
on the desktop CRT preview, never on the offload, whose window the render
loop sizes.
