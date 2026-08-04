// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_paint_engine.h"

#include "blitter_text_item.h"

#include <QBrush>
#include <QColor>
#include <QImage>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QRectF>
#include <QTextItem>
#include <QtMath>
#include <cstring>
#include <limits>
#include <vector>

namespace zaparoo::fpga
{

namespace
{

std::uint16_t rgb565(const QColor& c)
{
    return packRgb565(static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
                      static_cast<std::uint8_t>(c.blue()));
}

std::uint8_t alpha8(const QColor& c, qreal opacity)
{
    const qreal a = qBound(qreal(0), c.alphaF() * opacity, qreal(1));
    return static_cast<std::uint8_t>(qRound(a * 255.0));
}

// The blitter's destination geometry is integer pixels and its only
// source transforms are HFLIP/VFLIP, so translate and scale map while
// rotation and shear do not. Whole-scene 90-degree tate rotation is
// deliberately not this engine's problem: it belongs at the output
// stage (MiSTer sys/screen_rotate), which rotates the finished frame
// and leaves the compositor drawing upright.
bool axisAligned(const QTransform& t)
{
    return t.type() <= QTransform::TxScale;
}

QRect mapRect(const QTransform& t, const QRectF& r)
{
    return t.mapRect(r).toAlignedRect();
}

// Per-pixel alpha wins over constant alpha, and an opaque draw takes
// the cheapest path the fabric has.
std::uint8_t blendFor(bool hasAlphaChannel, std::uint8_t alpha)
{
    if (hasAlphaChannel)
    {
        return BLT_BLEND_PALPHA;
    }
    if (alpha < 255)
    {
        return BLT_BLEND_CONST_ALPHA;
    }
    return BLT_BLEND_COPY;
}

// Pack an ARGB32 image into the ARGB4444 the fabric's per-pixel-alpha
// blend path samples.
void packArgb4444(const QImage& src, std::vector<std::uint16_t>* out)
{
    out->resize(static_cast<std::size_t>(src.width()) * src.height());
    for (int y = 0; y < src.height(); y++)
    {
        const auto* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < src.width(); x++)
        {
            const QRgb c = row[x];
            (*out)[(static_cast<std::size_t>(y) * src.width()) + x] =
                static_cast<std::uint16_t>(((qAlpha(c) >> 4) << 12) | ((qRed(c) >> 4) << 8) |
                                           ((qGreen(c) >> 4) << 4) | (qBlue(c) >> 4));
        }
    }
}

} // namespace

BlitterPaintEngine::BlitterPaintEngine(BlitterSurface* surface)
    : QPaintEngine(QPaintEngine::PrimitiveTransform | QPaintEngine::PixmapTransform |
                   QPaintEngine::AlphaBlend | QPaintEngine::PainterPaths),
      m_surface(surface), m_uio(surface != nullptr ? surface->offload() : nullptr)
{
}

BlitterPaintEngine::~BlitterPaintEngine() = default;

// begin()/end() bracket one Qt paint session, not one blitter frame:
// the target buffer, the clear and the doorbell are transport
// decisions and live in BlitterSurface.
bool BlitterPaintEngine::begin(QPaintDevice* dev)
{
    Q_UNUSED(dev);
    if (m_uio == nullptr || m_surface == nullptr || !m_surface->isValid())
    {
        return false;
    }

    m_frame++;
    recycleScratch();
    m_fallback.reset();
    m_brush = Qt::transparent;
    m_pen = Qt::white;
    m_opacity = 1.0;
    m_transform = QTransform();
    m_hasClip = false;
    return true;
}

bool BlitterPaintEngine::end()
{
    // Any batch still open has to be flushed before the surface closes
    // the list; leaving it to endFrame() would work, but flushing here
    // keeps "the engine emits nothing after end()" true.
    if (m_surface != nullptr)
    {
        m_surface->flushTextBatch();
    }
    return true;
}

void BlitterPaintEngine::recycleScratch()
{
    const int old = m_scratchBank ^ 1;
    for (const Scratch& s : m_scratch[old])
    {
        blt_emitter_free(m_uio->e, s.off, s.size);
    }
    m_scratch[old].clear();
    m_scratchBank = old;
}

