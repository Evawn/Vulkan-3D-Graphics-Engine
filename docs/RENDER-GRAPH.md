# Render Graph

## 1. Context & Goals

The engine's frame pipeline is hardcoded in `Application::DrawFrame()`: Scene → UI, with raw Vulkan calls managing render passes, clear values, barriers, and framebuffers. `RenderTechnique` implementations record commands inside a render pass that Application begins and ends. This couples Application to the specifics of every rendering technique and makes multi-pass pipelines (compute→graphics, G-buffer→lighting→composite) difficult to add.

The render graph replaces this with a declarative system where **techniques define their own pass subgraphs** and the graph handles resource allocation, render pass management, barrier insertion, and execution ordering. The goal is to push raw Vulkan down into the graph implementation so that Application and techniques read as clean, high-level pipeline descriptions.

This is the foundation for SVO ray-casting with compute-generated geometry, but also for any future multi-pass rendering: global illumination, particle/fluid simulations, post-processing chains, etc.

---

## 2. Design Decisions

### The graph owns resources

**Decision**: The render graph allocates and manages transient images and buffers — it doesn't just track externally-owned resources.

**Rationale**: As soon as you have intermediate images between passes (compute output → fragment input, G-buffer layers, propagation volumes, bloom temps), manual resource creation becomes the dominant source of boilerplate. Each resource needs an `Image::Create`, `ImageView::Create`, format selection, usage flags, and lifecycle management. The graph eliminates all of this: techniques declare what they need via an `ImageDesc`, and the graph handles allocation, view creation, and cleanup.

**When transient resource management pays for itself:**

| Scenario | Intermediate resources | Without graph |
|----------|----------------------|---------------|
| SVO compute generation | 3D storage image (SDF volume) | Technique manually creates/destroys, manages layout transitions |
| Global illumination | G-buffer (position, normal, albedo), radiance volume, propagation buffers | 5–8 manually managed images with interdependent lifetimes |
| Particle/fluid sim | Particle buffer, density volume, velocity field | Multiple compute buffers + storage images, frame-scoped |
| Post-processing chain | Bloom temps, tone-map intermediate, FXAA input | Each pass produces an image consumed by the next |

In every case, these resources exist only within a frame and are consumed by later passes. Declaring them on the graph instead of managing them manually is the difference between `graph.CreateImage("bloom_temp", desc)` and 15 lines of VMA allocation + view creation + descriptor updates + cleanup.

**Phase 1 implementation**: Simple allocation via VMA — one image per transient resource, reused across frames. No memory aliasing or pooling yet. **Future**: Resources with non-overlapping lifetimes can share the same `VkDeviceMemory` (e.g., a bloom temp and a G-buffer normal texture that are never live simultaneously), reducing VRAM pressure for complex pipelines.

### Techniques register passes on the graph

**Decision**: `RenderTechnique` gains a `RegisterPasses()` method that defines its complete pass subgraph. The graph is the primary rendering abstraction; techniques are subgraph definitions.

**Rationale**: Techniques like the future SVOTracer need multiple passes (compute generation → graphics rendering). A particle system needs a compute sim pass → a graphics render pass. If Application wraps techniques in graph passes, it needs to know the internal structure of every technique — defeating the purpose. Techniques should be self-describing: they declare what passes they need, what resources they produce and consume, and what commands to record.

```
Application's job:        Create scene targets → technique.RegisterPasses() → add UI pass → compile → execute
Technique's job:          Declare passes, resources, and record callbacks
Graph's job:              Allocate resources, create render passes, sort, barrier, execute
```

### The graph manages render pass begin/end

**Decision**: For graphics passes, the graph creates VkRenderPass objects from attachment declarations, creates framebuffers, and handles begin/end. The technique's record callback fires with the render pass already active.

**Rationale**: VkRenderPass creation is 40–80 lines of boilerplate per configuration (attachment descriptions, subpass descriptions, dependencies). By generating these from high-level attachment declarations, techniques never touch `VkAttachmentDescription`, `VkSubpassDependency`, or `VkFramebufferCreateInfo`. MSAA resolve is handled automatically — if the color attachment has `samples > 1`, the graph adds a resolve attachment. The current `RenderPass::CreateOffscreen()` and `OffscreenTarget` are subsumed by the graph.

