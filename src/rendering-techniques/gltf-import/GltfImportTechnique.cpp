#include "GltfImportTechnique.h"
#include "RenderItem.h"
#include "RenderScene.h"
#include "PipelineDefaults.h"
#include "GltfLoader.h"
#include "MeshIR.h"
#include "PaletteResource.h"
#include "DefaultVoxPalette.h"
#include "config.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace {

// std140 push constant. 4 uint pad slots round up to 16-byte alignment so the
// SPIR-V layout matches the GLSL struct exactly. Total = 96 bytes — well under
// the Vulkan-guaranteed 128-byte push-constant minimum.
struct SkinnedMeshPC {
    glm::mat4  model;            // 64
    glm::vec4  baseColorFactor;  // 16
    uint32_t   firstJoint;       //  4
    uint32_t   jointCount;       //  4
    uint32_t   _pad0;            //  4
    uint32_t   _pad1;            //  4
};
static_assert(sizeof(SkinnedMeshPC) == 96, "SkinnedMeshPC must stay 96 bytes (std140)");

// ---- Per-frame UBO ----
//
// Shared across every pipeline this technique registers (skinned-mesh, voxel
// preview, sky pre-pass). Carries the camera + sky + sun state — the three
// pipelines all need the camera and at least one of (sun, sky, ambient), so
// unifying lets us bind one buffer at slot 0 of every binding table and
// ensures the inspector's lighting edits land on every pipeline at once.
//
// std140: vec3+scalar pairs share 16-byte slots; mat4 fields are naturally
// 16-aligned.
struct GltfImportFrameUbo {
    glm::mat4   viewProj;                                         // 64
    glm::mat4   ndcToWorld;                                       // 64 — sky pre-pass uses this
    glm::vec3   cameraWorldPos;     int32_t _pad0;                // 16
    glm::vec3   sunDirection;       float   sunCosHalfAngle;      // 16
    glm::vec3   sunColor;           float   sunIntensity;         // 16
    glm::vec3   skyColor;           float   ambientIntensity;     // 16
};
static_assert(sizeof(GltfImportFrameUbo) == 192,
    "GltfImportFrameUbo std140 layout drift — update skinned_mesh.{vert,frag}, voxel_preview.{vert,frag}, voxel_preview_sky.frag to match");

// Joint-arena upper bound. The biggest skin in a typical asset (the oak's
// 1311-joint armature) fits comfortably; we'd grow this if a user imports a
// rig dense enough to overflow.
constexpr uint32_t kMaxJointsInArena = 4096;

constexpr const char* kSceneNodeName        = "gltf_import_node";
constexpr const char* kPreviewVolumeName    = "gltf_import_preview_volume";

// ---- Voxel preview per-volume UBO ----
//
// Per-frame data that changes per drawn volume but doesn't fit the push
// constant comfortably. cameraLocalPos is recomputed each frame from
// inverse(model) * cameraWorldPos so the DDA traces a stable ray.

struct VoxelPreviewDrawUbo {
    glm::vec3   cameraLocalPos;     int32_t frameIdx;             // 16
    glm::ivec3  size;               int32_t frameCount;           // 16
    int32_t     maxIterations;      int32_t _pad0; int32_t _pad1; int32_t _pad2;  // 16
};
static_assert(sizeof(VoxelPreviewDrawUbo) == 48,
    "VoxelPreviewDrawUbo std140 layout drift — update voxel_preview.{vert,frag} to match");

struct VoxelPreviewDrawPC {
    glm::mat4 model;                                              // 64
    glm::vec3 aabbMin;  float _pad0;                              // 16
    glm::vec3 aabbMax;  float _pad1;                              // 16
};
static_assert(sizeof(VoxelPreviewDrawPC) == 96,
    "VoxelPreviewDrawPC must stay <= 128 B push-constant minimum");

// DDA iteration cap for the preview shader. The hardcoded 256 covers a 512^3
// volume with margin (a ray crosses at most size.x+size.y+size.z cells).
constexpr int kPreviewMaxIterations = 256;

// Default sky used when no SkyDescription is wired (rare — only during
// detached test paths). Same RGB the engine ships in SkyDescription's default.
constexpr glm::vec3 kFallbackSkyColor = glm::vec3(0.529f, 0.808f, 0.922f);

} // namespace

GltfImportTechnique::GltfImportTechnique() {
    // Inspector parameter list — kept empty in v1 because the BakerPanel
    // owns the import-side UI. The list is referenced by GetParameters()
    // so a future "advanced" toggle (e.g. wireframe) can land here without
    // touching call sites.
    //
    // CPU-side palette is set on the baker as soon as we have a quantizer to
    // build. The GPU-side PaletteResource lives until first RegisterPasses
    // (needs the device handles).
    m_baker.SetPalette(voxel::GetDefaultPalette());
}

GltfImportTechnique::~GltfImportTechnique() {
    // Worker thread joined here so a loop iteration in flight at shutdown
    // doesn't outlive the AssetRegistry the baker may still be reading.
    m_baker.Shutdown();
}

RenderTargetDesc GltfImportTechnique::DescribeTargets(const RendererCaps& caps) const {
    RenderTargetDesc desc{};
    desc.color.format       = caps.swapchainFormat;
    desc.color.samples      = caps.msaaSamples;
    desc.color.needsResolve = (caps.msaaSamples != VK_SAMPLE_COUNT_1_BIT);
    desc.hasDepth     = true;
    desc.depthFormat  = caps.depthFormat;
    desc.depthSamples = caps.msaaSamples;
    return desc;
}

