# Migrate Animated Voxel Volumes — Z-slab → Frame-as-Array-Layer

Status: design draft, pre-implementation
Owner: Evan
Last updated: 2026-05-02

## 1. The problem

Animated voxel volumes are currently packed as **Z-slabs inside a single 3D
image**: `image.depth = size.z * frameCount`, frame `f` lives at `[f*size.z,
(f+1)*size.z)`. This packing is what every animated-volume consumer (the
`InstancedVoxelTechnique`, `CombinedRenderer`'s foliage path, the
`GltfImportTechnique` voxel preview, the foliage compute generator) is built
around.

The packing has a hard ceiling: `size.z * frameCount ≤ maxImageDimension3D`.
Vulkan's spec floor for `maxImageDimension3D` is **2048**, and Apple Silicon
(Metal) enforces it exactly. So a `size.z = 32` bake is capped at 64 frames,
a `size.z = 64` bake at 32 frames. The M4 GLB-import bake hit this limit
immediately — `size.z = 20 * frameCount = 140` produces a 2800-deep image,
which Metal aborts on at validation:

```
vkCreateImage(): pCreateInfo->extent.depth (2800) exceeds allowable maximum
image extent depth 2048 for format VK_FORMAT_R8_UINT.
```

Today the engine works around this at the bake-job layer with a
`maxPackedDepth = 2048` budget guard (see `voxel_bake::FullBakeJob`). That
prevents the crash but limits the user to short or coarse bakes. This
document is the migration path to a packing scheme that lifts the limit.

## 2. The target packing

Switch from one big 3D image to a **2D image array where each frame is one
layer, with the frame's `(x, y, z)` data flattened into the layer's
`(x, y + z*size.y)` 2D extent**.

| Property                | Today (Z-slab)                     | Target (frame-as-layer)               |
| ----------------------- | ---------------------------------- | ------------------------------------- |
| `VkImageType`           | `VK_IMAGE_TYPE_3D`                 | `VK_IMAGE_TYPE_2D`                    |
| `VkImageViewType`       | `VK_IMAGE_VIEW_TYPE_3D`            | `VK_IMAGE_VIEW_TYPE_2D_ARRAY`         |
| GLSL sampler            | `usampler3D`                       | `usampler2DArray`                     |
| GLSL storage image      | `uimage3D`                         | `uimage2DArray`                       |
| Image extent            | `(size.x, size.y, size.z * N)`     | `(size.x, size.y * size.z, 1)` × `N` layers |
| Sample at frame `f`     | `texelFetch(s, ivec3(x, y, z + f*size.z), 0)` | `texelFetch(s, ivec3(x, y + z*size.y, f), 0)` |
| Capacity ceiling        | `size.z * N ≤ 2048` (Apple)        | `N ≤ maxImageArrayLayers (2048)` AND `size.y * size.z ≤ maxImageDimension2D (16384)` |

### Why frame-as-layer beats Z-as-layer

Vulkan's `maxImageArrayLayers` and `maxImageDimension3D` are both **2048 on
Apple Silicon**. So just doing "Z-slab → 2D array of Z-slices" (`arrayLayers
= size.z * frameCount`) gets you nothing on Apple. The win comes from
making the **frame** the array index instead of the Z-slice — `arrayLayers =
frameCount`, capped at 2048 (84 seconds at 24 fps; way more than any real
asset). Within a layer, Y and Z share the 2D height (capped at 16384 on
Apple — a `size.y = 128, size.z = 128` bake fits comfortably).

### Effective new ceiling

| Apple Silicon limit      | Resulting capacity                                    |
| ------------------------ | ----------------------------------------------------- |
| `frameCount ≤ 2048`      | At 24 fps → 85 s; at 60 fps → 34 s                    |
| `size.y * size.z ≤ 16384`| `size.y = size.z = 128`: room for a 128³ frame        |
| `size.x ≤ 16384`         | Effectively unbounded for voxel bakes                 |

Compared to today's 2048 cap on `size.z * frameCount`, this is roughly an
8–10× lift in practical capacity.

### Alternatives considered (and why not)

- **Multiple independent 3D images bound as a descriptor array** (`usampler3D
  volumes[MAX]`). Frame `f` samples `volumes[f]`. Conceptually clean, but
  requires a fixed-size descriptor array (or `runtimeDescriptorArray` +
  `descriptorBindingPartiallyBound` from VK_EXT_descriptor_indexing — Apple
  Silicon supports it via MoltenVK 1.2+, but extension juggling is more
  surface area than the 2D-array approach). Each frame also pays per-image
  allocation overhead (alignment padding, descriptor pool slots).
