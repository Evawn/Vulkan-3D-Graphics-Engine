# SVO Phase 1: Engine Expansion & Sparse Voxel Octree Rendering

## 1. Context & Goals

### Current State
The engine has a clean Vulkan RAII layer (VWrap), a pluggable `RenderTechnique` system, and two renderers: `MeshRasterizer` (forward rasterization) and `DDATracer` (fullscreen fragment-shader ray-casting against a flat 32³ 3D texture). The frame pipeline is hardcoded to two render passes (scene → UI), VWrap supports graphics pipelines only, and there is no compute shader infrastructure.

### Phase 1 Target
Expand the engine to support **compute shaders**, introduce a **render graph** for managing multi-pass pipelines, and implement a new **SVO ray-casting renderer** that generates voxel volumes procedurally via compute and renders them via hierarchical octree traversal in a fragment shader.

### Phase 2+ Preview
`.vox` / `.rsvo` file loading, GPU-side SVO construction, PBR materials, lighting, LOD streaming, large-scene support (16K³+). See `VOX-INTEGRATION.md` for format specifications.

---

## 2. VWrap Compute Expansion

The VWrap layer needs compute pipeline support, a compute queue, and additional command buffer methods. Much of the existing infrastructure (descriptors, buffers, images) is already flexible enough to support compute workloads without modification.

### 2.1 ComputePipeline

New class following VWrap's existing factory pattern (`static Create()`, `shared_ptr` ownership, RAII cleanup).

A compute pipeline is much simpler than a graphics pipeline — it needs only:
- Descriptor set layout(s)
- Push constant ranges
- A single compute shader module

No render pass, vertex input, rasterization state, or depth/stencil configuration. The pipeline layout creation logic can be shared with or extracted from the existing `Pipeline` class.

**Files:** New `lib/VWrap/include/ComputePipeline.h`, `lib/VWrap/src/ComputePipeline.cpp`
**Reference:** `Pipeline.h/cpp` for the factory pattern and layout creation

### 2.2 Compute Queue

Add `computeFamily` to `QueueFamilyIndices` in `PhysicalDevice`. The queue selection should prefer a dedicated compute-only queue family (for async compute capability), falling back to the graphics family if none exists. Most GPUs expose at least one compute-capable family.

Add `computeQueue` and `computeCommandPool` to `VulkanContext`. The existing `Device::Create()` already builds queues from a `std::set<uint32_t>` of unique families, so adding compute is a small change.

**Files:** `PhysicalDevice.h/cpp`, `Device.h/cpp`, `VulkanContext.h`

### 2.3 CommandBuffer Extensions

Add methods for compute dispatch and synchronization:
- `CmdDispatch(groupCountX, Y, Z)` — dispatch compute workgroups
- `CmdBindComputePipeline(pipeline)` — bind a compute pipeline
- `CmdBindDescriptorSets(bindPoint, layout, sets)` — general-purpose descriptor binding (works for both graphics and compute bind points)
- `CmdBufferMemoryBarrier(buffer, srcAccess, dstAccess, srcStage, dstStage)` — SSBO synchronization between passes
- `CmdImageMemoryBarrier(...)` — storage image synchronization

**Files:** `CommandBuffer.h/cpp`
**Reference:** Existing `CmdTransitionImageLayout()` for the barrier pattern

### 2.4 Image Layout Transitions

`CmdTransitionImageLayout()` needs two new transition paths for compute storage images:
- `UNDEFINED → GENERAL` (initial transition for storage images)
- `GENERAL → SHADER_READ_ONLY_OPTIMAL` (after compute writes, before fragment reads)

**Files:** `CommandBuffer.cpp` (the transition switch/if chain)

### 2.5 No Changes Required