void GltfImportTechnique::RegisterPasses(
    RenderGraph& graph,
    const RenderContext& ctx,
    const TechniqueTargets& targets)
{
    m_device       = ctx.device;
    m_allocator    = ctx.allocator;
    m_graphics_pool = ctx.graphicsPool;
    m_camera       = ctx.camera;
    m_assets       = ctx.assets;
    m_world        = ctx.world;
    m_lighting     = ctx.lighting;
    m_sky          = ctx.sky;
    m_graph        = &graph;
    m_max_frames_in_flight = ctx.maxFramesInFlight;

    auto logger = spdlog::get("Render");

    // First-time setup: if the user already loaded a GLB before the technique
    // was active (e.g. switched workspaces), the asset may exist but the scene
    // node hasn't been built yet. EnsureSceneNode is idempotent.
    if (m_session.hasLoadedAsset) {
        EnsureSceneNode();
    }

    // GPU-side palette resource — built once per technique lifetime. The
    // ImageView/sampler stay valid across graph rebuilds (own VkImage outside
    // the graph).
    if (!m_palette) {
        m_palette = std::make_unique<PaletteResource>(m_device, m_allocator, m_graphics_pool);
        m_palette->Create();
        // Re-seed with the MagicaVoxel default — PaletteResource defaults to
        // the engine palette, which doesn't match the bake's quantizer.
        m_palette->Upload(voxel::GetDefaultPalette().data());
    }
    if (!m_volume_sampler) {
        // R8_UINT volumes require NEAREST filtering — the integer texelFetch
        // in voxel_preview.frag would fail with a linear sampler regardless.
        m_volume_sampler = VWrap::Sampler::CreateNearestClamp(m_device);
    }

    EnsureBakerStarted();
    EnsurePreviewVolumeRegistered();

    CreatePerFrameBuffers(ctx.maxFramesInFlight);

    // ---- Sky pre-pass ----
    //
    // Painted FIRST so the skinned-mesh and voxel passes below can use
    // LoadOp::Load on color and overlay their draws. Reuses sky_fullscreen.vert
    // (NDC strip) + voxel_preview_sky.frag (consumes our shared per-frame
    // UBO). Same trick the InstancedVoxelTechnique uses for its sky.
    m_sky_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
    m_sky_bindings
        ->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
         .BindUniformBufferPerFrame(0, m_frame_ubos, sizeof(GltfImportFrameUbo));
    m_sky_bindings->Build();

    auto& skyPass = graph.AddGraphicsPass("GLB Import Sky");
    skyPass
        .SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
        .SetPipeline([this]() {
            GraphicsPipelineDesc d{};
            d.vertSpvPath = std::string(config::SHADER_DIR) + "/sky_fullscreen.vert.spv";
            d.fragSpvPath = std::string(config::SHADER_DIR) + "/voxel_preview_sky.frag.spv";
            d.descriptorSetLayout = m_sky_bindings->GetLayout();
            d.inputAssembly = PipelineDefaults::TriangleStrip();
            d.rasterizer    = PipelineDefaults::NoCullFill();
            d.depthStencil  = PipelineDefaults::NoDepthTest();
            d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            return d;
        })
        .SetRecord([this](PassContext& pctx) {
            // First pass each frame — owns the per-frame UBO write. The
            // skinned-mesh + voxel passes below can rely on it being already
            // populated. (If pass ordering ever changes, idempotent re-writes
            // in those passes would keep correctness; for now this single
            // write is enough.)
            WriteFrameUbo(pctx.frameIndex);

            auto vk_cmd = pctx.cmd->Get();
            vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pctx.graphicsPipeline->Get());
            VkDescriptorSet ds = m_sky_bindings->GetSet(pctx.frameIndex)->Get();
            vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
            vkCmdDraw(vk_cmd, 4, 1, 0, 0);
        })
        .SetBindings(m_sky_bindings);

    // Binding table for the skinned-mesh pipeline:
    //   binding 0: shared per-frame UBO (camera + sky + sun)
    //   binding 1: joint matrix SSBO
    m_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
    m_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
              .AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
              .BindUniformBufferPerFrame(0, m_frame_ubos, sizeof(GltfImportFrameUbo))
              .BindStorageBufferPerFrame(1, m_joint_ssbos, m_joint_ssbo_size);
    m_bindings->Build();

    auto& meshPass = graph.AddGraphicsPass("GLB Import Skinned Mesh");
    meshPass.AcceptsItemTypes({ RenderItemType::SkinnedMesh });
    meshPass
        // Color load — sky pass already painted. Depth clear since the sky
        // pass had no depth attachment.
        .SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
        .SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::DontCare)
        .SetResolveTarget(targets.resolve)
        .SetPipeline([this]() {
            GraphicsPipelineDesc d{};
            d.vertSpvPath = std::string(config::SHADER_DIR) + "/skinned_mesh.vert.spv";
            d.fragSpvPath = std::string(config::SHADER_DIR) + "/skinned_mesh.frag.spv";
            d.descriptorSetLayout = m_bindings->GetLayout();

            // Vertex layout — must match gltf_import::SkinnedVertex exactly.
            VkVertexInputBindingDescription binding{};
            binding.binding   = 0;
            binding.stride    = sizeof(gltf_import::SkinnedVertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            d.vertexBindings  = { binding };

            std::vector<VkVertexInputAttributeDescription> attrs(5);
            attrs[0].location = 0;
            attrs[0].binding  = 0;
            attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
            attrs[0].offset   = offsetof(gltf_import::SkinnedVertex, position);
            attrs[1].location = 1;
            attrs[1].binding  = 0;
            attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
            attrs[1].offset   = offsetof(gltf_import::SkinnedVertex, normal);
            attrs[2].location = 2;
            attrs[2].binding  = 0;
            attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
            attrs[2].offset   = offsetof(gltf_import::SkinnedVertex, uv);
            attrs[3].location = 3;
            attrs[3].binding  = 0;
            attrs[3].format   = VK_FORMAT_R32G32B32A32_UINT;
            attrs[3].offset   = offsetof(gltf_import::SkinnedVertex, joints);
            attrs[4].location = 4;
            attrs[4].binding  = 0;
            attrs[4].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
            attrs[4].offset   = offsetof(gltf_import::SkinnedVertex, weights);
            d.vertexAttributes = std::move(attrs);

            d.inputAssembly = PipelineDefaults::TriangleList();
            // No back-face culling — the AnimatedOak's foliage is single-
            // sided alpha-cut planes; rendering them double-sided keeps the
            // preview honest. CombinedRenderer's foliage uses the same
            // discipline. Refine in M5 when per-material state lands.
            d.rasterizer = PipelineDefaults::NoCullFill();
            d.depthStencil = PipelineDefaults::DepthTestWrite();
            d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

            VkPushConstantRange r{};
            r.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            r.offset     = 0;
            r.size       = sizeof(SkinnedMeshPC);
            d.pushConstantRanges = { r };

            return d;
        })
        .SetRecord([this](PassContext& pctx) {
            // Idempotent re-write of the per-frame UBO. Sky pass already
            // wrote it earlier this frame; the duplicate keeps correctness
            // independent of pass execution order.
            WriteFrameUbo(pctx.frameIndex);

            auto vk_cmd = pctx.cmd->Get();
            vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());

            VkDescriptorSet ds = m_bindings->GetSet(pctx.frameIndex)->Get();
            vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

            if (!pctx.scene) return;

            // Per-frame bake bookkeeping — debounced re-bakes get fired here
            // and any completed preview gets applied to the registry. Pinned
            // to the SkinnedMesh pass record (runs every frame, regardless of
            // PreviewMode) so the cadence is independent of UI state.
            TickBakeState();

            // Skip the skinned-mesh draws when the user has flipped to
            // Voxels mode. The pass still runs (the clear it's responsible
            // for is what gives the voxel pass a clean depth buffer), it
            // just emits no draws.
            if (m_previewMode != PreviewMode::Mesh) return;

            // Push the entire joint arena to this frame's SSBO in one shot.
            // Per-draw firstJoint slices into the buffer.
            const auto joints = pctx.scene->GetAllJoints();
            const VkDeviceSize jointBytes = joints.size() * sizeof(glm::mat4);
            if (jointBytes > m_joint_ssbo_size) {
                spdlog::get("Render")->error(
                    "GltfImportTechnique: joint arena ({} mats) exceeds SSBO capacity ({}). Skipping draws.",
                    joints.size(), kMaxJointsInArena);
                return;
            }
            if (jointBytes > 0) {
                std::memcpy(m_joint_ssbos_mapped[pctx.frameIndex], joints.data(), jointBytes);
            }

            // Sync the playback state from the asset session into the scene's
            // Component (paused, playbackSpeed, currentTime). This is a
            // one-frame-lag pattern matching MeshRasterizer's rotation update.
            UpdateNodeComponent();

            for (const auto& item : pctx.scene->Get(RenderItemType::SkinnedMesh)) {
                SkinnedMeshPC pc{};
                pc.model           = item.transform;
                pc.baseColorFactor = item.baseColorFactor;
                pc.firstJoint      = item.firstJoint;
                pc.jointCount      = item.jointCount;
                vkCmdPushConstants(vk_cmd, pctx.graphicsPipeline->GetLayout(),
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                DrawSkinnedMeshItem(pctx, item, *m_graph);
            }
        })
        .SetBindings(m_bindings);

    // ============================================================
    // Voxel preview pass — DDAs the baked volume inside the cube
    // rasterized by voxel_preview.{vert,frag}.
    // ============================================================
    //
    // Registered iff (a) the preview volume has been created (i.e. the user
    // has loaded a GLB) and (b) the volume's image handle is valid for this
    // graph. We bypass SceneExtractor / RenderItem here because the technique
    // owns all the state the draw needs (volume handle, mesh-local AABB,
    // camera transform), and the cube-rasterization is a single
    // vkCmdDraw(36, 1, 0, 0) — running it through the RenderItem framework
    // would require a new RenderItemType for one consumer.
    const auto* previewVol = m_assets ? m_assets->GetVoxelVolume(m_session.previewVolume) : nullptr;
    if (previewVol && previewVol->volumeImage.id != UINT32_MAX) {
        // Voxel-specific per-frame UBO — small (48 B) per-volume metadata
        // (cameraLocalPos, frameIdx, size, maxIterations). The shared frame
        // UBO at slot 0 carries everything else.
        m_voxel_draw_ubos.clear();
        m_voxel_draw_ubos_mapped.clear();
        m_voxel_draw_ubos.resize(ctx.maxFramesInFlight);
        m_voxel_draw_ubos_mapped.resize(ctx.maxFramesInFlight);
        for (uint32_t i = 0; i < ctx.maxFramesInFlight; ++i) {
            m_voxel_draw_ubos[i] = VWrap::Buffer::CreateMapped(
                m_allocator,
                sizeof(VoxelPreviewDrawUbo),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m_voxel_draw_ubos_mapped[i]);
        }

        // Voxel binding table:
        //   binding 0: shared per-frame UBO  (frame.viewProj, frame.sun, frame.sky, ...)
        //   binding 1: per-volume draw UBO   (cameraLocalPos, size, maxIterations)
        //   binding 2: usampler3D volume     (graph-managed image)
        //   binding 3: sampler2D palette     (external — owned by PaletteResource)
        m_voxel_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
        m_voxel_bindings
            ->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
             .AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
             .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
             .AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
             .BindUniformBufferPerFrame(0, m_frame_ubos,      sizeof(GltfImportFrameUbo))
             .BindUniformBufferPerFrame(1, m_voxel_draw_ubos, sizeof(VoxelPreviewDrawUbo))
             .BindGraphSampledImage(2, previewVol->volumeImage, m_volume_sampler)
             .BindExternalSampledImage(3, m_palette->GetImageView(), m_palette->GetSampler(),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_voxel_bindings->Build();

        auto& voxelPass = graph.AddGraphicsPass("GLB Import Voxel Preview");
        voxelPass
            // LoadOp::Load on color and depth — the SkinnedMesh pass above
            // already cleared. Voxel cube draws overlay on top.
            .SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
            .SetDepthAttachment(targets.depth, LoadOp::Load, StoreOp::DontCare)
            .SetResolveTarget(targets.resolve)
            .Read(previewVol->volumeImage, ResourceUsage::SampledRead)
            .SetPipeline([this]() {
                GraphicsPipelineDesc d{};
                d.vertSpvPath = std::string(config::SHADER_DIR) + "/voxel_preview.vert.spv";
                d.fragSpvPath = std::string(config::SHADER_DIR) + "/voxel_preview.frag.spv";
                d.descriptorSetLayout = m_voxel_bindings->GetLayout();
                d.inputAssembly = PipelineDefaults::TriangleList();
                // No back-face culling — the camera can be inside the cube
                // when zoomed in close, in which case we need back faces to
                // start the DDA. NoCullFill renders both; the DDA discards
                // misses so the redundancy is invisible.
                d.rasterizer    = PipelineDefaults::NoCullFill();
                d.depthStencil  = PipelineDefaults::DepthTestWrite();
                d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
                VkPushConstantRange r{};
                r.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                r.offset     = 0;
                r.size       = sizeof(VoxelPreviewDrawPC);
                d.pushConstantRanges = { r };
                return d;
            })
            .SetRecord([this](PassContext& pctx) {
                // Skip when not in Voxels mode or before the first bake
                // completes. We still tick the bake state — completion of an
                // in-flight bake should be picked up even when the preview
                // mode is Mesh (so flipping back to Voxels shows the latest
                // bake immediately).
                TickBakeState();
                if (m_previewMode != PreviewMode::Voxels) return;
                if (!m_session.hasBake) return;
                if (!m_node) return;

                // Idempotent re-write of the shared frame UBO — same defense
                // as the skinned-mesh pass, in case pass ordering ever shifts.
                WriteFrameUbo(pctx.frameIndex);

                // Per-volume meta. cameraLocalPos is the camera position in
                // the mesh-local AABB frame — we get it by inverse-transforming
                // the world-space camera position through the SceneNode's
                // current world matrix. (Frame-by-frame: the user can dolly
                // the camera and the volume must continue to look correct.)
                const glm::mat4 model    = m_node->cachedWorld;
                const glm::mat4 invModel = glm::inverse(model);
                const glm::vec3 camWorld = m_camera->GetPosition();
                const glm::vec3 camLocal = glm::vec3(invModel * glm::vec4(camWorld, 1.0f));

                VoxelPreviewDrawUbo draw{};
                draw.cameraLocalPos = camLocal;
                draw.frameIdx       = 0;                        // M3 is single-frame
                draw.size           = glm::ivec3(m_session.previewVolumeSize);
                draw.frameCount     = 1;
                draw.maxIterations  = kPreviewMaxIterations;
                std::memcpy(m_voxel_draw_ubos_mapped[pctx.frameIndex], &draw, sizeof(draw));

                auto vk_cmd = pctx.cmd->Get();
                vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pctx.graphicsPipeline->Get());
                VkDescriptorSet ds = m_voxel_bindings->GetSet(pctx.frameIndex)->Get();
                vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

                VoxelPreviewDrawPC pc{};
                pc.model   = model;
                pc.aabbMin = m_session.previewAabbMin;
                pc.aabbMax = m_session.previewAabbMax;
                vkCmdPushConstants(vk_cmd, pctx.graphicsPipeline->GetLayout(),
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(pc), &pc);

                // Procedural unit cube — voxel_preview.vert reads positions
                // from gl_VertexIndex.
                vkCmdDraw(vk_cmd, 36, 1, 0, 0);
            })
            .SetBindings(m_voxel_bindings);
    }

    logger->debug("GltfImportTechnique: registered passes");
}

