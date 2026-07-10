// Terrain per-layer UV-scale consumer: applies the host's ATSC texture-scale table at terrain draw.
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

#include "Adt.hpp"

#include "HeightBlend.hpp"

#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/ADT.hpp"
#include "runtime/ModuleInstall.hpp"
#include "runtime/storage/StorageHook.hpp"

// The ATSC/ATHB/ATL2 formats are owned by the modern-adt module (the host emits them); reuse the definitions.
#include "../shared/AdtExtraLayers.hpp"
#include "../shared/AdtTexHeight.hpp"
#include "../shared/AdtTexScale.hpp"

#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace off = wxl::offsets::game::adt;
namespace gx  = wxl::offsets::engine::gx;
namespace sa  = wxl::modern::adt;

namespace wxl::scripts::modernadt
{
    namespace
    {
        // (tileX<<16|tileY) -> normalized texture name -> UV-scale exponent (1<<n). The source authors
        // the scale PER TILE (each tile's texture table has its own entry), so the same texture can
        // scale differently on different tiles; a global map would keep the last-served tile's value.
        std::unordered_map<uint32_t, std::unordered_map<std::string, uint8_t>> g_texScale;

        // (tileX<<16|tileY) -> normalized diffuse name -> height-blend inputs, from the host ATHB
        // tables. Per-tile for the same reason (the height params live in the same per-tile entry).
        std::unordered_map<uint32_t, std::unordered_map<std::string, TexHeightParams>> g_texHeight;

        // (tileX<<16|tileY) -> (chunk index -> extras), from the host ATL2 tables. Per-tile data
        // (alpha/shadow maps are per chunk); re-serving a tile replaces its entry, and a served .wdt
        // (map switch) clears the whole table so coordinates never alias across maps.
        std::unordered_map<uint32_t, std::unordered_map<uint32_t, ChunkExtras>> g_extraLayers;

        // Parses the two trailing "_X_Y" numbers of a tile file name ("...\<map>_X_Y.adt").
        bool ParseTileCoords(const char* name, uint32_t& x, uint32_t& y)
        {
            const size_t len = strlen(name);
            if (len < 8)
                return false;
            size_t end = len - 4; // before ".adt"
            size_t p = end;
            while (p > 0 && isdigit(static_cast<unsigned char>(name[p - 1]))) --p;
            if (p == end || p == 0 || name[p - 1] != '_')
                return false;
            y = static_cast<uint32_t>(atoi(name + p));
            size_t end2 = p - 1;
            p = end2;
            while (p > 0 && isdigit(static_cast<unsigned char>(name[p - 1]))) --p;
            if (p == end2 || p == 0 || name[p - 1] != '_')
                return false;
            x = static_cast<uint32_t>(atoi(name + p));
            return x < 64 && y < 64;
        }

        off::Map_BuildTerrainConstantsFn g_origBuild = nullptr;

        std::string Normalize(const char* s)
        {
            std::string r(s);
            for (char& c : r) c = (c == '/') ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
            // The runtime texture name carries no extension; the host table key keeps ".blp". Strip it on
            // both sides so the keys match.
            if (r.size() >= 4 && r.compare(r.size() - 4, 4, ".blp") == 0)
                r.erase(r.size() - 4);
            return r;
        }

        bool EndsWithCI(const char* s, const char* suffix)
        {
            const size_t ls = strlen(s), lf = strlen(suffix);
            if (lf > ls) return false;
            for (size_t i = 0; i < lf; ++i)
                if (tolower(static_cast<unsigned char>(s[ls - lf + i])) != suffix[i]) return false;
            return true;
        }

