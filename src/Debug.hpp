// Terrain dev-tuning state: live layer-cap and height-blend-strength knobs plus the focus-chunk query.
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
#include <string>
#include <vector>

// Live tuning state for the terrain passes, driven from the dev overlay's ADT panel and read at every
// chunk draw. Defaults render the authored data; the knobs exist to bisect visual reports in place
// (cap the rendered layer count, scale the height-blend strength) without rebuilding.
namespace wxl::scripts::modernadt::debug
{
    /** @brief Rendered-layer cap, 1..8 (4 native + up to 4 extra); 8 = uncapped. */
    int  MaxLayers();
    void SetMaxLayers(int n);

    /** @brief Height-blend strength: 0 = plain alpha blend, 1 = authored data, >1 = exaggerated. */
    float HeightGain();
    void  SetHeightGain(float g);

    /** @brief Per-texture height-blend strength (keyed by texture name, composed onto HeightGain). */
    float LayerGain(const char* name);
    void  SetLayerGain(const char* name, float g);
    void  ClearLayerGains();

    /** @brief Identity, layer counts, and layer texture names of the chunk under the camera focus. */
    struct FocusChunk
    {
        bool     valid;
        uint32_t tileX;        // tile coordinates (the two numbers of the ADT file name)
        uint32_t tileY;
        uint32_t chunkIndex;   // 0..255 MCNK index within the tile (y*16 + x)
        uint32_t nativeLayers; // materialized layer count (0..4)
        uint32_t extraLayers;  // ATL2 extra layers beyond the native slots
        std::vector<std::string> nativeNames; // native layers' texture names, in slot order
        std::vector<std::string> extraNames;  // extra layers' texture names, in ATL2 order
    };

    /** @brief Reads the focus chunk's identity from the live world (invalid when none is loaded). */
    FocusChunk QueryFocusChunk();
}
