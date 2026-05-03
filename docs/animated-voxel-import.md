# Animated Voxel Import — Implementation Plan

Status: design draft, pre-implementation
Owner: Evan
Last updated: 2026-05-02

## 1. Goal & scope

Build a first-class engine utility that imports a `.glb` asset (mesh + skin +
animation), previews it live in the viewport, lets the user interactively
configure voxelization parameters (with live debounced re-bake), and bakes the
animation into the engine's existing animated-voxel asset format so it can be
loaded by the existing `Combined` / `InstancedVoxel` rendering paths.

V1 scope:
- `.glb` (binary glTF 2.0) only
- Skinned mesh + bone animation only (no morph targets, no rigid node animation)
- Surface voxelization (no solid-fill)
- Quantization into the engine's shared default `.vox` palette
- One animation per asset bake (user picks which one if the GLB has several)
- Per-frame `.vox` files + `.vxa` JSON manifest as the on-disk format

Out of scope for v1: morph targets, rigid node animation, lighting bake-in,
custom palettes per bake, multi-clip bakes, mesh decimation pre-pass, GPU
voxelization.

## 2. Confirmed design decisions

From the design discussion:

| Decision               | Choice                                                          |
| ---------------------- | --------------------------------------------------------------- |
| Voxelization method    | Surface voxelization (CPU)                                      |
| Color target           | Engine's shared default `.vox` palette (256 RGBA)               |
| Color sourcing         | Pluggable: per-material color → texture-sampled (extensible)    |
| Animation type         | Skinned mesh only                                               |
| Voxel size UX          | Live preview with debounced re-bake                             |
| Mesh / voxel preview   | One RenderTechnique, internal `PreviewMode` toggle              |
| Workflow framing       | New "Import" workspace in the editor (not just a panel)         |
| Output format          | `.vxa` JSON manifest + per-frame `.vox` files                   |

## 3. Architectural overview

### New modules
```
src/import/                           ← new top-level
  GltfLoader.{h,cpp}                  ← .glb → MeshIR (cgltf-backed)
  MeshIR.h                            ← skinned-mesh + animation IR
  AnimationEvaluator.{h,cpp}          ← clip + time → joint matrices
  Voxelizer.{h,cpp}                   ← posed mesh → VoxFrame (voxels + colors)
  PaletteQuantizer.{h,cpp}            ← RGB → palette-index lookup (cached)
  AnimationBaker.{h,cpp}              ← worker-thread orchestration
  VoxAnimFormat.{h,cpp}               ← .vxa manifest + per-frame .vox writer/reader

src/rendering/voxel/
  DefaultVoxPalette.{h,cpp}           ← hardcoded MagicaVoxel default 256 RGBA

src/rendering/
  AssetRegistry.{h,cpp}               ← extend: SkinnedMesh, AnimationClip
  Scene.{h,cpp}                       ← extend: Component::SkinnedMesh variant
  SceneExtractor.{h,cpp}              ← extend: emit RenderItem::SkinnedMesh
  RenderItem.{h,cpp}                  ← extend: SkinnedMesh item

src/rendering-techniques/gltf-import/
  GltfImportTechnique.{h,cpp}         ← combined mesh+voxel viewer
src/editor/panels/
  BakerPanel.{h,cpp}                  ← timeline + bake controls

src/editor/
  Workspace.h                         ← Workspace enum + layout switcher
  Editor.{h,cpp}                      ← workspace-aware panel visibility

src/utils/
  JobSystem.{h,cpp}                   ← single-worker async job queue (v1)

dep/
  cgltf.h                             ← single-header glTF parser
```