        // SEH-read the node's layer count and each layer's resolved-texture name into local buffers (no C++
        // objects, so it is safe inside __try).
        bool ReadLayerNames(void* node, uint8_t& count, char names[4][260])
        {
            __try
            {
                count = *reinterpret_cast<uint8_t*>(static_cast<char*>(node) + off::kOffChunkNodeLayerCount);
                if (count > 4) count = 4;
                for (uint8_t i = 0; i < count; ++i)
                {
                    names[i][0] = '\0';
                    void* ctex = *reinterpret_cast<void**>(static_cast<char*>(node) +
                        off::kOffChunkLayerRecords + i * off::kChunkLayerRecordStride + off::kOffLayerSlotTexture);
                    if (!ctex)
                        continue;
                    const char* nm = reinterpret_cast<const char*>(static_cast<char*>(ctex) + off::kOffTextureName);
                    size_t j = 0;
                    for (; j + 1 < 260 && nm[j]; ++j) names[i][j] = nm[j];
                    names[i][j] = '\0';
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // SEH-apply: divide c18+i.xy by 1<<exp[i] (a bigger texture covers more ground, so it tiles less),
        // then re-upload c18..c(18+count-1) so the GPU sees it.
        void ApplyScale(uint8_t count, const uint8_t exp[4])
        {
            __try
            {
                for (uint8_t i = 0; i < count; ++i)
                {
                    if (!exp[i])
                        continue;
                    float* c = reinterpret_cast<float*>(off::kVsConstC18 + i * 0x10);
                    const float f = 1.0f / static_cast<float>(1u << exp[i]);
                    c[0] *= f;
                    c[1] *= f;
                }
                void* device = *reinterpret_cast<void**>(gx::kGxDevicePtr);
                if (!device)
                    return;
                void** vt = *reinterpret_cast<void***>(device);
                auto setConst = reinterpret_cast<gx::Gx_SetShaderConstantFn>(vt[gx::kVtSetShaderConstant]);
                setConst(device, nullptr, 0, off::kVsConstC18Reg,
                         reinterpret_cast<const float*>(off::kVsConstC18), count);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        void __cdecl BuildConstantsDetour(void* node, uint32_t a1, uint32_t a2)
        {
            g_origBuild(node, a1, a2); // native build + upload of the per-chunk constant block

            if (g_texScale.empty())
                return;

            uint32_t gx = 0, gy = 0;
            if (!ReadChunkCoords(node, gx, gy))
                return;
            auto tile = g_texScale.find(((gx >> 4) << 16) | (gy >> 4));
            if (tile == g_texScale.end() || tile->second.empty())
                return;

            uint8_t count = 0;
            char names[4][260];
            if (!ReadLayerNames(node, count, names) || count == 0)
                return;

            uint8_t exp[4] = { 0, 0, 0, 0 };
            bool any = false;
            for (uint8_t i = 0; i < count; ++i)
            {
                if (!names[i][0])
                    continue;
                auto it = tile->second.find(Normalize(names[i]));
                if (it != tile->second.end() && it->second) { exp[i] = it->second; any = true; }
            }
            if (any)
                ApplyScale(count, exp);
        }
    }

    uint32_t IngestAdtBytes(const char* name, const uint8_t* buffer, uint32_t size)
    {
        if (!name || !buffer || size < 8)
            return size;
        if (EndsWithCI(name, ".wdt"))
        {
            // Map switch: tile coordinates must not alias across maps.
            g_extraLayers.clear();
            g_texScale.clear();
            g_texHeight.clear();
            return size;
        }
        if (!EndsWithCI(name, ".adt"))
            return size;

        uint32_t tileX = 0, tileY = 0;
        const bool hasTile = ParseTileCoords(name, tileX, tileY);
        const uint32_t tileKey = (tileX << 16) | tileY;

        sa::iff::Reader reader(std::span<const uint8_t>(buffer, size));
        uint32_t trim = size;

        sa::iff::Chunk atsc{};
        if (reader.Find(sa::kATSC, atsc))
        {
            std::vector<sa::TexScaleEntry> entries;
            if (hasTile && sa::ParseTexScales(atsc.data, atsc.size, entries))
            {
                auto& tile = g_texScale[tileKey];
                tile.clear(); // a re-served tile replaces its table
                for (sa::TexScaleEntry& e : entries)
                    if (e.exponent)
                        tile[Normalize(e.name.c_str())] = e.exponent;
            }
            if (atsc.pos - 8 < trim) trim = atsc.pos - 8; // hide the table from the native loader
        }

        sa::iff::Chunk athb{};
        if (reader.Find(sa::kATHB, athb))
        {
            std::vector<sa::TexHeightEntry> entries;
            if (hasTile && sa::ParseTexHeights(athb.data, athb.size, entries))
            {
                auto& tile = g_texHeight[tileKey];
                tile.clear();
                for (sa::TexHeightEntry& e : entries)
                    tile[Normalize(e.name.c_str())] =
                        TexHeightParams{ e.heightScale, e.heightOffset, std::move(e.heightName) };
            }
            if (athb.pos - 8 < trim) trim = athb.pos - 8; // hide the table from the native loader
        }

        sa::iff::Chunk atl2{};
        if (reader.Find(sa::kATL2, atl2))
        {
            uint32_t tx = 0, ty = 0;
            std::vector<sa::ExtraLayerChunk> chunks;
            if (ParseTileCoords(name, tx, ty) && sa::ParseExtraLayers(atl2.data, atl2.size, chunks))
            {
                auto& tile = g_extraLayers[(tx << 16) | ty];
                tile.clear();
                for (sa::ExtraLayerChunk& c : chunks)
                {
                    ChunkExtras& extras = tile[c.chunkIndex];
                    extras.shadow = std::move(c.shadow);
                    for (sa::ExtraLayer& l : c.layers)
                        extras.layers.push_back(ExtraLayer{ std::move(l.name), l.flags, l.groundEffect,
                                                            std::move(l.alpha) });
                }
            }
            if (atl2.pos - 8 < trim) trim = atl2.pos - 8; // hide the table from the native loader
        }

        return trim;
    }

    const TexHeightParams* FindTexHeight(uint32_t tileX, uint32_t tileY, const char* name)
    {
        if (!name || g_texHeight.empty())
            return nullptr;
        auto tile = g_texHeight.find((tileX << 16) | tileY);
        if (tile == g_texHeight.end())
            return nullptr;
        auto it = tile->second.find(Normalize(name));
        return it != tile->second.end() ? &it->second : nullptr;
    }

    uint8_t FindTexScaleExp(uint32_t tileX, uint32_t tileY, const char* name)
    {
        if (!name || g_texScale.empty())
            return 0;
        auto tile = g_texScale.find((tileX << 16) | tileY);
        if (tile == g_texScale.end())
            return 0;
        auto it = tile->second.find(Normalize(name));
        return it != tile->second.end() ? it->second : 0;
    }

    bool ReadChunkCoords(void* node, uint32_t& gx, uint32_t& gy)
    {
        __try
        {
            char* c = *reinterpret_cast<char**>(static_cast<char*>(node) + off::kOffChunkNodeChunk);
            if (!c) return false;
            gx = *reinterpret_cast<uint32_t*>(c + off::kOffMapChunkGlobalX);
            gy = *reinterpret_cast<uint32_t*>(c + off::kOffMapChunkGlobalY);
            return gx < 1024 && gy < 1024;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    const ChunkExtras* FindExtraLayers(uint32_t tileX, uint32_t tileY, uint32_t chunkIndex)
    {
        if (g_extraLayers.empty())
            return nullptr;
        auto it = g_extraLayers.find((tileX << 16) | tileY);
        if (it == g_extraLayers.end())
            return nullptr;
        auto ct = it->second.find(chunkIndex);
        return ct != it->second.end() ? &ct->second : nullptr;
    }

    void Install()
    {
        wxl::core::hook::Install("Adt::BuildTerrainConstants", off::kBuildTerrainConstants,
                                 reinterpret_cast<void*>(&BuildConstantsDetour),
                                 reinterpret_cast<void**>(&g_origBuild));
        WLOG_INFO("ADT: terrain UV-scale hook installed");
    }

    namespace
    {
        void InstallModule()
        {
            Install();            // terrain per-layer UV-scale (ATSC) post-hook
            InstallHeightBlend(); // terrain height-blend (ATHB) draw detour + extra-layer pass
        }

        // File-scope registration: the serve filter records the trailing ATSC/ATHB/ATL2 tables of every
        // host-served tile; the installer arms the terrain draw detours from the main thread.
        struct Registrar
        {
            Registrar()
            {
                wxl::runtime::storage::RegisterServeFilter(&IngestAdtBytes);
                wxl::runtime::modules::Register("wxl-modern-adt", &InstallModule);
            }
        } g_registrar;
    }
}
