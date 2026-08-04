# FPGA blitter offload

An alternative render path for MiSTer: instead of Qt rasterizing the whole
scene on the Cortex-A9 and copying the result to the FPGA, Qt emits a
per-frame **display list** that the fabric composites. The A9 stops touching
pixels.

This is the production implementation of the prototype in
[mister-fpga-blitter `prototypes/qt`](https://github.com/gmcnaught/mister-fpga-blitter/tree/claude/fpga-gpu-offload-example-zw4p3y).

> **Status: not yet run on hardware, and the target fabric is unresolved.**
> Everything below is implemented and the display-list layer is tested
> against the golden reference model, but no part of *this* has executed on a
> DE10-Nano — and the ABI it emits matches only one of the two diverged
> fabric lineages. Read "The two fabric lineages" and "Before trusting this"
> before drawing conclusions from it.

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
2. **Which fabric this runs against is unresolved, and it is not a detail** —
   see the next section.
3. **The QML entry point must be Item-rooted** — see below.

## The two fabric lineages

The blitter protocol has diverged into two incompatible specializations of
the same wire format, and this offload currently targets one of them.

|  | solarus lineage (`mister-fpga-blitter` `master`) | gmloader lineage (`trilist-opcode-10`, deployed in [`maldita.castilla-mister`](https://github.com/gmcnaught/maldita.castilla-mister)) |
|---|---|---|
| opcode 10 | `SPRITELIST` | **`TRILIST`** |
| opcode 11 | `TILEMAP` | **`SET_TARGET`** |
| opcode 12 | `TRILIST` | not decoded |
| opcode 13 | `SET_TARGET` | not decoded |
| `SPRITELIST` / `TILEMAP` | production, HW-validated | **not implemented** |
| `PAL8` + CLUT + `CLUT_UPLOAD` | production, HW-validated | **deliberately retired** (`blitter_top.sv`: *"gmloader never uses PAL8"*, CLUT_UPLOAD FSM deleted) |
| textured triangles | sim + model only | **deployed and HW-validated** — the Maldita core's bench logs measure `fabric_ms[tri=…]` per frame on real hardware |
| region | 18 MiB @ `0x3B000000` | fixed 16 MiB window, heap ~14.75 MiB |

`third_party/` is vendored from the **solarus** lineage, so the emitted
numbering matches that fabric. Against the gmloader fabric it is actively
wrong, not merely unsupported: the glyph batch emits opcode 10 meaning
`SPRITELIST`, which that core decodes as `TRILIST` and feeds to the triangle
FSM as vertex geometry.

So neither existing fabric serves the whole offload as written:

- **solarus lineage** — the text path (PAL8 coverage atlas, CLUT ramps,
  `SPRITELIST` batching) is exactly what that fabric provides, and the ABI
  matches. But arbitrary-ratio scaling needs `TRILIST`, which that lineage
  has not deployed.
- **gmloader lineage** — arbitrary-ratio scaling is proven in the field. But
  there is no `SPRITELIST` to batch glyphs into and no PAL8/CLUT to colour
  them with, so the text path has no hardware underneath it at all.

Choosing the target lineage is a prerequisite, not a follow-up: it decides
which branch `scripts/sync-fpga-vendor.sh` pulls from, whether the text path
survives in its current form, and whether `BlitterRegion`'s map is 16 or
18 MiB.

## Known blocker: the QML entry point

`QQuickRenderControl` requires a `QQuickWindow` it constructed, so a
QML-declared `Window` can never be redirected to a render target. The
frontend's tree is rooted at `ApplicationWindow` (`MainLayout.qml`, which
`Main.qml` extends), so the render loop currently has nothing Item-rooted to
host: it logs this and returns false, and the app stays on the software path.

Closing it means making the visual root an `Item` and moving the window
properties into a thin wrapper for the desktop build. That touches screen
routing, which `AGENTS.md` puts behind an explicit confirmation, so it is
deliberately **not** done here. Everything downstream of that change is
complete and waiting for it.