void BlitterPaintEngine::updateState(const QPaintEngineState& state)
{
    const QPaintEngine::DirtyFlags f = state.state();
    if (f.testAnyFlag(QPaintEngine::DirtyBrush))
    {
        m_brush = state.brush().color();
    }
    if (f.testAnyFlag(QPaintEngine::DirtyPen))
    {
        const QPen pen = state.pen();
        m_pen = pen.color();
        // A cosmetic (zero-width) pen still strokes one device pixel.
        m_penWidth = qMax(1, qRound(pen.widthF()));
        m_penVisible = pen.style() != Qt::NoPen && pen.color().alpha() != 0;
    }
    if (f.testAnyFlag(QPaintEngine::DirtyOpacity))
    {
        m_opacity = state.opacity();
    }
    if (f.testAnyFlag(QPaintEngine::DirtyTransform))
    {
        m_transform = state.transform();
    }
    if (f.testAnyFlags(QPaintEngine::DirtyClipRegion | QPaintEngine::DirtyClipPath |
                       QPaintEngine::DirtyClipEnabled))
    {
        // A rectangular clip (every `clip: true` Flickable and
        // ListView) becomes a destination-rect clamp. A non-rectangular
        // clip is not expressible on the fabric and is intersected
        // conservatively with its bounding rect, so content is never
        // drawn outside the clip -- only, at worst, inside a corner the
        // clip would have cut.
        if (state.clipOperation() == Qt::NoClip)
        {
            m_hasClip = false;
        }
        else
        {
            m_clip = state.clipRegion().boundingRect();
            m_hasClip = true;
        }
    }
}

QRect BlitterPaintEngine::clipped(const QRect& r) const
{
    return m_hasClip ? (r & m_clip) : r;
}

void BlitterPaintEngine::fillRect(const QRect& r, const QColor& color)
{
    const QRect d = clipped(r);
    if (d.isEmpty())
    {
        return;
    }
    m_surface->flushTextBatch();
    uio_fill(m_uio, uio_rect_t{d.x(), d.y(), d.width(), d.height()}, rgb565(color),
             alpha8(color, m_opacity));
}

// ---- rectangles ---------------------------------------------------

void BlitterPaintEngine::drawRects(const QRect* rects, int count)
{
    for (int i = 0; i < count; i++)
    {
        const QRectF r(rects[i]);
        drawRects(&r, 1);
    }
}

void BlitterPaintEngine::drawRects(const QRectF* rects, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (!axisAligned(m_transform))
        {
            const QRect b = mapRect(m_transform, rects[i]);
            m_fallback.transforms++;
            m_fallback.px += static_cast<quint64>(b.width()) * b.height();
            continue;
        }

        const QRect r = mapRect(m_transform, rects[i]);
        if (m_brush.alpha() != 0)
        {
            fillRect(r, m_brush);
        }

        // A pen-stroked rect is four thin fills, which is what the
        // fabric wants anyway -- Qt's software renderer would
        // tessellate and rasterize the same shape on the A9.
        if (m_penVisible)
        {
            const int w = qMin(m_penWidth, qMin(r.width(), r.height()));
            if (w > 0)
            {
                fillRect(QRect(r.left(), r.top(), r.width(), w), m_pen);
                fillRect(QRect(r.left(), r.bottom() - w + 1, r.width(), w), m_pen);
                const int inner = r.height() - (2 * w);
                if (inner > 0)
                {
                    fillRect(QRect(r.left(), r.top() + w, w, inner), m_pen);
                    fillRect(QRect(r.right() - w + 1, r.top() + w, w, inner), m_pen);
                }
            }
        }
    }
}

// ---- rounded rectangles: the antialiased-corner sites --------------

