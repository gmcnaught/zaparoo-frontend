// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include "blitter_paint_engine.h"
#include "blitter_surface.h"

#include <QPaintDevice>
#include <QSize>
#include <memory>

namespace zaparoo::fpga
{

// The QPaintDevice Qt Quick's software renderer is pointed at.
//
// It owns no pixels. Its whole job is to answer metric() so the scene
// graph believes it is painting a framebuffer of the right shape, and
// to hand back the paint engine that turns those draws into a display
// list. QQuickRenderTarget::fromPaintDevice() is the public, software-
// backend-only hook that makes this legal.
class BlitterPaintDevice : public QPaintDevice
{
  public:
    explicit BlitterPaintDevice(BlitterSurface* surface);
    ~BlitterPaintDevice() override;

    [[nodiscard]] QPaintEngine* paintEngine() const override;

    [[nodiscard]] BlitterPaintEngine* engine() const
    {
        return m_engine.get();
    }
    [[nodiscard]] QSize size() const
    {
        return m_size;
    }

  protected:
    [[nodiscard]] int metric(PaintDeviceMetric metric) const override;

  private:
    QSize m_size;
    std::unique_ptr<BlitterPaintEngine> m_engine;
};

} // namespace zaparoo::fpga
