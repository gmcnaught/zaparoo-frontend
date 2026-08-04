// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "fpga_offload.h"

#include "blitter_render_loop.h"
#include "blitter_surface.h"
#include "blitter_text_item.h"

#include <QByteArray>
#include <QLoggingCategory>
#include <memory>

namespace zaparoo::fpga
{

namespace
{

std::unique_ptr<BlitterSurface> g_surface;
std::unique_ptr<BlitterRenderLoop> g_loop;

bool envFlag(const char* name)
{
    const QByteArray value = qgetenv(name);
    return value == "1" || value.toLower() == "true";
}

int envInt(const char* name, int fallback)
{
    const QByteArray value = qgetenv(name);
    if (value.isEmpty())
    {
        return fallback;
    }
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? parsed : fallback;
}

} // namespace

bool startOffload(QQmlEngine* engine, QSize videoSize)
{
    if (!envFlag("ZAPAROO_FPGA_OFFLOAD"))
    {
        return false;
    }

    if (!videoSize.isValid() || videoSize.isEmpty())
    {
        qWarning("fpga offload: video size %dx%d is not usable; staying on the software path",
                 videoSize.width(), videoSize.height());
        return false;
    }

    qInfo("fpga offload: requested; shaped-glyph access is %s",
          haveShapedGlyphAccess() ? "available (QML text can reach the fabric)"
                                  : "UNAVAILABLE (QML text will rasterize on the CPU)");

    BlitterSurface::Config surfaceConfig;
    surfaceConfig.size = videoSize;
    surfaceConfig.glyphPhases = envInt("ZAPAROO_FPGA_GLYPH_PHASES", 1);
    surfaceConfig.glyphAtlasWidth = envInt("ZAPAROO_FPGA_GLYPH_ATLAS", 512);
    surfaceConfig.glyphAtlasHeight = surfaceConfig.glyphAtlasWidth;

    g_surface = std::make_unique<BlitterSurface>();
    if (!g_surface->initHardware(surfaceConfig))
    {
        qWarning("fpga offload: fabric unavailable; staying on the software path");
        g_surface.reset();
        return false;
    }

    BlitterRenderLoop::Config loopConfig;
    loopConfig.size = videoSize;
    loopConfig.targetFps = envInt("ZAPAROO_FPGA_TARGET_FPS", 60);
    loopConfig.statsInterval = envInt("ZAPAROO_FPGA_STATS_INTERVAL", 0);

    g_loop = std::make_unique<BlitterRenderLoop>();
    if (!g_loop->start(engine, g_surface.get(), loopConfig))
    {
        g_loop.reset();
        g_surface.reset();
        return false;
    }

    qInfo("fpga offload: active; the native video writer and /dev/fb0 copy are bypassed");
    return true;
}

void stopOffload()
{
    g_loop.reset();
    g_surface.reset();
}

bool offloadActive()
{
    return g_loop != nullptr;
}

} // namespace zaparoo::fpga