### Viewport and scissor are set automatically

**Decision**: For graphics passes, the graph sets viewport and scissor to match the render target extent before calling the record callback. Techniques record bind + draw commands only.

**Rationale**: Every current technique duplicates the same 12-line viewport/scissor setup. This is boilerplate that belongs in the graph.

---

## 3. Architecture

### 3.1 Resources

Resources are either **transient** (graph-allocated, frame-scoped) or **imported** (externally owned, registered for tracking).

```cpp
// Lightweight typed handles — no raw Vulkan types at the API surface
struct ImageHandle { uint32_t id; };
struct BufferHandle { uint32_t id; };

// Declarative image description
struct ImageDesc {
    uint32_t width, height;
    uint32_t depth = 1;
    VkFormat format;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageType imageType = VK_IMAGE_TYPE_2D;
};

// Declarative buffer description
struct BufferDesc {
    VkDeviceSize size;
};
```

**Transient images** are allocated by the graph during `Compile()`. The graph determines usage flags automatically from how passes reference the resource (written as color attachment → `COLOR_ATTACHMENT_BIT`, read as sampled → `SAMPLED_BIT`, written by compute → `STORAGE_BIT`, etc.).

**Imported images** (swapchain images, persistent textures) are registered for dependency tracking but not allocated or destroyed by the graph.

### 3.2 Passes

```cpp
enum class PassType { Graphics, Compute };

// Load/store operations for graphics attachments
enum class LoadOp  { Clear, Load, DontCare };
enum class StoreOp { Store, DontCare };

// Context provided to record callbacks
struct PassContext {
    std::shared_ptr<VWrap::CommandBuffer> cmd;
    uint32_t frameIndex;
    VkExtent2D extent;
};
```

**Graphics passes** declare color/depth attachments and sampled inputs. The graph creates a compatible VkRenderPass and VkFramebuffer, begins the render pass with the declared clear values, sets viewport/scissor, calls the record callback, and ends the render pass.

**Compute passes** declare read/write resources. The graph calls the record callback with no render pass active.

### 3.3 Pass Builders

```cpp
class GraphicsPassBuilder {
public:
    // Render target declarations
    GraphicsPassBuilder& SetColorAttachment(
        ImageHandle target, LoadOp load, StoreOp store,
        float r = 0, float g = 0, float b = 0, float a = 1);
    GraphicsPassBuilder& SetDepthAttachment(
        ImageHandle target, LoadOp load, StoreOp store,
        float depth = 1.0f, uint32_t stencil = 0);
    GraphicsPassBuilder& SetResolveTarget(ImageHandle target);

    // Sampled/storage inputs
    GraphicsPassBuilder& Read(ImageHandle resource);
    GraphicsPassBuilder& Read(BufferHandle resource);

    // Record callback
    GraphicsPassBuilder& SetRecord(std::function<void(PassContext&)> fn);

    // Access the VkRenderPass after registration (for pipeline creation)
    VkRenderPass GetRenderPass() const;
};

class ComputePassBuilder {
public:
    ComputePassBuilder& Read(ImageHandle resource);
    ComputePassBuilder& Read(BufferHandle resource);
    ComputePassBuilder& Write(ImageHandle resource);
    ComputePassBuilder& Write(BufferHandle resource);
    ComputePassBuilder& SetRecord(std::function<void(PassContext&)> fn);
};
```

### 3.4 RenderGraph

