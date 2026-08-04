# Vendored: mister-fpga-blitter host layer

These files are copied from
[gmcnaught/mister-fpga-blitter](https://github.com/gmcnaught/mister-fpga-blitter).
**Do not edit them here.** Change them upstream and re-run:

```sh
scripts/sync-fpga-vendor.sh /path/to/mister-fpga-blitter
```

The sync script stamps each file with its upstream path and commit, so a
review can always tell vendored code from ours and spot drift. They are
excluded from `just fmt` and `just lint` for the same reason — reformatting
them would only be reverted by the next sync.

## What is here

| File | Role |
|---|---|
| `blt_emitter.{h,c}` | Builds the per-frame command ring and manages the source heap. |
| `blt_alloc.{h,c}` | Free-list allocator over the heap, so uploads can be freed. |
| `blt_wire.h` | The packed 32-byte command layout the fabric reads. |
| `ui_offload.{h,c}` | UI primitives (fills, antialiased rounded rects, scaled images) to commands. |
| `corner_atlas.{h,c}` | Bakes antialiased quarter-disc coverage into ARGB4444 masks. |
| `glyph_cache.{h,c}` | PAL8 coverage atlas, CLUT colour ramps, and SPRITELIST batching for text. |
| `font6x8.{h,c}` | The fixed-cell bitmap face, used by `ui_offload`'s CRT text path. |
| `blitter_ref.{h,c}`, `blt_tri.{h,c}` | The golden reference model. **Test-only** — it executes display lists in software and must never link into the frontend. |

## Licensing

Upstream is GPL-3.0. Zaparoo Frontend is PolyForm Noncommercial 1.0.0, and
the two are not compatible, so these files carry an additional grant from
their copyright holder (who owns both this fork's changes and
mister-fpga-blitter) licensing them for use in Zaparoo Frontend under the
terms in `COPYING`. Each file's banner records the dual license as
`GPL-3.0-or-later OR LicenseRef-PolyForm-Noncommercial-1.0.0`.

Re-syncing from upstream re-applies that banner. If upstream ever gains a
contributor who has not made the same grant, the grant no longer covers the
result and the sync must stop until that is resolved.
