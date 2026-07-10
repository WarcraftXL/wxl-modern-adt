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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The Client renders every terrain layer at one fixed UV tiling; modern tiles author a per-texture scale
// (so a texture covers more ground and repeats less) the Client ignores, leaving those layers visibly
// tiled. The host ships the per-texture scale exponents as a trailing ATSC table on the merged tile; this
// consumes it: it records the scales from each served ADT and, at terrain draw, divides each drawn layer's
// UV-tiling constant by 1<<exponent.
namespace wxl::scripts::modernadt
{
    /**
     * @brief Records a served ADT's trailing ATSC scale table and returns the bytes to actually serve.
     *
     * Parses the trailing ATSC chunk into the global texture-scale map, then returns the byte count up to
     * that chunk, so the native loader is served the ADT alone and never sees the table.
     * @param name    served file name (only .adt files carry the table).
     * @param buffer  served file bytes.
     * @param size    served byte count.
     * @return the byte count to serve (size when there is no table to trim).
     */
    uint32_t IngestAdtBytes(const char* name, const uint8_t* buffer, uint32_t size);

    /** @brief Height-blend inputs recorded for one terrain texture, from the host ATHB tables. */
    struct TexHeightParams
    {
        float       heightScale;   // source height scale (0 = no height contribution)
        float       heightOffset;  // source height offset
        std::string heightTexture; // height texture path, empty when the source ships none
    };

    /**
     * @brief Looks up the height-blend inputs recorded for a terrain texture on one tile.
     *
     * The source authors the params per tile (each tile's texture table has its own entry for a
     * texture), so the same texture can carry different params on different tiles.
     * @param tileX  first tile coordinate.
     * @param tileY  second tile coordinate.
     * @param name   texture name as the engine stores it (extension optional).
     * @return the recorded params, or null when the tile records none for this texture.
     */
    const TexHeightParams* FindTexHeight(uint32_t tileX, uint32_t tileY, const char* name);

    /**
     * @brief Looks up the UV-scale exponent recorded for a terrain texture on one tile (ATSC).
     * @param tileX  first tile coordinate.
     * @param tileY  second tile coordinate.
     * @param name   texture name as the engine stores it (extension optional).
     * @return the exponent (tiling is divided by 1<<exp), 0 when the tile records none.
     */
    uint8_t FindTexScaleExp(uint32_t tileX, uint32_t tileY, const char* name);

    /**
     * @brief SEH-reads a chunk draw-node's global chunk coordinates.
     * @param node  the runtime chunk node.
     * @param gx    receives the global chunk X (tile = gx >> 4).
     * @param gy    receives the global chunk Y (tile = gy >> 4).
     * @return true when the reads succeeded and the coordinates are in range.
     */
    bool ReadChunkCoords(void* node, uint32_t& gx, uint32_t& gy);

    /** @brief One texture layer beyond the native 4-slot chunk cap, from the host ATL2 tables. */
    struct ExtraLayer
    {
        std::string          texture;      // diffuse texture path (lowercased, .blp kept)
        uint32_t             flags;        // MCLY flags, cleaned like the kept layers
        uint32_t             groundEffect; // GroundEffectTexture id
        std::vector<uint8_t> alpha;        // 64x64 4-bit packed alpha map (native MCAL form)
    };

    /** @brief One map chunk's extra layers plus its baked-shadow map. */
    struct ChunkExtras
    {
        std::vector<uint8_t>    shadow;    // 64x64 1-bit MCSH, empty when the chunk has none
        std::vector<ExtraLayer> layers;
    };

    /**
     * @brief Looks up the extra texture layers recorded for one map chunk of a tile.
     *
     * Keyed by tile coordinates (the two numbers of the tile file name, in order); the table is
     * cleared when a .wdt is served, so coordinates never cross maps.
     * @param tileX       first tile coordinate.
     * @param tileY       second tile coordinate.
     * @param chunkIndex  0..255 map-chunk index within the tile (y*16 + x).
     * @return the chunk's extras, or null when it has none.
     */
    const ChunkExtras* FindExtraLayers(uint32_t tileX, uint32_t tileY, uint32_t chunkIndex);

    /**
     * @brief Installs the terrain-constant post-hook that rescales each layer's UV tiling by its scale.
     */
    void Install();
}
