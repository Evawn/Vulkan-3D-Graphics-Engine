#include "GltfImportTechnique.h"
#include "RenderItem.h"
#include "RenderScene.h"
#include "PipelineDefaults.h"
#include "GltfLoader.h"
#include "MeshIR.h"
#include "PaletteResource.h"
#include "DefaultVoxPalette.h"
#include "VoxAnimFormat.h"
#include "DescriptorSetLayout.h"
#include "config.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace {

// std140 push constant. M6 grew this twice — first by 16 bytes for alpha-test
// (alphaCutoff + alphaMode + pad), then it absorbed `meshAlpha` into the
// trailing pad word for the Overlay crossfade (no size change). Total = 112
// bytes — still well under the 128-byte push-constant minimum.
struct SkinnedMeshPC {
    glm::mat4  model;            // 64
    glm::vec4  baseColorFactor;  // 16
    uint32_t   firstJoint;       //  4
    uint32_t   jointCount;       //  4
    float      alphaCutoff;      //  4
    uint32_t   alphaMode;        //  4   (0=Opaque, 1=Mask, 2=Blend)
    // Overlay crossfade weight: 1 in Mesh-only, (1 - overlayBlend) in
    // Overlay. The shader multiplies its existing per-fragment alpha by
    // this so opaque parts of the mesh get ghosted alongside any
    // alpha-tested foliage.
    float      meshAlpha;        //  4
    uint32_t   _pad1;            //  4
    uint32_t   _pad2;            //  4
    uint32_t   _pad3;            //  4
};
static_assert(sizeof(SkinnedMeshPC) == 112, "SkinnedMeshPC must stay 112 bytes (std140)");

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
    // Direct-lighting controls — mirror SceneLighting's aoStrength /
    // shadowsEnabled so the import preview shades to the same lighting model
    // as InstancedVoxel / BrickmapPalette. shadowsEnabled is an int because
    // GLSL's std140 has no native bool.
    float       aoStrength;         int32_t shadowsEnabled;
    int32_t     _pad1;              int32_t _pad2;                // 16
};
static_assert(sizeof(GltfImportFrameUbo) == 208,
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
    // Trailing pad of each vec3 row repurposed: the second pad now carries
    // `voxelAlpha` for the Overlay crossfade (1 in Voxels-only,
    // overlayBlend in Overlay). The first stays a pad — keeping the row
    // shape so std140 in GLSL still parses cleanly.
    glm::vec3 aabbMin;  float _pad0;                              // 16
    glm::vec3 aabbMax;  float voxelAlpha;                         // 16
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
    if (!m_base_color_sampler) {
        // M6: shared linear-repeat sampler for every skinned-mesh primitive's
        // base-color texture (set 1, binding 0). REPEAT matches the glTF
        // default sampler convention; LINEAR is what almost every glTF asset
        // expects for color textures. Future per-texture sampler config (per
        // glTF sampler) would need a small cache keyed on (filter × wrap).
        m_base_color_sampler = VWrap::Sampler::CreateLinearRepeat(m_device);
    }
    if (!m_material_set_layout) {
        // Build the descriptor-set layout for set 1 (per-primitive material).
        // Only one binding for v1: the base-color combined image+sampler. The
        // asset registry uses this exact layout when constructing each
        // primitive's BindingTable, so the layouts match at descriptor-set
        // bind time. Future PBR work adds more bindings here in lockstep
        // with the registry's per-primitive table builder.
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        m_material_set_layout = VWrap::DescriptorSetLayout::Create(m_device, { b });
    }
    // Register the material context with the registry so its texture-upload
    // pass can build per-primitive material BindingTables. Idempotent — same
    // sampler / framesInFlight is a no-op; new values flip every skinned
    // mesh's needsTextureUpload to true so they re-bind on the next
    // UploadPending tick.
    if (m_assets) {
        m_assets->SetSkinnedMeshMaterialContext(m_base_color_sampler, ctx.maxFramesInFlight);
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

    // Binding table for the skinned-mesh pipeline (set 0):
    //   binding 0: shared per-frame UBO (camera + sky + sun)
    //   binding 1: joint matrix SSBO
    //
    // M6 split set 1 out into per-primitive material BindingTables (owned by
    // each SkinnedMeshAsset::Primitive in the registry) — sampled per-pixel
    // for the base-color texture. Set 0 stays per-pass.
    m_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
    m_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
              .AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
              .BindUniformBufferPerFrame(0, m_frame_ubos, sizeof(GltfImportFrameUbo))
              .BindStorageBufferPerFrame(1, m_joint_ssbos, m_joint_ssbo_size);
    m_bindings->Build();

    // ---- Skinned-mesh draw is split into two passes: cull / no-cull ----
    //
    // Foliage cards are intrinsically double-sided (a single quad seen from
    // either side), and bark is single-sided (interior is hidden by the
    // outside). glTF marks this per-material as `doubleSided`. We honor it
    // by routing each primitive's draw through one of two pipelines that
    // differ only in cullMode — same shaders, same descriptor layouts, same
    // push-constants.
    //
    // Splitting into two passes (instead of one pass with two pipelines)
    // matches the graph's "one pass = one pipeline" idiom. Both passes
    // accept SkinnedMesh items; each filters by item.doubleSided in its
    // record callback. The cull pass runs first and is responsible for the
    // depth clear + per-frame joint upload + bake tick (which were folded
    // into the old single skinned-mesh pass's record callback). The no-cull
    // pass loads depth so foliage and bark interleave correctly on Z.
    //
    // The pipeline factory is shared: same SPIR-V, same layouts, same vertex
    // input. Only the rasterizer state differs.
    auto buildSkinnedMeshPipelineDesc = [this](bool doubleSided) {
        GraphicsPipelineDesc d{};
        d.vertSpvPath = std::string(config::SHADER_DIR) + "/skinned_mesh.vert.spv";
        d.fragSpvPath = std::string(config::SHADER_DIR) + "/skinned_mesh.frag.spv";
        // Multi-set descriptor layout: set 0 = frame UBO + joint SSBO,
        // set 1 = per-primitive material (base-color texture). Index in
        // the vector matches the shader's `layout(set = N, ...)`.
        d.descriptorSetLayouts = { m_bindings->GetLayout(), m_material_set_layout };

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
        d.rasterizer    = doubleSided
            ? PipelineDefaults::NoCullFill()
            : PipelineDefaults::BackCullFill();
        d.depthStencil  = PipelineDefaults::DepthTestWrite();
        d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

        // Overlay mode flips the blend state to alpha-over so the mesh ghosts
        // over whatever the previous pass painted. In Mesh-only mode the
        // shader emits alpha=1 anyway, so leaving blending opaque saves a
        // vector blend op per fragment.
        if (m_previewMode == PreviewMode::Overlay) {
            d.colorBlendAttachment = PipelineDefaults::AlphaOverBlend();
        }

        // Push constant covers vertex (model + factor + joints) AND fragment
        // (alphaCutoff + alphaMode + factor.a + overlay alpha). We declare
        // the range over both stages — it's still the same memory, just
        // visible from both.
        VkPushConstantRange r{};
        r.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        r.offset     = 0;
        r.size       = sizeof(SkinnedMeshPC);
        d.pushConstantRanges = { r };
        return d;
    };

    // The shared per-pass record callback — parameterized on which subset of
    // SkinnedMesh items this pass owns. `wantDoubleSided=false` is the cull
    // pass (also handles the per-frame joint upload + bake tick).
    auto recordSkinnedMeshPass = [this](PassContext& pctx, bool wantDoubleSided) {
        WriteFrameUbo(pctx.frameIndex);

        auto vk_cmd = pctx.cmd->Get();
        vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pctx.graphicsPipeline->Get());

        VkDescriptorSet ds = m_bindings->GetSet(pctx.frameIndex)->Get();
        vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

        if (!pctx.scene) return;

        if (!wantDoubleSided) {
            // The cull pass owns the once-per-frame bookkeeping. The no-cull
            // pass runs after; by then joints are already in the SSBO.
            TickBakeState();
        }

        // Skip the mesh draw only in pure Voxels mode. Overlay mode keeps
        // the mesh on so it can be crossfaded with the voxel layer.
        if (m_previewMode == PreviewMode::Voxels) return;

        // Per-pass alpha for the Overlay crossfade. In non-Overlay modes
        // meshAlpha = 1 and the alpha-over blend equation degrades to a
        // pure overwrite.
        const float meshAlpha = (m_previewMode == PreviewMode::Overlay)
                              ? (1.0f - m_overlayBlend)
                              : 1.0f;

        if (!wantDoubleSided) {
            // Joint arena upload — same once-per-frame placement.
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
            UpdateNodeComponent();
        }

        for (const auto& item : pctx.scene->Get(RenderItemType::SkinnedMesh)) {
            // Route each item to the matching pass — items destined for the
            // other pipeline are skipped here and picked up by the sibling
            // pass on the same frame.
            if (item.doubleSided != wantDoubleSided) continue;

            // ---- Bind set 1 (per-primitive material) ----
            //
            // The asset's per-primitive BindingTable has setCount =
            // ctx.maxFramesInFlight, so picking GetSet(frameIndex) gives a
            // valid descriptor for the current frame. If the primitive
            // somehow lacks a material (no texture and no fallback got
            // built — shouldn't happen post-upload, but defensive), skip
            // the draw to avoid a NULL descriptor binding.
            if (!item.materialBindings) continue;
            VkDescriptorSet matDs = item.materialBindings->GetSet(pctx.frameIndex)->Get();
            vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pctx.graphicsPipeline->GetLayout(), /*firstSet=*/1, 1, &matDs, 0, nullptr);

            SkinnedMeshPC pc{};
            pc.model           = item.transform;
            pc.baseColorFactor = item.baseColorFactor;
            pc.firstJoint      = item.firstJoint;
            pc.jointCount      = item.jointCount;
            pc.alphaCutoff     = item.alphaCutoff;
            pc.alphaMode       = static_cast<uint32_t>(item.alphaMode);
            pc.meshAlpha       = meshAlpha;
            vkCmdPushConstants(vk_cmd, pctx.graphicsPipeline->GetLayout(),
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(pc), &pc);
            DrawSkinnedMeshItem(pctx, item, *m_graph);
        }
    };

    // ---- Pass 1: cull (single-sided primitives, e.g. bark) ----
    auto& meshPassCull = graph.AddGraphicsPass("GLB Import Skinned Mesh (Cull)");
    meshPassCull.AcceptsItemTypes({ RenderItemType::SkinnedMesh });
    meshPassCull
        .SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
        .SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::Store)
        .SetResolveTarget(targets.resolve)
        .SetPipeline([buildSkinnedMeshPipelineDesc]() {
            return buildSkinnedMeshPipelineDesc(/*doubleSided=*/false);
        })
        .SetRecord([this, recordSkinnedMeshPass](PassContext& pctx) {
            recordSkinnedMeshPass(const_cast<PassContext&>(pctx), /*wantDoubleSided=*/false);
        })
        .SetBindings(m_bindings);

    // ---- Pass 2: no-cull (double-sided primitives, e.g. foliage cards) ----
    //
    // Loads the depth from the cull pass so foliage and bark interleave
    // correctly on Z. Color is also Load (the cull pass already wrote the
    // bark fragments). Both write to the same color attachment — alpha-test
    // discards mean foliage cards punch through to whatever bark is behind
    // them in the depth buffer.
    auto& meshPassNoCull = graph.AddGraphicsPass("GLB Import Skinned Mesh (NoCull)");
    meshPassNoCull.AcceptsItemTypes({ RenderItemType::SkinnedMesh });
    meshPassNoCull
        .SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
        .SetDepthAttachment(targets.depth, LoadOp::Load, StoreOp::DontCare)
        .SetResolveTarget(targets.resolve)
        .SetPipeline([buildSkinnedMeshPipelineDesc]() {
            return buildSkinnedMeshPipelineDesc(/*doubleSided=*/true);
        })
        .SetRecord([this, recordSkinnedMeshPass](PassContext& pctx) {
            recordSkinnedMeshPass(const_cast<PassContext&>(pctx), /*wantDoubleSided=*/true);
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
                // Overlay mode disables depth-test so the voxel layer always
                // contributes regardless of where the mesh pass wrote depth.
                // Without this the crossfade goes patchy at t=0.5 because
                // half the asset's pixels mesh-occlude the voxel layer and
                // half don't, depending on which surface happened to be
                // closer at that fragment. Voxels-only mode keeps the
                // standard depth-test+write so gl_FragDepth from
                // voxel_preview.frag still integrates with anything later.
                d.depthStencil  = (m_previewMode == PreviewMode::Overlay)
                                ? PipelineDefaults::NoDepthTest()
                                : PipelineDefaults::DepthTestWrite();
                if (m_previewMode == PreviewMode::Overlay) {
                    d.colorBlendAttachment = PipelineDefaults::AlphaOverBlend();
                }
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
                // Skip the voxel draw only in pure Mesh mode. Overlay keeps
                // the voxel layer on so it can be crossfaded with the mesh.
                if (m_previewMode == PreviewMode::Mesh) return;
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

                // Multi-frame frameIdx: when a full bake is active, the
                // baked volume animates synchronously with the SkinnedMesh
                // component's currentTime. The conversion uses the bake's
                // recorded fps so playback matches what was actually baked
                // (independent of the panel's playbackSpeed slider, which
                // already affects currentTime upstream in SceneExtractor).
                int frameIdx   = 0;
                int frameCount = 1;
                if (m_session.hasFullBake && m_session.bakeFrameCount > 0 && m_session.bakeFps > 0.0f) {
                    frameCount = static_cast<int>(m_session.bakeFrameCount);
                    const float t = GetTime();    // already wrapped to clip duration upstream
                    int idx = static_cast<int>(std::floor(t * m_session.bakeFps));
                    if (idx < 0) idx = 0;
                    frameIdx = idx % frameCount;
                }

                VoxelPreviewDrawUbo draw{};
                draw.cameraLocalPos = camLocal;
                draw.frameIdx       = frameIdx;
                draw.size           = glm::ivec3(m_session.previewVolumeSize);
                draw.frameCount     = frameCount;
                draw.maxIterations  = kPreviewMaxIterations;
                std::memcpy(m_voxel_draw_ubos_mapped[pctx.frameIndex], &draw, sizeof(draw));

                auto vk_cmd = pctx.cmd->Get();
                vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pctx.graphicsPipeline->Get());
                VkDescriptorSet ds = m_voxel_bindings->GetSet(pctx.frameIndex)->Get();
                vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

                // Per-pass alpha for the Overlay crossfade. In Voxels-only
                // mode voxelAlpha = 1 and the alpha-over blend equation
                // (when present) degrades to a pure overwrite.
                const float voxelAlpha = (m_previewMode == PreviewMode::Overlay)
                                       ? m_overlayBlend
                                       : 1.0f;

                VoxelPreviewDrawPC pc{};
                pc.model   = model;
                pc.aabbMin = m_session.previewAabbMin;
                pc.aabbMax = m_session.previewAabbMax;
                pc.voxelAlpha = voxelAlpha;
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
    // Move the IR into a shared_ptr<const> immediately. The bake worker reads
    // from this — `SkinnedMeshAsset` is a parallel view we'll build below for
    // the runtime mesh draw, but the bake pipeline doesn't go through it.
    // (See M5 plan: IR is the canonical fan-out point.)
    auto irShared = std::make_shared<gltf_import::MeshIR>(std::move(*irOpt));
    const gltf_import::MeshIR& ir = *irShared;
    logger->info("Loaded GLB: {} — {} nodes, {} skins, {} prims, {} animations, {} verts",
        path, ir.nodes.size(), ir.skins.size(), ir.primitives.size(),
        ir.animations.size(), ir.TotalVertices());

    // Material diagnostics — each material's alpha policy + texture binding.
    // Useful when a bake produces unexpected colors (e.g. mostly-black foliage
    // = leaf material in Blend mode pulling gutter pixels through bilinear
    // filtering). Logged once per import, cheap regardless of asset size.
    auto modeName = [](gltf_import::Material::AlphaMode m) {
        switch (m) {
            case gltf_import::Material::AlphaMode::Opaque: return "Opaque";
            case gltf_import::Material::AlphaMode::Mask:   return "Mask";
            case gltf_import::Material::AlphaMode::Blend:  return "Blend";
        }
        return "?";
    };
    for (size_t i = 0; i < ir.materials.size(); ++i) {
        const auto& m = ir.materials[i];
        logger->info("  mat[{}] '{}': alpha={} cutoff={:.3f} factor=({:.3f},{:.3f},{:.3f},{:.3f}) tex={}",
            i, m.name, modeName(m.alphaMode), m.alphaCutoff,
            m.baseColorFactor.r, m.baseColorFactor.g, m.baseColorFactor.b, m.baseColorFactor.a,
            m.baseColorTextureIndex);
    }

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

    // ---- M6: drop the averageBaseColor hack ----
    //
    // Pre-M6 the runtime mesh shader was textureless, so we needed a flat
    // tint per primitive — sampling 8×8 texels and folding the average into
    // baseColorFactor was the stand-in. With set 1 wired (per-primitive
    // baseColorTexture sampled per-pixel), the shader does `sample × factor`
    // and the average is no longer needed. Asset's baseColorFactor now
    // carries the literal glTF factor.

    for (const auto& irPrim : ir.primitives) {
        // Only the first skin's primitives in v1.
        if (!ir.skins.empty() && irPrim.skinIndex != activeSkinIdx) continue;

        SkinnedMeshAsset::Primitive p;
        p.vertices       = irPrim.vertices;
        p.indices        = irPrim.indices;
        p.skinIndex      = activeSkinIdx;
        p.ownerNodeIndex = irPrim.ownerNodeIndex;

        // RAW glTF material data — fragment shader handles the sample × factor
        // multiplication and the alpha-cut. A primitive without a material
        // (rare; usually only test assets) keeps the defaults: white factor,
        // no texture, opaque, single-sided.
        if (irPrim.materialIndex >= 0
         && irPrim.materialIndex < static_cast<int>(ir.materials.size())) {
            const auto& mat = ir.materials[irPrim.materialIndex];
            p.baseColorFactor       = mat.baseColorFactor;
            p.baseColorTextureIndex = mat.baseColorTextureIndex;
            p.alphaMode             = mat.alphaMode;
            p.alphaCutoff           = mat.alphaCutoff;
            p.doubleSided           = mat.doubleSided;
        }

        amin = glm::min(amin, irPrim.aabbMin);
        amax = glm::max(amax, irPrim.aabbMax);
        asset.primitives.push_back(std::move(p));
    }

    // ---- Stage IR textures into the asset's pendingTextures[] ----
    //
    // The asset stores its own copy of the texel bytes so the registry's
    // texture-upload pass (`UploadSkinnedMeshTextures`) can run independently
    // of whether the technique is still holding the IR. The registry walks
    // pendingTextures[] → GpuTexture[] (Vulkan images + views) and frees
    // pendingTextures only as part of the asset's lifecycle.
    asset.pendingTextures.reserve(ir.textures.size());
    for (const auto& tex : ir.textures) {
        SkinnedMeshAsset::PendingTexture pt;
        pt.width  = tex.width;
        pt.height = tex.height;
        pt.rgba8  = tex.rgba8;     // copy; the IR survives anyway, but the
                                    // asset's pending storage outlives any
                                    // re-import that swaps the IR pointer.
        asset.pendingTextures.push_back(std::move(pt));
    }
    asset.needsTextureUpload = !asset.pendingTextures.empty();
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
    m_session.totalTextures   = ir.textures.size();

    // Stash the IR for the bake worker. From this point on, any in-flight
    // bake holds its own shared_ptr to the *previous* IR (if any) — keeping
    // its pose/texture data alive until the bake completes — while subsequent
    // jobs use the new IR. No mutex; the IR is frozen at construction.
    m_meshIR = std::move(irShared);

    // If the user already had Texture mode selected from a previous asset
    // and the new asset has no textures, fall back to Material so the bake
    // doesn't quietly degrade. The radio in the panel is greyed in this case
    // so the state stays consistent.
    if (m_session.totalTextures == 0
     && m_session.colorSource.mode == voxel_bake::VoxColorSource::Mode::TextureSampled)
    {
        m_session.colorSource.mode = voxel_bake::VoxColorSource::Mode::MaterialBaseColor;
    }

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
    if (mode == m_previewMode) return;

    // Crossing the Overlay boundary changes pipeline state (alpha-over blend
    // for both passes; depth-test off for the voxel pass so it always
    // contributes regardless of mesh depth). Pipeline state is baked at
    // graph-build time, so trigger a graph rebuild on enter/leave.
    //
    // Posting RebuildGraph directly (not ReloadTechnique) — Reload() is a
    // no-op when there's no pending GLB load, so a ReloadTechnique event
    // would silently do nothing here and the new pipeline state would never
    // be picked up. RebuildGraph runs the technique's RegisterPasses again,
    // and the pipeline factory closures read m_previewMode at that time.
    const bool wasOverlay = (m_previewMode == PreviewMode::Overlay);
    const bool willOverlay = (mode == PreviewMode::Overlay);
    m_previewMode = mode;
    if (wasOverlay != willOverlay && m_eventSink) {
        m_eventSink({AppEventType::RebuildGraph});
    }
}