- **Buffer-backed sampling** (voxels in an SSBO, manual indexing in the
  shader). No image-extent limit (only `maxStorageBufferRange` ≈ 4 GB).
  Loses hardware texel fetch fast paths and clamping/border behavior; every
  read becomes manual bounds checking. Worth revisiting only if the 2D
  array's cap proves insufficient.
- **Tiled 3D images** (split a long bake into K chunks of ≤ 2048 depth, bind
  as descriptor array). Same descriptor-array complexity as the
  multiple-images approach plus arithmetic to find the chunk index per
  sample. Marginal compared to frame-as-layer.

## 3. What stays the same

Crucially, this migration is **engine-internal** — neither asset content nor
on-disk file formats change.

- **`.vxa` manifest**: untouched. Fields (`size`, `frameCount`, `fps`,
  `voxelSizeWorld`, `frames[]`) carry the same semantics.
- **Per-frame `.vox` files**: untouched. Each `.vox` is still one frame's
  voxel data at single-frame `size`.
- **`VoxFrame` (worker output)**: untouched. `indices` still holds
  `size.x * size.y * size.z` bytes per frame.
- **`VoxelVolumeAsset.data` (host-side blob)**: untouched. Frames are still
  packed sequentially: byte at position `f*S + z*size.x*size.y +
  y*size.x + x` (where `S = size.x*size.y*size.z`) belongs to frame `f`,
  voxel `(x, y, z)`. Only the **upload step** (`vkCmdCopyBufferToImage`)
  changes: now copies into N array layers instead of N consecutive Z-ranges.
- **Bitmask buffer** (used by `substrate.glsl` for shadow occupancy): an
  independent SSBO with its own per-frame indexing. Unaffected.
- **Substrate / shadow path**: no shader changes outside the volume sample
  expression itself.

## 4. Files affected

### C++ (5 files)

- **`lib/VWrap/include/Image.h`** + **`lib/VWrap/src/Image.cpp`**: add
  `arrayLayers` to `ImageCreateInfo` (default 1), forward to
  `VkImageCreateInfo::arrayLayers`. The current code hardcodes `arrayLayers =
  1` (Image.cpp:24).
- **`lib/VWrap/include/ImageView.h`** + **`lib/VWrap/src/ImageView.cpp`**:
  the auto-deriving overload (ImageView.cpp:6) currently picks
  `VK_IMAGE_VIEW_TYPE_3D` for 3D images. Add a path that picks
  `VK_IMAGE_VIEW_TYPE_2D_ARRAY` when `arrayLayers > 1`. The explicit
  overload already takes a `VkImageViewType` so callers can opt in
  unilaterally.
- **`src/rendering/RenderGraphTypes.h`**: add `uint32_t arrayLayers = 1` to
  `ImageDesc`. Default of 1 keeps every existing call site working.
- **`src/rendering/RenderGraph.cpp`**: forward `desc.arrayLayers` into
  `VWrap::ImageCreateInfo` at allocation time (RenderGraph.cpp:460–470).
  When `desc.arrayLayers > 1 && desc.imageType == VK_IMAGE_TYPE_2D`, the
  view auto-derive in `VWrap::ImageView::Create` should pick `2D_ARRAY`.
- **`src/rendering/AssetRegistry.cpp`**:
  - `DeclareVolume`: switch from `imageType = VK_IMAGE_TYPE_3D` /
    `depth = size.z * frameCount` to `imageType = VK_IMAGE_TYPE_2D` /
    `width = size.x` / `height = size.y * size.z` / `depth = 1` /
    `arrayLayers = frameCount`.
  - `UploadVolume`: change the `VkBufferImageCopy` region. The host data
    layout is unchanged — frame `f` is still bytes `[f*S, (f+1)*S)` — but
    the destination is now layer `f` of the 2D-array image. Easiest path:
    one copy region per frame (or one single region with `layerCount =
    frameCount` if the host buffer is laid out so that each
    `(width, height)` 2D slice is one frame's data, which it already is
    given the Y-then-Z packing within a layer).

### Shaders (8 files, mechanical edits)

Each consumer needs three changes:

1. **Sampler type**: `usampler3D` → `usampler2DArray` (and `uimage3D` →
   `uimage2DArray` for the compute generators).
2. **Texel address**: `ivec3(x, y, z + f*size.z)` →
   `ivec3(x, y + z*size.y, f)`.
3. **Bounds checks** (where present): `voxelCoord` is still 3D in the DDA's
   coordinate system; the address rewrite happens only at the texel-fetch
   call site. The `meta.size` / `meta.frameCount` interface to the DDA
   stays identical, so `instanced_voxel_dda.glsl` doesn't change.

Files:

- **`shaders/voxel_preview.frag`**: `volume_sampler` declaration +
  `sampleMaterial`.
