// WDT fixup: mask a modern tile-index (WDT) to the flags + MAIN-entry shape the Client reads.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "WdtFixup.hpp"

#include <cstring>

// MPHD flags live at +0x14; the MAIN tile grid (64x64 entries of 8 bytes) starts at +0x3C. The Client
// reads the full MPHD flag word and each MAIN entry flag byte without masking, so modern-only high bits
// make it misbehave; they are cleared here. big_alpha (0x4) drives the alpha genformat and is cleared for a
// merged (4-bit) map. Data-gated: a Client-shaped WDT comes out unchanged and is served raw.
namespace wxl::modern::adt
{
    namespace
    {
        constexpr uint32_t CC(char a, char b, char c, char d)
        {
            return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
                   (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
        }
        constexpr uint32_t MVER = CC('M','V','E','R');

        uint32_t rd32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (uint32_t(p[3])<<24); }

        constexpr uint32_t kFlagsOffset    = 0x14;          // MPHD flags word
        constexpr uint32_t kMainOffset     = 0x3C;          // first MAIN entry
        constexpr uint32_t kMainEntries    = 64u * 64u;     // 64x64 entries
        constexpr uint32_t kMainStride     = 8u;            // bytes per MAIN entry
        constexpr uint32_t kMphdClientMask = 0x0F;          // bits the Client reads: global-WMO, MCCV, big-alpha, sorted-refs
        constexpr uint32_t kMphdBigAlpha   = 0x4;           // map-wide 8-bit alpha selector
        constexpr uint8_t  kMainHasAdt     = 0x1;           // MAIN entry has-adt bit
    }

    bool FirstPresentTile(std::span<const uint8_t> in, uint32_t& x, uint32_t& y)
    {
        const uint8_t* b = in.data();
        const uint32_t n = static_cast<uint32_t>(in.size());
        if (n < kMainOffset + kMainEntries * kMainStride || rd32(b) != MVER)
            return false;
        for (uint32_t i = 0; i < kMainEntries; ++i)
            if (b[kMainOffset + i * kMainStride] & kMainHasAdt)
            {
                x = i % 64;
                y = i / 64;
                return true;
            }
        return false;
    }

    bool FixWdt(std::span<const uint8_t> in, bool clearBigAlpha, std::vector<uint8_t>& out)
    {
        const uint8_t* b = in.data();
        const uint32_t n = static_cast<uint32_t>(in.size());
        if (n < kMainOffset || rd32(b) != MVER)
            return false;

        out.assign(in.begin(), in.end());

        const uint32_t mask = clearBigAlpha ? (kMphdClientMask & ~kMphdBigAlpha) : kMphdClientMask;
        uint32_t flags = rd32(out.data() + kFlagsOffset);
        flags &= mask;
        for (int i = 0; i < 4; ++i) out[kFlagsOffset + i] = uint8_t(flags >> (i * 8));

        for (size_t i = kMainOffset;
             i + kMainStride <= out.size() && i < kMainOffset + kMainEntries * kMainStride;
             i += kMainStride)
            out[i] &= kMainHasAdt; // keep has-adt; clear modern/runtime entry bits

        // Client-shaped already (the masks were no-ops): serve raw.
        if (out.size() == in.size() && memcmp(out.data(), in.data(), in.size()) == 0)
        {
            out.clear();
            return false;
        }
        return true;
    }
}
