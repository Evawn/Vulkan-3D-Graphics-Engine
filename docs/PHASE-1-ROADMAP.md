# Phase 1: Robust SVO Renderer — Implementation Roadmap

## Overview

Transform the current procedural-only, binary-occupancy SVO renderer into a chunked architecture with per-voxel materials, dynamic tree depth, and basic lighting. This establishes the rendering foundation for file import (.vox, .rsvo) and all subsequent phases.

### Current State
- 128³ RGBA8 3D image with binary occupancy (filled or empty)
- Hardcoded 3-level SVO rebuilt every frame
- Flat-white shading, no lighting
- Procedural shapes only, monolithic volume

### Target State
- Flat SSBO of 16³ voxel chunks with packed material data
- Dynamic N-level SVO whose leaf nodes point into the chunk buffer
- SVO rebuilds only when chunk topology changes
- Directional lighting, face normals, ambient occlusion
- Non-cubic volume support
- Ready to accept file-imported voxel data

---

## Step 1: Chunk Buffer + Voxel Format

**Goal:** Replace the monolithic RGBA8 3D image with a flat SSBO of 16³ chunks. Each voxel carries packed material data.

### Voxel Format (32 bits)

```
bit  31:      occupied (1 = solid, 0 = empty)
bits 29-30:   material type (2 bits: 0=solid, 1=transparent, 2=emissive, 3=reserved)
bits 24-28:   roughness (5 bits → 0..31 maps to 0.0..1.0)
bits 16-23:   B channel (8 bits)
bits  8-15:   G channel (8 bits)
bits  0-7:    R channel (8 bits)
```

A voxel value of `0` means empty. Any non-zero value with bit 31 set is occupied.

### Chunk Buffer Layout

- Each chunk = 16³ = 4096 voxels × 4 bytes = **16 KB**
- Chunks stored contiguously in a flat `STORAGE_BUFFER`
- Chunk index from 3D grid coordinates: `cx + cy * chunks_x + cz * chunks_x * chunks_y`
- Voxel offset within chunk: `lz * 256 + ly * 16 + lx`
- Global buffer offset: `chunk_index * 4096 + voxel_offset`

### Chunk Metadata Buffer

A separate SSBO with one `uint` per chunk slot in the padded SVO grid:
- `0` = chunk is empty (no occupied voxels)
- `1` = chunk is occupied (at least one non-zero voxel)

Written by the generate pass. Read by the SVO build pass.

### File Changes

**`SVORenderer.h` / `SVORenderer.cpp`:**
- Remove `m_volume` (ImageHandle for 3D image) and its sampler
- Add `m_chunk_buffer` (BufferHandle) — voxel data SSBO
- Add `m_chunk_meta_buffer` (BufferHandle) — per-chunk occupancy
- Add volume dimension members: `m_volume_size = 128` (keep cubic for now)
- Compute: `chunks_per_axis = volume_size / 16`, `total_chunks = chunks³`
- Buffer sizes: `total_chunks * 16384` (chunk buffer), `total_chunks * 4` (metadata)
- Update descriptor sets:
  - Generate pass: bind chunk_buffer (write) + chunk_meta (write)
  - Build pass: bind chunk_meta (read) + svo_buffer (write)
  - Graphics pass: bind chunk_buffer (read) + svo_buffer (read)

**`svo_generate.comp` (rewrite):**
```glsl
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(std430, set = 0, binding = 0) writeonly buffer ChunkBuffer {
    uint voxels[];
} chunk_buf;

layout(std430, set = 0, binding = 1) buffer ChunkMeta {
    uint occupied[];
} chunk_meta;

layout(push_constant) uniform PC {
    int volume_size;
    int leaf_size;
    int chunks_per_axis;
    int shape;
    float time;
} pc;

// Pack/unpack helpers
uint packVoxel(vec3 color, float roughness, uint matType) { ... }

void main() {
    ivec3 coord = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(coord, ivec3(pc.volume_size)))) return;

    // Determine chunk and local position
    ivec3 chunk_id = coord / pc.leaf_size;
    ivec3 local = coord % pc.leaf_size;
    int chunk_idx = chunk_id.x + chunk_id.y * pc.chunks_per_axis
                  + chunk_id.z * pc.chunks_per_axis * pc.chunks_per_axis;
    int voxel_offset = local.z * 256 + local.y * 16 + local.x;

    // Generate shape (same SDF logic as before)
    vec3 p = (vec3(coord) + 0.5) / float(pc.volume_size) - 0.5;
    bool filled = /* ... shape logic ... */;

    // Write voxel data
    uint packed = filled ? packVoxel(vec3(1.0), 0.5, 0u) : 0u;
    chunk_buf.voxels[chunk_idx * 4096 + voxel_offset] = packed;

    // Update chunk occupancy (atomicOr so any thread can set it)
    if (filled) {
        atomicOr(chunk_meta.occupied[chunk_idx], 1u);
    }
}
```

