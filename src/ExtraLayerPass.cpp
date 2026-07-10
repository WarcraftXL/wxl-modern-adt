// Extra terrain layers (5..8): per-chunk coverage bake + resolved bindings for the terrain shader.
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

#include "ExtraLayerPass.hpp"

#include "Debug.hpp"

#include "core/Logger.hpp"
#include "offsets/game/ADT.hpp"

#include <windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace off = wxl::offsets::game::adt;

namespace wxl::scripts::modernadt::extrapass
{
    namespace
    {
        constexpr uint32_t kAlphaTexDim   = 64;
        constexpr uint32_t kMaxPassLayers = 3; // coverage r/g/b carry one layer each; .a stays shadow

        // One baked coverage texture per chunk: the engine texture handle plus the texel storage the
        // fill callback serves (re-invoked by the texture system on device events, so it must persist).
        struct BakeEntry
        {
            void* tex = nullptr;
            std::vector<uint8_t> texels; // 64x64 ARGB8888
        };
        std::unordered_map<uint32_t, BakeEntry> g_bakes;     // (globalX<<16)|globalY -> bake

        std::unordered_map<std::string, void*> g_diffuseTex; // extra-layer name -> texture handle

        /**
         * @brief Fill callback the texture system invokes for a baked coverage texture.
         *
         * op 1 asks for the texel source; the creation context (6th argument) is the texel buffer.
         */
        void __cdecl BakeFillCallback(int op, uint32_t w, uint32_t, uint32_t, uint32_t,
                                      void* ctx, uint32_t* outStride, const void** outBase)
        {
            if (op != 1)
                return;
            static const std::vector<uint8_t> empty(kAlphaTexDim * kAlphaTexDim * 4, 0);
            *outBase   = ctx ? ctx : static_cast<const void*>(empty.data());
            *outStride = w * 4;
        }

        /**
         * @brief Returns the baked coverage texture for one chunk, creating it on first use.
         *
         * The rgb channels carry the extras' SEQUENTIAL-CHAIN coverages (the shader converts them
         * back to absolute weights); a = the chunk's MCSH baked shadow (0 = shadowed, matching the
         * native combined-alpha texture's alpha channel).
         * @param key     (globalX<<16)|globalY chunk key.
         * @param extras  the chunk's extras (alpha maps in native 4-bit form + MCSH bits).
         * @param m       bound layer count (1..3).
         * @return the texture handle, or null when creation failed.
         */
        void* BakeTexture(uint32_t key, const ChunkExtras& extras, uint32_t m)
        {
            auto it = g_bakes.find(key);
            if (it != g_bakes.end())
                return it->second.tex;

            const bool hasShadow = extras.shadow.size() >= kAlphaTexDim * kAlphaTexDim / 8;
            BakeEntry& e = g_bakes[key];
            // Unused coverage channels stay 0: the shader always reads all three lanes and a stray
            // value would enter the competition as a phantom layer.
            e.texels.assign(kAlphaTexDim * kAlphaTexDim * 4, 0x00);
            for (uint32_t t = 0; t < kAlphaTexDim * kAlphaTexDim; ++t)
            {
                uint8_t* px = e.texels.data() + t * 4; // BGRA byte order
                for (uint32_t k = 0; k < m; ++k)
                {
                    const std::vector<uint8_t>& a = extras.layers[k].alpha;
                    const uint8_t nib = (t / 2 < a.size())
                        ? uint8_t((a[t / 2] >> ((t & 1) * 4)) & 0xF) : 0;
                    px[2 - k] = uint8_t(nib * 17); // R=layer0, G=layer1, B=layer2
                }
                px[3] = (hasShadow && ((extras.shadow[t >> 3] >> (t & 7)) & 1))
                    ? 0x00 : 0xFF; // A: baked-shadow factor, full-lit when unshadowed
            }

            auto alloc = reinterpret_cast<off::Map_AllocTerrainTextureFn>(off::kAllocTerrainTexture);
            e.tex = alloc(kAlphaTexDim, kAlphaTexDim, e.texels.data(),
                          reinterpret_cast<void*>(&BakeFillCallback),
                          off::kTexFormatArgb8888, off::kTexFormatArgb8888);
            if (!e.tex)
                WLOG_WARN("extrapass: coverage texture create failed (chunk %u,%u)",
                          key >> 16, key & 0xFFFF);
            return e.tex;
        }

        /**
         * @brief Returns the engine texture handle for an extra layer's diffuse, loading it once.
         * @param name  diffuse texture path from the ATL2 table.
         * @return the handle, or null when the load failed.
         */
        void* DiffuseHandle(const std::string& name)
        {
            auto it = g_diffuseTex.find(name);
            if (it != g_diffuseTex.end())
                return it->second;
            void* h = reinterpret_cast<off::Map_LoadTextureFn>(off::kMapLoadTexture)(name.c_str());
            if (!h) WLOG_WARN("extrapass: load failed %s", name.c_str());
            g_diffuseTex.emplace(name, h);
            return h;
        }

    }

    bool GetExtrasBind(void* node, ExtrasBind& out)
    {
        uint32_t gx = 0, gy = 0;
        if (!ReadChunkCoords(node, gx, gy))
            return false;

        const ChunkExtras* extras = FindExtraLayers(gx >> 4, gy >> 4, (gy & 15) * 16 + (gx & 15));
        if (!extras || extras->layers.empty())
            return false;

        // Dev-overlay layer cap: 4 or less disables the extras entirely.
        const int cap = debug::MaxLayers();
        if (cap <= 4)
            return false;
        uint32_t m = extras->layers.size() < kMaxPassLayers
            ? uint32_t(extras->layers.size()) : kMaxPassLayers;
        if (m > uint32_t(cap - 4))
            m = uint32_t(cap - 4);

        // Every binding must be resident: a handle whose content is still streaming resolves to
        // null, and binding it would sample garbage; skip the frame and retry at the next draw.
        auto resolve = reinterpret_cast<off::Map_TexResolveFn>(off::kTexResolve);
        for (uint32_t k = 0; k < m; ++k)
        {
            void* h = DiffuseHandle(extras->layers[k].texture);
            void* gxTex = h ? resolve(h, 0, 0) : nullptr;
            if (!gxTex)
                return false;
            out.diffuse[k] = gxTex;
            out.layers[k]  = &extras->layers[k];
        }
        void* cov = BakeTexture((gx << 16) | gy, *extras, m);
        out.coverage = cov ? resolve(cov, 0, 0) : nullptr;
        if (!out.coverage)
            return false;
        for (uint32_t k = m; k < kMaxPassLayers; ++k)
        {
            out.diffuse[k] = nullptr;
            out.layers[k]  = nullptr;
        }
        out.count = m;
        return true;
    }
}