### Data-flow at a glance
```
[BakerPanel: file picker]
        │
        ▼
GltfLoader (cgltf) ──► MeshIR (skinned meshes + clips + skeleton)
        │
        ▼
AssetRegistry.RegisterSkinnedMesh()  ──► AssetID (SkinnedMesh)
AssetRegistry.RegisterAnimationClip() ──► AssetID (AnimationClip)
        │
        ▼
Scene root: new SceneNode with Component::SkinnedMesh{ meshId, clipId, time, speed }
        │
        ▼
GltfImportTechnique active:
  - PreviewMode::Mesh    → SkinnedMesh pipeline (GPU skinning vert shader)
  - PreviewMode::Voxels  → reuses brickmap/instanced-voxel trace path on the
                           proceduralAnimatedVolume bound to a sibling node
  - PreviewMode::Overlay → both at once with alpha-ghost mesh on top of voxels

        │ (debounce timer fires on voxel-size change)
        ▼
AnimationBaker (worker thread):
  preview job:
    pose = AnimationEvaluator.Pose(clip, currentTime)
    skinnedVerts = CPU-skin(meshIR, pose)
    voxFrame = Voxelizer.Bake(skinnedVerts, voxelSize, colorSource)
    palette-quantize colors → indices into shared palette
    upload to AssetRegistry's preview animated-volume (frameCount=1)
  full bake job:
    for each frame in [t0..t1] step (1/fps):
      same as above
    pack frames sequentially → one byte buffer (uploaded as 2D-array layers)
    AssetRegistry.ReplacePreviewVolumeWithBake(...)

        │ (Save button)
        ▼
VoxAnimFormat.Write(directory, vxa)
  → writes <name>.vxa + <name>_000.vox … <name>_NNN.vox

        │ (Promote to scene button)
        ▼
Switch active workspace → Scene
Scene gets a permanent VoxelVolume node referencing the baked AssetID.
```

## 4. The Import Workspace (new editor concept)

A `Workspace` is a higher-level UI mode than a RenderTechnique. Each workspace
declares:
- Which RenderTechnique is active (or which subset of techniques the user can
  switch among)
- Which panels are visible by default
- Which top-level menus / shortcuts are available

V1 has two workspaces:
- **Scene** — current behaviour: full technique picker, all panels available,
  user authors the world
- **Import & Bake** — viewport drives `GltfImportTechnique` only; BakerPanel +
  Inspector + Output panels visible; HierarchyPanel hidden (irrelevant);
  RenderGraphPanel optional (debug)

Switching workspaces is reversible and non-destructive: the Scene workspace's
world state is preserved when entering Import. When the user "promotes to
scene" from Import, the engine appends the baked asset to the Scene world and
switches back automatically.

### Implementation shape
```cpp
// src/editor/Workspace.h
enum class Workspace : uint8_t {
    Scene,
    ImportBake,
};

struct WorkspaceConfig {
    const char* displayName;
    bool        showsHierarchy;
    bool        showsBaker;
    bool        showsInspector;
    bool        lockTechnique;          // if true, only one technique allowed
    const char* lockedTechniqueName;    // populated when lockTechnique == true
};
```

`Editor` gains a `Workspace m_workspace` field plus `void SetWorkspace(...)`.
`RenderingSystem` exposes a `RequestSwitchTechniqueByName(...)` so the
workspace switch can pick the right technique without knowing indices.

The menu bar grows a workspace switcher (`Edit → Workspace → Scene / Import`,
or a tab strip at the top of the main window).

## 5. Module designs

### 5.1 GLB import — `src/import/`

**Dependency**: `cgltf` (single-header C, MIT). Drop `cgltf.h` in `dep/` next
to `stb_image.h` and friends. Reasons:
- Matches your existing single-header convention (stb, vma, tinyobj)
- No JSON dependency added
- Successfully parses glTF 2.0 including skin + animation
- ~2k LOC, no allocator surprises

**`MeshIR`** — intermediate representation, distinct from `MeshAsset`:
```cpp
struct SkinnedVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::u16vec4 joints;
    glm::vec4 weights;
};

struct MeshIRPrimitive {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t>      indices;
    int materialIndex = -1;
};

struct MeshIRMaterial {
    std::string name;
    glm::vec4   baseColorFactor = glm::vec4(1.0f);
    int         baseColorTextureIndex = -1;   // index into MeshIR.textures
    bool        doubleSided = false;
    enum class AlphaMode : uint8_t { Opaque, Mask, Blend } alphaMode = AlphaMode::Opaque;
};

struct MeshIRTexture {
    std::vector<uint8_t> rgba8;       // decoded
    uint32_t width = 0, height = 0;
};

struct MeshIRSkin {
    std::vector<int>       jointNodeIndices;     // indexes into nodes
    std::vector<glm::mat4> inverseBindMatrices;
};

struct MeshIRNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    glm::vec3 translation = glm::vec3(0.0f);
    glm::quat rotation    = glm::quat(1, 0, 0, 0);
    glm::vec3 scale       = glm::vec3(1.0f);
    int meshIndex = -1;
    int skinIndex = -1;
};

struct AnimationChannel {
    int   targetNode = -1;
    enum class Path : uint8_t { Translation, Rotation, Scale } path;
    std::vector<float>     times;        // sorted
    std::vector<glm::vec4> values;       // packed: vec3 padded to vec4 for T/S, full quat for R
    enum class Interp : uint8_t { Linear, Step, Cubicspline } interp;
};

struct AnimationClipIR {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

struct MeshIR {
    std::vector<MeshIRNode>      nodes;
    std::vector<int>             rootNodes;
    std::vector<MeshIRPrimitive> primitives;     // flattened across all glTF meshes
    std::vector<MeshIRMaterial>  materials;
    std::vector<MeshIRTexture>   textures;
    std::vector<MeshIRSkin>      skins;
    std::vector<AnimationClipIR> animations;
};
```