void GltfImportTechnique::OnPostCompile(RenderGraph& graph) {
    (void)graph;
    // Asset uploads are handled by AssetRegistry::UploadPending — runs between
    // graph.Compile() and OnPostCompile, so by the time we get here the
    // skinned-mesh per-primitive vertex/index buffers already hold their data.
}

std::vector<std::string> GltfImportTechnique::GetShaderPaths() const {
    return {
        std::string(config::SHADER_DIR) + "/skinned_mesh.vert.spv",
        std::string(config::SHADER_DIR) + "/skinned_mesh.frag.spv",
    };
}

std::vector<TechniqueParameter>& GltfImportTechnique::GetParameters() {
    return m_parameters;
}

FrameStats GltfImportTechnique::GetFrameStats() const {
    FrameStats s{};
    s.drawCalls = static_cast<uint32_t>(m_session.totalPrimitives);
    s.vertices  = static_cast<uint32_t>(m_session.totalVertices);
    s.indices   = static_cast<uint32_t>(m_session.totalTriangles * 3);
    return s;
}

// =============================================================================
// Workspace-facing API
// =============================================================================

void GltfImportTechnique::LoadGlb(const std::string& path) {
    m_pendingLoadPath = path;
    if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
}

void GltfImportTechnique::SelectClip(int clipIndex) {
    m_session.activeClipIndex = clipIndex;
    if (auto* node = m_node) {
        for (auto& c : node->components) {
            if (c.type == ComponentType::SkinnedMesh) {
                if (clipIndex >= 0 && clipIndex < static_cast<int>(m_session.clipAssets.size())) {
                    c.clipAsset = m_session.clipAssets[clipIndex];
                } else {
                    c.clipAsset = AssetID{};
                }
                c.currentTime = 0.0f;
            }
        }
    }
}

