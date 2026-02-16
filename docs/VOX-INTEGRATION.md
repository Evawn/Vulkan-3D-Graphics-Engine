# Voxel Format Integration Guide

A self-contained reference for integrating MagicaVoxel `.vox` file loading, procedural voxel generation, and `.rsvo` sparse voxel octree support into a voxel rendering engine.

All specifications and test data originate from the [ephtracy/voxel-model](https://github.com/ephtracy/voxel-model) repository. All multi-byte integers are **little-endian**. All pseudocode uses C-like syntax.

---

**Which format do I need?**

| Goal | Format | Section |
|------|--------|---------|
| Load MagicaVoxel artist content (models, scenes, materials) | `.vox` | [Section 1](#1-the-vox-file-format-magicavoxel-v150) |
| Procedural voxel generation (spheres, terrain, fractals) | Shader system | [Section 2](#2-procedural-voxel-generation-shader-system) |
| Ray march massive voxel volumes (16K³+) | `.rsvo` | [Section 3](#3-the-rsvo-file-format-sparse-voxel-octree) |
| Convert between formats | Conversion pipelines | [Section 4](#4-format-conversion-pipelines) |

---

## 1. The .vox File Format (MagicaVoxel v150)

### 1.1 Format Summary

- **Type:** RIFF-style chunk-based binary format
- **Creator:** MagicaVoxel by ephtracy ([ephtracy.github.io](http://ephtracy.github.io/))
- **Version:** 150
- **Extension:** `.vox`
- **Capabilities:** One or more voxel models (max 256³ each), 256-color indexed palette, optional scene graph hierarchy, optional PBR materials
- **Byte order:** Little-endian throughout

### 1.2 File Header

```
Offset  Size  Type      Value
─────────────────────────────────────────
0       4     char[4]   'V','O','X',' '  (0x56 0x4F 0x58 0x20)
4       4     int32     150              (version)
```

The fourth byte is a space (`0x20`), not null. Validate both magic and version before proceeding.

### 1.3 Chunk Structure

Every chunk in the file follows this layout:

```
Offset  Size  Type      Description
─────────────────────────────────────────
0       4     char[4]   Chunk ID (ASCII)
4       4     int32     Content size in bytes (N)
8       4     int32     Children chunks total size in bytes (M)
12      N     bytes     Chunk content
12+N    M     bytes     Children chunks (nested)
```

Total chunk size on disk = `12 + N + M`. Unknown chunk IDs must be skipped gracefully by advancing `N + M` bytes past the 12-byte header.

### 1.4 Core Chunks

#### 1.4.1 MAIN — Root Container

The first and only top-level chunk after the file header. Its own content size `N` is typically 0. All other chunks are nested as its children (in the `M` bytes).

#### 1.4.2 PACK — Multi-Model Count (Optional)

```
Offset  Size  Type    Description
─────────────────────────────────────────
0       4     int32   numModels — number of SIZE/XYZI pairs
```

If absent, the file contains exactly one model (one SIZE + one XYZI pair). In modern files, multiple SIZE/XYZI pairs can exist without a PACK chunk — model ID is simply their index in stored order.

#### 1.4.3 SIZE — Model Dimensions

```
Offset  Size  Type    Description
─────────────────────────────────────────
0       4     int32   sizeX
4       4     int32   sizeY
8       4     int32   sizeZ (gravity/up direction)
```

Each SIZE chunk is immediately followed by its paired XYZI chunk. Maximum 256 per axis. **Z is up** in MagicaVoxel — convert to Y-up if your engine requires it.

#### 1.4.4 XYZI — Voxel Data

```
Offset  Size    Type        Description
─────────────────────────────────────────
0       4       int32       numVoxels (N)
4       4*N     byte[4]*N   Per voxel: x, y, z, colorIndex (1 byte each)
```

- Only occupied voxels are stored (sparse).
- `colorIndex` range is `[1, 255]`. Index 0 means empty and never appears in the data.
- Coordinates are 0-indexed: `[0, sizeX-1]`, `[0, sizeY-1]`, `[0, sizeZ-1]`.

#### 1.4.5 RGBA — Color Palette

```
Offset  Size      Type          Description
─────────────────────────────────────────
0       4*256     byte[4]*256   256 entries of (R, G, B, A), 1 byte each
```

In modern MagicaVoxel files (0.99+), the RGBA chunk is always present.

### 1.5 The Palette Index Gotcha

> **This is the single most common implementation bug.** Read carefully.

The RGBA chunk stores 256 RGBA values read sequentially as `data[0]` through `data[255]`. However, the mapping to palette indices is **off by one**:

```
palette[1] = data[0]     ← first RGBA value from file
palette[2] = data[1]
  ...
palette[255] = data[254]
palette[0] = (empty/transparent, never in file data)
data[255] = (unused padding, discard)
```

The correct reading loop:

```c
// Allocate palette with index 0 = transparent
palette[0] = { 0, 0, 0, 0 };

for (int i = 0; i <= 254; i++) {
    palette[i + 1] = read_rgba(stream);
}
read_rgba(stream);  // data[255]: discard
```

The `colorIndex` values from XYZI chunks directly index into `palette[]` — no further adjustment needed after the above mapping.

### 1.6 Default Palette

If no RGBA chunk is present (older files only), use this built-in 256-color palette. Format: `0xAABBGGRR` (ABGR in a little-endian uint32).

```c
uint32_t default_palette[256] = {
    0x00000000, 0xffffffff, 0xffccffff, 0xff99ffff, 0xff66ffff, 0xff33ffff,
    0xff00ffff, 0xffffccff, 0xffccccff, 0xff99ccff, 0xff66ccff, 0xff33ccff,
    0xff00ccff, 0xffff99ff, 0xffcc99ff, 0xff9999ff, 0xff6699ff, 0xff3399ff,
    0xff0099ff, 0xffff66ff, 0xffcc66ff, 0xff9966ff, 0xff6666ff, 0xff3366ff,
    0xff0066ff, 0xffff33ff, 0xffcc33ff, 0xff9933ff, 0xff6633ff, 0xff3333ff,
    0xff0033ff, 0xffff00ff, 0xffcc00ff, 0xff9900ff, 0xff6600ff, 0xff3300ff,
    0xff0000ff, 0xffffffcc, 0xffccffcc, 0xff99ffcc, 0xff66ffcc, 0xff33ffcc,
    0xff00ffcc, 0xffffcccc, 0xffcccccc, 0xff99cccc, 0xff66cccc, 0xff33cccc,
    0xff00cccc, 0xffff99cc, 0xffcc99cc, 0xff9999cc, 0xff6699cc, 0xff3399cc,
    0xff0099cc, 0xffff66cc, 0xffcc66cc, 0xff9966cc, 0xff6666cc, 0xff3366cc,
    0xff0066cc, 0xffff33cc, 0xffcc33cc, 0xff9933cc, 0xff6633cc, 0xff3333cc,
    0xff0033cc, 0xffff00cc, 0xffcc00cc, 0xff9900cc, 0xff6600cc, 0xff3300cc,
    0xff0000cc, 0xffffff99, 0xffccff99, 0xff99ff99, 0xff66ff99, 0xff33ff99,
    0xff00ff99, 0xffffcc99, 0xffcccc99, 0xff99cc99, 0xff66cc99, 0xff33cc99,
    0xff00cc99, 0xffff9999, 0xffcc9999, 0xff999999, 0xff669999, 0xff339999,
    0xff009999, 0xffff6699, 0xffcc6699, 0xff996699, 0xff666699, 0xff336699,
    0xff006699, 0xffff3399, 0xffcc3399, 0xff993399, 0xff663399, 0xff333399,
    0xff003399, 0xffff0099, 0xffcc0099, 0xff990099, 0xff660099, 0xff330099,
    0xff000099, 0xffffff66, 0xffccff66, 0xff99ff66, 0xff66ff66, 0xff33ff66,
    0xff00ff66, 0xffffcc66, 0xffcccc66, 0xff99cc66, 0xff66cc66, 0xff33cc66,
    0xff00cc66, 0xffff9966, 0xffcc9966, 0xff999966, 0xff669966, 0xff339966,
    0xff009966, 0xffff6666, 0xffcc6666, 0xff996666, 0xff666666, 0xff336666,
    0xff006666, 0xffff3366, 0xffcc3366, 0xff993366, 0xff663366, 0xff333366,
    0xff003366, 0xffff0066, 0xffcc0066, 0xff990066, 0xff660066, 0xff330066,
    0xff000066, 0xffffff33, 0xffccff33, 0xff99ff33, 0xff66ff33, 0xff33ff33,
    0xff00ff33, 0xffffcc33, 0xffcccc33, 0xff99cc33, 0xff66cc33, 0xff33cc33,
    0xff00cc33, 0xffff9933, 0xffcc9933, 0xff999933, 0xff669933, 0xff339933,
    0xff009933, 0xffff6633, 0xffcc6633, 0xff996633, 0xff666633, 0xff336633,
    0xff006633, 0xffff3333, 0xffcc3333, 0xff993333, 0xff663333, 0xff333333,
    0xff003333, 0xffff0033, 0xffcc0033, 0xff990033, 0xff660033, 0xff330033,
    0xff000033, 0xffffff00, 0xffccff00, 0xff99ff00, 0xff66ff00, 0xff33ff00,
    0xff00ff00, 0xffffcc00, 0xffcccc00, 0xff99cc00, 0xff66cc00, 0xff33cc00,
    0xff00cc00, 0xffff9900, 0xffcc9900, 0xff999900, 0xff669900, 0xff339900,
    0xff009900, 0xffff6600, 0xffcc6600, 0xff996600, 0xff666600, 0xff336600,
    0xff006600, 0xffff3300, 0xffcc3300, 0xff993300, 0xff663300, 0xff333300,
    0xff003300, 0xffff0000, 0xffcc0000, 0xff990000, 0xff660000, 0xff330000,
    0xff0000ee, 0xff0000dd, 0xff0000bb, 0xff0000aa, 0xff000088, 0xff000077,
    0xff000055, 0xff000044, 0xff000022, 0xff000011, 0xff00ee00, 0xff00dd00,
    0xff00bb00, 0xff00aa00, 0xff008800, 0xff007700, 0xff005500, 0xff004400,
    0xff002200, 0xff001100, 0xffee0000, 0xffdd0000, 0xffbb0000, 0xffaa0000,
    0xff880000, 0xff770000, 0xff550000, 0xff440000, 0xff220000, 0xff110000,
    0xffeeeeee, 0xffdddddd, 0xffbbbbbb, 0xffaaaaaa, 0xff888888, 0xff777777,
    0xff555555, 0xff444444, 0xff222222, 0xff111111
};
```

### 1.7 Extended Data Types

These types are used by the scene graph, material, and metadata chunks.

#### 1.7.1 STRING

```
int32     buffer_size (byte count, NOT including null terminator)
int8[N]   buffer (UTF-8, no trailing null)
```

#### 1.7.2 DICT

```
int32     num_pairs
For each pair:
    STRING  key
    STRING  value
```

All property values are stored as strings, even numeric ones. Parse floats/ints from their string representation (e.g., key `"_weight"`, value `"0.5"`).

#### 1.7.3 ROTATION

Encodes a 3x3 rotation matrix in a single byte. Only 24 valid rotations exist (axis permutations with sign flips — all are 90-degree increments).

```
Bit   Meaning
────────────────────────────────────────────────
0-1   Column index of non-zero entry in row 0
2-3   Column index of non-zero entry in row 1
4     Sign of row 0 entry (0 = positive, 1 = negative)
5     Sign of row 1 entry (0 = positive, 1 = negative)
6     Sign of row 2 entry (0 = positive, 1 = negative)
```

The third row's column is implicitly the one not used by rows 0 and 1.

**Worked example** from the spec:

```
R = | 0  1  0 |      row0: non-zero at column 1
    | 0  0 -1 |      row1: non-zero at column 2, negative
    |-1  0  0 |      row2: non-zero at column 0 (implicit), negative

byte = (1 << 0) | (2 << 2) | (0 << 4) | (1 << 5) | (1 << 6)
     = 1 | 8 | 0 | 32 | 64
     = 105 (0x69)
```

**Decode pseudocode:**

```c
void decode_rotation(uint8_t r, int mat[3][3]) {
    memset(mat, 0, sizeof(int) * 9);

    int row0_col = (r >> 0) & 0x3;
    int row1_col = (r >> 2) & 0x3;
    int row0_sign = (r >> 4) & 0x1 ? -1 : 1;
    int row1_sign = (r >> 5) & 0x1 ? -1 : 1;
    int row2_sign = (r >> 6) & 0x1 ? -1 : 1;

    // Third row column: the one not used by rows 0 and 1
    int row2_col = 3 - row0_col - row1_col;  // works because 0+1+2 = 3

    mat[0][row0_col] = row0_sign;
    mat[1][row1_col] = row1_sign;
    mat[2][row2_col] = row2_sign;
}
```

### 1.8 Scene Graph Chunks

The scene graph organizes models in a hierarchy of transforms, groups, and shapes:

```
     T (Transform)
     |
     G (Group)
    / \
   T   T
   |   |
   G   S (Shape → references a model)
  / \
 T   T
 |   |
 S   S
```

The root node is always a Transform (typically node ID 0).

#### 1.8.1 nTRN — Transform Node

```
int32   node_id
DICT    node_attributes
            "_name"   : string (optional)
            "_hidden" : "0" or "1" (optional)
int32   child_node_id
int32   reserved (must be -1)
int32   layer_id
int32   num_frames (must be >= 1)

For each frame:
    DICT  frame_attributes
              "_r" : int8     — ROTATION byte (see 1.7.3)
              "_t" : string   — translation as "x y z" (space-separated int32s)
              "_f" : int32    — frame index (0-based)
```

Defaults when frame attributes are absent: rotation = identity, translation = (0, 0, 0).

#### 1.8.2 nGRP — Group Node

```
int32   node_id
DICT    node_attributes
int32   num_children

For each child:
    int32  child_node_id
```

Children of a Group are always Transform nodes.

#### 1.8.3 nSHP — Shape Node

```
int32   node_id
DICT    node_attributes
int32   num_models (must be >= 1)

For each model:
    int32  model_id     — index into the SIZE/XYZI pairs (0-based)
    DICT   model_attributes
               "_f" : int32  — frame index (0-based, optional)
```

#### 1.8.4 Scene Graph Traversal

```c
void traverse(int node_id, mat4 parent_transform) {
    Node* node = nodes[node_id];

    if (node->type == TRANSFORM) {
        mat4 local = make_transform(node->rotation, node->translation);
        mat4 world = parent_transform * local;
        traverse(node->child_id, world);
    }
    else if (node->type == GROUP) {
        for (int i = 0; i < node->num_children; i++) {
            traverse(node->children[i], parent_transform);
        }
    }
    else if (node->type == SHAPE) {
        for (int i = 0; i < node->num_models; i++) {
            render_model(node->model_ids[i], parent_transform);
        }
    }
}

// Start from root: traverse(root_node_id, identity_matrix);
```

### 1.9 Material and Metadata Chunks

#### 1.9.1 MATL — PBR Materials

```
int32   material_id     — same as palette index [1-255]
DICT    properties
            "_type"    : "_diffuse" | "_metal" | "_glass" | "_emit"
            "_weight"  : float 0.0-1.0
            "_rough"   : float (roughness)
            "_spec"    : float (specularity)
            "_ior"     : float (index of refraction)
            "_att"     : float (attenuation)
            "_flux"    : float (emissive flux)
            "_plastic" : flag (presence indicates plastic metal)
```

Material ID maps directly to palette index: a voxel with `colorIndex = N` uses the MATL with `material_id = N`. Palette entries without a MATL chunk default to diffuse.

**PBR mapping suggestions:**

| .vox Type | Engine PBR |
|-----------|------------|
| `_diffuse` | Albedo only, metallic=0 |
| `_metal` | Metallic workflow, use `_rough` and `_spec` |
| `_glass` | Transmission/refraction, use `_ior` and `_att` |
| `_emit` | Emissive channel, use `_flux` for intensity |

#### 1.9.2 LAYR — Layers

```
int32   layer_id
DICT    attributes
            "_name"   : string
            "_hidden" : "0" or "1"
int32   reserved (must be -1)
```

#### 1.9.3 rOBJ — Render Objects

```
DICT    rendering_attributes
```

Engine-specific rendering settings. Parse what you recognize, skip the rest.

#### 1.9.4 rCAM — Camera

```
int32   camera_id
DICT    attributes
            "_mode"    : string
            "_focus"   : vec3 as string
            "_angle"   : vec3 as string
            "_radius"  : int
            "_frustum" : float
            "_fov"     : int
```

#### 1.9.5 NOTE — Color Names

```
int32   num_names
For each:
    STRING  color_name
```

Annotation labels for palette entries.

#### 1.9.6 IMAP — Index Mapping

```
256 entries, each:
    int32   palette_index_association
```

Remaps palette indices. Apply after loading if present.

### 1.10 Complete .vox Parsing Pseudocode

```c
struct VoxModel {
    int size_x, size_y, size_z;
    int num_voxels;
    struct { uint8_t x, y, z, color_index; } *voxels;
};

struct VoxFile {
    VoxModel *models;
    int num_models;
    uint8_t palette[256][4];  // RGBA
    // + materials, scene nodes, layers as needed
};

VoxFile parse_vox(Stream *s) {
    // Header
    assert(read_bytes(s, 4) == "VOX ");
    assert(read_int32(s) == 150);

    // MAIN chunk
    ChunkHeader main = read_chunk_header(s);
    assert(main.id == "MAIN");

    VoxFile file = {};
    memcpy(file.palette, default_palette, sizeof(default_palette));

    int model_index = 0;
    long end = s->position + main.children_size;

    while (s->position < end) {
        ChunkHeader chunk = read_chunk_header(s);
        long chunk_end = s->position + chunk.content_size;

        switch (chunk.id) {
            case "PACK":
                file.num_models = read_int32(s);
                file.models = calloc(file.num_models, sizeof(VoxModel));
                break;

            case "SIZE":
                if (!file.models) {
                    file.num_models = 1;
                    file.models = calloc(1, sizeof(VoxModel));
                }
                file.models[model_index].size_x = read_int32(s);
                file.models[model_index].size_y = read_int32(s);
                file.models[model_index].size_z = read_int32(s);
                break;

            case "XYZI": {
                int n = read_int32(s);
                file.models[model_index].num_voxels = n;
                file.models[model_index].voxels = malloc(n * 4);
                read_bytes(s, file.models[model_index].voxels, n * 4);
                model_index++;
                break;
            }

            case "RGBA":
                file.palette[0] = (RGBA){ 0, 0, 0, 0 };
                for (int i = 0; i <= 254; i++) {
                    file.palette[i + 1] = read_rgba(s);
                }
                read_rgba(s);  // discard data[255]
                break;

            case "nTRN": /* parse transform node */  break;
            case "nGRP": /* parse group node */       break;
            case "nSHP": /* parse shape node */       break;
            case "MATL": /* parse material */         break;
            case "LAYR": /* parse layer */            break;

            default:
                // Unknown chunk — skip it
                break;
        }

        // Advance past any unread content + children
        s->position = chunk_end + chunk.children_size;
    }

    return file;
}
```

### 1.11 Architectural Considerations

#### Memory Layout

| Strategy | Pros | Cons | Best For |
|----------|------|------|----------|
| Flat voxel list (as stored) | Minimal memory, direct from file | No random access by position | Streaming, conversion |
| Dense 3D array `[sizeZ][sizeY][sizeX]` | O(1) position lookup | 256³ = 16M entries, wasteful if sparse | Small models, GPU 3D textures |
| Hash map `(x,y,z) → colorIndex` | Memory-proportional to voxel count | Hash overhead, cache-unfriendly | Large sparse models |
| Octree | Hierarchical LOD, efficient ray traversal | Complex construction | Rendering pipeline |

#### GPU Upload

- **3D Texture:** Upload dense grid as `GL_R8UI` / `VK_FORMAT_R8_UINT`. Sample palette via 1D texture (256x1 RGBA8) or uniform buffer. Simple, fast, limited to 256³.
- **SSBO Voxel List:** Upload flat XYZI array, use compute shader to voxelize into 3D texture or build octree on GPU.
- **Palette:** Upload as 1D texture (256x1 RGBA8) or as a 1024-byte uniform/SSBO.

#### Coordinate System

MagicaVoxel uses **Z-up, right-handed**. If your engine uses Y-up:

```c
engine_x =  vox_x;
engine_y =  vox_z;
engine_z = -vox_y;  // or +vox_y depending on handedness
```

Apply this swap when reading XYZI data and when interpreting SIZE dimensions.

### 1.12 Edge Cases and Gotchas

- **Palette index off-by-one** — `data[i]` maps to `palette[i+1]`. See [Section 1.5](#15-the-palette-index-gotcha).
- **Z-up vs Y-up** — Z is gravity in MagicaVoxel. Swap axes for Y-up engines.
- **PACK may be absent** — Modern files can have multiple SIZE/XYZI pairs without PACK.
- **Unknown chunk IDs** — Must be skipped, not rejected. The format is extensible.
- **DICT values are always strings** — Even numeric properties like `"_weight"` store `"0.5"` as a string.
- **ROTATION third row is implicit** — The column index for row 2 = `3 - row0_col - row1_col`.
- **Translation is a string** — `_t` in nTRN frame attributes is `"x y z"` (space-separated), not binary.
- **colorIndex 0 is never stored** in XYZI — it means empty. Any 0 values in your grid represent air.
- **`data[255]` is unused** — The last RGBA entry read from the file is padding; discard it.
- **Model dimensions can theoretically be 0** on any axis — handle gracefully.

### 1.13 .vox Test Data

**Recommended first test:** `vox/character/chr_bow.vox` — smallest, simplest single model.

| Path | Category | Notes |
|------|----------|-------|
| `vox/character/chr_bow.vox` | Character | Small, simple |
| `vox/character/chr_cat.vox` | Character | |
| `vox/character/chr_fox.vox` | Character | |
| `vox/character/chr_gumi.vox` | Character | |
| `vox/character/chr_jp.vox` | Character | |
| `vox/character/chr_knight.vox` | Character | |
| `vox/character/chr_man.vox` | Character | |
| `vox/character/chr_mom.vox` | Character | |
| `vox/character/chr_old.vox` | Character | |
| `vox/character/chr_poem.vox` | Character | |
| `vox/character/chr_rain.vox` | Character | |
| `vox/character/chr_sasami.vox` | Character | |
| `vox/character/chr_sol.vox` | Character | |
| `vox/character/chr_sword.vox` | Character | |
| `vox/character/chr_tale.vox` | Character | |
| `vox/character/chr_tama.vox` | Character | |
| `vox/character/chr_tsurugi.vox` | Character | |
| `vox/monument/monu0.vox` | Monument | Small |
| `vox/monument/monu1.vox` — `monu10.vox` | Monument | Increasing complexity |
| `vox/monument/monu16.vox` | Monument | Large, good stress test |
| `vox/monument/monu6-without-water.vox` | Monument | Water variant comparison |
| `vox/monument/monu8-without-water.vox` | Monument | Water variant comparison |
| `vox/anim/deer.vox` | Animated | Multi-frame (PACK), tests animation loading |
| `vox/anim/horse.vox` | Animated | Multi-frame |
| `vox/anim/T-Rex.vox` | Animated | Multi-frame, largest animated model |
| `vox/scan/dragon.vox` | 3D Scan | Voxelized mesh, dense |
| `vox/scan/teapot.vox` | 3D Scan | Voxelized mesh |
| `vox/procedure/menger.vox` | Procedural | Menger sponge fractal |
| `vox/procedure/nature.vox` | Procedural | Terrain |
| `vox/procedure/maze.vox` | Procedural | 3D maze |
| `vox/procedure/maze2D.vox` | Procedural | 2D maze |
| `vox/procedure/snow.vox` | Procedural | |
| `vox/procedure/ff1.vox` — `ff3.vox` | Procedural | |

---

## 2. Procedural Voxel Generation (Shader System)

### 2.1 Concept

MagicaVoxel includes a procedural shader system where GLSL functions generate voxel content. The core pattern is portable to any engine: **a function `map(vec3 position) → colorIndex` evaluated over a 3D grid**.

This section extracts that pattern for use in your own rendering engine — not as MagicaVoxel plugins, but as a general-purpose procedural voxel generation architecture.

### 2.2 The Shader Contract

```glsl
float map(vec3 v) → float
```

- **Input:** `v` is an integer grid position in `[0, sizeX) × [0, sizeY) × [0, sizeZ)`.
- **Output:** Palette color index as float. Return `0.0` for empty. Return any value in `[1.0, 255.0]` for a filled voxel.
- **Property:** Each invocation is independent — the function is trivially parallelizable.

### 2.3 Uniform Inputs

These are the inputs available to every shader (consistent across all reference implementations):

| Uniform | Type | Description |
|---------|------|-------------|
| `iVolumeSize` | vec3 | Volume dimensions per axis |
| `iColorIndex` | float | Currently selected palette index [1-255] |
| `iMirror` | vec3 | Mirror mode per axis (0 or 1) |
| `iAxis` | vec3 | Axis constraint per axis (0 or 1) |
| `iFrame` | float | Current animation frame index |
| `iNumFrames` | float | Total number of animation frames |
| `iIter` | float | Current iteration index (for multi-pass shaders) |
| `iRand` | vec4 | Random seed values |
| `iArgs[8]` | float[8] | User-supplied parameters |

**Built-in function:**

```glsl
float voxel(vec3 v);  // Read existing voxel color index at position v
```

This enables shaders that modify existing content (e.g., height map extrusion reads the ground plane).

### 2.4 Reference Shaders

#### 2.4.1 Sphere Generator

Generates a sphere filling the volume. The simplest demonstration of the distance-field pattern.

```glsl
// Invoke: xs sphere

float map(vec3 v) {
    const float PI = 3.1415926;
    vec3 center = iVolumeSize * 0.5;
    float radius = min(min(center.x, center.y), center.z);
    float dist = length(v + 0.5 - center);
    return iColorIndex * step(dist, radius);
}
```

**Algorithm:** Compute distance from voxel center to volume center. If within the smallest half-dimension, fill with the active color. The `+ 0.5` centers the calculation on the voxel rather than its corner.

**Key pattern:** Distance-field thresholding. Generalize to any SDF (box, torus, CSG operations).

#### 2.4.2 Height Map Extruder

Reads color values on the ground plane and extrudes them vertically as a height field.

```glsl
// Invoke: xs height 0.5

float map(vec3 v) {
    float height = 255.0 - voxel(vec3(v.xy, 0.0));
    height = height * iArgs[0];
    return (v.z <= height ? iColorIndex : 0.0);
}
```

**Algorithm:** Sample the existing voxel at `(x, y, 0)`. Invert the color index (so brighter = taller). Scale by `iArgs[0]`. Fill all voxels from `z=0` up to that height.

**Key pattern:** Uses `voxel()` to read existing data — this shader modifies content rather than generating from scratch. Also demonstrates `iArgs` for parameterized control.

#### 2.4.3 Animated Wave

A time-varying radial wave surface.

```glsl
// Invoke: xs wave

float map(vec3 v) {
    vec3 center = iVolumeSize * 0.5;
    float t = length((v + 0.5 - center).xy);
    float s = center.z * (cos(t * 3.14159 * 0.1 +
              3.14159265 * 2.0 * iFrame / iNumFrames) * 0.25 + 1.0);
    return iColorIndex * step(abs(v.z - s), 2.0);
}
```

**Algorithm:** Compute radial distance from center in XY. Feed into a cosine with phase shift from `iFrame/iNumFrames`. The result is a surface height that oscillates over time. Fill voxels within 2 units of that surface.

**Key pattern:** Animation via frame uniforms. Generate all frames by iterating `iFrame` from 0 to `iNumFrames-1`.

#### 2.4.4 Regular Polygon / Star

Generates N-sided regular polygons or star shapes in the XY plane.

```glsl
// Invoke: xs poly 7        (heptagon)
// Invoke: xs poly 7 0.5    (star with inner ratio 0.5)

float map(vec3 v) {
    const float PI = 3.1415926;
    vec3 center = iVolumeSize * 0.5;
    vec2 u = (v + 0.5 - center).xy;

    float angle = PI * 2.0 / max(floor(iArgs[0]), 3.0);
    float t = min(center.x, center.y);

    float r = length(u);
    float theta = atan(u.y, u.x) + PI;

    r *= cos(angle * abs(fract(theta / angle) - 0.5));
    t *= cos(angle * 0.5);

    r = max(r, 0.0);
    t = max(t, 0.0);

    return iColorIndex * step(r, t) * step(t * iArgs[1], r);
}
```

**Algorithm:** Convert XY position to polar coordinates. Use angular modulus to project onto a polygon edge. `iArgs[0]` = number of sides, `iArgs[1]` = inner cutout ratio for star shapes (0 = filled polygon).

**Key pattern:** Multiple `iArgs` parameters for rich user control.

#### 2.4.5 3D Mandelbrot Fractal

Generates a 3D Mandelbulb-like fractal via spherical coordinate power iteration.

```glsl
// Invoke: xs mand 8

vec3 powern(vec3 v, float n) {
    float r = length(v);
    float phi = atan(v.y, v.x);
    float theta = atan(length(v.xy), v.z);
    r = pow(r, n);
    phi *= n;
    theta *= n;
    return r * vec3(
        sin(theta) * vec2(cos(phi), sin(phi)),
        cos(theta)
    );
}

float map(vec3 v) {
    float n = max(iArgs[0], 4.0);
    vec3 center = iVolumeSize * 0.5;
    float size = min(min(center.x, center.y), center.z);
    vec3 u = (v + 0.5 - center) / size * 1.1;

    for (int i = 0; i < 8; i++) {
        u = powern(u, n) + u;
    }

    return iColorIndex * step(length(u), 2.0);
}
```

**Algorithm:** Normalize voxel position to [-1.1, 1.1]. Iterate the power function 8 times (like Mandelbrot's `z = z² + c` but in 3D spherical coordinates with configurable power). Points that stay bounded (length < 2) are inside the fractal.

**Key pattern:** Iterative evaluation, helper functions, expensive per-voxel computation (benefits greatly from GPU dispatch).

### 2.5 Engine Integration Strategies

#### CPU Evaluation

The simplest approach — iterate every voxel position and call `map()`:

```c
for (int z = 0; z < size_z; z++)
  for (int y = 0; y < size_y; y++)
    for (int x = 0; x < size_x; x++) {
      float idx = map(vec3(x, y, z));
      if (idx > 0.0f)
        store_voxel(x, y, z, (uint8_t)round(idx));
    }
```

For 126³ = ~2M evaluations, this runs fast on a modern CPU. Each call is independent — trivially parallelizable across threads.

#### GPU Compute Shader Dispatch

Wrap the `map()` function in a compute shader:

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;

layout(r8ui, binding = 0) uniform writeonly uimage3D volume;

uniform vec3  iVolumeSize;
uniform float iColorIndex;
uniform float iArgs[8];
// ... other uniforms

// Paste map() and helpers here

void main() {
    ivec3 pos = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(pos, ivec3(iVolumeSize)))) return;

    float color = map(vec3(pos));
    imageStore(volume, pos, uvec4(uint(round(color))));
}
```

Dispatch: `glDispatchCompute(ceil(sizeX/8), ceil(sizeY/8), ceil(sizeZ/8))`. Output: a 3D `R8UI` texture ready for rendering.

#### Animation and Multi-Pass

- **Animation:** Generate all frames by dispatching with `iFrame` varying from 0 to `iNumFrames-1`. Store as an array of volumes or as keyframes.
- **Multi-pass (`iIter`):** Some shaders are designed for iterative refinement (erosion, cellular automata). Run the shader N times, piping output back as input for the `voxel()` function on the next pass. Use double-buffered 3D textures.

### 2.6 Procedural Pipeline Architecture

- **Shader registry:** Store `map()` functions as named generators. Allow chaining (output of one feeds as `voxel()` input to the next).
- **Parameter binding:** Expose `iArgs[0..7]` as user-facing sliders/inputs. Document what each argument means per shader.
- **Caching:** Deterministic generators with identical parameters produce identical output. Cache by parameter hash.
- **LOD:** Evaluate at half resolution per axis (1/8 the work) for preview, full resolution for final output.
- **Integration with .vox pipeline:** Output uses the same palette-indexed color model. Can be saved as .vox or fed directly into the render pipeline.

---

## 3. The .rsvo File Format (Sparse Voxel Octree)

### 3.1 Format Summary

- **Type:** Binary sparse voxel octree (SVO) occupancy format
- **Creator:** ephtracy, dated 03/22/2015
- **Version:** 1
- **Extension:** `.rsvo` (inside `.7z` archives in the test data)
- **Storage:** Children masks of internal nodes in breadth-first search (BFS) order
- **Depth:** Uniform — all leaf nodes are at level 0
- **Volume sizes:** Up to 2^N per side (N=14 → 16,384³)
- **Limitation:** Structure only. No color, material, or attribute data.

### 3.2 File Header

```
Offset  Size  Type      Description
──────────────────────────────────────────────
0       4     char[4]   "RSVO" ('R' at byte 0, 'O' at byte 3)
4       4     int32     Version (must be 1)
8       4     int32     Reserved info 0 (must be 0)
12      4     int32     Reserved info 1 (must be 0)
16      4     int32     Top level N (e.g., 14 → 2^14 = 16,384 volume)
```

### 3.3 Node Counts

Immediately following the header, one `uint32` per level from top to bottom:

```
Offset       Size        Description
──────────────────────────────────────────────
20           4           node_count at level N     (root, always 1)
24           4           node_count at level N-1
...                      ...
20 + 4*N     4           node_count at level 0     (leaf level)
```

There are `N + 1` entries total. The sum of counts at levels `N` through `1` gives total internal nodes.

### 3.4 Children Masks

After node counts, one byte per internal node in BFS order:

```
Offset                    Size    Description
──────────────────────────────────────────────
20 + 4*(N+1)              1       Root's children mask (level N, 1 node)
...                       X       Level N-1 masks (node_count[N-1] bytes)
...                       X       Level N-2 masks
...                       ...
...                       X       Level 1 masks
(no masks for level 0 — leaves have no children)
```

Each mask is 8 bits, one per octant child. Bit `i` = 1 means octant `i` is occupied.

**Standard octant ordering (Morton / Z-order):**

```
Bit  Octant  Position
─────────────────────
0    (0,0,0)  -X -Y -Z
1    (1,0,0)  +X -Y -Z
2    (0,1,0)  -X +Y -Z
3    (1,1,0)  +X +Y -Z
4    (0,0,1)  -X -Y +Z
5    (1,0,1)  +X -Y +Z
6    (0,1,1)  -X +Y +Z
7    (1,1,1)  +X +Y +Z
```

> **Note:** The octant bit ordering is not explicitly documented in the `.rsvo` spec. The above is standard Morton order; verify against the test data by checking that rendered output matches the preview `.png` images.

### 3.5 Complete Parsing Pseudocode

```c
struct SVO {
    int top_level;          // N
    uint32_t *node_counts;  // N+1 entries
    uint8_t  *masks;        // one per internal node
    uint32_t  total_internal_nodes;
};

SVO parse_rsvo(Stream *s) {
    // Header
    assert(read_bytes(s, 4) == "RSVO");
    assert(read_int32(s) == 1);       // version
    read_int32(s);                     // reserved 0
    read_int32(s);                     // reserved 1

    SVO svo;
    svo.top_level = read_int32(s);
    int N = svo.top_level;

    // Node counts: levels N down to 0
    svo.node_counts = malloc((N + 1) * sizeof(uint32_t));
    for (int level = N; level >= 0; level--) {
        svo.node_counts[level] = read_uint32(s);
    }

    // Total internal nodes = sum of counts at levels N through 1
    svo.total_internal_nodes = 0;
    for (int level = N; level >= 1; level--) {
        svo.total_internal_nodes += svo.node_counts[level];
    }

    // Children masks in BFS order (levels N down to 1)
    svo.masks = malloc(svo.total_internal_nodes);
    read_bytes(s, svo.masks, svo.total_internal_nodes);

    return svo;
}
```

### 3.6 In-Memory Reconstruction

#### Pointer-Based Tree (for clarity)

```c
uint32_t mask_offset = 0;

Node *root = new_node(N, svo.masks[mask_offset++]);
Queue queue = { root };

while (!queue_empty(&queue)) {
    Node *node = dequeue(&queue);
    if (node->level == 0) continue;  // leaf — no children

    for (int bit = 0; bit < 8; bit++) {
        if (node->mask & (1 << bit)) {
            int child_level = node->level - 1;
            Node *child;
            if (child_level > 0) {
                child = new_node(child_level, svo.masks[mask_offset++]);
            } else {
                child = new_leaf();
            }
            node->children[bit] = child;
            enqueue(&queue, child);
        }
    }
}
```

#### Flat Array for GPU (recommended)

The BFS-ordered flat mask array is already GPU-friendly. To traverse, you need to compute child pointers:

For a node at index `i` within its level, its first child's index in the next level is:

```c
// child_base[i] = sum of popcount(mask[j]) for j in 0..i-1 within this level
uint32_t child_base = 0;
for (int j = 0; j < i; j++) {
    child_base += popcount(level_masks[j]);
}
```

**Optimization:** Precompute a prefix-sum of `popcount` per level for O(1) child lookup. This turns the flat mask array into a fully traversable structure without building an explicit tree.

### 3.7 GPU Ray Marching

For ray marching through the SVO in a fragment or compute shader:

1. **Upload:** Store children masks as an SSBO or texture buffer. One buffer per level, or concatenated with level offsets.
2. **Prefix sums:** Precompute per-level prefix sums of `popcount` on CPU. Upload alongside masks.
3. **Traversal:** Start at root. For each level, compute which octant the ray enters, check the mask bit. If set, compute child index via prefix sum, descend. If not, step to the next octant (DDA or parametric).
4. **LOD:** Stop traversal at any level for distance-based level-of-detail. Coarser levels are naturally at the front of the buffer.

### 3.8 Streaming and LOD

The BFS ordering naturally supports progressive loading:

- **Coarse first:** The first `node_count[N]` bytes (just 1 byte — the root mask) give the coarsest representation.
- **Stream by level:** Read one level at a time. Each level refines the resolution by 2x per axis.
- **Memory budget:** Stop loading when memory is full. The tree remains valid at whatever depth was loaded.

### 3.9 Limitations

- **No color data.** The format stores only occupancy (occupied vs. empty). To render with color, you need a separate source (e.g., a `.vox` file of the same model) and must map leaf positions to colors.
- **Uniform depth.** All leaves are at level 0. No support for early termination at coarser levels for homogeneous regions (unlike some SVO formats with "leaf" flags).
- **No normals or materials.** Normals must be computed from the occupancy structure (e.g., gradient of the occupancy field or face normals from neighbor checks).

### 3.10 .rsvo Test Data

All test files are 7z-compressed. Extract with `7z x <file>` (install via `brew install p7zip` on macOS, `apt install p7zip-full` on Linux).

| File | Compressed | Volume | Description |
|------|-----------|--------|-------------|
| `svo/buddha_16k.7z` | 48 MB | 16,384³ | Stanford Happy Buddha |
| `svo/xyzrgb_dragon_16k.7z` | 43 MB | 16,384³ | Stanford Dragon |
| `svo/xyzrgb_statuette_8k.7z` | 17 MB | 8,192³ | XYZ RGB Statuette |
| `svo/sibenik_8k.7z` | 5.1 MB | 8,192³ | Sibenik Cathedral (architecture) |

Each archive has a matching `.png` preview image for visual verification.

**Recommended first test:** `sibenik_8k.7z` — smallest archive, 8K resolution, architectural model with clear structure for verifying correctness.

**Extraction example:**

```bash
cd svo/
7z x sibenik_8k.7z
# Produces: sibenik_8k.rsvo (or similar)
```

---

## 4. Format Conversion Pipelines

### 4.1 .vox → SVO

Convert a palette-indexed voxel model into a sparse octree:

```
Input:  VoxModel (SIZE dimensions + XYZI voxel list)
Output: SVO (children masks in BFS order)

1. Determine octree depth:
   N = ceil(log2(max(sizeX, sizeY, sizeZ)))
   volume_size = 2^N

2. Build occupancy lookup:
   occupied = new HashSet<(x,y,z)>()
   for each voxel in XYZI:
       occupied.add(voxel.x, voxel.y, voxel.z)

3. Build octree top-down via BFS:
   root = Node(level=N, origin=(0,0,0), size=volume_size)
   queue = [root]
   masks_by_level = { level: [] for level in N..1 }

   while queue not empty:
       node = dequeue(queue)
       if node.level == 0: continue

       mask = 0
       half = node.size / 2
       for bit in 0..7:
           octant_origin = node.origin + octant_offset(bit) * half
           if any_occupied(occupied, octant_origin, half):
               mask |= (1 << bit)
               child = Node(level=node.level-1, origin=octant_origin, size=half)
               enqueue(queue, child)

       masks_by_level[node.level].append(mask)

4. Write RSVO:
   write "RSVO", version=1, reserved=0, reserved=0, top_level=N
   write node_counts per level
   write masks in BFS order (level N, then N-1, ..., then 1)
```

**Note:** The `any_occupied()` check can be expensive. For large voxel sets, use a 3D boolean grid (`volume_size³` bits) for O(1) lookups, or precompute Morton codes and use range queries.

**Color loss:** The `.rsvo` format discards color. To preserve color, extend with a parallel per-leaf color array (non-standard).

### 4.2 SVO → Dense Voxel Grid

Reconstruct a flat 3D grid from the octree:

```
Input:  SVO (from .rsvo parse)
Output: bool grid[volume_size][volume_size][volume_size]

Traverse the octree (BFS or DFS), tracking each node's position and size.
At each leaf node (level 0), if the node exists (parent's mask bit was set),
mark grid[x][y][z] = true.
```

> **Warning:** A 16,384³ dense grid requires 4 TB of memory. This conversion is only practical for sub-volumes or moderate resolutions (up to ~1024³ ≈ 1 GB for a bool grid, 128 MB for a bit-packed grid).

### 4.3 Dense Grid → .vox Export

```
Input:  Dense grid + palette
Output: .vox file

1. If grid exceeds 256³, partition into 256³ sub-volumes.
   Each sub-volume becomes a separate model.

2. For each sub-volume:
   - Scan for occupied cells → build XYZI voxel list
   - Write SIZE chunk (dimensions of sub-volume)
   - Write XYZI chunk (voxel count + voxel data)

3. Write file:
   Header: "VOX " + version 150
   MAIN chunk containing:
     PACK chunk (if multiple sub-volumes)
     SIZE/XYZI pairs
     RGBA chunk (palette)
```

### 4.4 Octree from Flat Voxel List (Morton Code Approach)

For large datasets, building the octree bottom-up via Morton codes avoids materializing a dense grid:

```
1. Compute Morton code for each voxel:
   morton = encode_morton(x, y, z)  // interleave bits of x, y, z

2. Sort voxels by Morton code.
   (This spatially clusters nearby voxels.)

3. Build octree bottom-up:
   - Level 0: each voxel is a leaf.
   - Level 1: group leaves into parents by stripping the lowest 3 bits
     of the Morton code. Compute children mask from which of the 8
     possible children exist.
   - Level L: group level L-1 nodes into parents by stripping the next
     3 bits. Repeat until reaching the root.

4. Serialize BFS:
   BFS-traverse the tree, output node counts per level and masks in
   BFS order for RSVO compatibility.
```

This approach is O(N log N) dominated by the sort, and memory-efficient since it never allocates the full volume.

---

## 5. Quick Reference Tables

### 5.1 .vox Chunk IDs

| Chunk ID | Category | Required | Description |
|----------|----------|----------|-------------|
| `MAIN` | Structure | Yes | Root container |
| `PACK` | Structure | No | Multi-model count |
| `SIZE` | Model | Yes | Model dimensions (paired with XYZI) |
| `XYZI` | Model | Yes | Voxel positions + color indices |
| `RGBA` | Palette | Effectively yes | 256-color palette |
| `nTRN` | Scene | No | Transform node |
| `nGRP` | Scene | No | Group node |
| `nSHP` | Scene | No | Shape node |
| `MATL` | Material | No | PBR material properties |
| `LAYR` | Metadata | No | Layer info |
| `rOBJ` | Metadata | No | Render object settings |
| `rCAM` | Metadata | No | Camera settings |
| `NOTE` | Metadata | No | Color name annotations |
| `IMAP` | Metadata | No | Palette index remapping |

### 5.2 Material Properties

| Property | Type | Range | Applies To |
|----------|------|-------|------------|
| `_type` | string | `_diffuse`, `_metal`, `_glass`, `_emit` | All |
| `_weight` | float | 0.0 — 1.0 | All |
| `_rough` | float | 0.0 — 1.0 | `_metal`, `_glass` |
| `_spec` | float | 0.0 — 1.0 | `_metal` |
| `_ior` | float | varies | `_glass` |
| `_att` | float | varies | `_glass` |
| `_flux` | float | varies | `_emit` |
| `_plastic` | flag | presence | `_metal` |

### 5.3 .rsvo File Layout

```
┌──────────────────────────────────────────┐
│ "RSVO"  (4 bytes)                        │  Magic
│ 1       (4 bytes)                        │  Version
│ 0       (4 bytes)                        │  Reserved
│ 0       (4 bytes)                        │  Reserved
│ N       (4 bytes)                        │  Top level
├──────────────────────────────────────────┤
│ count_level_N     (4 bytes) ← always 1   │
│ count_level_N-1   (4 bytes)              │  Node counts
│ ...                                      │  (N+1 entries)
│ count_level_0     (4 bytes) ← leaf count │
├──────────────────────────────────────────┤
│ masks_level_N     (count_N bytes)        │
│ masks_level_N-1   (count_N-1 bytes)      │  Children masks
│ ...                                      │  (BFS order per level)
│ masks_level_1     (count_1 bytes)        │
│ (no level 0 — leaves have no children)   │
└──────────────────────────────────────────┘
```