**`GltfLoader::Load(path) → std::optional<MeshIR>`** — wraps cgltf, decodes
embedded PNGs with stb_image (already in `dep/`), validates that primitives
have JOINTS/WEIGHTS (skinned only for v1), produces flat MeshIR.

**`AnimationEvaluator::Pose(clip, time, outNodeLocals[])`** — for each channel,
binary-search keyframes bracketing `time`, lerp/slerp, write into the node's
local TRS. Then a separate `EvaluateGlobals(meshIR, outGlobalMatrices[])`
walks the node tree producing world matrices, and `ComputeJointMatrices(skin,
globals, meshNodeWorld, outJointMatrices[])` produces the array uploaded to
the GPU. Standard glTF spec arithmetic.

### 5.2 New asset types in `AssetRegistry`

Two additions, both follow the existing `MeshAsset` / `VoxelVolumeAsset`
shape:

```cpp
struct AssetID {
    enum class Type : uint8_t {
        Invalid, Mesh, VoxelVolume,
        SkinnedMesh,        // new
        AnimationClip,      // new
    };
    // ... unchanged
};

struct SkinnedMeshAsset {
    std::string name, sourcePath;

    // Per-primitive geometry; one draw per primitive (one material each).
    struct Primitive {
        std::vector<SkinnedVertex> vertices;
        std::vector<uint32_t>      indices;
        glm::vec4 baseColorFactor = glm::vec4(1.0f);
        // Texture handle (lives in graph) — populated by registry.
        ImageHandle baseColorTexture;
        BufferHandle vertexBuffer;
        BufferHandle indexBuffer;
    };
    std::vector<Primitive> primitives;

    // Skeleton snapshot — local TRS of every node + parent links + skin info.
    // Held as plain CPU data; AnimationEvaluator pokes into a working copy
    // each frame.
    std::vector<MeshIRNode> nodes;
    int                     skinNodeIndex = -1;
    std::vector<int>        jointNodeIndices;
    std::vector<glm::mat4>  inverseBindMatrices;

    glm::vec3 aabbMin = glm::vec3(0.0f);
    glm::vec3 aabbMax = glm::vec3(0.0f);
    bool needsUpload = true;
};

struct AnimationClipAsset {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
    // No GPU resources — pure CPU data, evaluated each frame.
};
```

A new `Component::SkinnedMesh` variant carries:
```cpp
ComponentType::SkinnedMesh,
asset      = skinnedMeshId,
clipAsset  = animationClipId,
currentTime,
playbackSpeed,
paused,
```

`SceneExtractor` evaluates the clip every frame, computes joint matrices,
and emits a `RenderItem::SkinnedMesh { skinnedMeshId, jointMatrixBuffer,
worldTransform }`. The joint matrix buffer is a transient (per-frame) UBO
allocated through the render graph.

### 5.3 Voxelizer — `src/import/Voxelizer.{h,cpp}`

```cpp
struct VoxColorSource {
    enum class Mode : uint8_t {
        MaterialBaseColor,    // single color per material
        TextureSampled,       // sample baseColorTexture at the voxel's UV
    };
    Mode mode = Mode::TextureSampled;
};

struct VoxFrame {
    glm::uvec3 size = glm::uvec3(0);
    std::vector<uint8_t> indices;     // size.x * size.y * size.z, palette indices, 0 = empty
};

struct VoxelizeInput {
    // Posed primitives — vertices already in world space (skinning applied).
    struct Primitive {
        const SkinnedVertex* vertices;
        size_t               vertexCount;
        const uint32_t*      indices;
        size_t               indexCount;
        const MeshIRMaterial* material;
        const MeshIRTexture*  baseColorTexture;   // nullable
    };
    const Primitive* primitives;
    size_t           primitiveCount;

    glm::vec3 worldOriginMin;     // pre-computed AABB (covers the entire animation, not just this frame)
    glm::vec3 worldOriginMax;
    float     voxelSizeWorld;     // edge length of one voxel in world units
    VoxColorSource colorSource;
};

VoxFrame Voxelize(const VoxelizeInput& in,
                  const PaletteQuantizer& quantizer);
```

