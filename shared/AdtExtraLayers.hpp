// Host->DLL terrain extra-layer table, carried as a trailing ATL2 chunk on the merged tile.
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

// Host->DLL terrain extra-layer table. Modern tiles author up to 8 texture layers per map chunk;
// the Client materializes at most 4, so the merge clamps MCLY and would drop the tail. Instead the
// tail layers ride a trailing ATL2 chunk on the merged tile bytes (invisible to the native loader,
// same contract as ATSC/ATHB): per map chunk, each extra layer's diffuse texture name, its layer
// flags, its ground-effect id, and its alpha map re-packed to the native 4-bit 64x64 form. The DLL
// ingests the table per tile and renders the extra layers as a second blended draw of the chunk.
// Keyed by CHUNK INDEX within the tile because alpha maps are per-chunk data.
namespace wxl::modern::adt
{
    constexpr uint32_t kATL2 = iff::FourCC('A', 'T', 'L', '2'); // Adt exTra Layer table

    constexpr uint32_t kAlphaBytes  = 2048; // 64x64 4-bit packed, native MCAL form
    constexpr uint32_t kShadowBytes = 512;  // 64x64 1-bit MCSH shadow map

    struct ExtraLayer
    {
        std::string name;          // diffuse texture path, as the Client stores it (lowercased)
        uint32_t    flags;         // MCLY flags, cleaned like the kept layers (low bits, no 0x200)
        uint32_t    groundEffect;  // GroundEffectTexture id
        std::vector<uint8_t> alpha; // kAlphaBytes 4-bit packed alpha map
    };

    struct ExtraLayerChunk
    {
        uint16_t chunkIndex;       // 0..255 map-chunk index within the tile
        std::vector<uint8_t> shadow; // kShadowBytes MCSH, empty when the chunk has none
        std::vector<ExtraLayer> layers;
    };

    // Serialize into an ATL2 payload (no chunk header).
    // Layout: u32 chunkCount, then per chunk: u16 chunkIndex, u8 layerCount, u8 hasShadow,
    // [kShadowBytes shadow], then per layer: u8 nameLen, name, u32 flags, u32 groundEffect,
    // kAlphaBytes alpha.
    inline void SerializeExtraLayers(const std::vector<ExtraLayerChunk>& chunks, std::vector<uint8_t>& out)
    {
        out.clear();
        uint8_t hdr[4];
        iff::Wr32(hdr, static_cast<uint32_t>(chunks.size()));
        out.insert(out.end(), hdr, hdr + 4);
        for (const ExtraLayerChunk& c : chunks)
        {
            out.push_back(static_cast<uint8_t>(c.chunkIndex));
            out.push_back(static_cast<uint8_t>(c.chunkIndex >> 8));
            out.push_back(static_cast<uint8_t>(c.layers.size()));
            const bool hasShadow = c.shadow.size() >= kShadowBytes;
            out.push_back(hasShadow ? 1 : 0);
            if (hasShadow)
                out.insert(out.end(), c.shadow.begin(), c.shadow.begin() + kShadowBytes);
            for (const ExtraLayer& l : c.layers)
            {
                const uint8_t len = static_cast<uint8_t>(l.name.size() > 255 ? 255 : l.name.size());
                out.push_back(len);
                out.insert(out.end(), l.name.begin(), l.name.begin() + len);
                uint8_t f[8];
                iff::Wr32(f + 0, l.flags);
                iff::Wr32(f + 4, l.groundEffect);
                out.insert(out.end(), f, f + 8);
                const size_t at = out.size();
                out.resize(at + kAlphaBytes, 0);
                std::memcpy(out.data() + at, l.alpha.data(),
                            l.alpha.size() < kAlphaBytes ? l.alpha.size() : kAlphaBytes);
            }
        }
    }

    inline bool ParseExtraLayers(const uint8_t* data, uint32_t len, std::vector<ExtraLayerChunk>& out)
    {
        out.clear();
        if (len < 4)
            return false;
        const uint32_t chunkCount = iff::Rd32(data);
        uint32_t p = 4;
        for (uint32_t i = 0; i < chunkCount; ++i)
        {
            if (p + 4 > len)
                return false;
            ExtraLayerChunk c;
            c.chunkIndex = static_cast<uint16_t>(data[p] | (data[p + 1] << 8));
            const uint8_t layerCount = data[p + 2];
            const uint8_t hasShadow  = data[p + 3];
            p += 4;
            if (hasShadow)
            {
                if (p + kShadowBytes > len)
                    return false;
                c.shadow.assign(data + p, data + p + kShadowBytes);
                p += kShadowBytes;
            }
            for (uint8_t k = 0; k < layerCount; ++k)
            {
                if (p + 1 > len)
                    return false;
                const uint8_t nameLen = data[p];
                p += 1;
                if (p + nameLen + 8 + kAlphaBytes > len)
                    return false;
                ExtraLayer l;
                l.name.assign(reinterpret_cast<const char*>(data + p), nameLen);
                p += nameLen;
                l.flags        = iff::Rd32(data + p);
                l.groundEffect = iff::Rd32(data + p + 4);
                p += 8;
                l.alpha.assign(data + p, data + p + kAlphaBytes);
                p += kAlphaBytes;
                c.layers.push_back(std::move(l));
            }
            out.push_back(std::move(c));
        }
        return true;
    }
}