A small clear pass (or beginning-of-frame memset) zeros the chunk_meta buffer before generate runs.

**`svo_trace.frag`:**
- Replace `sampler3D brick_sampler` with `readonly buffer ChunkBuffer { uint voxels[]; }`
- Leaf DDA reads: `uint v = chunk_buf.voxels[chunk_idx * 4096 + lz*256 + ly*16 + lx]`
- Unpack color from voxel: `vec3 color = vec3(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF) / 255.0`
- Use unpacked color for output instead of flat white

### Testable Result
Procedural shapes render in flat white (same as before) but data flows through the chunk buffer SSBO instead of a 3D image. The pipeline is: generate→chunks, build SVO from chunk_meta, trace reads from chunks.

---

## Step 2: Dynamic SVO Level Count

**Goal:** Compute the number of octree levels from volume dimensions instead of hardcoding 3.

### Logic

```
chunks_per_axis = ceil(volume_size / leaf_size)    // e.g. 128/16 = 8
svo_grid_size   = nextPow2(chunks_per_axis)        // 8
num_levels      = log2(svo_grid_size)              // 3 (internal levels)
```

The SVO has `num_levels` internal levels. At the deepest level, each node covers a 2³ group of chunks.

### File Changes

**`SVORenderer.cpp`:**
- Compute `m_num_levels` and `m_svo_grid_size` from volume dimensions
- Pass both via push constants to the build shader
- Size the SVO buffer with headroom: `(svo_grid_size³ + svo_grid_size³) * 4` bytes (generous upper bound)

**`svo_build.comp` (rewrite):**
- Accept `num_levels`, `svo_grid_size`, `chunks_per_axis` via push constants
- Read from `chunk_meta` buffer instead of sampling a 3D image
- Build N levels bottom-up using a loop instead of hardcoded level construction:

```glsl
// Pseudocode for N-level bottom-up build
// groups_per_axis[0] = chunks_per_axis / 2  (leaf-parent level)
// groups_per_axis[k] = groups_per_axis[k-1] / 2

// 1. Build leaf-parent masks from chunk_meta
// 2. For each higher level, build masks from child level masks
// 3. Root = final single node
// 4. Write nodes in root-first order with correct child pointers
```

- Chunks at grid positions outside the actual volume (padding) are always empty

**`svo_trace.frag`:**
- Already reads `svo_num_levels` from buffer header — no changes needed

### Testable Result
Change volume to e.g. 256³ (16 chunks/axis → 4 levels) and verify correct rendering. Change to 64³ (4 chunks/axis → 2 levels) and verify. The hardcoded `3` is gone.

---

## Step 3: SVO Leaf Nodes → Chunk Buffer Pointers

**Goal:** Leaf-parent nodes carry indices into the chunk buffer so the renderer can locate voxel data without a global 3D image.

### SVO Buffer Layout

```
Header (4 uints):
  [0] num_levels
  [1] volume_size
  [2] leaf_size
  [3] node_count

Node array (node_count uints):
  Internal nodes:  mask (8 bits) | first_child_index (24 bits)
  Leaf-parent nodes: mask (8 bits) | first_chunk_table_entry (24 bits)

Chunk index table (appended after nodes):
  One uint per occupied leaf slot → actual chunk buffer index
```

When the tracer reaches a leaf-parent node at the deepest SVO level:
1. Extract `first_chunk_table_entry` from bits 8-31
2. Determine which octant was hit
3. `chunk_table_offset = first_chunk_table_entry + bitCount(mask & ((1 << octant) - 1))`
4. `chunk_buffer_index = svo_data[chunk_table_offset]`
5. Read voxels from `chunk_buf.voxels[chunk_buffer_index * 4096 + local_offset]`

### File Changes

**`svo_build.comp`:**
- After building the node tree, iterate leaf-parent nodes
- For each occupied child of a leaf-parent, compute the chunk's flat buffer index from its 3D grid position
- Write these indices into the chunk index table region
- Store the table offset in the leaf-parent node

