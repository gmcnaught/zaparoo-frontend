// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_text_item.h"

#include <QFont>
#include <QString>

#ifdef ZAPAROO_FPGA_QT_PRIVATE
#include <QVarLengthArray>
#include <private/qfontengine_p.h>
#include <private/qtextengine_p.h>
#endif

namespace zaparoo::fpga
{

bool haveShapedGlyphAccess()
{
#ifdef ZAPAROO_FPGA_QT_PRIVATE
    return true;
#else
    return false;
#endif
}

namespace
{

// Re-shaping fallback. Only safe for runs where one glyph corresponds
// to one character advancing left to right by its own advance: no
// kerning, no ligatures, no marks, no bidi. Anything else is refused
// rather than laid out wrongly, because wrong text is worse than text
// the CPU drew.
bool shapeSimpleRun(const QPointF& origin, const QTextItem& item, GlyphRun* out)
{
    const QString text = item.text();
    if (text.isEmpty())
    {
        return false;
    }

    const QRawFont font = QRawFont::fromFont(item.font());
    if (!font.isValid())
    {
        return false;
    }

    const QList<quint32> indexes = font.glyphIndexesForString(text);
    if (indexes.isEmpty() || indexes.size() != text.size())
    {
        // A glyph count that does not match the character count means
        // the shaper did something (ligature, mark, cluster) that this
        // path is not entitled to reproduce.
        return false;
    }

    const QList<QPointF> advances = font.advancesForGlyphIndexes(indexes);
    if (advances.size() != indexes.size())
    {
        return false;
    }

    out->font = font;
    out->indexes = indexes;
    out->positions.clear();
    out->positions.reserve(indexes.size());

    QPointF pen = origin;
    for (const QPointF& advance : advances)
    {
        out->positions.append(pen);
        pen += advance;
    }
    return true;
}

} // namespace

#ifdef ZAPAROO_FPGA_QT_PRIVATE

bool extractGlyphRun(const QPointF& origin, const QTextItem& item, GlyphRun* out)
{
    if (out == nullptr)
    {
        return false;
    }

    const auto& internal = static_cast<const QTextItemInt&>(item);
    if (internal.fontEngine == nullptr || internal.glyphs.numGlyphs <= 0)
    {
        return shapeSimpleRun(origin, item, out);
    }

    // getGlyphPositions is the same call Qt's own raster engine makes,
    // so bidi ordering, per-glyph offsets and the run's render flags
    // are handled by Qt rather than re-derived here.
    QVarLengthArray<glyph_t> glyphs;
    QVarLengthArray<QFixedPoint> positions;
    internal.fontEngine->getGlyphPositions(internal.glyphs,
                                           QTransform::fromTranslate(origin.x(), origin.y()),
                                           internal.flags, glyphs, positions);
    if (glyphs.isEmpty() || glyphs.size() != positions.size())
    {
        return false;
    }

    const bool multi = internal.fontEngine->type() == QFontEngine::Multi;

    out->font = QRawFont::fromFont(item.font());
    if (!out->font.isValid())
    {
        return false;
    }
    out->indexes.clear();
    out->positions.clear();
    out->indexes.reserve(glyphs.size());
    out->positions.reserve(glyphs.size());

    for (qsizetype i = 0; i < glyphs.size(); i++)
    {
        glyph_t index = glyphs[i];
        if (multi)
        {
            // A multi engine packs the fallback-engine number into the
            // high byte, and a glyph index only means anything to the
            // engine that produced it. Serving a fallback glyph would
            // need that engine's QRawFont, which there is no public
            // way to build -- so those runs are refused whole and the
            // A9 draws them. Refusing is the point: rasterizing glyph
            // 42 of the wrong face is silently wrong output.
            if ((index >> 24) != 0)
            {
                return false;
            }
            index &= 0x00FFFFFFU;
        }
        out->indexes.append(static_cast<quint32>(index));
        out->positions.append(positions[i].toPointF());
    }
    return true;
}

#else

bool extractGlyphRun(const QPointF& origin, const QTextItem& item, GlyphRun* out)
{
    return out != nullptr && shapeSimpleRun(origin, item, out);
}

#endif

} // namespace zaparoo::fpga