These components already support compute workloads through their existing parameterized APIs:
- **Buffer** — `Create()` accepts arbitrary `VkBufferUsageFlags`; pass `STORAGE_BUFFER_BIT`
- **DescriptorSetLayout / DescriptorPool** — accept any `VkDescriptorType`; pass `STORAGE_BUFFER` or `STORAGE_IMAGE`
- **Image** — already supports 3D images, arbitrary usage flags including `STORAGE_BIT`

### 2.6 Shader Build System

Add `.comp` to the glob patterns in `shaders/CMakeLists.txt`. The runtime `ShaderCompiler` already compiles any shader type — it just invokes `glslc` on whatever source path is given.

**Files:** `shaders/CMakeLists.txt`

---

## 3. Render Graph

### 3.1 Design

A lightweight pass scheduler — not a full Frostbite-style frame graph. The render graph is a DAG where each node is a named render or compute pass, and edges are resource dependencies.

Each pass declares:
- **Type**: compute, graphics, or transfer
- **Resource reads**: which images/buffers it samples or reads, with required layouts and access flags
- **Resource writes**: which images/buffers it writes to
- **Record callback**: a function that records the pass's commands into a command buffer

The graph:
1. Topologically sorts passes based on read/write dependencies
2. Auto-inserts `vkCmdPipelineBarrier` between passes where a write in one pass is read by a subsequent pass
3. Manages image layout transitions (e.g., `GENERAL` after compute write → `SHADER_READ_ONLY` before fragment read)
4. Records all passes sequentially into the frame command buffer

### 3.2 Integration with Application

Replace the hardcoded 2-pass recording in `Application::DrawFrame()` with render graph execution. The initial graph for the DDA renderer is simply:

```
[Scene Pass (graphics)] → [UI Pass (graphics)]
```

For SVO rendering, it becomes:

```
[SVO Generate (compute)] → barrier → [SVO Render (graphics)] → [UI Pass (graphics)]
```

The compute pass only runs on-demand (when the procedural parameters change), not every frame. The render graph should support marking passes as "dirty" or "one-shot."

The UI pass remains special — it always runs last and targets the swapchain.

### 3.3 Dev Tooling

A new ImGui panel (alongside Viewport, Metrics, Output, Inspector) that displays:
- List of registered passes with type badges (compute/graphics)
- Resource dependencies per pass (what reads what)
- Per-pass GPU timing, using the existing `GPUProfiler` timestamp system extended to support multiple named timer scopes
- Pass enable/disable toggles for debugging

Table/list format — this is a debugging aid, not a visual graph editor. The `GPUProfiler` will need a small extension to support N timestamp pairs (currently hardcoded to 1 begin/end pair per frame).

**Files:** New `src/editor/panels/RenderGraphPanel.h/cpp`, new `src/rendering/RenderGraph.h/cpp`
**Reference:** Existing panel pattern in `src/editor/panels/`, `GPUProfiler.h/cpp` for timing

---

## 4. Rename OctreeTracer → DDATracer

The current "OctreeTracer" doesn't use an octree — it performs DDA traversal on a flat 3D texture. Rename for clarity before adding the actual SVO tracer.

| Current | New |
|---------|-----|
| `src/rendering/octree/OctreeTracer.h/cpp` | `src/rendering/voxel/DDATracer.h/cpp` |
| `src/rendering/octree/Octree.h/cpp` | `src/rendering/voxel/BrickVolume.h/cpp` |
| `shaders/shader_tracer.vert/frag` | `shaders/shader_dda.vert/frag` |
| Class `OctreeTracer` | Class `DDATracer` |
| Directory `src/rendering/octree/` | Directory `src/rendering/voxel/` |

Update all references in `Application.cpp`, `src/CMakeLists.txt`, shader path constants, and hot-reload paths. The `voxel/` directory will house both the DDA tracer and the new SVO tracer.

---

## 5. SVO Data Structure

### 5.1 Node Layout

Each SVO node is a compact struct stored in a flat GPU buffer:

