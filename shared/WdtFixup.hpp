// WDT fixup: mask a modern tile-index (WDT) to the flags + MAIN-entry shape the Client reads.
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

// The map-wide terrain alpha format is read from the WDT MPHD flags: the Client computes the alpha
// genformat as (MPHD.flags & 0x4 /* big_alpha */) ? 8-bit : 4-bit, map-wide. The ADT merge re-packs every
// merged tile's alpha to 4-bit, so a served WDT that still carries big_alpha makes the Client expect 8-bit
// data and abort in the alpha-unpack with a bad genformat. This masks the WDT down to the bits the
// Client reads and, for a merged (4-bit) map, clears big_alpha so the Client computes the 4-bit format.
namespace wxl::modern::adt
{
    // Mask a WDT's MPHD flag word + each MAIN entry flag to the bits the Client reads. When clearBigAlpha
    // is set (the map's tiles are served as 4-bit merged ADTs), also clears MPHD big_alpha (0x4). Returns
    // false (serve raw) when the mask changed nothing (an already Client-shaped WDT).
    bool FixWdt(std::span<const uint8_t> in, bool clearBigAlpha, std::vector<uint8_t>& out);

    // First present tile in the MAIN grid (entry flag bit 0x1 = has-adt) -> tile x,y. Returns false when
    // the WDT has no present tile (e.g. a global-WMO map).
    bool FirstPresentTile(std::span<const uint8_t> in, uint32_t& x, uint32_t& y);
}
