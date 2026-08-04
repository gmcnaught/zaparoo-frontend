// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_transport.h"

#include <QLoggingCategory>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace zaparoo::fpga
{

namespace
{

// Poll interval while waiting on done_seq. The fabric composites a
// frame in well under a 60 Hz period, so this only ever spins a
// handful of times; sleeping rather than busy-waiting keeps the A9
// free for the next frame's display-list build, which is the whole
// point of the offload.
constexpr long kPollNanos = 200000L; // 200 us

void sleepPoll()
{
    timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = kPollNanos;
    nanosleep(&ts, nullptr);
}

} // namespace

BlitterTransport::~BlitterTransport()
{
    close();
}

volatile std::uint32_t* BlitterTransport::word(ControlWord w) const
{
    // One u32 per qword slot: field i lives at byte offset i * 8.
    const std::size_t offset = static_cast<std::size_t>(w) * 8U;
    return reinterpret_cast<volatile std::uint32_t*>(
        const_cast<std::uint8_t*>(m_base + BlitterRegion::kCtrlOffset + offset));
}

std::uint8_t* BlitterTransport::ring() const
{
    return m_base == nullptr ? nullptr
                             : const_cast<std::uint8_t*>(m_base) + BlitterRegion::kRingOffset;
}

std::uint8_t* BlitterTransport::heap() const
{
    return m_base == nullptr ? nullptr
                             : const_cast<std::uint8_t*>(m_base) + BlitterRegion::kHeapOffset;
}

std::uint8_t* BlitterTransport::clutBuffer() const
{
    return m_base == nullptr ? nullptr
                             : const_cast<std::uint8_t*>(m_base) + BlitterRegion::kClutOffset;
}

bool BlitterTransport::open()
{
    if (isOpen())
    {
        return true;
    }

    // O_SYNC matches the native video writer: the mapping must reach
    // DDR rather than sit in a writeback cache the fabric cannot
    // snoop. It also means source uploads are uncached writes, which
    // is why images are uploaded once and re-blitted rather than
    // re-uploaded per frame.
    m_memFd = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (m_memFd < 0)
    {
        qWarning("blitter transport: failed to open /dev/mem (%s)", std::strerror(errno));
        return false;
    }

    void* map = mmap(nullptr, BlitterRegion::kSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_memFd,
                     static_cast<off_t>(BlitterRegion::kBase));
    if (map == MAP_FAILED)
    {
        qWarning("blitter transport: failed to map blitter DDR region at 0x%08zx (%s)",
                 static_cast<std::size_t>(BlitterRegion::kBase), std::strerror(errno));
        close();
        return false;
    }
    m_base = static_cast<volatile std::uint8_t*>(map);

    // Adopt the fabric's current sequence rather than resetting it.
    // The core may already be running (a previous frontend launch, or
    // another client), and forcing submit_seq backwards would make
    // "submit_seq != done_seq" true for a frame we never built.
    m_submitSeq = *word(ControlWord::DoneSeq);
    *word(ControlWord::SubmitSeq) = m_submitSeq;
    m_stalls = 0;

    // Sources are read straight from the DDR3 heap: this application
    // stages nothing into SDRAM, so the SDRAM source mux stays off.
    // Leaving it set from a previous client would make every blit read
    // an unstaged SDRAM offset and composite garbage.
    *word(ControlWord::SrcSel) = 0;
    *word(ControlWord::TargetBuf) = 0;

    const std::uint32_t latched = *word(ControlWord::Status);
    if (latched != 0)
    {
        qWarning("blitter transport: fabric status latch is 0x%08x at open (ring overflow or "
                 "error from a previous client)",
                 latched);
    }

    qInfo("blitter transport: mapped 0x%08zx (+%zu KiB ring, +%zu MiB heap), seq=%u",
          static_cast<std::size_t>(BlitterRegion::kBase), BlitterRegion::kRingBytes / 1024,
          BlitterRegion::kHeapBytes / (std::size_t{1024} * 1024), m_submitSeq);
    return true;
}

void BlitterTransport::close()
{
    if (m_base != nullptr)
    {
        munmap(const_cast<std::uint8_t*>(m_base), BlitterRegion::kSize);
        m_base = nullptr;
    }
    if (m_memFd >= 0)
    {
        ::close(m_memFd);
        m_memFd = -1;
    }
}

void BlitterTransport::submit(std::uint32_t cmdCount, int targetBuf, bool clear,
                              std::uint16_t clearColor)
{
    if (!isOpen())
    {
        return;
    }

    *word(ControlWord::CmdCount) = cmdCount;
    *word(ControlWord::TargetBuf) = static_cast<std::uint32_t>(targetBuf);
    *word(ControlWord::ClearColor) = clearColor;
    *word(ControlWord::Flags) = clear ? kFlagClearBeforeList : 0U;

    // Store-release: every ring byte and control field above must be
    // visible to the fabric's read master before the doorbell that
    // tells it to look. On ARM this compiles to a DMB ST.
    std::atomic_thread_fence(std::memory_order_release);
    m_submitSeq++;
    *word(ControlWord::SubmitSeq) = m_submitSeq;
}

bool BlitterTransport::waitForIdle(int timeoutMs)
{
    if (!isOpen())
    {
        return true;
    }

    const long budget = static_cast<long>(timeoutMs) * 1000000L / kPollNanos;
    for (long i = 0; i < budget; i++)
    {
        if (*word(ControlWord::DoneSeq) == m_submitSeq)
        {
            // Acquire: the frame's completion must not be reordered
            // ahead of reads of anything the fabric wrote for it.
            std::atomic_thread_fence(std::memory_order_acquire);
            return true;
        }
        sleepPoll();
    }

    m_stalls++;
    qWarning("blitter transport: fabric did not complete frame %u within %d ms (done=%u, "
             "status=0x%08x); %u stall(s) so far",
             m_submitSeq, timeoutMs, *word(ControlWord::DoneSeq), *word(ControlWord::Status),
             m_stalls);
    return false;
}

std::uint32_t BlitterTransport::status() const
{
    return isOpen() ? *word(ControlWord::Status) : 0U;
}

std::uint32_t BlitterTransport::submitSeq() const
{
    return m_submitSeq;
}

std::uint32_t BlitterTransport::doneSeq() const
{
    return isOpen() ? *word(ControlWord::DoneSeq) : 0U;
}

} // namespace zaparoo::fpga
