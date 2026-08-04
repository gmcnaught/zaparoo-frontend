#!/usr/bin/env bash
# Zaparoo Frontend
# Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
# SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0
#
# Re-sync src/app/fpga/vendor/ from a mister-fpga-blitter checkout.
#
# The vendored files are the FPGA blitter's host-side display-list layer. They
# are upstream-owned: never edit them in this tree, edit them in
# mister-fpga-blitter and re-run this script. Every file gets a provenance
# banner recording the upstream path and commit so drift is visible in review.
#
# Usage:
#   scripts/sync-fpga-vendor.sh /path/to/mister-fpga-blitter
set -euo pipefail

UPSTREAM="${1:-}"
if [[ -z "${UPSTREAM}" || ! -d "${UPSTREAM}/.git" ]]; then
    echo "usage: $0 /path/to/mister-fpga-blitter" >&2
    exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${REPO_ROOT}/src/app/fpga/vendor"
SHA="$(git -C "${UPSTREAM}" rev-parse HEAD)"

# Upstream path -> vendored basename. This is the full include closure of
# ui_offload.h plus the reference model, which the host tests execute display
# lists against. Nothing else from upstream is needed or wanted.
FILES=(
    refmodel/blitter_ref.h
    refmodel/blitter_ref.c
    refmodel/blt_tri.h
    refmodel/blt_tri.c
    refmodel/grid_cell.h
    host/blt_wire.h
    host/blt_alloc.h
    host/blt_alloc.c
    host/blt_emitter.h
    host/blt_emitter.c
    prototypes/qt/corner_atlas.h
    prototypes/qt/corner_atlas.c
    prototypes/qt/font6x8.h
    prototypes/qt/font6x8.c
    prototypes/qt/ui_offload.h
    prototypes/qt/ui_offload.c
    prototypes/qt/glyph_cache.h
    prototypes/qt/glyph_cache.c
)

mkdir -p "${DEST}"

for rel in "${FILES[@]}"; do
    src="${UPSTREAM}/${rel}"
    if [[ ! -f "${src}" ]]; then
        echo "missing upstream file: ${rel}" >&2
        exit 1
    fi
    out="${DEST}/$(basename "${rel}")"
    {
        cat <<EOF
/*
 *  VENDORED — do not edit in this tree.
 *
 *  Source : github.com/gmcnaught/mister-fpga-blitter — ${rel}
 *  Commit : ${SHA}
 *  Re-sync: scripts/sync-fpga-vendor.sh /path/to/mister-fpga-blitter
 *
 *  Upstream is GPL-3.0. The copyright holder additionally licenses these files
 *  for use in Zaparoo Frontend under the terms in COPYING, so the combined
 *  work links cleanly; see src/app/fpga/vendor/README.md.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-PolyForm-Noncommercial-1.0.0
 */
EOF
        cat "${src}"
    } >"${out}"
done

echo "synced ${#FILES[@]} files from ${SHA} into ${DEST}"