void GltfImportTechnique::SetOverlayBlend(float t) {
    m_overlayBlend = std::clamp(t, 0.0f, 1.0f);
}

void GltfImportTechnique::SetColorSource(voxel_bake::VoxColorSource::Mode mode) {
    if (m_session.colorSource.mode == mode) return;
    m_session.colorSource.mode = mode;
    // Reuse the voxel-size debounce path: flip dirty + restart the timer so
    // the next TickBakeState fires a re-bake after the standard debounce
    // interval. Avoids a second "color-source-changed" submit pipeline.
    m_voxelSizeDirty    = true;
    m_voxelSizeChangeAt = std::chrono::steady_clock::now();
}

void GltfImportTechnique::SetSamplesPerVoxel(int n) {
    n = std::clamp(n, 1, voxel_bake::kMaxSamplesPerVoxel);
    if (n == m_session.samplesPerVoxel) return;
    m_session.samplesPerVoxel = n;
    // Same debounce trick as SetColorSource — fold into the existing
    // dirty-flag path so the panel doesn't need a parallel submit pipeline.
    m_voxelSizeDirty    = true;
    m_voxelSizeChangeAt = std::chrono::steady_clock::now();
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
            //
            // A new preview bake also collapses any prior full bake — the
            // user has explicitly re-tuned the grid, so the multi-frame
            // animation no longer matches the single new pose. Resize back
            // to frameCount=1; both the slot and the AssetID are preserved.
            const bool wasFullBake = m_session.hasFullBake;
            if (auto* vol = m_assets->GetVoxelVolume(m_session.previewVolume)) {
                m_assets->ResizeProceduralVoxelVolume(m_session.previewVolume,
                    newSize, /*newFrameCount=*/1);
                vol->data         = std::move(completed->frame.indices);
                vol->palette      = voxel::GetDefaultPalette();
                vol->needsUpload  = true;
            }
            m_session.hasBake        = true;
            m_session.hasFullBake    = false;
            m_session.bakeFrameCount = 1;
            if ((sizeChanged || wasFullBake) && m_eventSink) {
                m_eventSink({AppEventType::RebuildGraph});
            }
        }
    }

    // ---- Step 1b: pick up any completed full bake ----
    //
    // The full bake produces N frames of identical grid size. We morph the
    // procedural volume in-place: ResizeProceduralVoxelVolume re-stamps the
    // size + frameCount on the same AssetID, then we Z-pack every frame's
    // bytes into one contiguous blob and let UploadPending push it next
    // graph cycle. The image's depth changes (size.z * N vs. size.z * 1) so
    // a graph rebuild is mandatory.
    if (auto fullBake = m_baker.TakeCompletedFullBake()) {
        spdlog::get("Render")->info(
            "TickBakeState: full bake result picked up — frames={} size=({},{},{}) budgetExceeded={} cancelled={}",
            fullBake->frames.size(),
            fullBake->frameSize.x, fullBake->frameSize.y, fullBake->frameSize.z,
            fullBake->budgetExceeded, fullBake->cancelled);
        if (fullBake->budgetExceeded) {
            m_session.lastBudgetExceeded   = true;
            // The worker logs the specific cause (per-frame cells, total
            // bytes, or packed-depth limit). The panel shows a generic
            // pointer to the log; we don't try to mirror the worker's
            // detailed reason here because that'd duplicate the policy in
            // two places. The tip covers all three guards uniformly.
            m_session.lastBakeStatusMessage =
                "Bake exceeded a budget (cells / bytes / packed-depth) — see log. "
                "Try: coarser voxel size, fewer fps, or shorter range.";
            spdlog::get("Render")->warn("GltfImportTechnique: full bake skipped — budget exceeded");
        } else if (fullBake->frames.empty() || fullBake->frameSize.x == 0) {
            // Worker returned an empty/invalid result without flagging it as
            // budget-exceeded. Surface the cause so the panel doesn't just go
            // silent when something upstream (no quantizer, AABB collapse,
            // etc.) bails the bake out.
            m_session.lastBakeStatusMessage =
                "Bake produced no frames — check log (no quantizer / invalid AABB / posing failure).";
        } else if (!fullBake->frames.empty() && fullBake->frameSize.x > 0) {
            m_session.lastBudgetExceeded = false;
            const glm::uvec3 newSize = fullBake->frameSize;
            const uint32_t   newFrames = static_cast<uint32_t>(fullBake->frames.size());

            const bool sizeChanged   = (newSize != m_session.previewVolumeSize);
            const bool framesChanged = (newFrames != m_session.bakeFrameCount);

            m_assets->ResizeProceduralVoxelVolume(m_session.previewVolume, newSize, newFrames);

            if (auto* vol = m_assets->GetVoxelVolume(m_session.previewVolume)) {
                const size_t bytesPerFrame = static_cast<size_t>(newSize.x) * newSize.y * newSize.z;
                vol->data.assign(bytesPerFrame * newFrames, 0);
                for (uint32_t i = 0; i < newFrames; ++i) {
                    if (fullBake->frames[i].indices.size() >= bytesPerFrame) {
                        std::memcpy(vol->data.data() + i * bytesPerFrame,
                                    fullBake->frames[i].indices.data(),
                                    bytesPerFrame);
                    }
                }
                vol->palette     = voxel::GetDefaultPalette();
                vol->needsUpload = true;
            }

            m_session.previewVolumeSize    = newSize;
            m_session.previewVoxelSize     = fullBake->voxelSizeWorld;
            m_session.previewAabbMin       = fullBake->worldOriginMin;
            m_session.previewAabbMax       = fullBake->worldOriginMax;
            m_session.hasBake              = true;
            m_session.hasFullBake          = true;
            m_session.bakeFrameCount       = newFrames;
            m_session.bakeFps              = fullBake->fps;
            m_session.lastBakeStatusMessage = "Bake complete — " + std::to_string(newFrames) + " frames.";
            m_session.lastSaveSucceeded    = false;
            m_session.lastSavedManifestPath.clear();

            // Image depth changed → graph rebuild required to re-allocate the
            // VkImage at (size.z * frameCount). Even if frameCount stayed the
            // same, a size change still requires the rebuild; only a same-
            // size, same-frameCount full bake (rare in practice) could skip.
            if (sizeChanged || framesChanged) {
                if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});
            }

            // Switch into Voxels view so the user immediately sees the bake.
            // (If they were in Mesh while a long bake ran, jump them over.)
            m_previewMode = PreviewMode::Voxels;
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

    // The preview supersedes any active full bake — once the user moves the
    // voxel-size slider, they're back in interactive exploration mode and
    // any in-flight long bake is no longer what they want. Discard it and
    // fall back to single-frame preview.
    if (m_session.hasFullBake) {
        m_session.hasFullBake    = false;
        m_session.bakeFrameCount = 0;
        m_baker.CancelFullBake();
    }
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
        ubo.aoStrength      = m_lighting->aoStrength;
        ubo.shadowsEnabled  = m_lighting->shadowsEnabled ? 1 : 0;
    } else {
        ubo.sunDirection    = glm::normalize(glm::vec3(0.3f, 0.2f, 1.0f));
        ubo.sunCosHalfAngle = std::cos(glm::radians(0.75f));
        ubo.sunColor        = glm::vec3(1.0f, 0.98f, 0.92f);
        ubo.sunIntensity    = 1.0f;
        ubo.ambientIntensity = 0.35f;
        ubo.aoStrength      = 1.0f;
        ubo.shadowsEnabled  = 1;
    }
    ubo.skyColor = m_sky ? m_sky->color : kFallbackSkyColor;
    std::memcpy(m_frame_ubos_mapped[frameIndex], &ubo, sizeof(ubo));
}

