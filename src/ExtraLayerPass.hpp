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

#pragma once

#include "Adt.hpp"

#include <cstdint>

// Modern tiles author up to 8 texture layers per map chunk; the Client materializes 4. The host moves
// the tail layers into the ATL2 side table. The served Terrain shader files blend every layer in one
// draw: they read the extras' diffuse and coverage from the samplers this module resolves, and the
// chunk-draw detour binds them (HeightBlend.cpp). This module owns the per-chunk coverage bake (chain
// coverages in rgb, baked MCSH shadow in a) and the extra-diffuse handle cache.
namespace wxl::scripts::modernadt::extrapass
{
    /**
     * @brief A chunk's extra layers resolved for the terrain shader bind.
     */
    struct ExtrasBind
    {
        void*             coverage;   ///< resolved coverage texture (chain coverages + shadow)
        void*             diffuse[3]; ///< resolved diffuse texture per extra layer
        const ExtraLayer* layers[3];  ///< the extra layers, in order
        uint32_t          count;      ///< bound layer count (1..3)
    };

    /**
     * @brief Resolves a chunk's extra layers for the terrain shader bind.
     *
     * Returns false when the chunk has no extras, the dev layer cap disables them, or any
     * texture is still streaming (the caller retries at the next draw of the chunk).
     * @param node  the runtime chunk node (this-register of the chunk draw).
     * @param out   receives the resolved bindings.
     * @return true when every binding is resident.
     */
    bool GetExtrasBind(void* node, ExtrasBind& out);
}
