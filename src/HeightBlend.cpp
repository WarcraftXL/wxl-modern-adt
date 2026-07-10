// Terrain shader feed: binds height textures, extra layers, and their constants at the chunk draw.
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

#include "HeightBlend.hpp"

#include "Adt.hpp"
#include "Debug.hpp"
#include "ExtraLayerPass.hpp"

#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/ADT.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

// The served Terrain*.bls pixel shaders blend every layer of a chunk in a single draw: a joint
// height competition over the native layers (weights from the engine's combined alpha at s4,
// heights from _h textures) and, for 4-layer chunks, up to three ATL2 extra layers (diffuse at
// s9..s11, chain coverages at s12, constant heights). The shaders read a fixed input contract the
// engine knows nothing about; this detour supplies it at every chunk draw:
//   s13..s15  native layers 0..2 height textures      c22..c24  extras (heightScale, heightOffset)
//   s9 ..s11  extra layers' diffuse                   c25..c27  natives 0..2 (scale, offset)
//   s12       extras coverage bake                    c28.y     native layer 3 constant height
//                                                     c29/c30   native/extras uv ratios vs layer 0
// A layer without height data takes a (0, 1) pair: h degrades to the constant 1 and the blend
// degrades to the plain alpha weights. A chunk without extras leaves s9..s12 unbound: a D3D9
// sampler with no texture reads black, so the extras' coverages (and weights) are zero.
namespace wxl::scripts::modernadt
{
    namespace off = wxl::offsets::game::adt;
    namespace gx  = wxl::offsets::engine::gx;

    namespace
    {
        off::Map_SurfaceChunkDrawShaderFn g_origDraw = nullptr;

        // Height texture path -> map texture handle (null = load failed, skip).
        std::unordered_map<std::string, void*> g_heightTex;

        // One-shot flow counters: how the chunk draws split across the early-outs, logged at two
        // depths so a stalled path is visible from a single session log.
        uint32_t g_statDraw = 0, g_statNoLayers = 0, g_statArmed = 0;

        void LogFlowStats()
        {
            WLOG_INFO("heightblend: flow draws=%u noLayers=%u armed=%u",
                      g_statDraw, g_statNoLayers, g_statArmed);
        }

        /**
         * @brief Returns the map texture handle for a height texture path, loading it once.
         * @param path  height texture path from the ATHB table.
         * @return the handle, or null when the load failed.
         */
        void* HeightTexHandle(const std::string& path)
        {
            auto it = g_heightTex.find(path);
            if (it != g_heightTex.end()) return it->second;
            void* h = reinterpret_cast<off::Map_LoadTextureFn>(off::kMapLoadTexture)(path.c_str());
            if (!h) WLOG_WARN("heightblend: load failed %s", path.c_str());
            g_heightTex.emplace(path, h);
            return h;
        }

