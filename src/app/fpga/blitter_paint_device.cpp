// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_paint_device.h"

namespace zaparoo::fpga
{

namespace
{
// The MiSTer output is a fixed-geometry CRT-timed signal, not a
// desktop, so the notional density is whatever makes Qt do no scaling
// at all.
constexpr int kNominalDpi = 96;
constexpr qreal kMillimetresPerInch = 25.4;
} // namespace

BlitterPaintDevice::BlitterPaintDevice(BlitterSurface* surface)
    : m_size(surface != nullptr ? surface->size() : QSize(0, 0)),
      m_engine(std::make_unique<BlitterPaintEngine>(surface))
{
}

BlitterPaintDevice::~BlitterPaintDevice() = default;

QPaintEngine* BlitterPaintDevice::paintEngine() const
{
    return m_engine.get();
}

int BlitterPaintDevice::metric(PaintDeviceMetric metric) const
{
    switch (metric)
    {
    case PdmWidth:
        return m_size.width();
    case PdmHeight:
        return m_size.height();
    case PdmWidthMM:
        return qRound(m_size.width() * kMillimetresPerInch / kNominalDpi);
    case PdmHeightMM:
        return qRound(m_size.height() * kMillimetresPerInch / kNominalDpi);
    case PdmNumColors:
        // The fabric's WORK framebuffer is RGB565.
        return 1 << 16;
    case PdmDepth:
        return 16;
    case PdmDpiX:
    case PdmDpiY:
    case PdmPhysicalDpiX:
    case PdmPhysicalDpiY:
        return kNominalDpi;
    case PdmDevicePixelRatio:
        return 1;
    case PdmDevicePixelRatioScaled:
        return static_cast<int>(QPaintDevice::devicePixelRatioFScale());
    default:
        return 0;
    }
}

} // namespace zaparoo::fpga