bool BlitterPaintEngine::asRoundedRect(const QPainterPath& path, QRect* rect, int* radius)
{
    const QRectF br = path.boundingRect();
    if (br.width() < 2 || br.height() < 2 || path.elementCount() < 4)
    {
        return false;
    }

    // A rounded rect meets its top edge at left + r: take the leftmost
    // element sitting on the top edge and read the radius off it.
    qreal best = br.width();
    bool found = false;
    for (int i = 0; i < path.elementCount(); i++)
    {
        const QPainterPath::Element e = path.elementAt(i);
        if (qAbs(e.y - br.top()) > 0.01)
        {
            continue;
        }
        const qreal r = e.x - br.left();
        if (r >= 0.5 && r < best)
        {
            best = r;
            found = true;
        }
    }
    if (!found)
    {
        return false;
    }

    const int r = qRound(best);
    if (r < 1 || r > UIO_CORNER_MAX_RADIUS)
    {
        return false;
    }

    // Verify by rebuilding the shape Qt would have produced. Anything
    // that is not exactly a uniform-radius rounded rect must take the
    // fallback rather than be redrawn as something else.
    QPainterPath probe;
    probe.addRoundedRect(br, r, r);
    if (probe.elementCount() != path.elementCount())
    {
        return false;
    }
    for (int i = 0; i < path.elementCount(); i++)
    {
        const QPainterPath::Element a = path.elementAt(i);
        const QPainterPath::Element b = probe.elementAt(i);
        if (a.type != b.type || qAbs(a.x - b.x) > 0.02 || qAbs(a.y - b.y) > 0.02)
        {
            return false;
        }
    }

    *rect = br.toAlignedRect();
    *radius = r;
    return true;
}

void BlitterPaintEngine::drawPath(const QPainterPath& path)
{
    QRect r;
    int radius = 0;
    if (axisAligned(m_transform) && asRoundedRect(path, &r, &radius))
    {
        const QRect d = mapRect(m_transform, QRectF(r));
        const int scaled = qMax(1, qRound(radius * m_transform.m11()));
        // A rounded rect cut by a clip is no longer four whole arcs,
        // so it takes the fallback instead of losing its corners.
        if (scaled <= UIO_CORNER_MAX_RADIUS && (!m_hasClip || m_clip.contains(d)))
        {
            m_surface->flushTextBatch();
            uio_rounded_rect(m_uio, uio_rect_t{d.x(), d.y(), d.width(), d.height()}, scaled,
                             rgb565(m_brush), alpha8(m_brush, m_opacity));
            return;
        }
    }

    // The lambda draws in DEVICE coordinates: fallbackRaster has
    // already translated the scratch painter by -topLeft, so combining
    // the painter transform in puts the path exactly where the scene
    // graph asked for it.
    const QRectF b = m_transform.mapRect(path.boundingRect());
    const QColor brush = m_brush;
    const QTransform transform = m_transform;
    quint32* counter = m_textFallbackDepth > 0 ? &m_fallback.text : &m_fallback.paths;
    fallbackRaster(
        b,
        [&](QPainter& p)
        {
            p.setWorldTransform(transform, true);
            p.fillPath(path, brush);
        },
        counter);
}

// ---- images: the arbitrary-ratio scaling sites ---------------------

void BlitterPaintEngine::releaseImage(CachedImage& entry)
{
    if (entry.ref.surf.valid != 0)
    {
        blt_emitter_free(m_uio->e, entry.ref.surf.off, entry.ref.surf.size);
    }
    m_imageBytes -= qMin(m_imageBytes, entry.bytes);
}

void BlitterPaintEngine::evictImages(quint32 needed)
{
    // Evict least-recently-used first, and never anything used in the
    // last two frames: the fabric may still be reading the frame the
    // A9 just submitted.
    while (m_imageBytes + needed > m_imageBudget && !m_images.isEmpty())
    {
        auto victim = m_images.end();
        quint32 oldest = std::numeric_limits<quint32>::max();
        for (auto it = m_images.begin(); it != m_images.end(); ++it)
        {
            if (it.value().lastUsedFrame + 2 > m_frame)
            {
                continue;
            }
            if (it.value().lastUsedFrame < oldest)
            {
                oldest = it.value().lastUsedFrame;
                victim = it;
            }
        }
        if (victim == m_images.end())
        {
            // Everything in the cache is still in flight. Refusing to
            // upload is better than freeing memory the fabric reads.
            return;
        }
        releaseImage(victim.value());
        m_images.erase(victim);
    }
}