```cpp
class RenderGraph {
public:
    RenderGraph(std::shared_ptr<VWrap::Device> device,
                std::shared_ptr<VWrap::Allocator> allocator);

    // ---- Resources ----
    ImageHandle CreateImage(const std::string& name, const ImageDesc& desc);
    BufferHandle CreateBuffer(const std::string& name, const BufferDesc& desc);
    ImageHandle ImportImage(const std::string& name,
                            std::shared_ptr<VWrap::ImageView> view,
                            VkFormat format, VkImageLayout layout);

    // ---- Passes ----
    GraphicsPassBuilder& AddGraphicsPass(const std::string& name);
    ComputePassBuilder& AddComputePass(const std::string& name);
    void SetPassEnabled(const std::string& name, bool enabled);

    // ---- Lifecycle ----
    void Compile();    // allocate resources, create render passes/framebuffers,
                       // topological sort, compute barriers
    void Execute(std::shared_ptr<VWrap::CommandBuffer> cmd,
                 uint32_t frameIndex);
    void Resize(VkExtent2D newExtent);  // recreate size-dependent resources
    void Clear();                       // reset graph for rebuild

    // ---- Resource Access ----
    // For techniques that need Vulkan handles (descriptor writes, ImGui texture)
    std::shared_ptr<VWrap::ImageView> GetImageView(ImageHandle handle) const;
    VkImage GetVkImage(ImageHandle handle) const;

    // ---- Introspection (for future dev tooling) ----
    struct PassInfo { std::string name; PassType type; bool enabled; };
    const std::vector<PassInfo>& GetPassInfos() const;

    // ---- Dynamic imports (swapchain image changes per frame) ----
    void UpdateImport(ImageHandle handle,
                      std::shared_ptr<VWrap::ImageView> view);
};
```

### 3.5 Graph Lifecycle

```
Init:     graph.CreateImage() / ImportImage()
          technique->RegisterPasses(graph, ...)
          graph.AddGraphicsPass("UI")...
          graph.Compile()

Frame:    graph.UpdateImport(swapchain, currentView)
          graph.Execute(cmd, frameIndex)

Resize:   graph.Resize(newExtent)       // recreates transient images + framebuffers
          technique->OnResize(newExtent) // technique updates descriptors
```

---

## 4. Technique Integration

### Updated RenderTechnique Interface

```cpp
class RenderTechnique {
public:
    virtual ~RenderTechnique() = default;
    virtual std::string GetName() const = 0;

    // Define this technique's passes on the render graph.
    // Called once during init, and again on renderer switch.
    // The technique creates its pipelines, descriptors, and
    // registers pass callbacks here.
    virtual void RegisterPasses(
        RenderGraph& graph,
        const RenderContext& ctx,
        ImageHandle colorTarget,
        ImageHandle depthTarget,
        ImageHandle resolveTarget) = 0;

    virtual void Shutdown() = 0;

    // Called after graph.Resize() — update descriptors referencing graph images.
    virtual void OnResize(VkExtent2D newExtent, RenderGraph& graph) = 0;

    // Shader hot-reload
    virtual std::vector<std::string> GetShaderPaths() const = 0;
    virtual void RecreatePipeline(const RenderContext& ctx) = 0;

    // UI integration (unchanged)
    virtual std::vector<TechniqueParameter>& GetParameters();
    virtual FrameStats GetFrameStats() const;
    virtual void SetWireframe(bool enabled);
    virtual bool GetWireframe() const;
};
```

`Init()` and `RecordCommands()` are gone — `RegisterPasses()` handles both setup and pass registration. The record callback replaces `RecordCommands()`.

### Example: DDA Tracer (single graphics pass)

```cpp
void DDATracer::RegisterPasses(
    RenderGraph& graph, const RenderContext& ctx,
    ImageHandle color, ImageHandle depth, ImageHandle resolve)
{
    m_device = ctx.device;
    m_camera = ctx.camera;
    m_extent = ctx.extent;

    CreateDescriptors(ctx.maxFramesInFlight);
    CreateSampler();
    CreateBrickTexture(ctx.graphicsPool);
    WriteDescriptors();

    auto& pass = graph.AddGraphicsPass("DDA Scene")
        .SetColorAttachment(color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
        .SetDepthAttachment(depth, LoadOp::Clear, StoreOp::DontCare)
        .SetResolveTarget(resolve)
        .SetRecord([this](PassContext& ctx) {
            auto vk_cmd = ctx.cmd->Get();
            vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->Get());
            // bind descriptors, push constants, draw
            vkCmdDraw(vk_cmd, 4, 1, 0, 0);
        });

    CreatePipeline(pass.GetRenderPass());
}
```

