/*
 *  VENDORED — do not edit in this tree.
 *
 *  Source : github.com/gmcnaught/mister-fpga-blitter — refmodel/blt_tri.h
 *  Commit : 6ced360e28987a31723ff79515df35777f2cd318
 *  Re-sync: scripts/sync-fpga-vendor.sh /path/to/mister-fpga-blitter
 *
 *  Upstream is GPL-3.0. The copyright holder additionally licenses these files
 *  for use in Zaparoo Frontend under the terms in COPYING, so the combined
 *  work links cleanly; see third_party/mister-fpga-blitter/README.md.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-PolyForm-Noncommercial-1.0.0
 */
/*
 *  blt_tri.h — reference textured-triangle rasterizer (BLT_OP_TRILIST).
 *  Golden spec for the RTL blt_tri module. Copyright (C) 2026 — GPL-3.0.
 */
#ifndef BLT_TRI_H
#define BLT_TRI_H
#include "blitter_ref.h"
void blt_raster_tri(uint16_t *fb, const blt_surface_heap_t *heap,
                    const blt_cmd_t *h, const blt_vtx_t *tris, int ntris,
                    const uint16_t *surface);
#endif /* BLT_TRI_H */