void GltfImportTechnique::SetPaused(bool paused) {
    if (auto* node = m_node) {
        for (auto& c : node->components) {
            if (c.type == ComponentType::SkinnedMesh) c.paused = paused;
        }
    }
}

void GltfImportTechnique::SetPlaybackSpeed(float speed) {
    if (auto* node = m_node) {
        for (auto& c : node->components) {
            if (c.type == ComponentType::SkinnedMesh) c.playbackSpeed = speed;
        }
    }
}

void GltfImportTechnique::SetTime(float seconds) {
    if (auto* node = m_node) {
        for (auto& c : node->components) {
            if (c.type == ComponentType::SkinnedMesh) c.currentTime = seconds;
        }
    }
}

float GltfImportTechnique::GetTime() const {
    if (auto* node = m_node) {
        for (const auto& c : node->components) {
            if (c.type == ComponentType::SkinnedMesh) return c.currentTime;
        }
    }
    return 0.0f;
}

void GltfImportTechnique::Reload(const RenderContext& ctx) {
    (void)ctx;
    if (!m_pendingLoadPath.empty()) {
        PerformPendingLoad();
    }
}

// =============================================================================
// Internals
// =============================================================================

void GltfImportTechnique::CreatePerFrameBuffers(uint32_t frames) {
    // Re-allocate on every RegisterPasses (matches MeshRasterizer's pattern;
    // simpler than diffing against the previous frame count). Old buffers
    // unmap when the shared_ptrs drop.
    m_frame_ubos.clear();
    m_frame_ubos_mapped.clear();
    m_joint_ssbos.clear();
    m_joint_ssbos_mapped.clear();
    m_frame_ubos.resize(frames);
    m_frame_ubos_mapped.resize(frames);
    m_joint_ssbos.resize(frames);
    m_joint_ssbos_mapped.resize(frames);

    m_joint_ssbo_size = sizeof(glm::mat4) * kMaxJointsInArena;

    for (uint32_t i = 0; i < frames; ++i) {
        m_frame_ubos[i] = VWrap::Buffer::CreateMapped(
            m_allocator,
            sizeof(GltfImportFrameUbo),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_frame_ubos_mapped[i]);

        m_joint_ssbos[i] = VWrap::Buffer::CreateMapped(
            m_allocator,
            m_joint_ssbo_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_joint_ssbos_mapped[i]);
    }
}

