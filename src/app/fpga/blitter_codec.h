// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <cstdint>

// Wire-format helpers with no Qt dependency, so the two encodings that
// are easy to get subtly wrong can be tested directly rather than only
// through a running scene.

namespace zaparoo::fpga
{

// Glyph cache keys pack (fontId << 16) | glyphIndex. fontId starts at
// 1 so a key is never 0, which would terminate the string it is
// encoded into.
constexpr int kGlyphKeyFontShift = 16;
constexpr std::uint32_t kGlyphKeyIndexMask = 0xFFFFU;
constexpr int kMaxGlyphFonts = 31;

constexpr std::uint32_t makeGlyphKey(int fontId, std::uint32_t glyphIndex)
{
    return (static_cast<std::uint32_t>(fontId) << kGlyphKeyFontShift) |
           (glyphIndex & kGlyphKeyIndexMask);
}

// Encode a key the way the vendored glyph cache's decoder reads it.
//
// That decoder (utf8_next in mister-fpga-blitter/glyph_cache.c) is a permissive
// UTF-8 reader: it rejects neither surrogates nor overlong forms, so
// any value that fits in 21 bits round-trips exactly. This is NOT a
// conforming UTF-8 encoder and must not be used for text -- it exists
// only to carry an opaque cache key across an API that takes a string.
// Writes at most four bytes plus a terminator.
void encodeGlyphKey(std::uint32_t key, char out[5]);

// Expand the packed u32 CLUT mirror the host and the reference model
// share into the fabric's DMA source layout, which carries one u32 per
// QWORD slot. `dst` must have room for entries * 2 u32 words.
void expandClut(const std::uint32_t* src, std::size_t entries, std::uint32_t* dst);

} // namespace zaparoo::fpga