**`svo_trace.frag`:**
- Modify `descendTree` to return the chunk buffer index when it reaches a leaf:
```glsl
bool descendTree(vec3 p, out vec3 leaf_min, out float leaf_sz, out int chunk_idx) {
    // ... existing traversal ...
    if (i == levels - 1) {
        // At leaf-parent: look up chunk index
        uint table_base = data >> 8;
        uint child_offset = bitCount(mask & ((1u << octant) - 1u));
        chunk_idx = int(svo_nodes[table_base + child_offset]);
        return true;
    }
}
```
- Leaf DDA uses `chunk_idx` to index into the chunk buffer

### Testable Result
Visually identical rendering, but the data path is now fully indirected: SVO traverse → leaf node → chunk index table → chunk buffer → voxel data.

---

## Step 4: Conditional SVO Rebuild

**Goal:** Only rebuild the SVO when chunk topology changes.

### Approach

- Track a `m_svo_dirty` flag on the CPU side
- Set dirty when:
  - Shape selection changes (`m_shape != m_prev_shape`)
  - First frame (no SVO built yet)
  - Volume dimensions change
- For animated shapes (gyroid, sine blob, menger sponge): topology may change every frame, so mark dirty every frame for those shapes
- For static shapes: topology is fixed after first build

### Pipeline

```
Every frame:
  1. [Chunk Meta Clear] (compute) — zero the chunk_meta buffer
  2. [Voxel Generate]   (compute) — write voxels + atomicOr chunk_meta
  3. [SVO Build]        (compute) — CONDITIONAL: only if m_svo_dirty
  4. [SVO Trace]        (graphics) — always runs
```

### File Changes

**`SVORenderer.h`:**
- Add `bool m_svo_dirty = true`
- Add `int m_prev_shape = -1`

**`SVORenderer.cpp`:**
- In the SVO Build pass record function, check dirty flag:
```cpp
graph.AddComputePass("SVO Build")
    .Read(m_chunk_meta_buffer)
    .Write(m_svo_buffer)
    .SetRecord([this](PassContext& ctx) {
        if (!m_svo_dirty) return;  // Skip dispatch
        // ... bind pipeline, push constants, dispatch ...
        m_svo_dirty = false;
        m_prev_shape = m_shape;
    });
```
- In the generate pass, detect topology changes:
```cpp
// Before recording:
bool is_animated = (m_shape >= 6);  // gyroid, sine blob, menger sponge
if (m_shape != m_prev_shape || is_animated) {
    m_svo_dirty = true;
}
```

- Add a small compute pass (or use `vkCmdFillBuffer`) to clear `chunk_meta` before generate

### Testable Result
- Select a static shape (sphere) → SVO Build runs once, then skips on subsequent frames
- Select an animated shape (gyroid) → SVO Build runs every frame
- GPU profiler shows 0ms for SVO Build on static shapes

---

## Step 5: Basic Lighting

**Goal:** Add directional lighting and simple ambient occlusion.

### Lighting Model

1. **Face normal from DDA:** The `step_dir` bvec3 already identifies which face was hit. Convert to a world-space normal:
   ```glsl
   vec3 normal = vec3(0.0);
   if (step_dir.x) normal.x = advance.x ? -1.0 : 1.0;
   if (step_dir.y) normal.y = advance.y ? -1.0 : 1.0;
   if (step_dir.z) normal.z = advance.z ? -1.0 : 1.0;
   ```

2. **Directional light:** `float NdotL = max(dot(normal, normalize(lightDir)), 0.0)`

3. **Ambient term:** Constant factor (e.g., 0.15) so shadowed faces aren't black

4. **Simple neighbor AO:** Sample the 6 face-adjacent voxels. Each occupied neighbor darkens the voxel slightly:
   ```glsl
   float ao = 1.0;
   for (int i = 0; i < 6; i++) {
       ivec3 neighbor = global_coord + face_offsets[i];
       if (readVoxelOccupied(neighbor)) ao -= 0.08;
   }
   ```

### File Changes

**`svo_trace.frag`:**
- Add `lightDir` (vec3) and `ambientStrength` (float) to push constants
- Compute face normal from DDA step direction
- Apply `color = voxelColor * (ambient + (1.0 - ambient) * NdotL) * ao`

