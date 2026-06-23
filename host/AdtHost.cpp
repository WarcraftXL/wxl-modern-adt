// Host face for wxl-modern-adt: serves a split source terrain tile as one monolithic Client tile.
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

#include "Host.hpp"
#include "mpq/MpqStore.hpp"
#include "core/Logger.hpp"

#include "../shared/AdtMerge.hpp"
#include "../shared/WdtFixup.hpp"
#include "../shared/WdlFixup.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace madt = wxl::modern::adt;

    // Module-owned archive mount: the host's own store is private to its serve loop, and Host.hpp hands
    // modules the client root precisely so they can mount their own. StormLib handles are single-thread;
    // the serve loop is single-threaded, and this is only ever touched from a transform on that thread.
    wxl::host::mpq::MpqStore g_store;
    bool g_mounted = false;
    bool g_mountTried = false;

    bool EnsureMounted()
    {
        if (g_mounted) return true;
        if (g_mountTried) return false;
        g_mountTried = true;
        const std::string root = wxl::host::ClientRoot();
        g_mounted = !root.empty() && g_store.Mount(root);
        wxl::core::log::Printf("modern-adt: archive mount %s (root '%s')",
            g_mounted ? "ok" : "FAILED", root.c_str());
        return g_mounted;
    }

    bool EndsWithCI(std::string_view s, std::string_view suffix)
    {
        if (suffix.size() > s.size()) return false;
        const size_t off = s.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
                std::tolower(static_cast<unsigned char>(suffix[i]))) return false;
        return true;
    }

    // Replace the trailing ".adt" of `root` with `siblingSuffix` (e.g. "_tex0.adt").
    std::string SiblingName(std::string_view root, std::string_view siblingSuffix)
    {
        std::string s(root.substr(0, root.size() - 4)); // drop ".adt"
        s.append(siblingSuffix);
        return s;
    }

    // FileDataID -> path adapter for the merge: routes to the host resolver (the DB2 path tables a module
    // registers). Cold; called once per unresolved texture/model/map-object reference.
    bool ResolveThunk(void* /*user*/, uint32_t fileDataId, std::string& outPath)
    {
        return wxl::host::ResolveFdid(fileDataId, outPath);
    }

    // Merge a split source terrain tile into one monolithic Client tile. Returns false (serve raw) for a
    // tile with no split siblings, or one whose merge does not apply.
    bool TransformAdt(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        if (!EndsWithCI(name, ".adt")) return false;
        // The split siblings are .adt files too; never treat one as a root.
        if (EndsWithCI(name, "_tex0.adt") || EndsWithCI(name, "_obj0.adt") ||
            EndsWithCI(name, "_obj1.adt") || EndsWithCI(name, "_lod.adt")) return false;
        if (!EnsureMounted()) return false;

        const std::string texName = SiblingName(name, "_tex0.adt");
        const std::string objName = SiblingName(name, "_obj0.adt");

        std::vector<uint8_t> tex0, obj0;
        const bool hasTex = g_store.ReadAll(texName, tex0);
        const bool hasObj = g_store.ReadAll(objName, obj0);
        if (!hasTex && !hasObj) return false; // monolithic native tile: serve root unchanged


        madt::ResolveCtx rc{ &ResolveThunk, nullptr };
        const bool ok = madt::MergeSplitAdt(raw, tex0, obj0, out, name, rc);
        if (!ok)
            wxl::core::log::Printf("modern-adt: merge declined for %.*s", int(name.size()), name.data());
        return ok;
    }

    // Build a tile's _tex0 sibling name from the map base (the WDT path without ".wdt") and tile x,y.
    std::string TileTex0(std::string_view base, uint32_t a, uint32_t b)
    {
        return std::string(base) + "_" + std::to_string(a) + "_" + std::to_string(b) + "_tex0.adt";
    }

    // Mask a modern tile-index (WDT) to the shape the Client reads. The merge serves every split tile's
    // alpha as 4-bit, so the WDT must clear big_alpha (else the Client computes the 8-bit alpha genformat
    // and aborts in the alpha-unpack). big_alpha is cleared only when the map is actually
    // served split: a present tile's _tex0 sibling exists in the archive. A native (monolithic) map keeps
    // its alpha format, so its WDT is left untouched.
    bool TransformWdt(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        if (!EndsWithCI(name, ".wdt")) return false;
        // The modern sibling indices (occlusion, lighting, fog, volumes) are not the terrain WDT the Client
        // opens; never reshape one as the tile index.
        if (EndsWithCI(name, "_occ.wdt") || EndsWithCI(name, "_lgt.wdt") || EndsWithCI(name, "_fogs.wdt") ||
            EndsWithCI(name, "_mpv.wdt") || EndsWithCI(name, "_tex.wdt")  || EndsWithCI(name, "_obj.wdt"))
            return false;

        bool clearBigAlpha = false;
        uint32_t tx = 0, ty = 0;
        if (EnsureMounted() && madt::FirstPresentTile(raw, tx, ty))
        {
            const std::string base(name.substr(0, name.size() - 4)); // drop ".wdt"
            if (g_store.Exists(TileTex0(base, tx, ty)) || g_store.Exists(TileTex0(base, ty, tx)))
                clearBigAlpha = true;
        }

        const bool ok = madt::FixWdt(raw, clearBigAlpha, out);
        if (ok)
            wxl::core::log::Printf("modern-wdt: %.*s masked (bigAlpha cleared=%d)",
                int(name.size()), name.data(), int(clearBigAlpha));
        return ok;
    }

    // Reshape a modern low-detail map index (.wdl) to the Client layout. Returns false (serve raw) for a
    // tile index that is already Client-shaped.
    bool TransformWdl(std::string_view name, std::span<const uint8_t> raw, std::vector<uint8_t>& out)
    {
        if (!EndsWithCI(name, ".wdl")) return false;
        const bool ok = madt::FixWdl(raw, out);
        if (ok)
            wxl::core::log::Printf("modern-wdl: %.*s reshaped (%u -> %u bytes)",
                int(name.size()), name.data(), uint32_t(raw.size()), uint32_t(out.size()));
        return ok;
    }

    // File-scope registrar: self-registers the transforms before the host serve loop starts.
    struct Registrar
    {
        Registrar()
        {
            wxl::host::RegisterTransform("modern-adt", &TransformAdt);
            wxl::host::RegisterTransform("modern-wdt", &TransformWdt);
            wxl::host::RegisterTransform("modern-wdl", &TransformWdl);
        }
    } g_registrar;
}
