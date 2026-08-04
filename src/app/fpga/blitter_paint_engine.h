// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include "blitter_surface.h"

#include <QColor>
#include <QHash>
#include <QList>
#include <QPaintEngine>
#include <QRect>
#include <QString>
#include <QTransform>
#include <functional>

namespace zaparoo::fpga
{

// Draws that did not reach the fabric.
//
// The counters record destination AREA, not just call counts, because
// the number that decides whether the offload is worth anything is what
// fraction of A9 *time* still rasterizes -- and a frame with a nonzero
// `px` here is a frame the CPU still painted. Nothing falls back
// silently.
struct FallbackStats
{
    quint32 paths = 0;      // a path that is not a uniform rounded rect
    quint32 text = 0;       // glyph runs the atlas could not serve
    quint32 transforms = 0; // rotated or sheared draws
    quint32 images = 0;     // scaled art with per-pixel alpha
    quint32 other = 0;
    quint64 px = 0; // destination pixels rasterized on the A9

    void reset()
    {
        paths = text = transforms = images = other = 0;
        px = 0;
    }
    [[nodiscard]] quint32 draws() const
    {
        return paths + text + transforms + images + other;
    }
};

// Translates QPainter calls into a blitter display list.
//
// With Qt Quick's Software adaptation there is no GPU and no
// widget/QML split: one QPaintEngine sees the whole scene, which is
// what makes this a single interception point rather than a set of
// per-component hooks.
//
// Every decision that can be made without Qt types is delegated to the
// vendored offload layer, which is gated against the golden reference
// model. This class only translates.
class BlitterPaintEngine : public QPaintEngine
{
  public:
    explicit BlitterPaintEngine(BlitterSurface* surface);
    ~BlitterPaintEngine() override;

    bool begin(QPaintDevice* dev) override;
    bool end() override;
    [[nodiscard]] Type type() const override
    {
        return QPaintEngine::User;
    }

    void updateState(const QPaintEngineState& state) override;

    void drawRects(const QRect* rects, int count) override;
    void drawRects(const QRectF* rects, int count) override;
    void drawPath(const QPainterPath& path) override;
    void drawImage(const QRectF& r, const QImage& img, const QRectF& sr,
                   Qt::ImageConversionFlags flags) override;
    void drawPixmap(const QRectF& r, const QPixmap& pm, const QRectF& sr) override;
    void drawTiledPixmap(const QRectF& r, const QPixmap& pm, const QPointF& offset) override;
    void drawTextItem(const QPointF& p, const QTextItem& ti) override;

    [[nodiscard]] const FallbackStats& fallbacks() const
    {
        return m_fallback;
    }

    // Cap on heap bytes held by the decoded-image cache. Cover art is
    // uploaded once and re-blitted for free, which is what makes
    // per-frame recompositing cheap -- but a browse grid walks through
    // more art than the heap holds, so the cache has to evict.
    void setImageCacheBudget(quint32 bytes)
    {
        m_imageBudget = bytes;
    }

  private:
    struct CachedImage
    {
        uio_image_ref_t ref{};
        quint32 bytes = 0;
        quint32 lastUsedFrame = 0;
    };
    struct Scratch
    {
        quint32 off = 0;
        quint32 size = 0;
    };

    // Recognise the one path shape that matters: a rounded rectangle
    // with a single uniform radius (cards, focus rings, pills,
    // modals). Anything else -- a squircle, per-corner radii, a
    // clipped path -- must fall back rather than be silently redrawn
    // as something it is not.
    [[nodiscard]] static bool asRoundedRect(const QPainterPath& path, QRect* rect, int* radius);

    void fillRect(const QRect& r, const QColor& color);
    uio_image_ref_t imageRef(const QImage& img);
    void evictImages(quint32 needed);
    void releaseImage(CachedImage& entry);

    // Draw a glyph run through the fabric's coverage atlas. Returns
    // false if the font or its glyphs could not be served, in which
    // case the caller rasterizes on the A9 and counts it.
    bool drawGlyphs(const QPointF& origin, const QTextItem& item);

    void fallbackRaster(const QRectF& bounds, const std::function<void(QPainter&)>& draw,
                        quint32* counter);
    void recycleScratch();

    [[nodiscard]] QRect clipped(const QRect& r) const;

    BlitterSurface* m_surface = nullptr;
    uio_t* m_uio = nullptr;

    QHash<qint64, CachedImage> m_images;
    quint32 m_imageBytes = 0;
    quint32 m_imageBudget = 8U * 1024U * 1024U;

    // Scratch uploads made by the fallback path are per-frame garbage,
    // but the fabric is still reading frame N's sources while the A9
    // builds N+1, so they are freed two frames later -- never at the
    // end of the frame that made them.
    QList<Scratch> m_scratch[2];
    int m_scratchBank = 0;
    quint32 m_frame = 0;

    FallbackStats m_fallback;
    bool m_warnedGlyphSource = false;
    // Nonzero while QPaintEngine's text implementation is re-entering
    // through drawPath, so those pixels are attributed to text.
    int m_textFallbackDepth = 0;

    // Mirrored painter state.
    QColor m_brush = Qt::transparent;
    QColor m_pen = Qt::white;
    int m_penWidth = 1;
    bool m_penVisible = false;
    qreal m_opacity = 1.0;
    QTransform m_transform;
    QRect m_clip;
    bool m_hasClip = false;
};

} // namespace zaparoo::fpga
