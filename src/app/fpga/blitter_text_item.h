// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include <QList>
#include <QPointF>
#include <QRawFont>
#include <QTextItem>

namespace zaparoo::fpga
{

// One glyph run, already shaped, ready to be blitted from the coverage
// atlas: glyph indexes into `font` and their baseline-origin positions
// in device pixels.
struct GlyphRun
{
    QRawFont font;
    QList<quint32> indexes;
    QList<QPointF> positions;

    bool isEmpty() const
    {
        return indexes.isEmpty();
    }
};

// Recover the glyphs and positions Qt already shaped for a QTextItem.
//
// Two paths, and which one is available decides how much of the
// frontend's text can reach the fabric:
//
//  * Qt Quick's Software adaptation paints text through
//    QPainter::drawGlyphRun. For a plain (non-extended) QPaintEngine
//    that arrives as drawTextItem with a QTextItemInt whose glyph
//    layout is populated but whose text() is EMPTY -- the source string
//    never travelled with it. Reading those glyphs needs Qt's private
//    text headers, which is what ZAPAROO_FPGA_QT_PRIVATE enables.
//
//  * Without that, only text items that still carry a string can be
//    served, by re-shaping through QRawFont. That covers QPainter
//    callers but NOT QML Text, and it cannot reproduce kerning,
//    ligatures, or any complex or right-to-left script, so it is
//    deliberately limited to runs that shape trivially.
//
// Returns false when the run cannot be served, and the caller then
// rasterizes on the A9 and counts it.
bool extractGlyphRun(const QPointF& origin, const QTextItem& item, GlyphRun* out);

// True when the build can read Qt's own shaped glyphs, i.e. when QML
// text can reach the fabric at all. Reported at startup so the
// distinction is never silent.
bool haveShapedGlyphAccess();

} // namespace zaparoo::fpga
