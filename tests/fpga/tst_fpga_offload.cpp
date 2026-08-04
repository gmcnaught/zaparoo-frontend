// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0
//
// Host tests for the FPGA offload's wire encodings and display lists.
//
// No Qt and no hardware: every display list these build is executed by
// the golden reference model the RTL is diffed against, so a regression
// here is a real regression in what the fabric would composite. What is
// deliberately NOT covered is the Qt-facing translation layer, which
// needs a running scene graph.

#include "blitter_codec.h"

extern "C"
{
#include "vendor/blitter_ref.h"
#include "vendor/glyph_cache.h"
#include "vendor/ui_offload.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
    g_checks++;
    if (!ok)
    {
        g_failures++;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

// ── a display-list harness over plain memory ─────────────────────────

constexpr std::size_t kRingBytes = 256U * 1024U;
constexpr std::size_t kHeapBytes = 4U * 1024U * 1024U;
constexpr std::size_t kClutBytes = static_cast<std::size_t>(BLT_CLUT_BANKS) * BLT_CLUT_ENTRIES * 4U;

struct Harness
{
    std::vector<std::uint8_t> ring{std::vector<std::uint8_t>(kRingBytes, 0)};
    std::vector<std::uint8_t> heap{std::vector<std::uint8_t>(kHeapBytes, 0)};
    std::vector<std::uint8_t> clut{std::vector<std::uint8_t>(kClutBytes, 0)};
    std::vector<std::uint16_t> fb{std::vector<std::uint16_t>(BLT_FB_PIXELS, 0)};
    blt_emitter_t emitter{};
    uio_t uio{};

    bool init(std::uint32_t vertexBytes = 64U * 1024U, std::uint32_t spriteBytes = 64U * 1024U)
    {
        blt_emitter_init(&emitter, ring.data(), ring.size(), heap.data(), heap.size());
        if (uio_init(&uio, &emitter, heap.data(), vertexBytes, spriteBytes) != 0)
        {
            return false;
        }
        return uio_clut_bind(&uio, clut.data(), clut.size()) == 0;
    }

    // Run the frame's list against the reference model, exactly as the
    // fabric walks it. The ring holds the packed 8-word wire format,
    // not the host struct, so every command is unpacked first.
    int execute()
    {
        // The CLEAR flag is a control-block field, not a ring command:
        // the fabric fills the target before walking the list. The
        // model has no notion of it, so the harness does what the
        // hardware would.
        if ((emitter.flags & 0x1U) != 0)
        {
            std::fill(fb.begin(), fb.end(), emitter.clear_color);
        }

        const int count = emitter.cmd_count;
        std::vector<blt_cmd_t> cmds(static_cast<std::size_t>(count) + 1);
        for (int i = 0; i < count; i++)
        {
            blt_unpack_cmd(ring.data() + (static_cast<std::size_t>(i) * BLT_CMD_BYTES), &cmds[i]);
        }

        blt_surface_heap_t sources{};
        sources.base = heap.data();
        sources.size = heap.size();
        sources.clut = clut.data();
        return blt_execute(fb.data(), &sources, cmds.data(), count);
    }

    std::uint16_t pixel(int x, int y) const
    {
        return fb[(static_cast<std::size_t>(y) * BLT_FB_WIDTH) + x];
    }
};

// ── a stub rasterizer that records what the cache asked for ──────────

struct StubFont
{
    std::vector<std::uint32_t> requested;
    std::vector<std::uint8_t> coverage;

    static int raster(void* ctx, std::uint32_t codepoint, int pxSize, int phase,
                      uio_glyph_bmp_t* out)
    {
        auto* self = static_cast<StubFont*>(ctx);
        self->requested.push_back(codepoint);

        // A solid square, so coverage is unambiguous and a fully
        // covered texel must composite exactly like a FILL.
        const int side = pxSize > 0 ? pxSize : 8;
        self->coverage.assign(static_cast<std::size_t>(side) * side, 255);
        (void)phase;

        *out = uio_glyph_bmp_t{};
        out->cov = self->coverage.data();
        out->pitch = side;
        out->w = side;
        out->h = side;
        out->bearing_x = 0;
        out->bearing_y = side;
        out->advance = side + 1;
        return 0;
    }
};

// ── tests ────────────────────────────────────────────────────────────

// The glyph key travels to the cache through a string, so the encoder
// has to be the exact inverse of the vendored decoder. If it is not,
// glyphs silently collide or land in the wrong cache slot.
void testGlyphKeyRoundTrip()
{
    Harness h;
    check(h.init(), "harness init");

    StubFont stub;
    std::vector<uio_glyph_t> slots(256);
    uio_glyph_atlas_t atlas{};
    check(uio_glyph_atlas_init(&h.uio, &atlas, 256, 256, slots.data(),
                               static_cast<int>(slots.size()), &StubFont::raster, &stub, 1) == 0,
          "glyph atlas init");

    // Sweep the whole key space this encoding has to serve: every
    // font id crossed with glyph indexes that straddle each UTF-8
    // length boundary and the surrogate range a conforming encoder
    // would have refused.
    const std::uint32_t indexes[] = {0U,      1U,      0x7FU,   0x80U,   0x7FFU, 0x800U,
                                     0xD7FFU, 0xD800U, 0xDFFFU, 0xE000U, 0xFFFFU};

    uio_begin_frame(&h.uio, 0, 1, 0);
    for (int fontId = 1; fontId <= zaparoo::fpga::kMaxGlyphFonts; fontId++)
    {
        for (std::uint32_t index : indexes)
        {
            const std::uint32_t key = zaparoo::fpga::makeGlyphKey(fontId, index);
            char encoded[5];
            zaparoo::fpga::encodeGlyphKey(key, encoded);

            check(std::strlen(encoded) > 0, "encoded key is never empty");

            stub.requested.clear();
            uio_text_run_fx(&h.uio, &atlas, 8, 0, 100, encoded, 0xFFFFU);
            check(stub.requested.size() == 1, "exactly one glyph decoded from a one-key string");
            if (!stub.requested.empty())
            {
                check(stub.requested.front() == key, "decoded key matches the encoded key");
            }
        }
    }
    uio_end_frame(&h.uio);
}

// Distinct (font, glyph) pairs must never collide, or one font's
// glyphs render with another's shapes.
void testGlyphKeyPacking()
{
    bool distinct = true;
    std::vector<std::uint32_t> seen;
    for (int fontId = 1; fontId <= zaparoo::fpga::kMaxGlyphFonts; fontId++)
    {
        for (std::uint32_t index = 0; index < 8U; index++)
        {
            seen.push_back(zaparoo::fpga::makeGlyphKey(fontId, index));
        }
    }
    for (std::size_t i = 0; i < seen.size() && distinct; i++)
    {
        for (std::size_t j = i + 1; j < seen.size(); j++)
        {
            if (seen[i] == seen[j])
            {
                distinct = false;
                break;
            }
        }
    }
    check(distinct, "glyph keys are collision-free across fonts");
    check(zaparoo::fpga::makeGlyphKey(1, 0) != 0,
          "a key is never zero (it would terminate its own string)");

    // The largest key must still fit the four-byte form the decoder
    // reads, i.e. 21 bits.
    const std::uint32_t largest =
        zaparoo::fpga::makeGlyphKey(zaparoo::fpga::kMaxGlyphFonts, 0xFFFFU);
    check(largest < (1U << 21), "the largest key fits the permissive 4-byte form");
}

// The host mirror is packed u32; the fabric's DMA source is one u32
// per qword. Getting this wrong shows up as every second colour ramp
// being wrong, which is exactly the kind of bug that is easier to
// pin here than on hardware.
void testClutExpansion()
{
    constexpr std::size_t kEntries = 64;
    std::vector<std::uint32_t> src(kEntries);
    for (std::size_t i = 0; i < kEntries; i++)
    {
        src[i] = static_cast<std::uint32_t>(0xA0000000U | i);
    }

    std::vector<std::uint32_t> dst(kEntries * 2, 0xDEADBEEFU);
    zaparoo::fpga::expandClut(src.data(), kEntries, dst.data());

    bool ok = true;
    for (std::size_t i = 0; i < kEntries; i++)
    {
        ok = ok && dst[i * 2] == src[i];
        ok = ok && dst[(i * 2) + 1] == 0U;
    }
    check(ok, "CLUT expands to one u32 per qword slot with a zeroed high half");
}

// The engine flushes the text batch before every non-text draw so
// z-order survives. That must not change the pixels a pure-text frame
// produces -- if it does, batching is not transparent and the
// ordering fix would be trading correctness for correctness.
void testBatchingIsPixelIdentical()
{
    std::vector<std::uint16_t> batched;
    std::vector<std::uint16_t> unbatched;

    for (int pass = 0; pass < 2; pass++)
    {
        Harness h;
        check(h.init(), "harness init");

        StubFont stub;
        std::vector<uio_glyph_t> slots(256);
        uio_glyph_atlas_t atlas{};
        check(uio_glyph_atlas_init(&h.uio, &atlas, 256, 256, slots.data(),
                                   static_cast<int>(slots.size()), &StubFont::raster, &stub,
                                   1) == 0,
              "glyph atlas init");

        blt_sprite_channel_t channel{};
        blt_sprite_channel_init(&channel, &h.emitter, BLT_SPRITE_CHANNEL_MAX);

        uio_begin_frame(&h.uio, 0, 1, 0x0000);
        uio_glyph_atlas_frame(&atlas);
        if (pass == 0)
        {
            uio_text_batch_begin(&h.uio, &channel);
        }

        // Three colours, so the batch has to carry a per-entry palette
        // word rather than one per command.
        const std::uint16_t colors[] = {0xF800U, 0x07E0U, 0x001FU};
        for (int i = 0; i < 12; i++)
        {
            const std::uint32_t key = zaparoo::fpga::makeGlyphKey(1, static_cast<std::uint32_t>(i));
            char encoded[5];
            zaparoo::fpga::encodeGlyphKey(key, encoded);
            uio_text_run_fx(&h.uio, &atlas, 8, (10 + (i * 10)) << 8, 60, encoded, colors[i % 3]);
        }

        if (uio_clut_dirty(&h.uio) != 0)
        {
            uio_clut_upload(&h.uio);
        }
        if (pass == 0)
        {
            uio_text_batch_flush(&h.uio);
        }
        uio_end_frame(&h.uio);
        h.execute();

        (pass == 0 ? batched : unbatched) = h.fb;
    }

    check(batched == unbatched, "batched glyph output is pixel-identical to unbatched");

    bool inked = false;
    for (std::uint16_t px : batched)
    {
        if (px != 0)
        {
            inked = true;
            break;
        }
    }
    check(inked, "the glyph test actually drew something");
}

// A fully covered glyph texel resolves through the ramp's top entry,
// which must composite exactly like a FILL of the same colour --
// otherwise text sits on a slightly different colour to the shapes
// around it.
void testFullCoverageMatchesFill()
{
    Harness h;
    check(h.init(), "harness init");

    StubFont stub;
    std::vector<uio_glyph_t> slots(64);
    uio_glyph_atlas_t atlas{};
    check(uio_glyph_atlas_init(&h.uio, &atlas, 128, 128, slots.data(),
                               static_cast<int>(slots.size()), &StubFont::raster, &stub, 1) == 0,
          "glyph atlas init");

    const std::uint16_t color = 0xFD20U;

    uio_begin_frame(&h.uio, 0, 1, 0x0000);
    uio_glyph_atlas_frame(&atlas);
    uio_fill(&h.uio, uio_rect_t{10, 10, 8, 8}, color, 255);

    char encoded[5];
    zaparoo::fpga::encodeGlyphKey(zaparoo::fpga::makeGlyphKey(1, 65), encoded);
    // Baseline at y = 38 with bearing_y = 8 puts the 8x8 solid block
    // at y = 30.
    uio_text_run_fx(&h.uio, &atlas, 8, 10 << 8, 38, encoded, color);
    if (uio_clut_dirty(&h.uio) != 0)
    {
        uio_clut_upload(&h.uio);
    }
    uio_end_frame(&h.uio);
    h.execute();

    check(h.pixel(12, 12) == color, "FILL wrote the expected colour");
    check(h.pixel(12, 32) == color, "a fully covered glyph texel matches the FILL exactly");
}

// The frame the offload exists for: a background, rounded cards, and a
// cover scaled at a ratio that changes per frame. Asserts the shape of
// the list, not just that it ran.
void testFrameShape()
{
    Harness h;
    check(h.init(), "harness init");

    // A small RGB565 source, uploaded once and re-blitted.
    constexpr int kSrc = 32;
    std::vector<std::uint16_t> art(kSrc * kSrc);
    for (int y = 0; y < kSrc; y++)
    {
        for (int x = 0; x < kSrc; x++)
        {
            art[(y * kSrc) + x] = static_cast<std::uint16_t>((x * 2048) ^ (y * 31));
        }
    }
    const uio_image_ref_t image = uio_upload_image(&h.uio, art.data(), kSrc, kSrc, kSrc * 2);
    check(image.surf.valid != 0, "image uploaded");

    uio_begin_frame(&h.uio, 0, 1, 0x18E3U);
    const int afterClear = h.emitter.cmd_count;

    check(uio_rounded_rect(&h.uio, uio_rect_t{20, 20, 80, 60}, 8, 0x4208U, 255) == 0,
          "rounded rect emitted");
    check(h.emitter.cmd_count - afterClear == 7,
          "a rounded rect is 3 fills + 4 antialiased corner blits");

    const int beforeScale = h.emitter.cmd_count;
    uio_scale_t scale{};
    scale.dst = uio_rect_t{120, 20, 101, 101}; // a tier-mismatched cover box
    scale.blend = BLT_BLEND_COPY;
    scale.alpha = 255;
    const int path = uio_image_scaled(&h.uio, image, &scale);
    check(path == UIO_PATH_TRILIST, "an arbitrary ratio resamples on the fabric");
    check(h.emitter.cmd_count - beforeScale == 1, "a scaled cover is one command at any ratio");

    uio_end_frame(&h.uio);

    check(h.uio.last_error == 0, "no emitter errors");
    check(h.emitter.overflow == 0, "no ring or heap overflow");
    check(h.emitter.dropped == 0, "no commands dropped");

    const int executed = h.execute();
    check(executed > 0, "the reference model executed the list");
    check(h.uio.stats.a9_fill_px > 0, "pixels were accounted as moved off the A9");

    // The clear colour must survive where nothing was drawn, and the
    // card must not have leaked outside its rect.
    check(h.pixel(2, 2) == 0x18E3U, "background is the clear colour");
    check(h.pixel(60, 50) == 0x4208U, "card interior is filled");
    check(h.pixel(19, 19) == 0x18E3U, "nothing drew outside the card");
}

// An animated focus zoom walks a different ratio every frame. It must
// stay at one command per frame and must not leak the vertex arena,
// which is the failure that would surface as a mid-session overflow.
void testAnimatedZoomDoesNotLeak()
{
    Harness h;
    check(h.init(), "harness init");

    constexpr int kSrc = 16;
    std::vector<std::uint16_t> art(kSrc * kSrc, 0x07E0U);
    const uio_image_ref_t image = uio_upload_image(&h.uio, art.data(), kSrc, kSrc, kSrc * 2);
    check(image.surf.valid != 0, "image uploaded");

    bool oneCommandPerFrame = true;
    for (int frame = 0; frame < 240; frame++)
    {
        uio_begin_frame(&h.uio, 0, 1, 0);
        const int before = h.emitter.cmd_count;

        uio_scale_t scale{};
        const int side = 40 + (frame % 60);
        scale.dst = uio_rect_t{10, 10, side, side};
        scale.blend = BLT_BLEND_COPY;
        scale.alpha = 255;
        uio_image_scaled(&h.uio, image, &scale);

        if (h.emitter.cmd_count - before != 1)
        {
            oneCommandPerFrame = false;
        }
        uio_end_frame(&h.uio);
    }

    check(oneCommandPerFrame, "an animated zoom stays at one command per frame");
    check(h.emitter.overflow == 0, "the vertex arena is reset per frame, not leaked");
    check(h.uio.last_error == 0, "no errors across 240 animated frames");
}

} // namespace

int main()
{
    testGlyphKeyRoundTrip();
    testGlyphKeyPacking();
    testClutExpansion();
    testBatchingIsPixelIdentical();
    testFullCoverageMatchesFill();
    testFrameShape();
    testAnimatedZoomDoesNotLeak();

    std::printf("fpga offload: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
