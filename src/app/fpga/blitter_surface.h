// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include "blitter_codec.h"
#include "blitter_glyph_source.h"
#include "blitter_transport.h"
#include "blitter_vendor.h"

#include <QSize>
#include <cstdint>
#include <memory>
#include <vector>

namespace zaparoo::fpga
{

// Owns one frame's worth of blitter state: the display-list emitter,
// the offload layer bound to it, the glyph atlas and its CLUT ramps,
// and the transport that publishes finished lists.
//
// The frame lifecycle is deliberately here rather than in the paint
// engine: which buffer to target, whether to clear, and when to ring
// the doorbell are transport decisions, and a QPaintEngine's
// begin()/end() bracket a paint session, which is not the same thing.
//
// A "surface" can also be built with no transport at all, over
// caller-supplied memory. That is what the host tests use to execute
// display lists against the golden reference model with no hardware in
// sight.
class BlitterSurface
{
  public:
    struct Config
    {
        QSize size{352, 240};
        std::uint16_t clearColor = 0;
        int glyphAtlasWidth = 512;
        int glyphAtlasHeight = 512;
        int glyphSlots = 2048;
        int glyphPhases = 1;
        std::uint32_t vertexArenaBytes = 256U * 1024U;
        std::uint32_t spriteArenaBytes = 128U * 1024U;
        int frameTimeoutMs = 100;
    };

    BlitterSurface();
    ~BlitterSurface();

    BlitterSurface(const BlitterSurface&) = delete;
    BlitterSurface& operator=(const BlitterSurface&) = delete;
    BlitterSurface(BlitterSurface&&) = delete;
    BlitterSurface& operator=(BlitterSurface&&) = delete;

    // Map the fabric and bring the offload layer up. Returns false and
    // logs the reason if the region cannot be mapped or an arena does
    // not fit; the caller then stays on the software path.
    bool initHardware(const Config& config);

    // Bring the offload layer up over caller-owned memory, with no
    // hardware. `clut` must be a packed BLT_CLUT_BANKS*BLT_CLUT_ENTRIES
    // u32 mirror. Used by the host tests.
    bool initMemory(const Config& config, std::uint8_t* ring, std::size_t ringBytes,
                    std::uint8_t* heap, std::size_t heapBytes, std::uint8_t* clut,
                    std::size_t clutBytes);

    void shutdown();
    [[nodiscard]] bool isValid() const
    {
        return m_ready;
    }

    // Wait for the fabric to finish the previous frame, then start a
    // new display list.
    void beginFrame();
    // Close any open text batch, publish the CLUT if ramps changed,
    // append END, and ring the doorbell.
    void endFrame();

    uio_t* offload()
    {
        return &m_uio;
    }
    uio_glyph_atlas_t* glyphAtlas()
    {
        return &m_atlas;
    }
    BlitterGlyphSource& glyphSource()
    {
        return *m_glyphs;
    }
    [[nodiscard]] const Config& config() const
    {
        return m_config;
    }
    [[nodiscard]] QSize size() const
    {
        return m_config.size;
    }

    // Text accumulates into one SPRITELIST batch while consecutive
    // glyph draws arrive. Any non-text draw MUST call this first: the
    // batch's commands are emitted at flush time, so leaving it open
    // across another primitive would composite the text above a draw
    // that the scene graph put on top of it.
    void openTextBatch();
    void flushTextBatch();
    [[nodiscard]] bool textBatchOpen() const
    {
        return uio_text_batch_active(&m_uio) != 0;
    }

    [[nodiscard]] const uio_stats_t& stats() const
    {
        return m_uio.stats;
    }
    BlitterTransport& transport()
    {
        return m_transport;
    }
    // Commands lost to a full ring, accumulated since init. Nonzero
    // means frames were published with content missing.
    [[nodiscard]] std::uint32_t droppedCommands() const
    {
        return m_dropped;
    }

  private:
    bool bring_up(const Config& config, std::uint8_t* ring, std::size_t ringBytes,
                  std::uint8_t* heap, std::size_t heapBytes, std::uint8_t* clut,
                  std::size_t clutBytes);
    // Expand the packed host CLUT mirror into the fabric's fixed
    // qword-strided CLUT_BUF region and emit the upload command.
    void publishClut();

    Config m_config;
    BlitterTransport m_transport;
    blt_emitter_t m_emitter{};
    uio_t m_uio{};
    uio_glyph_atlas_t m_atlas{};
    blt_sprite_channel_t m_channel{};
    std::vector<uio_glyph_t> m_slots;
    std::vector<std::uint8_t> m_clutMirror;
    std::unique_ptr<BlitterGlyphSource> m_glyphs;
    std::uint8_t* m_clutDevice = nullptr; // fabric CLUT_BUF, or null in memory mode
    std::uint32_t m_dropped = 0;
    bool m_ready = false;
    bool m_hardware = false;
};

} // namespace zaparoo::fpga