**Algorithm (surface voxelization, per-triangle, CPU):**

1. Compute the grid: `size = ceil((max - min) / voxelSize)`. The same min/max
   is used for every frame of the animation so the volume size is constant
   across frames (required by the layered upload — every layer must share
   the same 2D extent).
2. For each triangle:
   - Compute triangle AABB → grid cell range
   - For each cell in range:
     - Test if the voxel (an AABB of side `voxelSize` centered on the cell
       center) intersects the triangle (separating axis test, ~13 axes)
     - If intersect: compute the closest point on the triangle to the voxel
       center, compute its barycentric coords, interpolate vertex UV (or use
       barycentric for vertex color), sample the color source, quantize to
       palette index, write to the cell
     - If the cell is already filled: keep whichever color is closer to the
       triangle (track distance per cell), or first-write-wins (simpler — v1)

**Why surface voxelization is simple here**: we don't need to flood-fill or
worry about non-watertight meshes. Each triangle paints the cells it touches.
For alpha-cut leaves (single triangles), each leaf becomes ~1 voxel — that
matches your existing foliage aesthetic.

**Triangle-AABB intersection**: the standard Akenine-Möller "Fast 3D Triangle-
Box Overlap" test. ~50 lines of code, no dependencies.

**Coordinate stability across frames**: the voxel grid origin/size MUST be
constant for every frame so all 2D-array layers share one extent. The baker
computes a *bake-wide AABB* by sampling joint matrices over the full clip
duration once, before per-frame voxelization starts.

### 5.4 PaletteQuantizer — `src/import/PaletteQuantizer.{h,cpp}`

```cpp
class PaletteQuantizer {
public:
    explicit PaletteQuantizer(const std::array<uint8_t, 256 * 4>& palette);
    // RGB in [0,255]. Returns palette index in [1, 255]. Index 0 reserved for empty.
    uint8_t Quantize(uint8_t r, uint8_t g, uint8_t b) const;
private:
    // Cache: 16x16x16 LUT of palette indices keyed on quantized RGB.
    // Each lookup: ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4).
    std::array<uint8_t, 4096> m_lut{};
    std::array<uint8_t, 256 * 4> m_palette{};
    void BuildLut();
};
```

The LUT is built once at construction (4096 nearest-neighbor searches across
255 palette entries, ~ms). Per-voxel quantization is then O(1).

For v1, "nearest" = squared Euclidean distance in RGB. Future improvements
(perceptual distance via OkLab, dithering across voxels) can replace the
internals without touching callers.

### 5.5 AnimationBaker — `src/import/AnimationBaker.{h,cpp}`

A worker thread + job queue. Two job kinds:

```cpp
struct PreviewBakeJob {
    AssetID  meshId;
    AssetID  clipId;
    float    time;             // single timestamp
    float    voxelSizeWorld;
    VoxColorSource colorSource;
    std::atomic<bool> cancelled;
};

struct FullBakeJob {
    AssetID  meshId;
    AssetID  clipId;
    float    startTime, endTime;
    float    fps;
    float    voxelSizeWorld;
    VoxColorSource colorSource;
    std::atomic<bool> cancelled;
    std::atomic<int>  framesDone;     // for progress bar
    int               framesTotal = 0;
};

class AnimationBaker {
public:
    void Init(AssetRegistry* assets);
    void Shutdown();

    // Submits / replaces. If a preview job is already in-flight, it's cancelled.
    void SubmitPreview(PreviewBakeJob job);
    void SubmitFullBake(FullBakeJob job);

    // Polled by BakerPanel each frame to update progress UI.
    BakeStatus PollStatus() const;

    // Set on completion — main thread polls and uploads to AssetRegistry.
    std::optional<VoxFrame> TakeCompletedPreview();
    std::optional<std::vector<VoxFrame>> TakeCompletedFullBake();
};
```

