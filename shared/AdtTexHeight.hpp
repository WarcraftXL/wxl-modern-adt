// Host->DLL terrain texture height-blend table, carried as a trailing ATHB chunk on the merged tile.
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

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ChunkIO.hpp"

// Host->DLL terrain height-blend table. Modern tiles author per-texture height-blend inputs the
// Client has no path for: a height texture (the _h sibling, referenced by MHID FileDataID) and
// the MTXP heightScale/heightOffset pair. The merge records them per diffuse texture name and
// ships them as a trailing ATHB chunk on the merged tile bytes; the Client never reads past the
// MHDR/MCIN offsets, so the table is invisible to the native loader. The DLL accumulates a global
// {textureName -> params} map for the terrain height-blend feature. Keyed by texture NAME because
// the params are a property of the texture asset (global), not of a tile.
namespace wxl::modern::adt
{
    constexpr uint32_t kATHB = iff::FourCC('A', 'T', 'H', 'B'); // Adt Texture Height-Blend table

    struct TexHeightEntry
    {
        float       heightScale;  // MTXP height scale (source default 0 = no height contribution)
        float       heightOffset; // MTXP height offset (source default 1)
        std::string name;         // diffuse texture path, as the Client stores it (lowercased)
        std::string heightName;   // height texture path (_h), empty when the source has none
    };

    // Serialize into an ATHB payload (no chunk header).
    // Layout: u32 count, then per entry: f32 scale, f32 offset, u8 nameLen, name, u8 hLen, heightName.
    inline void SerializeTexHeights(const std::vector<TexHeightEntry>& entries, std::vector<uint8_t>& out)
    {
        out.clear();
        uint8_t hdr[4];
        iff::Wr32(hdr, static_cast<uint32_t>(entries.size()));
        out.insert(out.end(), hdr, hdr + 4);
        for (const TexHeightEntry& e : entries)
        {
            uint8_t f[8];
            std::memcpy(f + 0, &e.heightScale, 4);
            std::memcpy(f + 4, &e.heightOffset, 4);
            out.insert(out.end(), f, f + 8);
            const uint8_t len  = static_cast<uint8_t>(e.name.size()       > 255 ? 255 : e.name.size());
            const uint8_t hlen = static_cast<uint8_t>(e.heightName.size() > 255 ? 255 : e.heightName.size());
            out.push_back(len);
            out.insert(out.end(), e.name.begin(), e.name.begin() + len);
            out.push_back(hlen);
            out.insert(out.end(), e.heightName.begin(), e.heightName.begin() + hlen);
        }
    }

    inline bool ParseTexHeights(const uint8_t* data, uint32_t len, std::vector<TexHeightEntry>& out)
    {
        out.clear();
        if (len < 4)
            return false;
        const uint32_t count = iff::Rd32(data);
        uint32_t p = 4;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (p + 9 > len)
                return false;
            TexHeightEntry e;
            std::memcpy(&e.heightScale,  data + p + 0, 4);
            std::memcpy(&e.heightOffset, data + p + 4, 4);
            uint8_t nameLen = data[p + 8];
            p += 9;
            if (p + nameLen + 1 > len)
                return false;
            e.name.assign(reinterpret_cast<const char*>(data + p), nameLen);
            p += nameLen;
            const uint8_t hLen = data[p];
            p += 1;
            if (p + hLen > len)
                return false;
            e.heightName.assign(reinterpret_cast<const char*>(data + p), hLen);
            p += hLen;
            out.push_back(std::move(e));
        }
        return true;
    }
}
