# Zaparoo Frontend
# Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
# SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

# Host tests for the FPGA offload. Deliberately Qt-free: they execute
# real display lists against the golden reference model, so they run
# anywhere and need no scene graph and no hardware.
add_executable(tst_fpga_offload tst_fpga_offload.cpp
                                "${CMAKE_SOURCE_DIR}/src/app/fpga/blitter_codec.cpp"
)
target_include_directories(tst_fpga_offload PRIVATE "${CMAKE_SOURCE_DIR}/src/app/fpga")
target_link_libraries(tst_fpga_offload PRIVATE zaparoo_fpga_refmodel Zaparoo::CompileOptions)

add_test(NAME fpga_offload COMMAND tst_fpga_offload)