| Field | Size | Description |
|-------|------|-------------|
| `childMask` | 8 bits | Which of the 8 octants have children (1 = occupied) |
| `childOffset` | 24 bits | Index of first child in the node buffer |

Packed into a single `uint32`. Children are stored contiguously — child `i` is at `childOffset + popcount(childMask & ((1 << i) - 1))`. This is the standard SVO layout used in GPU ray-casting literature.

Leaf nodes (at the finest level) store a color/material index instead of child data. The leaf flag can be implicit (known from traversal depth) or encoded in a reserved bit.

### 5.2 GPU Representation

The node buffer is uploaded as an SSBO, with nodes in BFS order. This layout:
- Matches the `.rsvo` format (see `VOX-INTEGRATION.md` Section 3), enabling direct loading later
- Has good cache coherence for top-down traversal (parent nodes are near each other in memory)
- Supports progressive LOD (coarser levels at lower addresses)

A separate palette buffer (256 RGBA entries) maps color indices to colors — either as a small UBO or a 1D texture.

### 5.3 CPU Construction (Phase 1)

For Phase 1 with small procedural volumes (128³–256³), SVO construction happens on CPU:
1. Evaluate the SDF at each voxel position to determine occupancy
2. Build the octree top-down via BFS (same algorithm as `VOX-INTEGRATION.md` Section 4.1)
3. Serialize to the flat node buffer format
4. Upload to GPU SSBO via staging buffer

This is fast for small volumes and avoids the complexity of GPU-side octree construction, which is a Phase 2 optimization.

---

## 6. SVO Rendering Technique

### 6.1 SVOTracer

New `RenderTechnique` implementation following the same patterns as `DDATracer`:

| Aspect | DDATracer (existing) | SVOTracer (new) |
|--------|---------------------|-----------------|
| Geometry | Vertex-less FSQ (4 verts, triangle strip) | Same |
| Data source | 3D texture (combined image sampler) | SSBO (node buffer) + palette |
| Traversal | DDA stepping through flat grid | Hierarchical octree descent |
| Push constants | Camera matrix, position, params | Camera matrix, position, SVO metadata |
| Shader | `shader_dda.frag` | `shader_svo.frag` |

**Descriptors:**
- Binding 0: SSBO (`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`) — SVO node buffer
- Binding 1: Combined image sampler or UBO — palette colors

**Push constants:** NDC-to-world matrix, camera position, SVO root offset, max depth, volume scale/position, debug flags.

### 6.2 SVO Ray-Casting Algorithm

The fragment shader implements a stack-based octree traversal (based on Laine & Karras, "Efficient Sparse Voxel Octrees"):

1. **Ray setup**: compute ray origin and direction from screen pixel via NDC-to-world matrix (identical to DDA tracer)
2. **Root intersection**: ray-AABB test against the SVO bounding cube
3. **Hierarchical descent**: maintain a small stack (max depth entries). At each node:
   - Determine which octant the ray's entry point falls in
   - Check `childMask` bit for that octant
   - If set: push current node, descend to child
   - If not set: advance ray to next octant boundary (parametric stepping)
4. **Leaf hit**: look up palette color, compute surface normal from the step direction (which face was entered)
5. **Miss**: sky gradient, same as DDA tracer

The stack depth equals the octree depth (7 for 128³, 8 for 256³) — well within GPU limits.

### 6.3 Inspector Parameters

Exposed via the existing `TechniqueParameter` system:
- Max traversal depth (int slider)
- Volume resolution (enum: 64³, 128³, 256³)
- SDF primitive type (enum: sphere, torus, box, rounded box)
- SDF parameters (floats: radii, rounding, etc.)
- Debug visualization mode (enum: normal, octree level, iteration count, normals)
- Sky color (color3)

Parameter changes to SDF type/params trigger SVO regeneration.

---

## 7. Compute Shader Procedural Generation

### 7.1 SDF Evaluation Shader

