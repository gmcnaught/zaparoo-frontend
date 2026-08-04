// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_render_loop.h"

#include <QByteArray>
#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>

namespace zaparoo::fpga
{

BlitterRenderLoop::BlitterRenderLoop(QObject* parent) : QObject(parent) {}

BlitterRenderLoop::~BlitterRenderLoop()
{
    stop();
}

bool BlitterRenderLoop::start(QQmlEngine* qmlEngine, BlitterSurface* surface, const Config& config)
{
    if (qmlEngine == nullptr || surface == nullptr || !surface->isValid())
    {
        qCritical("blitter render loop: no QML engine or the fabric surface is not up");
        return false;
    }

    m_surface = surface;
    m_config = config;

    // The scene graph must be the software adaptation: fromPaintDevice
    // is only legal there, and the Cyclone V has no GPU to run
    // anything else anyway. Asserting it here turns a confusing
    // runtime failure into a startup message.
    if (qgetenv("QSG_RHI_BACKEND") != QByteArrayLiteral("software") &&
        qgetenv("QT_QUICK_BACKEND") != QByteArrayLiteral("software"))
    {
        qWarning("blitter render loop: QT_QUICK_BACKEND is not 'software'; the blitter render "
                 "target requires the software adaptation");
    }

    m_control = std::make_unique<QQuickRenderControl>();
    m_window = std::make_unique<QQuickWindow>(m_control.get());
    m_window->setGeometry(0, 0, config.size.width(), config.size.height());
    m_window->setColor(Qt::black);

    m_device = std::make_unique<BlitterPaintDevice>(surface);
    // Must be set before initialize(): the scene graph reads the
    // target when it brings its renderer up.
    m_window->setRenderTarget(QQuickRenderTarget::fromPaintDevice(m_device.get()));

    if (!m_control->initialize())
    {
        qCritical("blitter render loop: QQuickRenderControl::initialize() failed");
        stop();
        return false;
    }

    QQmlComponent component(qmlEngine);
    component.loadFromModule(config.module, config.type);
    if (component.isError())
    {
        for (const QQmlError& error : component.errors())
        {
            qCritical("blitter render loop: %s", qPrintable(error.toString()));
        }
        stop();
        return false;
    }

    std::unique_ptr<QObject> created(component.create());
    if (created == nullptr)
    {
        qCritical("blitter render loop: %s.%s produced no object", qPrintable(config.module),
                  qPrintable(config.type));
        stop();
        return false;
    }

    auto* item = qobject_cast<QQuickItem*>(created.get());
    if (item == nullptr)
    {
        // Almost always because the entry point is a Window. A
        // QQuickWindow has to be built with its render control, so a
        // QML-declared Window can never be redirected into a paint
        // device -- there is no way to make this work at runtime, and
        // pretending otherwise would render a blank screen.
        qCritical("blitter render loop: %s.%s is not an Item (a QML Window cannot be redirected "
                  "to a render target; the offload entry point must be Item-rooted). Falling "
                  "back to the software path.",
                  qPrintable(config.module), qPrintable(config.type));
        stop();
        return false;
    }

    created.release();
    m_root = item;
    m_root->setParent(m_window.get());
    m_root->setParentItem(m_window->contentItem());
    m_root->setSize(QSizeF(config.size));

    // Qt asks for a new frame through the render control rather than
    // through a platform window, since there is no platform window.
    connect(m_control.get(), &QQuickRenderControl::renderRequested, this,
            &BlitterRenderLoop::requestUpdate);
    connect(m_control.get(), &QQuickRenderControl::sceneChanged, this,
            &BlitterRenderLoop::requestUpdate);

    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &BlitterRenderLoop::renderFrame);

    m_running = true;
    m_uptime.start();
    qInfo("blitter render loop: running %dx%d at up to %d fps, hosting %s.%s", config.size.width(),
          config.size.height(), config.targetFps, qPrintable(config.module),
          qPrintable(config.type));

    requestUpdate();
    return true;
}

void BlitterRenderLoop::stop()
{
    m_running = false;
    m_timer.stop();

    if (m_control != nullptr)
    {
        m_control->invalidate();
    }
    // m_root's QObject parent is the window, so destroying the window
    // destroys it. Deleting it here as well would be a double free.
    m_root = nullptr;
    m_window.reset();
    m_control.reset();
    m_device.reset();
}

void BlitterRenderLoop::requestUpdate()
{
    if (!m_running || m_pending)
    {
        return;
    }
    m_pending = true;

    // Frames are only built when something changed. An idle menu
    // screen emits no scene change, so it costs no display list, no
    // ring traffic, and no A9 time -- the same property the existing
    // frameSwapped-driven copy path relies on.
    const int interval = m_config.targetFps > 0 ? 1000 / m_config.targetFps : 0;
    m_timer.start(interval);
}

void BlitterRenderLoop::renderFrame()
{
    m_pending = false;
    if (!m_running || m_surface == nullptr)
    {
        return;
    }

    m_control->polishItems();

    // beginFrame() blocks until the fabric finished the PREVIOUS list,
    // so the A9 spends the fabric's compositing time doing sync/render
    // rather than waiting.
    m_surface->beginFrame();

    m_control->beginFrame();
    m_control->sync();
    m_control->render();
    m_control->endFrame();

    m_surface->endFrame();

    m_frames++;
    if (m_config.statsInterval > 0 && (m_frames % m_config.statsInterval) == 0)
    {
        logFrameStats();
    }
}

void BlitterRenderLoop::logFrameStats()
{
    const uio_stats_t& s = m_surface->stats();
    const FallbackStats& fb = m_device->engine()->fallbacks();

    // a9_* are the pixels a QPainter path would have rasterized and no
    // longer does; fb.px is what still rasterizes. The second number is
    // the one that decides whether this was worth building.
    qInfo("blitter frame %llu: %u cmds (%u fill, %u blit, %u tri, %u glyph in %u batch), "
          "%llu px on fabric, %llu px avoided, fallback %u draws / %llu px",
          static_cast<unsigned long long>(m_frames), s.cmds, s.fills, s.blits, s.trilists, s.glyphs,
          s.glyph_batches, static_cast<unsigned long long>(s.fabric_px),
          static_cast<unsigned long long>(s.a9_fill_px + s.a9_aa_px + s.a9_resample_px +
                                          s.a9_glyph_px),
          fb.draws(), static_cast<unsigned long long>(fb.px));
}

} // namespace zaparoo::fpga