**`SVORenderer.cpp`:**
- Extend `SVOTracePC` with `lightDir` and `ambientStrength`
- Add UI parameters for light direction (azimuth/elevation or direct vec3) and ambient strength

### Testable Result
Shapes show visible 3D depth: faces toward the light are bright, faces away are shaded. Corners/edges with many neighbors are subtly darkened.

---

## Step 6: Procedural Color + Material Assignment

**Goal:** Generate meaningful per-voxel color and material data for each procedural shape.

### Color Schemes per Shape

| Shape | Color Strategy |
|-------|---------------|
| Sphere | Latitude gradient: red (top) → blue (bottom) |
| Torus | Hue wraps around major radius angle |
| Box Frame | Edges colored by axis (R=X, G=Y, B=Z) |
| Cylinder | Radial gradient from center |
| Cone | Height-based warm gradient |
| Octahedron | Face-based coloring by dominant axis |
| Gyroid | Curvature-based iridescent palette |
| Sine Blob | Pulsing hue shift with time |
| Menger Sponge | Iteration-depth coloring |

### File Changes

**`svo_generate.comp`:**
- Each shape branch computes a `vec3 color` and `float roughness` alongside the occupancy test
- Pack with `packVoxel(color, roughness, matType)` and write to chunk buffer
- Surface voxels get lower roughness; interior gets higher

### Testable Result
Each procedural shape renders with a unique, visually distinct color scheme, properly lit by the directional light from Step 5.

---

## Step 7: Non-Cubic Volume Support

**Goal:** Handle volumes where X, Y, Z dimensions differ.

### Approach

- Volume defined by `(volume_x, volume_y, volume_z)` — each can differ
- `chunks_x = ceil(volume_x / 16)`, `chunks_y = ceil(volume_y / 16)`, `chunks_z = ceil(volume_z / 16)`
- `svo_grid = nextPow2(max(chunks_x, chunks_y, chunks_z))`
- The SVO grid is always a cube of side `svo_grid`, but chunks outside `(chunks_x, chunks_y, chunks_z)` are empty
- The chunk buffer only allocates `chunks_x * chunks_y * chunks_z` chunks (no wasted memory for padding)

### Coordinate Mapping

- World-space volume AABB scales per-axis: longest axis maps to [-1, 1], shorter axes are proportionally smaller
- `worldToVoxel` and `voxelToWorld` use per-axis scaling

### File Changes

**`SVORenderer.h` / `SVORenderer.cpp`:**
- Replace `m_volume_size` with `m_volume_x/y/z`
- Compute `m_chunks_x/y/z` and `m_svo_grid_size` accordingly
- Pass all dimensions via push constants

**`svo_generate.comp`:**
- Accept per-axis volume dimensions
- Bounds check per-axis: `if (coord.x >= volume_x || ...) return`
- Normalize shape coordinates per-axis for non-distorted shapes

**`svo_build.comp`:**
- Map chunk 3D coordinates to flat index using `chunks_x`, `chunks_y`
- Chunks at grid positions `>= chunks_x/y/z` are always empty

**`svo_trace.frag`:**
- Pass `volume_x/y/z` for correct world↔voxel coordinate conversion
- Volume AABB in world space adjusts for aspect ratio

### Testable Result
Set volume to 256×64×128 and verify shapes render correctly (stretched volume, correct proportions, no artifacts at boundaries).

---

## Dependency Graph

```
Step 1: Chunk Buffer + Voxel Format
  ├──→ Step 2: Dynamic SVO Levels
  │      ├──→ Step 3: Leaf→Chunk Pointers
  │      │      └──→ Step 4: Conditional Rebuild
  │      └──→ Step 7: Non-Cubic Volumes
  └──→ Step 5: Lighting
         └──→ Step 6: Procedural Colors
```

**Critical path:** 1 → 2 → 3 → 4

Steps 5-6 (lighting/color) can begin after Step 1 and proceed in parallel with Steps 2-4.
Step 7 (non-cubic) can begin after Step 2.

---

## Post-Completion State

After all 7 steps, the SVO renderer will:
- Store voxel data in a structured chunk buffer with 32-bit packed materials
- Build an N-level SVO whose depth adapts to volume dimensions
- Only rebuild the SVO when chunk occupancy changes
- Render voxels with per-voxel color, directional lighting, and basic AO
- Handle non-cubic volumes correctly
- Be architecturally ready for .vox/.rsvo file import (populate the chunk buffer from file data instead of procedural generation)