A compute shader with workgroup size `8×8×8` that evaluates an SDF function at each voxel position and writes to a 3D `R8UI` texture (palette index per voxel).

**Uniforms/push constants:** volume size, SDF type selector, SDF-specific parameters (radii, etc.), color index.

**SDF primitives to implement:**
- Sphere: `length(p - center) - radius`
- Torus: classic torus SDF
- Box: `max(abs(p) - halfExtents)`
- Rounded box: box SDF with corner rounding
- CSG combinations via `min`/`max`/`smooth_min`

**Dispatch:** `ceil(volumeSize/8)³` workgroups. Runs once on init and on parameter change, not every frame.

### 7.2 SVO Construction Pipeline

After the compute shader fills the 3D texture:
1. CPU evaluates the same SDF independently and builds the SVO (no GPU readback needed — the SDF functions are trivial to duplicate on CPU)
2. Node buffer uploaded to SSBO via staging buffer
3. The 3D texture remains available for the DDA renderer (useful for side-by-side comparison)

In Phase 2, this can be replaced with GPU-side SVO construction via a multi-pass compute pipeline, eliminating the CPU path entirely.

### 7.3 Render Graph Integration

The procedural generation fits into the render graph as a conditional compute pass:

```
[SDF Compute (on-demand)] → barrier → [SVO Render (every frame)] → [UI (every frame)]
```