No `VkClearValue` arrays, no `CmdBeginRenderPass`, no viewport/scissor setup, no `vkCmdEndRenderPass`. The technique just records draw commands.

### Example: Compute Test (compute + graphics)

```cpp
void ComputeTest::RegisterPasses(
    RenderGraph& graph, const RenderContext& ctx,
    ImageHandle color, ImageHandle depth, ImageHandle resolve)
{
    m_device = ctx.device;
    m_extent = ctx.extent;

    // Transient storage image — graph allocates it
    m_storage = graph.CreateImage("compute_output", {
        ctx.extent.width, ctx.extent.height, 1,
        VK_FORMAT_R8G8B8A8_UNORM
    });

    // Compute pass: write to storage image
    graph.AddComputePass("Compute Generate")
        .Write(m_storage)
        .SetRecord([this](PassContext& ctx) {
            ctx.cmd->CmdBindComputePipeline(m_compute_pipeline);
            // bind descriptors, push constants, dispatch
        });

    // Graphics pass: display the compute result
    auto& gfx = graph.AddGraphicsPass("Compute Display")
        .SetColorAttachment(color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
        .SetDepthAttachment(depth, LoadOp::Clear, StoreOp::DontCare)
        .SetResolveTarget(resolve)
        .Read(m_storage)  // graph auto-inserts compute→fragment barrier
        .SetRecord([this](PassContext& ctx) {
            auto vk_cmd = ctx.cmd->Get();
            vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline->Get());
            // bind descriptors, draw FSQ
        });

    CreateComputePipeline();
    CreateGraphicsPipeline(gfx.GetRenderPass());
    // After Compile(), get the VkImageView for descriptor writes:
    // graph.GetImageView(m_storage)
}
```

The graph inserts a `vkCmdPipelineBarrier` between "Compute Generate" and "Compute Display" with `COMPUTE_SHADER → FRAGMENT_SHADER` stage transition and `GENERAL → SHADER_READ_ONLY_OPTIMAL` layout transition.

### Example: Future SVO Tracer (on-demand compute + graphics)

```cpp
void SVOTracer::RegisterPasses(
    RenderGraph& graph, const RenderContext& ctx,
    ImageHandle color, ImageHandle depth, ImageHandle resolve)
{
    m_sdf_volume = graph.CreateImage("sdf_volume", {
        128, 128, 128, VK_FORMAT_R8_UINT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TYPE_3D
    });

    graph.AddComputePass("SVO Generate")
        .Write(m_sdf_volume)
        .SetRecord([this](PassContext& ctx) { /* dispatch SDF evaluation */ });

    // Compute pass only runs when parameters change
    graph.SetPassEnabled("SVO Generate", false);

    auto& gfx = graph.AddGraphicsPass("SVO Render")
        .SetColorAttachment(color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
        .SetDepthAttachment(depth, LoadOp::Clear, StoreOp::DontCare)
        .SetResolveTarget(resolve)
        .Read(m_sdf_volume)
        .SetRecord([this](PassContext& ctx) { /* bind SVO SSBO, draw FSQ */ });

    // ...
}

// On parameter change:
void SVOTracer::OnParameterChange(RenderGraph& graph) {
    graph.SetPassEnabled("SVO Generate", true);  // runs once next frame
    m_needs_disable = true;
}

// After frame:
void SVOTracer::PostFrame(RenderGraph& graph) {
    if (m_needs_disable) {
        graph.SetPassEnabled("SVO Generate", false);
        m_needs_disable = false;
    }
}
```

---

## 5. Application Integration

### Before (current)