void GltfImportTechnique::PerformPendingLoad() {
    auto logger = spdlog::get("Render");
    const std::string path = std::move(m_pendingLoadPath);
    m_pendingLoadPath.clear();
    if (!m_assets) {
        logger->warn("GltfImportTechnique::PerformPendingLoad called before AssetRegistry was wired");
        return;
    }

    auto irOpt = gltf_import::LoadGlb(path);
    if (!irOpt) {
        logger->warn("GLB load failed: {}", path);
        return;
    }
    auto& ir = *irOpt;
    logger->info("Loaded GLB: {} — {} nodes, {} skins, {} prims, {} animations, {} verts",
        path, ir.nodes.size(), ir.skins.size(), ir.primitives.size(),
        ir.animations.size(), ir.TotalVertices());

    // ---- Build SkinnedMeshAsset ----
    //
    // v1 simplification: only emit primitives that bind to the FIRST skin.
    // Multi-skin assets (e.g. AnimatedOak's three trees) thus render as a
    // single tree for now. The asset still carries every skin in `skins[]`
    // so future per-primitive multi-skin can land without re-importing.
    //
    // If the asset has no skins at all (static mesh), we fall back to a
    // synthetic identity skin so the skinned-mesh pipeline can still draw it
    // (every vertex's weight=1 on joint 0, joint 0's matrix = identity).

    SkinnedMeshAsset asset;
    asset.name       = std::filesystem::path(path).stem().string();
    asset.sourcePath = path;
    asset.nodes      = ir.nodes;
    asset.skins      = ir.skins;

    // Synthetic identity skin so the same draw path renders static GLBs.
    if (asset.skins.empty()) {
        gltf_import::Skin synth;
        synth.name = "(identity)";
        synth.joints.push_back(0);
        synth.inverseBindMatrices.push_back(glm::mat4(1.0f));
        asset.skins.push_back(synth);
    }
    const int activeSkinIdx = 0;

    // Build flat rest-pose TRS arrays parallel to nodes — these are what the
    // SceneExtractor memcpys + mutates per frame, instead of deep-copying
    // the heavy Node[] (which carries std::string + child vector).
    asset.restTranslation.reserve(ir.nodes.size());
    asset.restRotation.reserve(ir.nodes.size());
    asset.restScale.reserve(ir.nodes.size());
    for (const auto& n : ir.nodes) {
        asset.restTranslation.push_back(n.translation);
        asset.restRotation.push_back(n.rotation);
        asset.restScale.push_back(n.scale);
    }

    // Build the active-node mask once — joints + ancestors of skin 0. The
    // SceneExtractor passes this to ComputeWorldMatricesFlat so the BFS
    // skips subtrees we never read from. Stored on the asset so multiple
    // clips reuse the same mask.
    asset.activeNodeMask.assign(ir.nodes.size(), false);
    if (activeSkinIdx < static_cast<int>(asset.skins.size())) {
        for (int jointNode : asset.skins[activeSkinIdx].joints) {
            int n = jointNode;
            while (n >= 0 && n < static_cast<int>(ir.nodes.size()) && !asset.activeNodeMask[n]) {
                asset.activeNodeMask[n] = true;
                n = ir.nodes[n].parent;
            }
        }
    }

    glm::vec3 amin( std::numeric_limits<float>::max());
    glm::vec3 amax(-std::numeric_limits<float>::max());

    // Texture-average helper: when a primitive's material has a baseColorTexture,
    // sample a few representative texels and average them into a flat RGB tint.
    // This is the v1 stand-in for true per-fragment texture sampling — the
    // bark texture averages to a brown, the foliage to a green, so the user
    // sees the asset's intended palette even though the shader still does
    // flat shading. Sampled texels skip transparent ones (alpha < 32) so
    // alpha-cut foliage doesn't average toward the background gutters.
    auto averageBaseColor = [&](int texIndex, glm::vec4 factor) -> glm::vec4 {
        if (texIndex < 0 || texIndex >= static_cast<int>(ir.textures.size())) return factor;
        const auto& tex = ir.textures[texIndex];
        if (tex.rgba8.empty() || tex.width == 0 || tex.height == 0) return factor;

        // 8×8 stratified grid of samples — cheap (64 texels) and dodges any
        // single-texel bias from a corner pixel.
        constexpr int N = 8;
        double r = 0, g = 0, b = 0;
        int counted = 0;
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                uint32_t px = (static_cast<uint32_t>(x) * tex.width)  / N;
                uint32_t py = (static_cast<uint32_t>(y) * tex.height) / N;
                size_t idx = (size_t)(py * tex.width + px) * 4;
                uint8_t a = tex.rgba8[idx + 3];
                if (a < 32) continue;
                r += tex.rgba8[idx + 0];
                g += tex.rgba8[idx + 1];
                b += tex.rgba8[idx + 2];
                ++counted;
            }
        }
        if (counted == 0) return factor;
        glm::vec3 avg(r / counted, g / counted, b / counted);
        avg /= 255.0f;
        return glm::vec4(avg * glm::vec3(factor), factor.a);
    };

    for (const auto& irPrim : ir.primitives) {
        // Only the first skin's primitives in v1.
        if (!ir.skins.empty() && irPrim.skinIndex != activeSkinIdx) continue;

        SkinnedMeshAsset::Primitive p;
        p.vertices       = irPrim.vertices;
        p.indices        = irPrim.indices;
        p.skinIndex      = activeSkinIdx;
        p.ownerNodeIndex = irPrim.ownerNodeIndex;
        if (irPrim.materialIndex >= 0 && irPrim.materialIndex < static_cast<int>(ir.materials.size())) {
            const auto& mat = ir.materials[irPrim.materialIndex];
            p.baseColorFactor = averageBaseColor(mat.baseColorTextureIndex, mat.baseColorFactor);
        }
        amin = glm::min(amin, irPrim.aabbMin);
        amax = glm::max(amax, irPrim.aabbMax);
        asset.primitives.push_back(std::move(p));
    }
    asset.aabbMin = (asset.primitives.empty() ? glm::vec3(0.0f) : amin);
    asset.aabbMax = (asset.primitives.empty() ? glm::vec3(0.0f) : amax);

    if (asset.primitives.empty()) {
        logger->warn("GLB has no primitives bound to skin 0; nothing to render: {}", path);
        return;
    }

    // ---- Replace or register the asset (one slot is reused across imports
    // so AssetID stability is naturally preserved) ----
    bool sizeChanged = false;
    if (m_session.hasLoadedAsset && m_session.meshAsset.valid()) {
        sizeChanged = m_assets->ReplaceSkinnedMesh(m_session.meshAsset, std::move(asset));
    } else {
        m_session.meshAsset = m_assets->RegisterSkinnedMesh(std::move(asset));
        sizeChanged = true;
    }

    // The active-node mask was populated on `asset` above; `asset` itself
    // has been moved into the registry, so re-read the mask through the
    // registry's stored copy. Lookup is O(1) since it's just an array index.
    static const std::vector<bool> kEmptyMask;
    const auto* registered = m_assets->GetSkinnedMesh(m_session.meshAsset);
    const std::vector<bool>& activeMask = registered ? registered->activeNodeMask : kEmptyMask;

    // ---- Replace or register clips ----
    // We keep clip slots equal to ir.animations.size() and either Replace or
    // Register each. Surplus old slots get cleared (best-effort; v1 doesn't
    // free them).
    const size_t newCount = ir.animations.size();
    if (newCount < m_session.clipAssets.size()) {
        for (size_t i = newCount; i < m_session.clipAssets.size(); ++i) {
            m_assets->ClearAnimationClip(m_session.clipAssets[i]);
        }
        m_session.clipAssets.resize(newCount);
    }
    m_session.clipNames.assign(newCount, std::string());
    m_session.clipDurations.assign(newCount, 0.0f);
    for (size_t i = 0; i < newCount; ++i) {
        AnimationClipAsset clip;
        clip.name       = ir.animations[i].name.empty() ? ("anim_" + std::to_string(i)) : ir.animations[i].name;
        clip.sourcePath = path;
        clip.duration   = ir.animations[i].duration;

        // Filter channels by activeMask. Channels targeting nodes outside
        // skin 0's needed set don't affect any rendered vertex.
        size_t kept = 0;
        clip.channels.reserve(ir.animations[i].channels.size());
        for (auto& ch : ir.animations[i].channels) {
            if (ch.targetNode >= 0
             && static_cast<size_t>(ch.targetNode) < activeMask.size()
             && activeMask[ch.targetNode])
            {
                clip.channels.push_back(std::move(ch));
                ++kept;
            }
        }
        logger->info("Clip '{}': filtered {} → {} channels (active-skin mask)",
            clip.name, ir.animations[i].channels.size(), kept);

        m_session.clipNames[i]     = clip.name;
        m_session.clipDurations[i] = clip.duration;
        if (i < m_session.clipAssets.size()) {
            m_assets->ReplaceAnimationClip(m_session.clipAssets[i], std::move(clip));
        } else {
            m_session.clipAssets.push_back(m_assets->RegisterAnimationClip(std::move(clip)));
        }
    }

    // ---- Session metadata ----
    m_session.sourcePath     = path;
    m_session.sourceFileName = std::filesystem::path(path).filename().string();
    m_session.totalNodes     = ir.nodes.size();
    m_session.totalSkins     = ir.skins.size();
    if (const auto* registered = m_assets->GetSkinnedMesh(m_session.meshAsset)) {
        m_session.totalPrimitives = registered->primitives.size();
        size_t verts = 0, tris = 0;
        for (const auto& p : registered->primitives) {
            verts += p.vertices.size();
            tris  += p.indices.size() / 3;
        }
        m_session.totalVertices  = verts;
        m_session.totalTriangles = tris;
    }
    m_session.activeClipIndex = (newCount > 0) ? 0 : -1;
    m_session.hasLoadedAsset  = true;

    EnsureSceneNode();

    // Buffer sizes changed → graph rebuild required so the AssetRegistry can
    // re-declare the per-primitive vertex/index buffers at the new sizes and
    // upload the new bytes. m_assetsRegistered=false flag would also work,
    // but a full RebuildGraph keeps the lifecycle uniform with other
    // techniques' Reload paths.
    if (sizeChanged) {
        if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});
    }
}