        /**
         * @brief SEH-reads the chunk's layer count, texture handles, and texture names.
         * @param chunk  the runtime chunk object.
         * @param count  receives the layer count (clamped to 4).
         * @param tex    receives each layer's resolved-texture handle.
         * @param names  receives each layer's texture name.
         * @return true when the reads succeeded.
         */
        bool ReadLayers(void* chunk, uint8_t& count, void* tex[4], char names[4][260])
        {
            __try
            {
                count = *reinterpret_cast<uint8_t*>(static_cast<char*>(chunk) + off::kOffChunkNodeLayerCount);
                if (count > 4) count = 4;
                for (uint8_t i = 0; i < count; ++i)
                {
                    names[i][0] = '\0';
                    tex[i] = *reinterpret_cast<void**>(static_cast<char*>(chunk) +
                        off::kOffChunkLayerRecords + i * off::kChunkLayerRecordStride + off::kOffLayerSlotTexture);
                    if (!tex[i]) return false;
                    const char* nm = reinterpret_cast<const char*>(static_cast<char*>(tex[i]) + off::kOffTextureName);
                    size_t j = 0;
                    for (; j + 1 < 260 && nm[j]; ++j) names[i][j] = nm[j];
                    names[i][j] = '\0';
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        /**
         * @brief Composes the dev-overlay tuning onto one (scale, offset) height pair.
         *
         * The per-layer gain rides the master; the offset pivots around the neutral h = 1, and a
         * neutral (0, 1) no-data pair takes the gain as a constant h so master = 0 stays plain
         * alpha blending.
         * @param pair  the (scale, offset) pair, tuned in place.
         * @param name  the layer's texture name (per-layer gain key).
         */
        void Tune(float pair[4], const char* name)
        {
            const float master = debug::HeightGain();
            const float lg     = debug::LayerGain(name);
            if (pair[0] == 0.0f && pair[1] == 1.0f)
                pair[1] = 1.0f + (lg - 1.0f) * master;
            else
            {
                const float g = master * lg;
                pair[0] *= g;
                pair[1]  = 1.0f + (pair[1] - 1.0f) * g;
            }
        }

        /**
         * @brief Binds the terrain shader inputs for one chunk draw.
         *
         * Uploads the full c13..c21 block on every eligible draw (the shader would otherwise read
         * the previous chunk's constants) and binds only the textures that are resident.
         * @param chunk  the runtime chunk object.
         * @param gxDev  the engine device.
         * @return true when inputs were bound (the caller clears the samplers after the draw).
         */
        bool BindTerrain(void* chunk, void* gxDev)
        {
            uint8_t count = 0;
            void*   tex[4] = {};
            char    names[4][260];
            uint32_t gx = 0, gy = 0;
            if (!ReadLayers(chunk, count, tex, names) || count < 2 || !ReadChunkCoords(chunk, gx, gy))
            {
                ++g_statNoLayers;
                return false;
            }
            const uint32_t tileX = gx >> 4, tileY = gy >> 4;
            ++g_statArmed;

            auto resolve = reinterpret_cast<off::Map_TexResolveFn>(off::kTexResolve);
            auto bind    = reinterpret_cast<off::Map_SamplerBindFn>(off::kSetSamplerTexture);

            // c22..c30: extras pairs, native pairs, native layer-3 height, uv ratios.
            float block[off::kPsConstTerrainBindCount][4] = {};

            // Native layers 0..2: real height texture when resident, else the constant-h pair.
            const int cap = debug::MaxLayers();
            for (uint8_t i = 0; i < count && i < 3; ++i)
            {
                float* pair = block[3 + i];
                pair[0] = 0.0f; pair[1] = 1.0f;
                const TexHeightParams* hp = FindTexHeight(tileX, tileY, names[i]);
                if (hp)
                {
                    void* bound = nullptr;
                    if (!hp->heightTexture.empty())
                    {
                        void* hh = HeightTexHandle(hp->heightTexture);
                        if (hh) bound = resolve(hh, 0, 0);
                    }
                    if (bound)
                    {
                        pair[0] = hp->heightScale;
                        pair[1] = hp->heightOffset;
                        bind(gxDev, nullptr, off::kSamplerSelNativeH0 + i, bound);
                    }
                    else
                        pair[1] = hp->heightScale + hp->heightOffset; // white-texel equivalent
                }
                Tune(pair, names[i]);
                if (int(i) >= cap) { pair[0] = 0.0f; pair[1] = -8.0f; }
            }
            // Native layer 3 competes with a constant height (mid-texel equivalent).
            {
                block[6][1] = 1.0f;
                if (count >= 4)
                {
                    const TexHeightParams* hp = FindTexHeight(tileX, tileY, names[3]);
                    if (hp) block[6][1] = 0.5f * hp->heightScale + hp->heightOffset;
                    Tune(block[6], names[3]);
                    if (3 >= cap) { block[6][0] = 0.0f; block[6][1] = -8.0f; }
                }
            }
            // UV ratios: native and extras height/diffuse uvs are rebuilt from layer 0's uv by the
            // ratio of the ATSC tiling exponents.
            const uint8_t exp0 = FindTexScaleExp(tileX, tileY, names[0]);
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint8_t expN = (i < count) ? FindTexScaleExp(tileX, tileY, names[i]) : exp0;
                block[7][i] = float(1u << exp0) / float(1u << expN);
            }

            // Extra layers (4-layer chunks with an ATL2 entry): diffuse + coverage + constant-h pairs.
            extrapass::ExtrasBind eb{};
            const bool hasExtras = count == 4 && extrapass::GetExtrasBind(chunk, eb);
            if (hasExtras)
            {
                for (uint32_t k = 0; k < eb.count; ++k)
                {
                    float* pair = block[k];
                    pair[0] = 0.0f; pair[1] = 1.0f;
                    const char* nm = eb.layers[k]->texture.c_str();
                    const TexHeightParams* hp = FindTexHeight(tileX, tileY, nm);
                    if (hp) { pair[0] = hp->heightScale; pair[1] = hp->heightOffset; }
                    Tune(pair, nm);
                    block[8][k] = float(1u << exp0) /
                                  float(1u << FindTexScaleExp(tileX, tileY, nm));
                    bind(gxDev, nullptr, off::kSamplerSelHeight0 + k, eb.diffuse[k]);
                }
                bind(gxDev, nullptr, off::kSamplerSelNativeAlpha, eb.coverage);
            }

            void** vt = *reinterpret_cast<void***>(gxDev);
            auto setConst = reinterpret_cast<gx::Gx_SetShaderConstantFn>(vt[gx::kVtSetShaderConstant]);
            setConst(gxDev, nullptr, 4, off::kPsConstTerrainBindBase, &block[0][0],
                     off::kPsConstTerrainBindCount);
            return true;
        }

        /**
         * @brief Detours the shader-path per-chunk terrain draw: feeds the served terrain shaders.
         * @param chunk  the runtime chunk object (this-register).
         * @param edx    unused register slot for the thiscall convention.
         */
        void __fastcall hkChunkDraw(void* chunk, void* edx)
        {
            if (++g_statDraw == 512 || g_statDraw == 8192) LogFlowStats();

            void* gxDev = *reinterpret_cast<void**>(gx::kGxDevicePtr);
            if (!gxDev)
            {
                g_origDraw(chunk, edx);
                return;
            }

            const bool bound = BindTerrain(chunk, gxDev);
            g_origDraw(chunk, edx);
            if (bound)
            {
                auto bind = reinterpret_cast<off::Map_SamplerBindFn>(off::kSetSamplerTexture);
                for (uint32_t s = 0; s < off::kSamplerSelFreeCount; ++s)
                    bind(gxDev, nullptr, off::kSamplerSelHeight0 + s, nullptr);
            }
        }
    }

    void InstallHeightBlend()
    {
        wxl::core::hook::Install("Adt::TerrainDrawBind", off::kSurfaceChunkDrawShader,
                                 reinterpret_cast<void*>(&hkChunkDraw),
                                 reinterpret_cast<void**>(&g_origDraw));
        WLOG_INFO("ADT: terrain shader feed installed");
    }
}