uio_image_ref_t BlitterPaintEngine::imageRef(const QImage& img)
{
    const qint64 key = img.cacheKey();
    auto it = m_images.find(key);
    if (it != m_images.end())
    {
        it.value().lastUsedFrame = m_frame;
        return it.value().ref;
    }

    // One texel of padding on each side, matching what
    // uio_upload_image adds: the triangle rasterizer's half-texel bias
    // needs uv to reach outside the image rect, and blt_vtx_t's uv is
    // unsigned.
    const auto bytes = static_cast<quint32>((img.width() + 2) * (img.height() + 2) * 2);
    evictImages(bytes);

    uio_image_ref_t ref{};
    if (img.hasAlphaChannel())
    {
        const QImage a = img.convertToFormat(QImage::Format_ARGB32);
        std::vector<std::uint16_t> buf;
        packArgb4444(a, &buf);
        ref = uio_upload_image_alpha(m_uio, buf.data(), a.width(), a.height(), a.width() * 2);
    }
    else
    {
        const QImage c = img.convertToFormat(QImage::Format_RGB16);
        ref = uio_upload_image(m_uio, reinterpret_cast<const std::uint16_t*>(c.constBits()),
                               c.width(), c.height(), static_cast<int>(c.bytesPerLine()));
    }

    if (ref.surf.valid != 0)
    {
        m_images.insert(key, CachedImage{ref, ref.surf.size, m_frame});
        m_imageBytes += ref.surf.size;
    }
    return ref;
}

void BlitterPaintEngine::drawImage(const QRectF& r, const QImage& img, const QRectF& sr,
                                   Qt::ImageConversionFlags flags)
{
    Q_UNUSED(flags);
    if (img.isNull())
    {
        return;
    }

    const QRect dst = mapRect(m_transform, r);
    if (!axisAligned(m_transform))
    {
        m_fallback.transforms++;
        m_fallback.px += static_cast<quint64>(dst.width()) * dst.height();
        return;
    }
    if (dst.isEmpty())
    {
        return;
    }

    const QRect src = sr.isNull() ? img.rect() : sr.toAlignedRect();
    const bool scaled = dst.width() != src.width() || dst.height() != src.height();

    if (scaled && img.hasAlphaChannel())
    {
        // Per-pixel alpha AND an arbitrary ratio: the triangle path
        // samples 16bpp colour with no alpha channel, so this one
        // genuinely stays on the A9. Counted, never dropped.
        fallbackRaster(
            QRectF(dst), [&](QPainter& p) { p.drawImage(QRectF(dst), img, sr); },
            &m_fallback.images);
        return;
    }

    // Clip by trimming the destination and carrying the same trim into
    // the source, so a clipped image is a smaller draw rather than a
    // fallback.
    QRect d = dst;
    QRectF s(src);
    if (m_hasClip)
    {
        d = dst & m_clip;
        if (d.isEmpty())
        {
            return;
        }
        if (d != dst)
        {
            const qreal sx = static_cast<qreal>(src.width()) / dst.width();
            const qreal sy = static_cast<qreal>(src.height()) / dst.height();
            s = QRectF(src.x() + ((d.x() - dst.x()) * sx), src.y() + ((d.y() - dst.y()) * sy),
                       d.width() * sx, d.height() * sy);
        }
    }

    const uio_image_ref_t ref = imageRef(img);
    if (ref.surf.valid == 0)
    {
        m_fallback.other++;
        m_fallback.px += static_cast<quint64>(d.width()) * d.height();
        return;
    }

    m_surface->flushTextBatch();

    const std::uint8_t a = alpha8(QColor(Qt::white), m_opacity);
    const std::uint8_t blend = blendFor(img.hasAlphaChannel(), a);

    if (!scaled)
    {
        // A 1:1 blit of a sub-rect still has to honour the source
        // offset, which uio_image_blit does not take -- so a trimmed
        // source goes through the scaled path at ratio 1:1, which
        // resolves to the same BLIT.
        if (s.toRect() == QRect(0, 0, ref.w, ref.h))
        {
            uio_image_blit(m_uio, ref, d.x(), d.y(), blend, 0, a);
            return;
        }
    }

    // Arbitrary-ratio scaling on the FABRIC. This is the draw a
    // decode-time pre-scale cannot serve when the ratio changes per
    // frame (a focus zoom, a held-focus animation): here it is one
    // command at any ratio.
    uio_scale_t scale{};
    scale.dst = uio_rect_t{d.x(), d.y(), d.width(), d.height()};
    scale.src = uio_rect_t{qRound(s.x()), qRound(s.y()), qRound(s.width()), qRound(s.height())};
    scale.blend = blend;
    scale.alpha = a;
    if (uio_image_scaled(m_uio, ref, &scale) < 0)
    {
        m_fallback.images++;
        m_fallback.px += static_cast<quint64>(d.width()) * d.height();
    }
}