void GltfImportTechnique::EnsureSceneNode() {
    if (!m_world) return;
    if (!m_session.meshAsset.valid()) return;

    if (!m_node) {
        m_node = m_world->GetRoot().AddChild(kSceneNodeName);
    }
    // Drop existing components and reattach a fresh SkinnedMesh — simpler than
    // diffing on Replace paths, and the cost is one component vector clear.
    m_node->components.clear();
    Component c{};
    c.type           = ComponentType::SkinnedMesh;
    c.asset          = m_session.meshAsset;
    if (m_session.activeClipIndex >= 0
     && m_session.activeClipIndex < static_cast<int>(m_session.clipAssets.size())) {
        c.clipAsset = m_session.clipAssets[m_session.activeClipIndex];
    }
    c.skinIndex     = 0;
    c.currentTime   = 0.0f;
    c.playbackSpeed = 1.0f;
    c.paused        = false;
    m_node->AddComponent(c);

    // Axis convention: glTF is right-handed Y-up; the engine is Z-up (cf.
    // MeshRasterizer applying the same +90° around X to its OBJ models).
    // Rotating around X by +90° maps glTF +Y → engine +Z and glTF +Z →
    // engine -Y, so the asset stands "up" in the viewport.
    static const glm::quat kYUpToZUp =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    m_node->rotation = kYUpToZUp;

    // Frame the imported asset reasonably: if the AABB is non-empty, scale
    // the node so the longest axis maps to ~10 world units. The AnimatedOak's
    // mesh AABB is roughly 50 units tall in glTF coords; the engine's camera
    // starts ~5 units away.
    if (const auto* asset = m_assets->GetSkinnedMesh(m_session.meshAsset)) {
        glm::vec3 ext = asset->aabbMax - asset->aabbMin;
        float longest = std::max({ ext.x, ext.y, ext.z });
        if (longest > 1e-6f) {
            float k = 10.0f / longest;
            m_node->scale = glm::vec3(k);
            // Center the mesh roughly at world XY by translating by
            // -rotation*center*scale. The Y-up→Z-up rotation has to be
            // composed in here so the AABB centering still hits world origin
            // after the up-axis swap.
            glm::vec3 center = 0.5f * (asset->aabbMax + asset->aabbMin);
            glm::vec3 rotated = kYUpToZUp * center;
            m_node->position = -rotated * k;
        }
    }
}

