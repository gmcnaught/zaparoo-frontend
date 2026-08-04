// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace zaparoo::fpga
{

// Shipped v4 DDR layout of the blitter core, from mister-fpga-blitter
// docs/blitter-protocol.md §2. The region is reserved out of Linux's
// memory by the kernel command line, exactly like the Menu fork's
// native-video region at 0x3A000000.
//
// Note this contract and the Menu fork's native-video contract both
// claim 0x3A000000: the blitter fabric owns scanout itself (it burst-
// writes WORK to the DDR3 scanout double-buffer and flips fb_bank),
// so a blitter core and a native-video Menu core are alternative
// fabrics, never both loaded. That is why enabling the offload path
// disables the fb0 copy path outright rather than layering on it.
struct BlitterRegion
{
    static constexpr std::uintptr_t kBase = 0x3B000000U;
    static constexpr std::size_t kSize = 0x01200000U; // 18 MiB

    static constexpr std::size_t kCtrlOffset = 0x00000000U;
    static constexpr std::size_t kCtrlBytes = 0x40U;

    // ~16382 x 32 B, walked until BLT_OP_END.
    static constexpr std::size_t kRingOffset = 0x00000040U;
    static constexpr std::size_t kRingBytes = 0x00080000U; // 512 KiB

    // Texture upload heap. SPRITELIST and TRILIST entry arrays are
    // addressed as byte offsets into THIS heap (the fabric and the
    // reference model agree on that; only TILEMAP's GRID_BUF is a
    // separate read region, and we emit no TILEMAP), so the entry
    // arenas are carved from the heap rather than from the v4 map's
    // standalone SP_BUF/TL_BUF regions, which this application leaves
    // unused.
    static constexpr std::size_t kHeapOffset = 0x00080000U;
    static constexpr std::size_t kHeapBytes = 0x00EC0000U; // ~15.4 MiB

    // CLUT upload DMA source. The fabric's CLUT_UPLOAD FSM reads a
    // FIXED region (blitter_top.sv S_CLUT_RD/S_CLUT_WR) plus a running
    // index, so unlike the heap arenas this address is not negotiable
    // and the command's src_off field is ignored. 32 banks x 256
    // entries, one 32-bit entry per QWORD slot -- which is why the
    // host's packed u32 mirror has to be expanded into it rather than
    // memcpy'd (see BlitterSurface::publishClut).
    static constexpr std::size_t kClutOffset = 0x00FC3000U;
    static constexpr std::size_t kClutBytes = 0x00010000U; // 64 KiB
    static constexpr std::size_t kClutEntries = 32U * 256U;

    static_assert(kRingOffset + kRingBytes <= kHeapOffset);
    static_assert(kHeapOffset + kHeapBytes <= kClutOffset);
    static_assert(kClutEntries * 8U <= kClutBytes);
    static_assert(kClutOffset + kClutBytes <= kSize);
};

// Control block field indices. Each field is a u32 living at the start
// of its own qword slot, so field i is at byte offset i * 8. The block
// is exactly 8 slots: qword 8 is already the first ring command, and
// the protocol doc records a real bug from a control bit that landed
// there and read cmd0's opcode.
enum class ControlWord : std::size_t
{
    SubmitSeq = 0,  // ARM: doorbell, bumped last
    CmdCount = 1,   // ARM: valid commands in the ring this frame
    TargetBuf = 2,  // ARM: legacy target select
    ClearColor = 3, // ARM: RGB565 clear color
    Flags = 4,      // ARM: bit0 = clear before list
    DoneSeq = 5,    // fabric: equals SubmitSeq once composited
    Status = 6,     // fabric: 0 = OK, nonzero = error latch
    SrcSel = 7,     // ARM: bit0 SDRAM source enable, bit1 pipelined compositor
};

constexpr std::uint32_t kFlagClearBeforeList = 0x1U;

// Maps the blitter's reserved DDR region and drives the submit/done
// doorbell handshake. The emitter builds its command ring and uploads
// its sources directly into the mapped region, so a submitted frame
// costs no copy: publishing is four control writes and a doorbell.
//
// Not thread-safe. Every method must be called from the thread that
// owns the render loop.
class BlitterTransport
{
  public:
    BlitterTransport() = default;
    ~BlitterTransport();

    BlitterTransport(const BlitterTransport&) = delete;
    BlitterTransport& operator=(const BlitterTransport&) = delete;

    // Open /dev/mem and map the region. Logs the reason and returns
    // false on failure; the caller falls back to the software path.
    bool open();
    void close();
    bool isOpen() const
    {
        return m_base != nullptr;
    }

    // Caller-owned buffers handed to blt_emitter_init(). Valid only
    // while isOpen().
    std::uint8_t* ring() const;
    std::uint8_t* heap() const;
    std::uint8_t* clutBuffer() const;
    static constexpr std::size_t ringBytes()
    {
        return BlitterRegion::kRingBytes;
    }
    static constexpr std::size_t heapBytes()
    {
        return BlitterRegion::kHeapBytes;
    }

    // Publish the list already sitting in the ring. Writes cmd_count,
    // target_buf, clear_color and flags, then bumps submit_seq last
    // behind a release fence, which is the ordering the fabric's
    // "composite when submit_seq != done_seq" test depends on.
    void submit(std::uint32_t cmdCount, int targetBuf, bool clear, std::uint16_t clearColor);

    // Block until the fabric has finished the frame we last submitted.
    // Called at the START of the next frame, not the end of this one:
    // the contract allows a single frame in flight, so the A9 should
    // be building frame N+1 while the fabric composites N, and the
    // only thing that must not happen is overwriting the ring out from
    // under it. Returns false on timeout.
    bool waitForIdle(int timeoutMs);

    std::uint32_t status() const;
    std::uint32_t submitSeq() const;
    std::uint32_t doneSeq() const;

    // Frames whose waitForIdle() timed out since open(). A nonzero
    // value means the fabric stalled and frames were built on top of a
    // ring the fabric may still have been reading, so it is reported
    // rather than swallowed.
    std::uint32_t stalls() const
    {
        return m_stalls;
    }

  private:
    volatile std::uint32_t* word(ControlWord w) const;

    int m_memFd = -1;
    volatile std::uint8_t* m_base = nullptr;
    std::uint32_t m_submitSeq = 0;
    std::uint32_t m_stalls = 0;
};

} // namespace zaparoo::fpga