void BlitterPaintEngine::drawPixmap(const QRectF& r, const QPixmap& pm, const QRectF& sr)
{
    drawImage(r, pm.toImage(), sr, Qt::AutoColor);
}

void BlitterPaintEngine::drawTiledPixmap(const QRectF& r, const QPixmap& pm, const QPointF& offset)
{
    // MainLayout's tiled background is the whole screen, every frame.
    // The fabric has a TILELIST opcode for exactly this, but it is a
    // tile-map channel keyed on a resident atlas; a plain repeated
    // blit is the honest mapping for an arbitrary pixmap and still
    // moves every pixel off the A9.
    if (pm.isNull() || !axisAligned(m_transform))
    {
        QPaintEngine::drawTiledPixmap(r, pm, offset);
        return;
    }

    const QRect dst = clipped(mapRect(m_transform, r));
    if (dst.isEmpty())
    {
        return;
    }

    const QImage img = pm.toImage();
    const uio_image_ref_t ref = imageRef(img);
    if (ref.surf.valid == 0 || ref.w == 0 || ref.h == 0)
    {
        QPaintEngine::drawTiledPixmap(r, pm, offset);
        return;
    }

    m_surface->flushTextBatch();

    const int tw = ref.w;
    const int th = ref.h;
    // Qt's offset runs the opposite way to the tile origin.
    int ox = qRound(offset.x()) % tw;
    int oy = qRound(offset.y()) % th;
    if (ox > 0)
    {
        ox -= tw;
    }
    if (oy > 0)
    {
        oy -= th;
    }

    const std::uint8_t a = alpha8(QColor(Qt::white), m_opacity);
    const std::uint8_t blend = blendFor(img.hasAlphaChannel(), a);

    const int endX = dst.x() + dst.width();
    const int endY = dst.y() + dst.height();
    for (int y = dst.top() + oy; y < endY; y += th)
    {
        for (int x = dst.left() + ox; x < endX; x += tw)
        {
            // Whole tiles only: a partial edge tile would need a
            // source sub-rect, which the scaled path covers.
            const QRect tile(x, y, tw, th);
            const QRect vis = tile & dst;
            if (vis.isEmpty())
            {
                continue;
            }
            if (vis == tile)
            {
                uio_image_blit(m_uio, ref, x, y, blend, 0, a);
                continue;
            }
            uio_scale_t scale{};
            scale.dst = uio_rect_t{vis.x(), vis.y(), vis.width(), vis.height()};
            scale.src = uio_rect_t{vis.x() - x, vis.y() - y, vis.width(), vis.height()};
            scale.blend = blend;
            scale.alpha = a;
            uio_image_scaled(m_uio, ref, &scale);
        }
    }
}

// ---- text ----------------------------------------------------------

