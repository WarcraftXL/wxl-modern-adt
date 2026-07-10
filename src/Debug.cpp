// Terrain dev-tuning state: live layer-cap and height-blend-strength knobs plus the ADT overlay panel.
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

#include "Debug.hpp"

#include "game/world/Loading.hpp"
#include "offsets/game/ADT.hpp"
#include "overlay/Panels.hpp"
#include "Adt.hpp"

#include <windows.h>

#include "imgui.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace off = wxl::offsets::game::adt;

namespace wxl::scripts::modernadt::debug
{
    namespace
    {
        std::atomic<int>   g_maxLayers{ 8 };
        std::atomic<float> g_heightGain{ 1.0f };

        // Per-texture gain overrides. Written from the overlay panel and read at the chunk draws;
        // both run on the client main thread, so no lock is needed.
        std::unordered_map<std::string, float> g_layerGain;

        // Same key normalization as the ATHB tables: lowercase, backslashes, no .blp.
        std::string NormalizeName(const char* s)
        {
            std::string r(s);
            for (char& c : r) c = (c == '/') ? '\\' : char(tolower(static_cast<unsigned char>(c)));
            if (r.size() >= 4 && r.compare(r.size() - 4, 4, ".blp") == 0)
                r.erase(r.size() - 4);
            return r;
        }

        /** @brief SEH-reads a chunk object's layer count, global coordinates, and layer names. */
        bool ReadChunk(void* chunk, uint32_t& count, uint32_t& gx, uint32_t& gy, char names[4][260])
        {
            __try
            {
                char* n = static_cast<char*>(chunk);
                count = *reinterpret_cast<uint8_t*>(n + off::kOffChunkNodeLayerCount);
                char* c = *reinterpret_cast<char**>(n + off::kOffChunkNodeChunk);
                if (!c || count > 4) return false;
                gx = *reinterpret_cast<uint32_t*>(c + off::kOffMapChunkGlobalX);
                gy = *reinterpret_cast<uint32_t*>(c + off::kOffMapChunkGlobalY);
                for (uint32_t i = 0; i < count; ++i)
                {
                    names[i][0] = '\0';
                    void* ctex = *reinterpret_cast<void**>(n + off::kOffChunkLayerRecords +
                        i * off::kChunkLayerRecordStride + off::kOffLayerSlotTexture);
                    if (!ctex)
                        continue;
                    const char* nm = reinterpret_cast<const char*>(static_cast<char*>(ctex) +
                                                                   off::kOffTextureName);
                    size_t j = 0;
                    for (; j + 1 < 260 && nm[j]; ++j) names[i][j] = nm[j];
                    names[i][j] = '\0';
                }
                return gx < 1024 && gy < 1024;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        /** @brief Returns the file name part of a texture path. */
        const char* Basename(const char* path)
        {
            const char* name = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/')
                    name = p + 1;
            return name;
        }

        /** @brief One per-layer panel row: name, authored height params, and the gain slider. */
        void DrawLayerRow(const char* tag, uint32_t idx, const std::string& name,
                          uint32_t tileX, uint32_t tileY)
        {
            ImGui::PushID(int(idx));
            ImGui::PushID(tag);
            const TexHeightParams* hp = FindTexHeight(tileX, tileY, name.c_str());
            float g = LayerGain(name.c_str());
            ImGui::SetNextItemWidth(120.0f);
            char label[32];
            snprintf(label, sizeof(label), "%s%u", tag, idx);
            if (ImGui::SliderFloat(label, &g, 0.0f, 3.0f, "%.2f"))
                SetLayerGain(name.c_str(), g);
            ImGui::SameLine();
            ImGui::TextUnformatted(Basename(name.c_str()));
            if (hp && (hp->heightScale != 0.0f || hp->heightOffset != 1.0f))
                ImGui::TextDisabled("      s=%.2f o=%.2f%s", hp->heightScale, hp->heightOffset,
                                    hp->heightTexture.empty() ? "  (no _h)" : "");
            else
                ImGui::TextDisabled("      (no height data)");
            ImGui::PopID();
            ImGui::PopID();
        }

        /** @brief The overlay panel: focus-chunk identity plus the two tuning sliders. */
        void DrawAdtPanel()
        {
            const FocusChunk fc = QueryFocusChunk();
            if (fc.valid)
            {
                ImGui::Text("tile %u_%u  chunk %u", fc.tileX, fc.tileY, fc.chunkIndex);
                ImGui::Text("layers: %u native + %u extra", fc.nativeLayers, fc.extraLayers);
            }
            else
            {
                ImGui::TextDisabled("(no chunk under focus)");
            }

            ImGui::Spacing();
            int cap = MaxLayers();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Texture layers", &cap, 1, 8))
                SetMaxLayers(cap);
            ImGui::TextDisabled("cap on rendered layers (5+ = extra pass)");

            float gain = HeightGain();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderFloat("Blend height", &gain, 0.0f, 3.0f, "%.2f"))
                SetHeightGain(gain);
            ImGui::TextDisabled("master: 0 = alpha only, 1 = authored");

            // Per-layer strength for the focus chunk's textures, keyed by texture name (the
            // override follows the texture everywhere, not just this chunk).
            if (fc.valid && (!fc.nativeNames.empty() || !fc.extraNames.empty()))
            {
                ImGui::Spacing();
                ImGui::TextUnformatted("Layer height gain");
                for (uint32_t i = 0; i < fc.nativeNames.size(); ++i)
                    DrawLayerRow("N", i, fc.nativeNames[i], fc.tileX, fc.tileY);
                for (uint32_t k = 0; k < fc.extraNames.size(); ++k)
                    DrawLayerRow("E", k, fc.extraNames[k], fc.tileX, fc.tileY);
            }

            if (ImGui::Button("Reset"))
            {
                SetMaxLayers(8);
                SetHeightGain(1.0f);
                ClearLayerGains();
            }
        }

        // File-scope registration: adds the panel at DLL load, before the overlay first draws.
        struct PanelRegistrar
        {
            PanelRegistrar() { wxl::overlay::RegisterPanel("ADT", &DrawAdtPanel); }
        } g_panelRegistrar;
    }

