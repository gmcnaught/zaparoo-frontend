// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include "blitter_paint_device.h"
#include "blitter_surface.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <memory>

QT_BEGIN_NAMESPACE
class QQmlEngine;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;
QT_END_NAMESPACE

namespace zaparoo::fpga
{

// Drives Qt Quick's software renderer into the blitter instead of into
// a framebuffer.
//
// The scene is rendered through a QQuickRenderControl into an
// invisible QQuickWindow whose render target is a BlitterPaintDevice,
// so every draw the scene graph makes lands in a display list and the
// A9 composites nothing. There is no /dev/fb0 and no per-frame memcpy
// on this path: the fabric owns scanout.
//
// IMPORTANT -- the QML entry point must be rooted at an Item, not a
// Window. A QQuickWindow has to be constructed with its render control,
// so a QML-declared Window (which constructs its own) cannot be
// redirected. start() reports this and returns false rather than
// half-working, and the caller falls back to the software path. See
// docs/fpga-offload.md for what the frontend's own tree needs.
class BlitterRenderLoop : public QObject
{
    Q_OBJECT

  public:
    struct Config
    {
        QSize size{352, 240};
        // Item-rooted QML component to host.
        QString module = QStringLiteral("Zaparoo.App");
        QString type = QStringLiteral("OffloadRoot");
        int targetFps = 60;
        // Log a frame accounting line every N frames; 0 disables.
        int statsInterval = 0;
    };

    explicit BlitterRenderLoop(QObject* parent = nullptr);
    ~BlitterRenderLoop() override;

    BlitterRenderLoop(const BlitterRenderLoop&) = delete;
    BlitterRenderLoop& operator=(const BlitterRenderLoop&) = delete;
    BlitterRenderLoop(BlitterRenderLoop&&) = delete;
    BlitterRenderLoop& operator=(BlitterRenderLoop&&) = delete;

    // Bring up the render control, load the QML, and start rendering.
    // Returns false (having logged why) if the fabric, the render
    // control, or the QML entry point is unusable.
    bool start(QQmlEngine* qmlEngine, BlitterSurface* surface, const Config& config);
    void stop();

    // The window the scene lives in, so the caller can route input to
    // it. Null until start() succeeds.
    [[nodiscard]] QQuickWindow* window() const
    {
        return m_window.get();
    }

    [[nodiscard]] quint64 framesRendered() const
    {
        return m_frames;
    }

  private:
    void renderFrame();
    void requestUpdate();
    void logFrameStats();

    BlitterSurface* m_surface = nullptr;
    Config m_config;

    std::unique_ptr<QQuickRenderControl> m_control;
    std::unique_ptr<QQuickWindow> m_window;
    std::unique_ptr<BlitterPaintDevice> m_device;
    QQuickItem* m_root = nullptr;

    QTimer m_timer;
    bool m_pending = false;
    bool m_running = false;
    quint64 m_frames = 0;
    QElapsedTimer m_uptime;
};

} // namespace zaparoo::fpga