void GltfImportTechnique::SubmitPreviewBake() {
    if (!m_meshIR) {
        spdlog::get("Render")->warn("GltfImportTechnique: no MeshIR loaded; preview bake skipped");
        return;
    }

    voxel_bake::PreviewBakeJob job;
    job.meshIR               = m_meshIR;          // shared_ptr copy — refcount keeps the IR alive
    job.clipIndex            = m_session.activeClipIndex;
    job.skinIndex            = 0;
    job.time                 = GetTime();
    job.voxelSizeWorld       = m_voxelSizeRequested;
    job.colorSource          = m_session.colorSource;
    job.samplesPerVoxel      = m_session.samplesPerVoxel;
    job.maxGridCellsPerFrame = m_maxGridCells;

    m_baker.SubmitPreview(std::move(job));
}

// =============================================================================
// Full-bake API (M4)
// =============================================================================

void GltfImportTechnique::StartFullBake(float startTime, float endTime, float fps) {
    auto logger = spdlog::get("Render");
    if (logger) logger->info("StartFullBake: enter (start={:.3f} end={:.3f} fps={:.1f})", startTime, endTime, fps);
    if (!m_meshIR) {
        if (logger) logger->warn("StartFullBake: no MeshIR loaded — bake aborted");
        m_session.lastBakeStatusMessage = "No mesh loaded.";
        return;
    }
    if (m_session.activeClipIndex < 0
     || m_session.activeClipIndex >= static_cast<int>(m_session.clipDurations.size()))
    {
        if (logger) logger->warn("StartFullBake: activeClipIndex={} (clipDurations={})",
            m_session.activeClipIndex, m_session.clipDurations.size());
        m_session.lastBakeStatusMessage = "No active clip selected.";
        return;
    }

    // Clamp range to clip duration; keep at least one frame.
    const float duration = m_session.clipDurations[m_session.activeClipIndex];
    startTime = std::clamp(startTime, 0.0f, duration);
    endTime   = std::clamp(endTime,   startTime, duration);
    fps       = std::clamp(fps, 1.0f, 240.0f);

    voxel_bake::FullBakeJob job;
    job.meshIR              = m_meshIR;
    job.clipIndex           = m_session.activeClipIndex;
    job.skinIndex           = 0;
    if (logger) logger->info("StartFullBake: IR ready ({} prims, {} animations)",
        m_meshIR->primitives.size(), m_meshIR->animations.size());
    job.startTime           = startTime;
    job.endTime             = endTime;
    job.fps                 = fps;
    job.voxelSizeWorld      = m_voxelSizeRequested;
    job.colorSource         = m_session.colorSource;
    job.samplesPerVoxel     = m_session.samplesPerVoxel;
    job.maxGridCellsPerFrame = m_maxGridCells;
    // Total-bytes budget — defaults to 1 GB across all frames (R8_UINT == 1
    // byte/voxel). Prevents a "high-res long bake" from quietly allocating
    // tens of GB on the worker thread before failing.
    job.maxTotalBytes        = 1ull * 1024 * 1024 * 1024;

    m_session.bakeRangeStart       = startTime;
    m_session.bakeRangeEnd         = endTime;
    m_session.bakeFps              = fps;
    m_session.bakeStartedAtSeconds = static_cast<float>(
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
    m_session.lastBakeStatusMessage = "Submitted bake job…";
    m_session.lastSaveSucceeded    = false;
    m_session.lastSavedManifestPath.clear();

    // Defensive: if the baker thread hasn't started yet (panel click landed
    // before the first RegisterPasses for this technique), the job would sit
    // in m_pendingFull forever and the user would see "nothing happens".
    EnsureBakerStarted();

    const uint64_t gen = m_baker.SubmitFullBake(std::move(job));
    if (logger) logger->info("StartFullBake: submitted gen={} (worker started={})", gen, m_bakerStarted);
}

void GltfImportTechnique::CancelFullBake() {
    m_baker.CancelFullBake();
    m_session.lastBakeStatusMessage = "Bake cancelled.";
}

bool GltfImportTechnique::SaveBake(const std::string& directory, const std::string& name) {
    if (!m_session.hasFullBake || m_session.bakeFrameCount == 0) {
        m_session.lastSaveSucceeded = false;
        m_session.lastBakeStatusMessage = "No full bake to save.";
        return false;
    }
    if (!m_assets) return false;
    const auto* vol = m_assets->GetVoxelVolume(m_session.previewVolume);
    if (!vol || vol->data.empty()) {
        m_session.lastSaveSucceeded = false;
        m_session.lastBakeStatusMessage = "Bake bytes not available — re-bake and retry.";
        return false;
    }

    // Slice the contiguous Z-slab buffer back into per-frame VoxFrames for
    // the writer. We deliberately don't keep the per-frame vector alive in
    // memory after the bake completes — the procedural volume's `data` is
    // the source of truth, so this slicing happens once at save time.
    const glm::uvec3 size = m_session.previewVolumeSize;
    const size_t bytesPerFrame = static_cast<size_t>(size.x) * size.y * size.z;
    if (bytesPerFrame == 0) {
        m_session.lastSaveSucceeded = false;
        m_session.lastBakeStatusMessage = "Bake has zero size.";
        return false;
    }
    if (vol->data.size() < bytesPerFrame * m_session.bakeFrameCount) {
        m_session.lastSaveSucceeded = false;
        m_session.lastBakeStatusMessage = "Bake byte count smaller than expected.";
        return false;
    }

    std::vector<voxel_bake::VoxFrame> frames(m_session.bakeFrameCount);
    for (uint32_t i = 0; i < m_session.bakeFrameCount; ++i) {
        frames[i].size = size;
        frames[i].indices.assign(
            vol->data.data() + i * bytesPerFrame,
            vol->data.data() + (i + 1) * bytesPerFrame);
    }

    const bool ok = voxel_bake::WriteVxa(
        directory, name,
        m_session.bakeFrameCount,
        m_session.bakeFps,
        m_session.previewVoxelSize,
        m_session.previewAabbMin,
        m_session.previewAabbMax,
        frames,
        voxel::GetDefaultPalette());

    m_session.lastSaveSucceeded = ok;
    if (ok) {
        m_session.lastSavedManifestPath =
            (std::filesystem::path(directory) / (name + ".vxa")).string();
        m_session.lastBakeStatusMessage = "Saved to " + m_session.lastSavedManifestPath;
    } else {
        m_session.lastBakeStatusMessage = "Save failed (see log).";
    }
    return ok;
}

bool GltfImportTechnique::LoadBakeFromDisk(const std::string& vxaPath) {
    if (!m_assets) return false;
    auto loaded = voxel_bake::LoadVxa(vxaPath);
    if (!loaded) {
        m_session.lastBakeStatusMessage = "Load failed (see log).";
        return false;
    }

    EnsurePreviewVolumeRegistered();   // creates the slot if it doesn't exist yet

    const glm::uvec3 size = loaded->manifest.size;
    const uint32_t frames = loaded->manifest.frameCount;
    m_assets->ResizeProceduralVoxelVolume(m_session.previewVolume, size, frames);
    if (auto* vol = m_assets->GetVoxelVolume(m_session.previewVolume)) {
        vol->data        = std::move(loaded->framesData);
        vol->palette     = loaded->palette;
        vol->needsUpload = true;
    }

    m_session.previewVolumeSize    = size;
    m_session.previewVoxelSize     = loaded->manifest.voxelSizeWorld;
    m_session.previewAabbMin       = loaded->manifest.originWorldMin;
    m_session.previewAabbMax       = loaded->manifest.originWorldMax;
    m_session.hasBake              = true;
    m_session.hasFullBake          = (frames > 1);
    m_session.bakeFrameCount       = frames;
    m_session.bakeFps              = loaded->manifest.fps;
    m_session.bakeRangeStart       = 0.0f;
    m_session.bakeRangeEnd         = (loaded->manifest.fps > 0.0f && frames > 0)
        ? (static_cast<float>(frames - 1) / loaded->manifest.fps) : 0.0f;
    m_session.lastBakeStatusMessage = "Loaded " + vxaPath;
    m_session.lastSaveSucceeded    = false;
    m_session.lastSavedManifestPath = vxaPath;
    m_session.lastBudgetExceeded   = false;

    // Switch to Voxels view so the load is immediately visible.
    m_previewMode = PreviewMode::Voxels;

    if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});
    return true;
}