```cpp
// Application.h — scattered Vulkan state
std::shared_ptr<VWrap::RenderPass> m_scene_render_pass;
std::shared_ptr<VWrap::RenderPass> m_presentation_render_pass;
std::vector<std::shared_ptr<VWrap::Framebuffer>> m_presentation_framebuffers;
std::shared_ptr<VWrap::OffscreenTarget> m_offscreen_target;

// Application.cpp — DrawFrame() with raw Vulkan
void Application::DrawFrame() {
    // ... acquire ...
    command_buffer->Begin();
    std::vector<VkClearValue> sceneClearValues(3);
    sceneClearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    sceneClearValues[1].depthStencil = { 1.0f, 0 };
    sceneClearValues[2].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    command_buffer->CmdBeginRenderPass(m_scene_render_pass, m_offscreen_target->GetFramebuffer(), sceneClearValues);
    m_renderers[m_active_renderer_index]->RecordCommands(command_buffer, frame_index, m_camera);
    vkCmdEndRenderPass(command_buffer->Get());
    // ... UI pass ...
    vkEndCommandBuffer(command_buffer->Get());
    // ... present ...
}
```

### After

```cpp
// Application.h — clean
RenderGraph m_render_graph;
ImageHandle m_scene_color, m_scene_depth, m_scene_resolve, m_swapchain;

// Application.cpp
void Application::BuildRenderGraph() {
    m_render_graph.Clear();

    VkExtent2D extent = m_offscreen_extent;
    VkFormat format = m_vk.frameController->GetSwapchain()->GetFormat();

    m_scene_color = m_render_graph.CreateImage("scene_color", {
        extent.width, extent.height, 1, format, m_vk.msaaSamples });
    m_scene_depth = m_render_graph.CreateImage("scene_depth", {
        extent.width, extent.height, 1, FindDepthFormat(), m_vk.msaaSamples });
    m_scene_resolve = m_render_graph.CreateImage("scene_resolve", {
        extent.width, extent.height, 1, format });

    auto ctx = BuildRenderContext();
    m_renderers[m_active_renderer_index]->RegisterPasses(
        m_render_graph, ctx, m_scene_color, m_scene_depth, m_scene_resolve);

    m_swapchain = m_render_graph.ImportImage("swapchain",
        m_vk.frameController->GetImageViews()[0], format,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    m_render_graph.AddGraphicsPass("UI")
        .SetColorAttachment(m_swapchain, LoadOp::Clear, StoreOp::Store,
                            0.059f, 0.059f, 0.059f, 1.0f)
        .Read(m_scene_resolve)
        .SetRecord([this](PassContext& ctx) {
            m_gui_renderer->CmdDraw(ctx.cmd);
        });

    m_render_graph.Compile();
    RegisterSceneTexture();  // uses graph.GetImageView(m_scene_resolve)
}

void Application::DrawFrame() {
    // ... viewport resize check ...
    m_vk.frameController->AcquireNext();
    uint32_t image_index = m_vk.frameController->GetImageIndex();
    uint32_t frame_index = m_vk.frameController->GetCurrentFrame();
    auto cmd = m_vk.frameController->GetCurrentCommandBuffer();

    m_last_metrics = m_gpu_profiler->GetMetrics(frame_index);
    m_render_graph.UpdateImport(m_swapchain, m_vk.frameController->GetImageViews()[image_index]);

    cmd->Begin();
    m_gpu_profiler->CmdBegin(cmd, frame_index);
    m_render_graph.Execute(cmd, frame_index);
    m_gpu_profiler->CmdEnd(cmd, frame_index);
    cmd->End();

    m_vk.frameController->Render();
}
```

**Removed from Application**: `m_scene_render_pass`, `m_presentation_render_pass`, `m_presentation_framebuffers`, `m_offscreen_target`, all `VkClearValue` arrays, all `CmdBeginRenderPass`/`vkCmdEndRenderPass` calls.

---

## 6. Automatic Barrier Insertion

### Algorithm

During `Compile()`, after topological sort:

```
for each pass P in execution order:
    for each resource R that P reads:
        W = most recent pass that writes R (in execution order before P)
        if W exists:
            emit barrier before P:
                srcStage  = W's pipeline stage for R
                dstStage  = P's pipeline stage for R
                srcAccess = W's access flags for R
                dstAccess = P's access flags for R
                if R is image:
                    oldLayout = W's output layout for R
                    newLayout = P's required layout for R
```

