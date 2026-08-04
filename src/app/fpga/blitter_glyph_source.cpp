// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_glyph_source.h"

#include <QImage>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <cstring>

namespace zaparoo::fpga
{

BlitterGlyphSource::BlitterGlyphSource(int phases) : m_phases(phases < 1 ? 1 : phases) {}

QString BlitterGlyphSource::fontKey(const QRawFont& font)
{
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(font.familyName(), font.styleName())
        .arg(font.pixelSize())
        .arg(static_cast<int>(font.weight()))
        .arg(static_cast<int>(font.style()));
}

int BlitterGlyphSource::registerFont(const QRawFont& font)
{
    if (!font.isValid() || font.pixelSize() <= 0)
    {
        return -1;
    }

    const QString key = fontKey(font);
    const auto it = m_ids.constFind(key);
    if (it != m_ids.constEnd())
    {
        return it.value();
    }

    if (m_fonts.size() >= kMaxFonts)
    {
        // 31 distinct (family, size, weight, style) combinations is
        // already far more than the UI draws; running out means
        // something is generating fonts per frame, which the caller
        // needs to see rather than have papered over.
        qWarning("blitter glyphs: font registry full (%d); '%s' at %gpx falls back to A9 raster",
                 kMaxFonts, qPrintable(font.familyName()), font.pixelSize());
        return -1;
    }

    m_fonts.append(font);
    const int id = static_cast<int>(m_fonts.size()); // ids start at 1
    m_ids.insert(key, id);
    return id;
}

void BlitterGlyphSource::encodeKey(std::uint32_t key, char out[5])
{
    encodeGlyphKey(key, out);
}

uio_rasterize_fn BlitterGlyphSource::callback() const
{
    return &BlitterGlyphSource::rasterizeThunk;
}

int BlitterGlyphSource::rasterizeThunk(void* ctx, std::uint32_t codepoint, int pxSize, int phase,
                                       uio_glyph_bmp_t* out)
{
    // pxSize is part of the cache's key, not an input: the registered
    // QRawFont already carries the pixel size the key was minted for.
    Q_UNUSED(pxSize);
    auto* self = static_cast<BlitterGlyphSource*>(ctx);
    return self == nullptr ? -1 : self->rasterize(codepoint, phase, out);
}

int BlitterGlyphSource::rasterize(std::uint32_t key, int phase, uio_glyph_bmp_t* out)
{
    const int id = static_cast<int>(key >> 16);
    const std::uint32_t glyph = key & kMaxGlyphIndex;
    if (id < 1 || id > m_fonts.size() || out == nullptr)
    {
        m_refused++;
        return -1;
    }

    const QRawFont& font = m_fonts.at(id - 1);
    if (!font.isValid())
    {
        m_refused++;
        return -1;
    }

    *out = uio_glyph_bmp_t{};
    const QList<QPointF> advances = font.advancesForGlyphIndexes({glyph});
    out->advance = advances.isEmpty() ? 0 : qRound(advances.first().x());

    const bool ok = m_phases > 1 ? renderFromOutline(font, glyph, phase, out)
                                 : renderFromAlphaMap(font, glyph, out);
    if (!ok)
    {
        // A glyph with no ink (space, or an outline the rasterizer
        // produced nothing for) is a success with a zero-sized bitmap:
        // the cache stores the advance and emits no blit. Only a
        // genuine failure refuses.
        if (out->advance <= 0)
        {
            m_refused++;
            return -1;
        }
        out->cov = nullptr;
        out->w = 0;
        out->h = 0;
        out->pitch = 0;
        return 0;
    }

    m_rasterized++;
    return 0;
}

bool BlitterGlyphSource::renderFromAlphaMap(const QRawFont& font, std::uint32_t glyph,
                                            uio_glyph_bmp_t* out)
{
    QImage map = font.alphaMapForGlyph(glyph, QRawFont::PixelAntialiasing);
    if (map.isNull() || map.width() <= 0 || map.height() <= 0)
    {
        return false;
    }

    // alphaMapForGlyph returns Alpha8 for antialiased faces and Mono
    // for a NoAntialias bitmap face (the CRT path's 6x8 font). Fold
    // both onto one byte of coverage per pixel; Grayscale8 already is
    // that, and anything else is normalized through ARGB32.
    switch (map.format())
    {
    case QImage::Format_Alpha8:
    case QImage::Format_Grayscale8:
        break;
    case QImage::Format_Mono:
    case QImage::Format_MonoLSB:
        map = map.convertToFormat(QImage::Format_Grayscale8);
        break;
    default:
        map = map.convertToFormat(QImage::Format_ARGB32);
        break;
    }

    const int w = map.width();
    const int h = map.height();
    m_coverage.resize(static_cast<qsizetype>(w) * h);
    auto* dst = reinterpret_cast<std::uint8_t*>(m_coverage.data());

    if (map.format() == QImage::Format_ARGB32)
    {
        for (int y = 0; y < h; y++)
        {
            const auto* row = reinterpret_cast<const QRgb*>(map.constScanLine(y));
            for (int x = 0; x < w; x++)
            {
                dst[(y * w) + x] = static_cast<std::uint8_t>(qAlpha(row[x]));
            }
        }
    }
    else
    {
        for (int y = 0; y < h; y++)
        {
            std::memcpy(dst + (static_cast<qsizetype>(y) * w), map.constScanLine(y),
                        static_cast<std::size_t>(w));
        }
    }

    // The alpha map's top-left corresponds to the glyph bounding
    // rect's top-left, measured from the pen on the baseline.
    const QRect bounds = font.boundingRectForGlyph(glyph).toAlignedRect();
    out->cov = dst;
    out->pitch = w;
    out->w = w;
    out->h = h;
    out->bearing_x = bounds.x();
    out->bearing_y = -bounds.y();
    return true;
}

bool BlitterGlyphSource::renderFromOutline(const QRawFont& font, std::uint32_t glyph, int phase,
                                           uio_glyph_bmp_t* out)
{
    const QPainterPath path = font.pathForGlyph(glyph);
    if (path.isEmpty())
    {
        return false;
    }

    // Shift by the phase's fraction of a pixel before choosing the
    // pixel grid, so each phase is a genuinely different rasterization
    // rather than the same bitmap moved a whole pixel.
    const qreal offset = static_cast<qreal>(phase) / static_cast<qreal>(m_phases);
    const QPainterPath shifted = path.translated(offset, 0.0);
    const QRect box = shifted.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1);
    if (box.width() <= 0 || box.height() <= 0)
    {
        return false;
    }

    // ARGB32_Premultiplied rather than Alpha8: QPainter supports it
    // everywhere and on every Qt build, and this runs once per cached
    // glyph, so the extra bytes cost nothing that matters.
    QImage img(box.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.translate(-box.topLeft());
        p.fillPath(shifted, Qt::white);
    }

    const int w = box.width();
    const int h = box.height();
    m_coverage.resize(static_cast<qsizetype>(w) * h);
    auto* dst = reinterpret_cast<std::uint8_t*>(m_coverage.data());
    for (int y = 0; y < h; y++)
    {
        const auto* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; x++)
        {
            dst[(y * w) + x] = static_cast<std::uint8_t>(qAlpha(row[x]));
        }
    }

    out->cov = dst;
    out->pitch = w;
    out->w = w;
    out->h = h;
    out->bearing_x = box.x();
    out->bearing_y = -box.y();
    return true;
}

} // namespace zaparoo::fpga
