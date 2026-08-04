// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#include "blitter_codec.h"

namespace zaparoo::fpga
{

void encodeGlyphKey(std::uint32_t key, char out[5])
{
    int n = 0;
    if (key < 0x80U)
    {
        out[n++] = static_cast<char>(key);
    }
    else if (key < 0x800U)
    {
        out[n++] = static_cast<char>(0xC0U | (key >> 6));
        out[n++] = static_cast<char>(0x80U | (key & 0x3FU));
    }
    else if (key < 0x10000U)
    {
        out[n++] = static_cast<char>(0xE0U | (key >> 12));
        out[n++] = static_cast<char>(0x80U | ((key >> 6) & 0x3FU));
        out[n++] = static_cast<char>(0x80U | (key & 0x3FU));
    }
    else
    {
        out[n++] = static_cast<char>(0xF0U | (key >> 18));
        out[n++] = static_cast<char>(0x80U | ((key >> 12) & 0x3FU));
        out[n++] = static_cast<char>(0x80U | ((key >> 6) & 0x3FU));
        out[n++] = static_cast<char>(0x80U | (key & 0x3FU));
    }
    out[n] = '\0';
}

void expandClut(const std::uint32_t* src, std::size_t entries, std::uint32_t* dst)
{
    for (std::size_t i = 0; i < entries; i++)
    {
        dst[i * 2U] = src[i];
        dst[(i * 2U) + 1U] = 0U;
    }
}

} // namespace zaparoo::fpga