void GltfImportTechnique::UpdateNodeComponent() {
    // No-op today (the panel writes Component fields directly via the
    // SelectClip / SetPaused / SetPlaybackSpeed / SetTime hooks). The hook is
    // kept so future bake-state mutations have a single funnel.
}

// =============================================================================
// Voxel preview — public API + helpers
// =============================================================================

void GltfImportTechnique::SetVoxelSize(float worldUnits) {
    // Hard floor — sub-millimeter voxels would blow the 512^3 cell budget on
    // any non-tiny mesh. Upper cap is informational; the bake-time budget
    // check handles the real safety.
    constexpr float kMinVoxelSize = 0.001f;
    constexpr float kMaxVoxelSize = 10.0f;
    worldUnits = std::clamp(worldUnits, kMinVoxelSize, kMaxVoxelSize);
    if (worldUnits == m_voxelSizeRequested) return;
    m_voxelSizeRequested  = worldUnits;
    m_voxelSizeDirty      = true;
    m_voxelSizeChangeAt   = std::chrono::steady_clock::now();
}

void GltfImportTechnique::SetPreviewMode(PreviewMode mode) {
    m_previewMode = mode;
}

void GltfImportTechnique::EnsureBakerStarted() {
    if (m_bakerStarted) return;
    m_baker.Start();
    m_bakerStarted = true;
}