    int MaxLayers()
    {
        return g_maxLayers.load(std::memory_order_relaxed);
    }

    void SetMaxLayers(int n)
    {
        g_maxLayers.store(n < 1 ? 1 : (n > 8 ? 8 : n), std::memory_order_relaxed);
    }

    float HeightGain()
    {
        return g_heightGain.load(std::memory_order_relaxed);
    }

    void SetHeightGain(float g)
    {
        g_heightGain.store(g < 0.0f ? 0.0f : (g > 8.0f ? 8.0f : g), std::memory_order_relaxed);
    }

    float LayerGain(const char* name)
    {
        if (!name || g_layerGain.empty())
            return 1.0f;
        auto it = g_layerGain.find(NormalizeName(name));
        return it != g_layerGain.end() ? it->second : 1.0f;
    }

    void SetLayerGain(const char* name, float g)
    {
        if (!name)
            return;
        g_layerGain[NormalizeName(name)] = g < 0.0f ? 0.0f : (g > 8.0f ? 8.0f : g);
    }

    void ClearLayerGains()
    {
        g_layerGain.clear();
    }

    FocusChunk QueryFocusChunk()
    {
        FocusChunk fc{};
        if (wxl::game::world::MapId() < 0)
            return fc;

        float pos[3];
        wxl::game::world::FocusPos(pos);
        void* chunk = reinterpret_cast<off::Map_GetChunkFn>(off::kGetChunk)(pos);
        if (!chunk)
            return fc;

        uint32_t count = 0, gx = 0, gy = 0;
        char names[4][260];
        if (!ReadChunk(chunk, count, gx, gy, names))
            return fc;

        fc.valid        = true;
        fc.tileX        = gx >> 4;
        fc.tileY        = gy >> 4;
        fc.chunkIndex   = (gy & 15) * 16 + (gx & 15);
        fc.nativeLayers = count;
        for (uint32_t i = 0; i < count; ++i)
            if (names[i][0])
                fc.nativeNames.emplace_back(names[i]);
        const ChunkExtras* extras = FindExtraLayers(fc.tileX, fc.tileY, fc.chunkIndex);
        fc.extraLayers  = extras ? uint32_t(extras->layers.size()) : 0;
        if (extras)
            for (const ExtraLayer& l : extras->layers)
                fc.extraNames.push_back(l.texture);
        return fc;
    }
}