**Threading model (v1)**: single worker thread, single in-flight job per kind.
New preview job cancels the current preview job (sets the atomic, the job
checks it between triangles or frames). Full bake jobs queue behind the
preview slot but cannot be cancelled by a new preview.

This is intentionally simple. If voxelization becomes the bottleneck later,
parallelize per-frame within a full bake (frames are independent) before
parallelizing within a frame (which is harder due to grid race conditions).

**Result handoff**: the worker fills `VoxFrame`s in shared memory; the main
thread polls `TakeCompleted*()` each frame, and on success calls into the
AssetRegistry to upload the new bytes. Upload runs in the next graph cycle —
no GPU work happens off-thread.

### 5.6 .vxa output format — `src/import/VoxAnimFormat.{h,cpp}`

```json
{
  "version": 1,
  "name": "AnimatedOak",
  "frameCount": 132,
  "fps": 24,
  "voxelSizeWorld": 0.0254,
  "size": [64, 96, 64],
  "originWorld": [-1.6, 0.0, -1.6],
  "frames": [
    "AnimatedOak_000.vox",
    "AnimatedOak_001.vox",
    "..."
  ]
}
```

`Write(directory, name, vxa)`:
- For each frame, write a `.vox` file (re-use whatever `.vox` writer you have
  / write a tiny one alongside the existing `VoxLoader`)
- Write the manifest as `<name>.vxa`

`Load(manifestPath) → AnimatedVoxAsset`:
- Parse manifest
- Iterate `frames[]`, load each `.vox`, validate sizes match `size`
- Concatenate volumes into one byte buffer (one frame per array layer; the
  byte order matches what `vkCmdCopyBufferToImage` expects for a layered
  copy with imageExtent = (size.x, size.y * size.z))
- Return ready-to-pass-to `AssetRegistry::RegisterAnimatedVoxelAsset` shape

### 5.7 GltfImportTechnique — combined mesh + voxel viewer

```cpp
class GltfImportTechnique : public RenderTechnique {
public:
    enum class PreviewMode : uint8_t { Mesh, Voxels, Overlay };

    // ... standard RenderTechnique methods

    // Workspace-facing API (invoked by BakerPanel through a shared facade):
    void   LoadGlb(const std::string& path);
    void   SelectAnimation(int clipIndex);
    void   SetVoxelSize(float worldUnits);   // triggers debounced re-bake
    void   SetPreviewMode(PreviewMode mode);
    void   SetTime(float t);
    void   SetPaused(bool paused);
    void   SetPlaybackSpeed(float s);
    void   StartFullBake(float startTime, float endTime, float fps);
    void   SaveBake(const std::string& directory, const std::string& name);
    void   PromoteBakeToScene();             // hands current bake to Scene workspace

    // Polled by BakerPanel for UI rendering:
    const ImportSession& GetSession() const;
};
```

**Internal pipelines**:
- **Skinned mesh pipeline**: vertex shader reads SkinnedVertex attributes,
  reads joint matrices from a per-frame UBO, computes
  `Σ weight[i] * jointMatrix[joint[i]] * position`, fragment shader does
  base color × baseColorTexture sample. Standard skinning.
- **Voxel preview pipeline**: reuses (by composition or shader-include) the
  trace shader from `BrickmapPaletteRenderer` / `InstancedVoxelTechnique`. The
  technique adds a `VoxelVolume` SceneNode component pointing at the preview
  animated-volume asset.
- **Overlay pipeline**: both run; the mesh pass writes after the voxel pass
  with per-pixel alpha = `m_overlayMeshAlpha` (UI slider, default 0.5).

**Debouncing**: `SetVoxelSize` records the new value + timestamp into the
session. The technique's record callback checks each frame: if more than
`m_debounceMs` have elapsed since the last `SetVoxelSize`, submit a new
preview bake. Default debounce = 250ms.

### 5.8 BakerPanel — `src/editor/panels/BakerPanel.{h,cpp}`

ImGui panel. Layout:

```
┌─ Source ────────────────────────────────┐
│ [ Open .glb… ]  /path/to/AnimatedOak.glb│
│ Meshes: 6   Skins: 3   Animations: 1    │
└─────────────────────────────────────────┘
┌─ Animation ─────────────────────────────┐
│ Clip: [WindSwayAnimation       ▾]       │
│ ▶ ⏸ ⏮ ⏭   speed [─────●───────] 1.00x   │
│ time [══════●─────────────] 6.21 / 11.08│
└─────────────────────────────────────────┘
┌─ Voxelization ──────────────────────────┐
│ Voxel size [─●─────────────] 0.025 m    │
│ Color source: ( ) Material  (●) Texture │
│ Grid: 64 × 96 × 64    (preview)         │
│ Preview status: ✓ baked at 6.21s        │
│                                         │
│ View mode: [Mesh] [Voxels] [Overlay]    │
│ Overlay alpha [────●─────] 0.50         │
└─────────────────────────────────────────┘
┌─ Bake animation ────────────────────────┐
│ Range: 0.00 ─ 11.08    fps [24 ▾]       │
│ Frames: 266                             │
│ [ Bake animation ]    progress: ▓░░ 12% │
│ [ Save bake… ]                          │
│ [ Promote to scene ]                    │
└─────────────────────────────────────────┘
```

The panel reads from `GltfImportTechnique::GetSession()` and writes via the
public API. It owns no state of its own beyond UI scratch (sliders, etc.).

## 6. Roadmap

Phased so each milestone leaves the engine in a working, mergeable state and
builds toward something demoable. Skinned-tree-on-screen is the first hero
moment; live re-bake is the second.

### Milestone 0 — Spike: parse and inspect (1 PR)
- Add `cgltf.h` to `dep/`
- Add `src/import/GltfLoader.{h,cpp}` + `MeshIR.h`
- Wire a temporary debug log: open a hardcoded path on startup, dump node
  count / animation count / vertex count to OutputPanel
- **Demo**: console shows the oak's structure; we know cgltf works.

### Milestone 1 — Skinned mesh preview (no voxels yet)
- Extend `AssetRegistry`: `SkinnedMeshAsset`, `AnimationClipAsset`,
  `AssetID::Type::SkinnedMesh / AnimationClip`
- Extend `Scene`: `Component::SkinnedMesh` variant, `Component.clipAsset`,
  `currentTime`, `playbackSpeed`, `paused`
- Extend `RenderItem`: `RenderItem::SkinnedMesh` with joint matrix buffer
- `AnimationEvaluator` in `src/import/`
- Update `SceneExtractor` to evaluate clips and produce joint matrices
  (transient buffer per frame)
- New `GltfImportTechnique` (Mesh mode only) with skinned-mesh pipeline
- Hardcoded BakerPanel scaffold: file picker, animation picker, time scrub,
  speed slider — talks to the technique
- **Demo**: load AnimatedOak.glb, see it sway in the viewport.

### Milestone 2 — Workspace concept
- `src/editor/Workspace.h` + `Editor::SetWorkspace`
- Menu bar workspace tabs (Scene / Import & Bake)
- Workspace-driven panel visibility + locked technique
- `RenderingSystem::RequestSwitchTechniqueByName`
- **Demo**: clicking "Import & Bake" hides the hierarchy, reveals BakerPanel,
  switches the technique. "Scene" restores the previous state.

### Milestone 3 — Static voxelization (single frame)
- `DefaultVoxPalette` (hardcoded MagicaVoxel default 256)
- `PaletteQuantizer` with 4096-entry LUT
- `Voxelizer` (surface, per-triangle, with material-color sourcing first)
- `JobSystem` (single worker)
- `AnimationBaker` preview-bake path only
- `GltfImportTechnique` gains `PreviewMode::Voxels` — wires the preview
  animated-volume asset (frameCount=1) and switches the viewport draw path
- Voxel size slider + debounce + live re-bake
- **Demo**: load oak, drag voxel-size slider, watch the voxel preview update
  ~250ms after each move. Toggle Mesh / Voxels.

### Milestone 4 — Full animation bake + write
- `AnimationBaker` full-bake path (all frames, packed sequentially into one
  byte buffer; uploaded as 2D-array layers — see
  docs/migrate-animated-format.md)
- `AssetRegistry::ResizeProceduralVoxelVolume` integration: preview
  (frameCount=1) → full bake (frameCount=N) reuses the same AssetID
- "Bake animation" button + progress bar
- `VoxAnimFormat` writer (`.vxa` + per-frame `.vox`)
- `VoxAnimFormat` reader → wraps `AssetRegistry::RegisterAnimatedVoxelAsset`
- "Save bake" file dialog
- **Demo**: bake the full oak swing, save it, reload it from disk.