The compute pass writes to the 3D texture (storage image). A pipeline barrier transitions the image from `GENERAL` (compute write) to `SHADER_READ_ONLY` (fragment read) before the graphics pass. The SVO SSBO is uploaded separately via transfer, not through the render graph (it's a one-time upload, not a per-frame dependency).

---

## 8. Implementation Order

Suggested sequence for building this out across multiple sessions. Each step is independently testable.

| Step | What | Validates |
|------|------|-----------|
| 1 | VWrap compute expansion (ComputePipeline, queue, CommandBuffer methods) | Compute infrastructure compiles and links |
| 2 | Shader build system update (`.comp` glob in CMake) | Compute shaders compile via build system |
| 3 | Rename OctreeTracer → DDATracer, `octree/` → `voxel/` | Build still works, DDA rendering unchanged |
| 4 | Simple compute test — generate 3D texture via compute, render with DDA | End-to-end compute pipeline validation |
| 5 | Render graph core (pass nodes, resource tracking, barrier insertion) | Existing 2-pass pipeline works through graph |
| 6 | Integrate render graph into `Application::DrawFrame()` | No visual change, but pipeline is now graph-driven |
| 7 | SVO data structure + CPU construction from procedural SDF | SVO builds correctly (validate node counts, structure) |
| 8 | SVOTracer technique with FSQ ray-casting shader | Sphere renders correctly via SVO traversal |
| 9 | Wire up compute generation → SVO construction → rendering | Full pipeline: compute SDF → build SVO → render |
| 10 | Render graph dev tooling panel | Passes and timing visible in UI |
| 11 | Polish: SDF primitives, parameter tuning, hot-reload for `.comp` shaders | Multiple primitives, interactive parameter adjustment |

---

## 9. Architectural Decisions

### Why FSQ Fragment Shader (not Vulkan RT Extensions)

The Vulkan ray-tracing extensions (`VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`) are designed for hardware-accelerated BVH traversal where the **driver** builds and manages the acceleration structure. They are the wrong tool for SVO ray-casting:

- **No custom traversal control.** The RT pipeline traverses the driver's BVH. You can't substitute your own SVO hierarchy. `VK_KHR_ray_query` allows custom intersection shaders but still requires building BLAS/TLAS structures — overhead with no benefit for a data structure you're managing yourself.
- **Portability.** RT extensions require RTX 20-series+ or RDNA2+ GPUs. This directly contradicts the accessibility goal. A fragment shader runs on any Vulkan 1.0 device.
- **Established approach.** The SVO ray-casting literature (Laine & Karras 2010, Crassin et al. 2009) uses screen-space ray marching in fragment shaders. The technique is well-understood and maps cleanly to the existing FSQ pattern.

### Why Not a Full Frame Graph

A full Frostbite-style frame graph with transient resource allocation, resource aliasing, and automatic lifetime management is powerful but overengineered for Phase 1's 2–3 passes. The lightweight render graph described here handles the core requirements (barrier insertion, pass ordering, resource transitions) and can be evolved into a full frame graph later if the pass count grows significantly.

### Memory Manager: Future Work

VMA handles all GPU allocation for Phase 1. A custom memory abstraction becomes valuable in Phase 2+ for:
- **Node pool allocator**: sub-allocate SVO nodes from a large pre-allocated SSBO, avoiding per-node VMA overhead
- **Brick cache**: fixed-size GPU cache of leaf voxel data for large scenes, with LRU eviction
- **Streaming manager**: progressive loading of octree levels from disk, managing GPU memory budget

For 128³–256³ procedural volumes, none of this is needed.

---

## 10. Future Directions (Phase 2+)

- **`.vox` file loading** → SVO construction using the parser from `VOX-INTEGRATION.md`
- **`.rsvo` file loading** → direct SVO import (BFS node buffer maps directly to GPU format)
- **GPU-side SVO construction** via multi-pass compute (eliminate CPU path)
- **Materials and PBR lighting** — palette entries map to material IDs, MATL chunk properties feed PBR parameters
- **LOD streaming** — BFS ordering enables progressive level loading with distance-based cutoff
- **Large-scene support** — 16K³+ volumes via out-of-core streaming and brick caching
- **Scene graph** — multiple SVO volumes with transforms, from `.vox` scene hierarchy

---

## File Reference

### New Files (Phase 1)
| File | Purpose |
|------|---------|
| `lib/VWrap/include/ComputePipeline.h` | Compute pipeline wrapper |
| `lib/VWrap/src/ComputePipeline.cpp` | Compute pipeline implementation |
| `src/rendering/RenderGraph.h/cpp` | Render graph system |
| `src/rendering/voxel/SVOTracer.h/cpp` | SVO ray-casting render technique |
| `src/rendering/voxel/SVO.h/cpp` | SVO data structure and CPU construction |
| `src/editor/panels/RenderGraphPanel.h/cpp` | Render graph dev tooling panel |
| `shaders/shader_svo.vert` | FSQ vertex shader (shared with DDA or identical) |
| `shaders/shader_svo.frag` | SVO ray-casting fragment shader |
| `shaders/svo_generate.comp` | Procedural SDF compute shader |

### Modified Files
| File | Change |
|------|--------|
| `lib/VWrap/include/PhysicalDevice.h` | Add `computeFamily` to `QueueFamilyIndices` |
| `lib/VWrap/src/PhysicalDevice.cpp` | Compute queue family discovery |
| `lib/VWrap/include/VulkanContext.h` | Add `computeQueue`, `computeCommandPool` |
| `lib/VWrap/include/CommandBuffer.h` | Add dispatch/barrier/bind methods |
| `lib/VWrap/src/CommandBuffer.cpp` | Implement new methods + layout transitions |
| `shaders/CMakeLists.txt` | Add `.comp` glob |
| `src/Application.h/cpp` | Render graph integration, renderer registration |
| `src/utils/GPUProfiler.h/cpp` | Multi-scope timing for render graph |

### Renamed Files (OctreeTracer → DDATracer)
| From | To |
|------|-----|
| `src/rendering/octree/OctreeTracer.h/cpp` | `src/rendering/voxel/DDATracer.h/cpp` |
| `src/rendering/octree/Octree.h/cpp` | `src/rendering/voxel/BrickVolume.h/cpp` |
| `shaders/shader_tracer.vert` | `shaders/shader_dda.vert` |
| `shaders/shader_tracer.frag` | `shaders/shader_dda.frag` |
