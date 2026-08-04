# Zaparoo Frontend
# Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
# SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0
#
# FPGA blitter offload: Qt Quick's software renderer emits a display list
# the MiSTer fabric composites, instead of rasterizing on the Cortex-A9.

# The blitter host layer is C. The root project only enables CXX.
enable_language(C)

# ── vendored display-list layer ───────────────────────────────────────────────
# Upstream code from github.com/gmcnaught/mister-fpga-blitter, re-synced by
# scripts/sync-fpga-vendor.sh. Built as its own target so the project's strict
# warning set and clang-tidy configuration are NOT imposed on code we do not
# own and must not edit locally.
add_library(
    zaparoo_fpga_vendor STATIC
    vendor/blt_alloc.c
    vendor/blt_emitter.c
    vendor/corner_atlas.c
    vendor/font6x8.c
    vendor/ui_offload.c
    vendor/glyph_cache.c
)
set_target_properties(
    zaparoo_fpga_vendor
    PROPERTIES C_STANDARD 11
               C_STANDARD_REQUIRED ON
               POSITION_INDEPENDENT_CODE ON
)
target_include_directories(zaparoo_fpga_vendor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# ── the golden reference model ────────────────────────────────────────────────
# Executes display lists in software. Test-only: it is what the host tests diff
# the emitted lists against, and it must never be linked into the frontend.
add_library(zaparoo_fpga_refmodel STATIC vendor/blitter_ref.c vendor/blt_tri.c)
set_target_properties(
    zaparoo_fpga_refmodel
    PROPERTIES C_STANDARD 11
               C_STANDARD_REQUIRED ON
               POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(zaparoo_fpga_refmodel PUBLIC zaparoo_fpga_vendor)

# ── the Qt-facing offload ─────────────────────────────────────────────────────
qt_add_library(
    zaparoo_fpga STATIC
    blitter_transport.h
    blitter_transport.cpp
    blitter_surface.h
    blitter_surface.cpp
    blitter_glyph_source.h
    blitter_glyph_source.cpp
    blitter_text_item.h
    blitter_text_item.cpp
    blitter_paint_engine.h
    blitter_paint_engine.cpp
    blitter_paint_device.h
    blitter_paint_device.cpp
    blitter_render_loop.h
    blitter_render_loop.cpp
    fpga_offload.h
    fpga_offload.cpp
)
target_include_directories(zaparoo_fpga PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    zaparoo_fpga
    PUBLIC Qt6::Quick Qt6::Gui zaparoo_fpga_vendor
    PRIVATE Zaparoo::CompileOptions
)
target_compile_definitions(zaparoo_fpga PUBLIC ZAPAROO_FPGA_OFFLOAD)

# Reading the glyphs Qt already shaped needs its private text headers. Without
# them only text that still carries a source string can reach the fabric, which
# excludes every QML Text item -- Qt Quick paints those as glyph runs whose
# string does not travel with them. Off by default because Qt private headers
# are not a compatibility-guaranteed interface: turning it on pins the build to
# a Qt patch release.
option(ZAPAROO_FPGA_QT_PRIVATE "Read shaped glyphs via Qt private headers (needed for QML text)"
       OFF
)
if(ZAPAROO_FPGA_QT_PRIVATE)
    find_package(Qt6 6.10 REQUIRED COMPONENTS GuiPrivate)
    target_link_libraries(zaparoo_fpga PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(zaparoo_fpga PRIVATE ZAPAROO_FPGA_QT_PRIVATE)
    message(STATUS "FPGA offload: shaped-glyph access ON (Qt private headers)")
else()
    message(STATUS "FPGA offload: shaped-glyph access OFF -- QML text will rasterize on the CPU")
endif()