### Milestone 5 — Texture color sourcing
- `Voxelizer` gains `VoxColorSource::TextureSampled`: barycentric → UV →
  bilinear texel fetch → quantize
- BakerPanel: color source radio
- **Demo**: oak leaves bake in green hues instead of solid material color.

### Milestone 6 — Overlay view + promote-to-scene
- `PreviewMode::Overlay` (mesh + voxel composited with alpha)
- "Promote to scene" hands the baked AssetID to the Scene world (new SceneNode
  with VoxelVolume component) and switches workspace to Scene
- **Demo**: bake an oak in Import workspace, promote it, see it standing on
  the existing terrain in Scene workspace under CombinedRenderer.

### Milestone 7 — Polish
- Better progress / cancel UX for long bakes
- Bake gallery (small thumbnails of recent bakes at different voxel sizes,
  click to switch)
- Keyboard shortcuts (space = play/pause, [/] = step frame, etc.)
- Error surfacing (what if the GLB has no skin? no animation? unsupported
  attributes?)

## 7. Open questions

These need answers before or during implementation. Capturing here so they're
not lost.

1. **Workspace switching when leaving Import:** if the user has an unsaved
   bake and switches back to Scene, do we warn them? Auto-discard? My
   suggestion: silently discard the preview volume but keep any completed
   full bake reachable from a "Recent bakes" list (so they can get it back).

2. **Promote-to-scene — auto-switch?** When the user clicks "Promote to
   scene," do we automatically switch to the Scene workspace, or just append
   to the Scene world and stay in Import? Suggestion: auto-switch (clearer
   feedback that the bake is now "real").

3. **Default vox palette source:** hardcode the well-known MagicaVoxel default
   256 RGBA palette (table in the .vox spec), or seed it by parsing a
   reference `.vox` file shipped in the repo? Suggestion: hardcode — fewer
   moving parts, palette is stable across MV versions.

4. **Multi-mesh GLBs:** the oak has bark + foliage as separate primitives
   with different materials. Voxelize them as one unified volume (one
   palette quantization across both, last-write-wins per voxel) or maintain
   separate volumes? Suggestion: unified volume — matches the engine's
   "one VoxelVolume component per asset" model and avoids shipping multi-
   asset bakes.

5. **Bake-wide AABB sampling:** to keep the Z-slab volume the same size every
   frame, we need an AABB that contains the mesh at every keyframe. Suggested
   approach: at bake start, evaluate the clip at K samples (K=32) and union
   the deformed-mesh AABBs. Dilate by 1 voxel for safety. Confirm K=32 is
   enough for slow-moving foliage; characters with rapid motion may need
   more.

6. **Debounce duration:** proposed 250ms. Could be exposed as an inspector
   parameter for testing. Cancellation policy on slider drag: cancel-and-
   restart (each preview job is short).

7. **Out-of-budget volumes:** what's the max grid size we allow? E.g. a
   user dragging the voxel-size slider to 0.001m on a 2-meter mesh produces
   a 2000^3 grid (~8GB). Suggested: clamp `size.x * size.y * size.z` to a
   configurable budget (default 256^3) and warn in the panel if the slider
   would exceed it.

8. **Color closer-fit per voxel:** when two triangles paint the same voxel,
   first-write-wins is simplest but produces visible "seams" in the bake.
   Tracking the distance from voxel-center to triangle and keeping the
   nearest is straightforward (one extra float per cell, discarded after
   bake) and gives much nicer results. Suggestion: do nearest-triangle from
   the start.

9. **Animation playback time vs. engine time:** the technique's preview clock
   should be independent of `RenderTechnique::GetTimeSeconds()` — the user
   can pause, scrub, and change speed without affecting the engine's logical
   clock. The full bake, however, should use clip-local time directly (not
   the engine clock) so saves are reproducible.

## 8. Out of scope for v1 (for the record)

- Morph targets
- Rigid (non-skinned) node animation
- Multiple animation clips per bake
- Mesh decimation pre-pass
- GPU voxelization
- Normal-cone / SDF voxelization
- Per-bake custom palettes
- Lighting bake-in (AO, baked irradiance)
- glTF extensions beyond core 2.0 (KHR_materials_*, etc.)
- `.gltf` (JSON+bin pair) loading — `.glb` only
