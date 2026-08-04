// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include "blitter_codec.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QRawFont>
#include <QString>
#include <cstdint>

extern "C"
{
#include "mister-fpga-blitter/glyph_cache.h"
}

namespace zaparoo::fpga
{

// Supplies the fabric's glyph cache with coverage bitmaps rasterized by
// the font stack Qt already has loaded, replacing the prototype's
// stand-in stroke rasterizer.
//
// The cache stores one entry per (font, glyph index, pixel size,
// subpixel phase) and never rasterizes that combination again, so this
// runs a few hundred times at startup and then effectively never. What
// deliberately does NOT happen here is shaping: glyph indices and pen
// positions come from Qt, which already ran the real shaper. Re-deriving
// them from a UTF-8 string would break kerning, ligatures, and every
// right-to-left and complex script the frontend ships translations for.
//
// Cache keys are packed (fontId << 16) | glyphIndex and handed to the
// vendored cache through its UTF-8 entry point. That codec is a
// permissive one (see utf8_next in mister-fpga-blitter/glyph_cache.c: no surrogate
// or overlong rejection), so any 21-bit value round-trips exactly.
// fontId starts at 1 so a key is never 0, which would terminate the
// string it is encoded into.
class BlitterGlyphSource
{
  public:
    static constexpr int kMaxFonts = kMaxGlyphFonts;
    static constexpr std::uint32_t kMaxGlyphIndex = kGlyphKeyIndexMask;

    explicit BlitterGlyphSource(int phases = 1);

    // Register a font and get its id, or -1 if the font is unusable or
    // the registry is full. Repeat registrations of the same font
    // return the same id.
    int registerFont(const QRawFont& font);

    static std::uint32_t cacheKey(int fontId, std::uint32_t glyphIndex)
    {
        return makeGlyphKey(fontId, glyphIndex);
    }

    // Encode a key with the same permissive UTF-8 codec the vendored
    // decoder implements. Writes at most 4 bytes plus a terminator.
    static void encodeKey(std::uint32_t key, char out[5]);

    // Subpixel phases the atlas should be built with. 1 keeps every
    // glyph pixel-aligned and rasterized by Qt's own font engine, so
    // text is identical to what the software renderer draws today.
    // Above 1, glyphs are rasterized from outlines instead (see
    // rasterize()), which buys smooth marquee scrolling at the cost of
    // the font engine's hinting.
    [[nodiscard]] int phases() const
    {
        return m_phases;
    }

    [[nodiscard]] uio_rasterize_fn callback() const;
    void* context()
    {
        return this;
    }

    [[nodiscard]] std::uint32_t rasterized() const
    {
        return m_rasterized;
    }
    [[nodiscard]] std::uint32_t refused() const
    {
        return m_refused;
    }

  private:
    static int rasterizeThunk(void* ctx, std::uint32_t codepoint, int pxSize, int phase,
                              uio_glyph_bmp_t* out);
    int rasterize(std::uint32_t key, int phase, uio_glyph_bmp_t* out);

    // Coverage via QRawFont::alphaMapForGlyph — the font engine's own
    // rasterizer, hinting included. Pixel-aligned only.
    bool renderFromAlphaMap(const QRawFont& font, std::uint32_t glyph, uio_glyph_bmp_t* out);
    // Coverage by filling the glyph outline at a fractional offset.
    bool renderFromOutline(const QRawFont& font, std::uint32_t glyph, int phase,
                           uio_glyph_bmp_t* out);

    static QString fontKey(const QRawFont& font);

    QList<QRawFont> m_fonts;
    QHash<QString, int> m_ids;
    QByteArray m_coverage; // valid until the next rasterize() call
    int m_phases = 1;
    std::uint32_t m_rasterized = 0;
    std::uint32_t m_refused = 0;
};

} // namespace zaparoo::fpga