### Stage and Layout Inference

The graph infers pipeline stages, access flags, and image layouts from how resources are used in pass declarations:

| Usage | Stage | Access | Layout |
|-------|-------|--------|--------|
| Graphics color attachment (write) | `COLOR_ATTACHMENT_OUTPUT` | `COLOR_ATTACHMENT_WRITE` | `COLOR_ATTACHMENT_OPTIMAL` |
| Graphics depth attachment (write) | `EARLY_FRAGMENT_TESTS` | `DEPTH_STENCIL_WRITE` | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` |
| Graphics sampled input (read) | `FRAGMENT_SHADER` | `SHADER_READ` | `SHADER_READ_ONLY_OPTIMAL` |
| Compute storage image (write) | `COMPUTE_SHADER` | `SHADER_WRITE` | `GENERAL` |
| Compute storage image (read) | `COMPUTE_SHADER` | `SHADER_READ` | `GENERAL` |
| Compute storage buffer (read/write) | `COMPUTE_SHADER` | `SHADER_READ`/`WRITE` | N/A |

This eliminates manual `vkCmdPipelineBarrier` calls from technique code entirely.

### Render Pass Creation

For each graphics pass, the graph generates a VkRenderPass from the declared attachments:

1. Color attachment description: format and samples from `ImageDesc`, load/store from declaration, `initialLayout = UNDEFINED`, `finalLayout` based on downstream usage
2. Depth attachment (if declared): same pattern with depth format
3. Resolve attachment (auto-added if `samples > 1`): format from color, `1x` samples, `finalLayout = SHADER_READ_ONLY_OPTIMAL`
4. Single subpass with appropriate attachment references
5. Subpass dependencies for external synchronization

This replaces `RenderPass::CreateOffscreen()` and `RenderPass::CreatePresentation()` with a general-purpose generator.

---

## 7. Implementation Scope

### Phase 1 (this step)

**New files:**

| File | Purpose |
|------|---------|
| `src/rendering/RenderGraph.h` | RenderGraph, PassBuilder, ImageHandle/BufferHandle, PassContext, ImageDesc |
| `src/rendering/RenderGraph.cpp` | Resource allocation, render pass generation, compile, execute |

**Modified files:**

| File | Change |
|------|--------|
| `src/rendering/RenderTechnique.h` | Replace `Init`/`RecordCommands` with `RegisterPasses`, update `OnResize` signature |
| `src/rendering/voxel/DDATracer.h/cpp` | Port to `RegisterPasses` pattern |
| `src/rendering/ComputeTest.h/cpp` | Port to `RegisterPasses` pattern (compute + graphics) |
| `src/rendering/MeshRasterizer.h/cpp` | Port to `RegisterPasses` pattern |
| `src/Application.h/cpp` | Remove render pass/framebuffer/offscreen target management, add RenderGraph, BuildRenderGraph() |

**Potentially removable:**

| File | Reason |
|------|--------|
| `RenderPass::CreateOffscreen()` | Subsumed by graph's render pass generator |
| `RenderPass::CreatePresentation()` | Subsumed by graph's render pass generator |
| `OffscreenTarget` | Subsumed by graph's transient image management |

### Phase 2 (future)

- Dev tooling panel (pass list, per-pass timing, enable/disable toggles)
- Per-pass GPU profiling integrated into graph execution
- Memory aliasing for transient resources with non-overlapping lifetimes
- Render pass caching (dedup identical configurations)
- Graph-level resize handling (technique-transparent)

---

## 8. Verification

1. **Build**: `cmake --build build` — new files discovered by GLOB_RECURSE
2. **DDA Tracer**: renders identically to before (same clear colors, same visual output)
3. **Compute Test**: compute dispatch + graphics display still works
4. **Mesh Rasterizer**: rasterization still works
5. **Renderer switching**: Inspector panel switches between techniques without artifacts
6. **Viewport resize**: resize viewport panel — no crashes, images recreated correctly
7. **Hot-reload**: F5 recompiles shaders and recreates pipelines
8. **GPU profiler**: Metrics panel still shows FPS and render time