void GltfImportTechnique::EnsurePreviewVolumeRegistered() {
    if (!m_assets) return;
    if (m_session.previewVolume.valid()) return;
    // Initial size is a 1^3 placeholder — the first completed bake resizes
    // the procedural volume to the real bake AABB and triggers a graph
    // rebuild to re-allocate the image at the new dims.
    m_session.previewVolume = m_assets->CreateProceduralAnimatedVoxelVolume(
        kPreviewVolumeName, kInitialPreviewSize, /*frameCount=*/1, VK_FORMAT_R8_UINT);
    m_session.previewVolumeSize = kInitialPreviewSize;
}

void GltfImportTechnique::TickBakeState() {
    // ---- Step 1: pick up any completed preview bake ----
    //
    // We poll once per frame regardless of PreviewMode so the latest bake is
    // always reflected on the AssetRegistry — flipping the panel from Mesh
    // to Voxels later then immediately shows the latest bake without a
    // re-bake cycle.
    if (auto completed = m_baker.TakeCompletedPreview()) {
        if (completed->budgetExceeded) {
            // Slider drag would have exceeded the cell budget. Keep the
            // existing bake (if any) on screen and surface the warning to
            // the panel via the session flag.
            m_session.lastBudgetExceeded = true;
            spdlog::get("Render")->warn(
                "GltfImportTechnique: preview bake skipped — cell count exceeded budget at voxelSize={:.4f}",
                completed->voxelSizeWorld);
        } else if (completed->frame.size.x > 0 && !completed->frame.indices.empty()) {
            m_session.lastBudgetExceeded = false;
            m_session.previewAabbMin     = completed->worldOriginMin;
            m_session.previewAabbMax     = completed->worldOriginMax;
            m_session.previewVoxelSize   = completed->voxelSizeWorld;

            const glm::uvec3 newSize = completed->frame.size;
            const bool sizeChanged = (newSize != m_session.previewVolumeSize);
            m_session.previewVolumeSize = newSize;

            // Push the bake bytes into the procedural volume. AssetRegistry's
            // upload path runs after Compile; if size changed, trigger a
            // graph rebuild so the image re-allocates at the new dims first.
            if (auto* vol = m_assets->GetVoxelVolume(m_session.previewVolume)) {
                m_assets->ResizeProceduralVoxelVolume(m_session.previewVolume,
                    newSize, /*newFrameCount=*/1);
                vol->data         = std::move(completed->frame.indices);
                vol->palette      = voxel::GetDefaultPalette();
                vol->needsUpload  = true;
            }
            m_session.hasBake = true;
            if (sizeChanged && m_eventSink) {
                m_eventSink({AppEventType::RebuildGraph});
            }
        }
    }

    // ---- Step 2: fire a debounced re-bake if the slider settled ----
    //
    // The user's drag updates m_voxelSizeRequested + m_voxelSizeChangeAt
    // every frame they're moving. Once the slider has been still for
    // m_debounceMs, we submit one bake (and clear the dirty flag).
    if (!m_voxelSizeDirty) return;
    if (!m_session.hasLoadedAsset) return;
    if (!m_session.previewVolume.valid()) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_voxelSizeChangeAt).count();
    if (elapsed < m_debounceMs) return;

    SubmitPreviewBake();
    m_voxelSizeDirty = false;
}

void GltfImportTechnique::WriteFrameUbo(uint32_t frameIndex) {
    if (frameIndex >= m_frame_ubos_mapped.size()) return;
    GltfImportFrameUbo ubo{};
    ubo.viewProj   = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
    ubo.ndcToWorld = glm::inverse(ubo.viewProj);
    ubo.cameraWorldPos = m_camera->GetPosition();

    // Sun + sky default to "noon overhead with the standard SkyDescription"
    // when the technique runs detached from a Scene (rare — only test paths).
    if (m_lighting) {
        ubo.sunDirection    = m_lighting->GetSunDirection();
        ubo.sunCosHalfAngle = m_lighting->GetSunCosHalfAngle();
        ubo.sunColor        = glm::vec3(m_lighting->sunColor[0],
                                        m_lighting->sunColor[1],
                                        m_lighting->sunColor[2]);
        ubo.sunIntensity    = m_lighting->sunIntensity;
        ubo.ambientIntensity = m_lighting->ambientIntensity;
    } else {
        ubo.sunDirection    = glm::normalize(glm::vec3(0.3f, 0.2f, 1.0f));
        ubo.sunCosHalfAngle = std::cos(glm::radians(0.75f));
        ubo.sunColor        = glm::vec3(1.0f, 0.98f, 0.92f);
        ubo.sunIntensity    = 1.0f;
        ubo.ambientIntensity = 0.35f;
    }
    ubo.skyColor = m_sky ? m_sky->color : kFallbackSkyColor;
    std::memcpy(m_frame_ubos_mapped[frameIndex], &ubo, sizeof(ubo));
}

void GltfImportTechnique::SubmitPreviewBake() {
    if (!m_assets) return;
    if (!m_session.meshAsset.valid()) return;
    AssetID clipId{};
    if (m_session.activeClipIndex >= 0
     && m_session.activeClipIndex < static_cast<int>(m_session.clipAssets.size()))
    {
        clipId = m_session.clipAssets[m_session.activeClipIndex];
    }

    voxel_bake::PreviewBakeJob job;
    if (!voxel_bake::BuildSnapshot(*m_assets, m_session.meshAsset, clipId, /*skinIndex=*/0,
                                   job.snapshot))
    {
        spdlog::get("Render")->warn("GltfImportTechnique: BuildSnapshot failed; bake cancelled");
        return;
    }
    job.time             = GetTime();
    job.voxelSizeWorld   = m_voxelSizeRequested;
    job.colorSource.mode = voxel_bake::VoxColorSource::Mode::MaterialBaseColor;
    job.maxGridCells     = m_maxGridCells;

    m_baker.SubmitPreview(std::move(job));
}
