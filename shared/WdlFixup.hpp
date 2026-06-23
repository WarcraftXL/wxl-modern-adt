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

#pragma once

#include <cstdint>
#include <span>
#include <vector>

// The Client's low-detail map loader reads ONE shape: MVER, then MWMO/MWID/MODF, then MAOF (a 64x64 table
// of absolute offsets to per-tile area blocks), then for each present tile a MARE height block optionally
// followed by a MAHO hole block. It walks these positionally with no magic check, so a modern source's
// extra chunks (object/doodad/sound lists before MAOF, and a MAOE block wedged between MARE and MAHO)
// desync its cursor and fault on a wild pointer. This rebuilds the index to the exact shape the loader
// reads: keep MVER, emit empty MWMO/MWID/MODF, copy each tile's MARE (+ MAHO), drop the source-only chunks,
// and rebuild the MAOF offset table over the new layout.
namespace wxl::modern::adt
{
    // Reshape a low-detail map index to the Client layout. Returns false (serve raw) when the index is
    // already Client-shaped (no source-only chunks to strip).
    bool FixWdl(std::span<const uint8_t> in, std::vector<uint8_t>& out);
}
