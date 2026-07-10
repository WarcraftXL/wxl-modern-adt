// Terrain height-blend consumer: blends terrain layers by height texture at the terrain draw.
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

// Modern terrain blends its texture layers by per-texel HEIGHT (a _h height texture per layer plus a
// per-texture heightScale/heightOffset pair), not by the alpha masks alone; the Client only alpha-lerps,
// which renders modern near-binary masks as hard cuts. This consumer applies the modern formula on the
// Client's own shader terrain path: it captures the bound terrain pixel shader at the per-chunk draw,
// assembles a replacement with the height-blend math injected, binds the layers' height textures on the
// free samplers, and substitutes the shader for the chunks whose textures carry height data (from the
// host ATHB tables ingested by IngestAdtBytes).
namespace wxl::scripts::modernadt
{
    /** @brief Installs the terrain-draw detour and the pixel-shader tracking hook. */
    void InstallHeightBlend();
}