- **`shaders/instanced_voxel.frag`**: `volume_sampler` declaration +
  `sampleMaterialAtFrame`.
- **`shaders/combined_foliage_trace.frag`**: same as instanced_voxel.frag.
- **`shaders/animated_geometry_trace.frag`**: same shape, but only
  single-frame today (`frameCount` always 1). Migrate to keep the API
  consistent — sampling becomes `texelFetch(s, ivec3(x, y + z*size.y, 0),
  0)`.
- **`shaders/instanced_voxel_generate.comp`**: `imageStore(volume, ivec3(x,
  y, zf), ...)` (where `zf = frame * sizeZ + z`) → split `zf` back into
  `(frame, zInFrame)` (it already does) and store as
  `imageStore(volume, ivec3(x, y + zInFrame * sizeY, frame), ...)`.
- **`shaders/animated_geometry_generate.comp`**: same shape, single-frame.
- **`shaders/shadow_foliage_write.comp`**: writes to a different image
  (`uimage3D shadow`), which is the *world* shadow brickmap, not an
  animated-asset volume. **Out of scope** for this migration — the shadow
  brickmap is its own unrelated 3D image. (Confirm by reading the binding;
  it likely doesn't pack frames at all.)
- **`shaders/include/instanced_voxel_dda.glsl`**: the comment block at the
  top documents the `volume_sampler` contract. Update the contract example
  to `usampler2DArray`. The DDA itself doesn't sample; it calls back into
  the includer-supplied `sampleMaterial`, which is where the address change
  lives. **No code changes**, only doc.
- **`shaders/include/substrate.glsl`**: indexes into the bitmask SSBO, not
  the volume image. Unaffected.

### Techniques (no API changes — only the shader recompile)

The C++ side of `InstancedVoxelTechnique`, `CombinedRenderer`,
`GltfImportTechnique`, `AnimatedGeometryRenderer` doesn't change. They
already read `meta.size` and `meta.frameCount` and pass them through; the
shader-side address arithmetic shifts but the C++-side binding setup is
identical. The `m_volume_sampler` (`NEAREST clamp`) stays correct for
2D-array sampling.

## 5. Migration phases

Land in dependency order so each phase leaves the tree green and demoable.

### Phase 1 — VWrap + RenderGraph plumbing (1 PR)

- Add `arrayLayers` to `VWrap::ImageCreateInfo` and `ImageDesc`.
- Forward into `vkCreateImage` and through the auto-derived `ImageView`
  type selection.
- No callers change behavior; `arrayLayers = 1` everywhere preserves the
  current path.
- **Demo**: existing test scenes render unchanged.

### Phase 2 — AssetRegistry switch (1 PR)

- `DeclareVolume` emits a 2D-array image when `frameCount > 1`, and a
  plain 2D image (or keep 3D for back-compat) when `frameCount == 1`.
  - **Decision**: also emit 2D-array for the single-frame case
    (`arrayLayers = 1`) so the binding type is always
    `VK_IMAGE_VIEW_TYPE_2D_ARRAY` and shaders only need one path. A 1-layer
    2D array is a valid Vulkan resource with no overhead.
- `UploadVolume` switches its `VkBufferImageCopy` region(s) to address
  the layered destination.
- **At this point shaders are still `usampler3D` and will fail validation**
  — the binding type mismatches the descriptor type. Bundle Phase 2 with
  Phase 3 in the same PR, or land Phase 2 behind a build flag.

### Phase 3 — Migrate every consumer shader (1 PR, bundled with Phase 2)

- All 7 sampling shaders + the 2 storage-image generators above.
- Each change is mechanical: swap sampler/image type, rewrite the texel
  address.
- Update the comment header in `instanced_voxel_dda.glsl`.
- Update `voxel_preview.frag`'s comment block that mentions Z-slab packing.
- **Demo**: scenes render identically; the GLB-import bake test that
  previously crashed at depth 2800 now runs to completion and previews
  correctly.

### Phase 4 — Lift the bake-side budget guard (1 PR)

- Change `FullBakeJob.maxPackedDepth = 2048` (the conservative interim cap)
  to `maxFrameCount = 2048` and `maxFrameDimYZ = 16384`. Same total of
  budget guards; new dimensions match the new ceiling.
- Update the panel's "Packed depth" hint to "Layers / YZ extent" with the
  new caps.
- Update `docs/animated-voxel-import.md` open-question §10.
- **Demo**: a 200-frame bake at 24 fps that was rejected pre-migration now
  bakes successfully.

### Phase 5 — Cleanup (1 PR, optional)

- Remove old "Z-slab" terminology from comments throughout the codebase.
- Search for `* size.z * frameCount` / `+ frameIdx * size.z` patterns and
  replace with the new `* arrayLayers` / `+ frameIdx` conventions in any
  comment / log message that still references the old layout.

## 6. Risks & gotchas

- **`maxImageArrayLayers` is sometimes < `maxImageDimension2D`**. On Apple
  Silicon both are 2048. On older Intel iGPU paths via MoltenVK, layers can
  be limited further. Add a runtime check against `caps.maxImageArrayLayers`
  in `AssetRegistry::DeclareVolume`; if `frameCount` exceeds the device
  cap, fail the asset declaration with a clear log line (the bake would
  then need to be regenerated with fewer frames).
- **2D-array sampling cost vs 3D sampling cost**: on Apple GPUs, 2D-array
  texel fetch and 3D texel fetch use the same underlying sampler hardware
  with comparable cost. No measurable perf delta expected. Verify with a
  micro-benchmark on a frame-heavy bake post-migration; if 2D-array fetch
  is meaningfully slower (e.g., due to cache layout), reconsider.
- **The `(y + z*size.y)` address arithmetic loses Z-locality**: today, two
  voxels adjacent in Z within one frame are also adjacent in the 3D image
  (within a Z-slab). After migration, they're `size.y` rows apart in the
  layer's 2D image. DDA marches that touch many Z-adjacent voxels in
  sequence may see worse texture-cache hit rates. Mitigation: swap the
  packing to `(y * size.z + z)` instead of `(y + z * size.y)` — same total
  size, but Z-adjacency becomes Y-stride-1 instead of Y-stride-`size.y`.
  Decide based on which axis the DDA marches most (typically Y for
  surface-near rays). Default: `(y + z * size.y)` for symmetry with the
  current `(z + f * size.z)` Z-major layout, then profile.
- **Existing baked `.vxa` files keep working**. The on-disk Z-slab byte
  layout is identical to what the new upload reads from. No data migration
  needed.
- **The host-side `VoxelVolumeAsset.data` blob is still
  `size.x * size.y * size.z * frameCount` bytes**. The shape of the upload
  changes; the buffer doesn't. Tests that examine `data.size()` against
  expected byte counts continue to work.
- **`VK_IMAGE_USAGE_STORAGE_BIT` for compute writers**: the
  generator compute shaders write to `uimage3D` today. Switching to
  `uimage2DArray` requires the Vulkan device to support storage on 2D-array
  images (`maxImageDimension2D` features apply, which they always do for
  the formats we use — `R8_UINT`).

## 7. Effort estimate

- Phase 1: 0.5 day (mechanical, well-isolated to VWrap).
- Phase 2 + 3 bundle: 1.5 days (the shader edits are mechanical but each
  needs to be tested against an existing scene to confirm no regression).
- Phase 4: 0.25 day (config + docs).
- Phase 5: 0.25 day (find/replace + comment hygiene).

**Total**: ~2.5 engineer-days. Most of the time is testing — every active
technique must be smoke-tested at least once after the shader switch.

## 8. Open questions

1. **Within-layer Y/Z packing order** (`y + z*size.y` vs `y*size.z + z`):
   pick based on DDA stride benchmark, or default to one and measure later?
2. **1-layer 2D arrays for static volumes**: do we keep static volumes on
   `VK_IMAGE_TYPE_2D` with `arrayLayers = 1`, or stay on
   `VK_IMAGE_TYPE_3D` for them and only switch animated volumes? Mixed
   would mean shaders need to know which they're sampling — easier to
   unify on 2D-array even for `frameCount == 1`.
3. **MoltenVK descriptor-indexing path for the future**: if 2048 frames
   ever stops being enough (long cinematic bakes), the next move is
   descriptor arrays of independent volumes. Keep the asset format and
   the in-memory `VoxelVolumeAsset` structure compatible with that
   future direction — no fields should bake in the "one image per asset"
   assumption.
4. **Animated geometry renderer migration**: it currently uses
   `CreateProceduralVoxelVolume` (single-frame). Do we keep it on the old
   `usampler3D` path forever, or migrate it to `usampler2DArray` for
   uniformity? Recommendation: migrate for uniformity — the cost is one
   shader edit, the benefit is one shader convention.

## 9. Out of scope

- Format change for the `.vxa` files. The on-disk format is decoupled from
  the GPU packing on purpose; if we ever want a better disk format
  (compression, per-frame palettes, sparse encoding), it should be a
  separate doc.
- Multi-image / descriptor-array packing. Documented above as a fallback
  if the 2048-layer cap is ever insufficient.
- Sparse residency for very large bakes. Apple Silicon doesn't support
  sparse texture arrays in MoltenVK as of 1.2; revisit if MoltenVK ever
  ships it.
- The bitmask SSBO that backs `substrate.glsl`. Independent data
  structure, has its own per-frame indexing in the shader, not affected
  by image-extent limits.
