// WDL fixup: reshape a modern low-detail map index into the layout the Client's low-detail loader reads.
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

#include "WdlFixup.hpp"

namespace wxl::modern::adt
{
    namespace
    {
        constexpr uint32_t CC(char a, char b, char c, char d)
        {
            return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
                   (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
        }
        constexpr uint32_t MVER = CC('M','V','E','R'), MWMO = CC('M','W','M','O');
        constexpr uint32_t MWID = CC('M','W','I','D'), MODF = CC('M','O','D','F');
        constexpr uint32_t MAOF = CC('M','A','O','F'), MARE = CC('M','A','R','E');
        constexpr uint32_t MAHO = CC('M','A','H','O'), MAOE = CC('M','A','O','E');

        constexpr uint32_t kMaofEntries = 64u * 64u; // one offset per tile, row-major
        constexpr uint32_t kMaofBytes   = kMaofEntries * 4u;

        uint32_t rd32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (uint32_t(p[3])<<24); }

        // The chunks the Client's low-detail loader has a slot for; anything else marks a modern source.
        bool IsClientChunk(uint32_t magic)
        {
            return magic == MVER || magic == MWMO || magic == MWID || magic == MODF ||
                   magic == MAOF || magic == MARE || magic == MAHO;
        }

        struct Out
        {
            std::vector<uint8_t> d;
            uint32_t tell() const { return uint32_t(d.size()); }
            void u32(uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (i * 8))); }
            void patch32(uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) d[at + i] = uint8_t(v >> (i * 8)); }
            // Emit a chunk; data null means a zero-filled payload of n bytes. Returns the chunk start.
            uint32_t chunk(uint32_t magic, const uint8_t* data, uint32_t n)
            {
                uint32_t s = tell();
                u32(magic); u32(n);
                if (data) d.insert(d.end(), data, data + n);
                else      d.insert(d.end(), n, 0);
                return s;
            }
        };
    }

    bool FixWdl(std::span<const uint8_t> in, std::vector<uint8_t>& out)
    {
        const uint8_t* b = in.data();
        const uint32_t n = static_cast<uint32_t>(in.size());
        if (n < 8 || rd32(b) != MVER)
            return false;

        // Walk the top-level chunks once: capture the version payload + the MAOF table, and decide whether
        // any source-only chunk is present (else the index is Client-shaped and served raw).
        uint32_t verOff = 0, verLen = 0, maofData = 0;
        bool modern = false;
        for (uint32_t o = 0; o + 8 <= n; )
        {
            const uint32_t m = rd32(b + o), s = rd32(b + o + 4);
            if (o + 8 + size_t(s) > n) break;
            if (m == MVER) { verOff = o + 8; verLen = s; }
            else if (m == MAOF) { maofData = o + 8; }
            if (!IsClientChunk(m)) modern = true;
            o += 8 + s;
        }
        if (!modern || maofData == 0 || maofData + kMaofBytes > n)
            return false;

        Out o;
        { uint32_t v = (verLen >= 4) ? rd32(b + verOff) : 18; uint8_t p[4]; for (int i=0;i<4;++i) p[i]=uint8_t(v>>(i*8)); o.chunk(MVER, p, 4); }
        o.chunk(MWMO, nullptr, 0); // low-detail object lists dropped: distant WMOs are not drawn, but the
        o.chunk(MWID, nullptr, 0); // loader's positional walk + name-hash pass over an EMPTY list is a no-op
        o.chunk(MODF, nullptr, 0); // (a non-empty modern list is what faults the native hash).

        const uint32_t maofPos = o.chunk(MAOF, nullptr, kMaofBytes);
        const uint32_t maofOut = maofPos + 8;
        std::vector<uint32_t> rebuilt(kMaofEntries, 0);

        // Per present tile: copy MARE (+ its MAHO), dropping any MAOE wedged between them, and record the new
        // absolute offset of the MARE chunk in the rebuilt MAOF table.
        for (uint32_t i = 0; i < kMaofEntries; ++i)
        {
            const uint32_t inOff = rd32(b + maofData + i * 4);
            if (!inOff || inOff + 8 > n || rd32(b + inOff) != MARE)
                continue;
            const uint32_t mareSz = rd32(b + inOff + 4);
            if (inOff + 8 + mareSz > n)
                continue;

            rebuilt[i] = o.tell();
            o.chunk(MARE, b + inOff + 8, mareSz);

            uint32_t p = inOff + 8 + mareSz;
            if (p + 8 <= n && rd32(b + p) == MAOE) p += 8 + rd32(b + p + 4); // drop MAOE
            if (p + 8 <= n && rd32(b + p) == MAHO)
            {
                const uint32_t hs = rd32(b + p + 4);
                if (p + 8 + hs <= n) o.chunk(MAHO, b + p + 8, hs);
            }
        }

        for (uint32_t i = 0; i < kMaofEntries; ++i)
            o.patch32(maofOut + i * 4, rebuilt[i]);

        out = std::move(o.d);
        return true;
    }
}
