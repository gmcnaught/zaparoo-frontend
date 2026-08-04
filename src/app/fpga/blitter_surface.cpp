// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_surface.h"

#include <QLoggingCategory>
#include <cstring>

namespace zaparoo::fpga
{

namespace
{
constexpr std::size_t kClutMirrorBytes =
    static_cast<std::size_t>(BLT_CLUT_BANKS) * static_cast<std::size_t>(BLT_CLUT_ENTRIES) * 4U;
}

BlitterSurface::BlitterSurface() = default;

BlitterSurface::~BlitterSurface()
{
    shutdown();
}

bool BlitterSurface::initHardware(const Config& config)
{
    if (!m_transport.open())
    {
        return false;
    }
    m_hardware = true;
    m_clutDevice = m_transport.clutBuffer();

    m_clutMirror.assign(kClutMirrorBytes, 0);
    if (!bring_up(config, m_transport.ring(), BlitterTransport::ringBytes(), m_transport.heap(),
                  BlitterTransport::heapBytes(), m_clutMirror.data(), m_clutMirror.size()))
    {
        shutdown();
        return false;
    }

    qInfo("blitter surface: %dx%d, glyph atlas %dx%d (%d slots, %d phase(s))", config.size.width(),
          config.size.height(), config.glyphAtlasWidth, config.glyphAtlasHeight, config.glyphSlots,
          config.glyphPhases);
    return true;
}

bool BlitterSurface::initMemory(const Config& config, std::uint8_t* ring, std::size_t ringBytes,
                                std::uint8_t* heap, std::size_t heapBytes, std::uint8_t* clut,
                                std::size_t clutBytes)
{
    m_hardware = false;
    m_clutDevice = nullptr;
    return bring_up(config, ring, ringBytes, heap, heapBytes, clut, clutBytes);
}

bool BlitterSurface::bring_up(const Config& config, std::uint8_t* ring, std::size_t ringBytes,
                              std::uint8_t* heap, std::size_t heapBytes, std::uint8_t* clut,
                              std::size_t clutBytes)
{
    if (ring == nullptr || heap == nullptr || clut == nullptr)
    {
        qWarning("blitter surface: init called with a null region");
        return false;
    }

    m_config = config;

    // blt_emitter_init resets the heap allocator, which uio_init then
    // requires: its sprite arena has to land at heap offset 0, and it
    // refuses if the heap was not virgin. Re-initialising here is what
    // makes a restart over an already-used DDR region safe.
    blt_emitter_init(&m_emitter, ring, ringBytes, heap, heapBytes);

    if (uio_init(&m_uio, &m_emitter, heap, config.vertexArenaBytes, config.spriteArenaBytes) != 0)
    {
        qWarning("blitter surface: uio_init failed (vertex arena %u B, sprite arena %u B do not "
                 "fit in a %zu B heap)",
                 config.vertexArenaBytes, config.spriteArenaBytes, heapBytes);
        return false;
    }

    if (uio_clut_bind(&m_uio, clut, clutBytes) != 0)
    {
        qWarning("blitter surface: CLUT bind failed (%zu B, need %zu B)", clutBytes,
                 kClutMirrorBytes);
        return false;
    }

    m_glyphs = std::make_unique<BlitterGlyphSource>(config.glyphPhases);
    m_slots.assign(static_cast<std::size_t>(config.glyphSlots), uio_glyph_t{});
    if (uio_glyph_atlas_init(&m_uio, &m_atlas, config.glyphAtlasWidth, config.glyphAtlasHeight,
                             m_slots.data(), config.glyphSlots, m_glyphs->callback(),
                             m_glyphs->context(), m_glyphs->phases()) != 0)
    {
        qWarning("blitter surface: glyph atlas (%dx%d) did not fit in the source heap",
                 config.glyphAtlasWidth, config.glyphAtlasHeight);
        return false;
    }

    // The cap is the number of entries the channel will accept, NOT a
    // "use the default" sentinel: passing 0 makes every push a silent
    // drop and text never reaches the fabric. Ask for the maximum; the
    // channel clamps it to BLT_SPRITE_CHANNEL_MAX itself.
    blt_sprite_channel_init(&m_channel, &m_emitter, BLT_SPRITE_CHANNEL_MAX);
    m_dropped = 0;
    m_ready = true;
    return true;
}

void BlitterSurface::shutdown()
{
    m_ready = false;
    m_transport.close();
    m_glyphs.reset();
    m_slots.clear();
    m_clutMirror.clear();
    m_clutDevice = nullptr;
    m_uio = uio_t{};
    m_atlas = uio_glyph_atlas_t{};
    m_emitter = blt_emitter_t{};
}

void BlitterSurface::beginFrame()
{
    if (!m_ready)
    {
        return;
    }

    // Wait here rather than after submitting: the contract allows one
    // frame in flight, so the A9 should have spent the fabric's
    // compositing time building this list. The only thing that must
    // not happen is overwriting a ring the fabric is still walking.
    m_transport.waitForIdle(m_config.frameTimeoutMs);

    // Bumping the atlas frame is what makes LRU eviction safe: a glyph
    // touched in the last two frames is never evicted, because the
    // fabric may still be reading the frame we just submitted.
    uio_glyph_atlas_frame(&m_atlas);
    uio_begin_frame(&m_uio, 0, 1, m_config.clearColor);
}

void BlitterSurface::endFrame()
{
    if (!m_ready)
    {
        return;
    }

    flushTextBatch();
    uio_end_frame(&m_uio);

    if (m_emitter.dropped != 0)
    {
        // A dropped command means this frame is missing content. The
        // emitter still submits, so nothing else would ever surface
        // it.
        m_dropped += m_emitter.dropped;
        qWarning("blitter surface: ring full, %u command(s) dropped from frame %u (%u total)",
                 m_emitter.dropped, m_transport.submitSeq() + 1, m_dropped);
    }

    if (m_hardware)
    {
        m_transport.submit(static_cast<std::uint32_t>(m_emitter.cmd_count), m_emitter.target_buf,
                           (m_emitter.flags & kFlagClearBeforeList) != 0, m_emitter.clear_color);
    }
}

void BlitterSurface::openTextBatch()
{
    if (m_ready && !textBatchOpen())
    {
        uio_text_batch_begin(&m_uio, &m_channel);
    }
}

void BlitterSurface::flushTextBatch()
{
    if (!m_ready || !textBatchOpen())
    {
        return;
    }

    // Order matters: the batch's SPRITELIST commands are emitted by
    // the flush, so a CLUT carrying newly allocated colour ramps has
    // to be uploaded ahead of them or those glyphs resolve through
    // stale palette entries.
    if (uio_clut_dirty(&m_uio) != 0)
    {
        publishClut();
    }
    uio_text_batch_flush(&m_uio);
}

void BlitterSurface::publishClut()
{
    if (m_clutDevice != nullptr)
    {
        // The host mirror is a packed u32 array (what the reference
        // model reads); the fabric's DMA source is one u32 per QWORD
        // slot. Expanding rather than memcpy'ing is the whole
        // difference, and getting it wrong shows up as every fourth
        // colour ramp being right.
        expandClut(reinterpret_cast<const std::uint32_t*>(m_clutMirror.data()),
                   BlitterRegion::kClutEntries, reinterpret_cast<std::uint32_t*>(m_clutDevice));
    }
    uio_clut_upload(&m_uio);
}

} // namespace zaparoo::fpga
