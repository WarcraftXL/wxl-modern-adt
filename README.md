# wxl-modern-adt

Loads modern split terrain tiles in the Client by merging them, in memory, into the one monolithic
`.adt` the native terrain loader reads. No files are written and no native map is touched.

## What it does

A modern terrain tile is split across sibling files:

| File              | Carries                                             |
|-------------------|-----------------------------------------------------|
| `<map>_<x>_<y>.adt`        | root: header, heightmap, normals, vertex colors, liquid |
| `<map>_<x>_<y>_tex0.adt`   | texture layers (MCLY), alpha maps (MCAL), shadow (MCSH) |
| `<map>_<x>_<y>_obj0.adt`   | doodad/object placement refs (MCRD/MCRW), name tables   |

The Client opens only the root file and expects everything inline. When the root is opened, the host
reads the two siblings and assembles a single monolithic tile
(`MVER | MHDR | MCIN[256] | name tables | MCNK[256]`). A tile with no siblings is already monolithic
(a native tile) and is served unchanged.

The merge is **data-gated** (it keys off chunk presence and field values, not a version number), so one
path serves any split source. It normalizes the deltas the native loader cannot consume directly:

- **Alpha maps** are re-packed to 4-bit (the small-alpha path) so merged tiles coexist with the native
  4-bit tiles a map mixes in.
- **Holes** authored as a hi-res 8x8 mask are folded down to the 16-bit lo-res mask the Client reads
  (and kept hi-res in spare header words for the optional hi-res hole patch).
- **Placement flags** are masked to the bits the Client understands; the per-instance WMO scale is
  preserved for the dynamic-scaling runtime.
- **Texture layers** beyond the native cap of 4 are clamped (the >4 tail belongs to the multi-pass
  terrain work and ships with it, never alone).

Sources that name their assets directly (a texture/model name table) need no resolver. Sources that
reference assets by FileDataID rebuild the name tables from a resolver callback (not wired yet).

## Layout

- `shared/` — the byte transform, compiled into both the host and the DLL:
  - `AdtMerge.{hpp,cpp}` — split tile -> monolithic tile.
  - `ChunkIO.hpp` — generic IFF chunk walker.
  - `AdtTexScale.hpp` — optional per-texture UV-scale side table (ATSC).
  - `Resolver.hpp` — the FileDataID -> path callback contract.
- `host/AdtHost.cpp` — registers the serve-time transform; reads the siblings from a module-owned
  archive mount and runs the merge.

## Building

The merge runs host-side, so build the host alongside the DLL:

```
.\build.ps1 -BuildHost
```