bool BlitterPaintEngine::drawGlyphs(const QPointF& origin, const QTextItem& item)
{
    GlyphRun run;
    if (!extractGlyphRun(origin, item, &run))
    {
        return false;
    }

    BlitterGlyphSource& source = m_surface->glyphSource();
    const int fontId = source.registerFont(run.font);
    if (fontId < 0)
    {
        return false;
    }

    const int pxSize = qRound(run.font.pixelSize());
    if (pxSize <= 0)
    {
        return false;
    }

    m_surface->openTextBatch();
    const std::uint16_t color = rgb565(m_pen);

    for (qsizetype i = 0; i < run.indexes.size(); i++)
    {
        const quint32 glyph = run.indexes.at(i);
        if (glyph > BlitterGlyphSource::kMaxGlyphIndex)
        {
            return false;
        }

        // One cache call per glyph, at exactly the position Qt shaped
        // it to. The cache stores, rasterizes on a miss, and pushes a
        // SPRITELIST entry; what it must NOT do is walk a pen of its
        // own, which would re-derive a layout Qt already computed.
        char key[5];
        BlitterGlyphSource::encodeKey(BlitterGlyphSource::cacheKey(fontId, glyph), key);

        const QPointF pos = run.positions.at(i);
        const int xFixed = qRound(pos.x() * 256.0);
        uio_text_run_fx(m_uio, m_surface->glyphAtlas(), pxSize, xFixed, qRound(pos.y()), key,
                        color);
    }
    return true;
}

void BlitterPaintEngine::drawTextItem(const QPointF& p, const QTextItem& ti)
{
    if (axisAligned(m_transform))
    {
        const QPointF origin = m_transform.map(p);
        if (drawGlyphs(origin, ti))
        {
            return;
        }
    }

    if (!m_warnedGlyphSource && !haveShapedGlyphAccess())
    {
        m_warnedGlyphSource = true;
        qWarning("blitter engine: text is rasterizing on the CPU. Qt Quick paints QML text as "
                 "glyph runs whose source string does not travel with them, so reaching the "
                 "glyph atlas needs ZAPAROO_FPGA_QT_PRIVATE (Qt6::GuiPrivate). See "
                 "docs/fpga-offload.md.");
    }

    // Hand it to QPaintEngine's own implementation rather than
    // re-drawing from ti.text(): for a glyph run that string is empty,
    // and drawing it would silently produce a blank where the label
    // should be. The base implementation converts the run to outlines
    // and re-enters through drawPath, which rasterizes and counts it --
    // the depth flag is what attributes those pixels to text rather
    // than to paths.
    m_textFallbackDepth++;
    QPaintEngine::drawTextItem(p, ti);
    m_textFallbackDepth--;
}

// ---- the fallback: raster on the A9, upload, blit, COUNT ------------

void BlitterPaintEngine::fallbackRaster(const QRectF& bounds,
                                        const std::function<void(QPainter&)>& draw,
                                        quint32* counter)
{
    QRect b = clipped(bounds.toAlignedRect());
    if (b.isEmpty() || m_uio == nullptr)
    {
        return;
    }

    QImage tmp(b.size(), QImage::Format_ARGB32);
    tmp.fill(Qt::transparent);
    {
        QPainter p(&tmp);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setOpacity(m_opacity);
        // Callers draw in device coordinates; this is the only place
        // that knows where the scratch tile sits.
        p.translate(-b.topLeft());
        draw(p);
    }

    std::vector<std::uint16_t> buf;
    packArgb4444(tmp, &buf);

    m_surface->flushTextBatch();
    const blt_surface_ref_t s =
        blt_upload_argb4444(m_uio->e, buf.data(), b.width(), b.height(), b.width() * 2);
    if (s.valid != 0)
    {
        blt_blit(m_uio->e, s, 0, 0, b.width(), b.height(), b.x(), b.y(), BLT_BLEND_PALPHA, 0, 255,
                 0);
        m_scratch[m_scratchBank].append(Scratch{s.off, s.size});
    }

    if (counter != nullptr)
    {
        (*counter)++;
    }
    m_fallback.px += static_cast<quint64>(b.width()) * b.height();
}

} // namespace zaparoo::fpga
