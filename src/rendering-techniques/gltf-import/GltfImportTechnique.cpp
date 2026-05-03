#include "GltfImportTechnique.h"
#include "RenderItem.h"
#include "RenderScene.h"
#include "PipelineDefaults.h"
#include "GltfLoader.h"
#include "MeshIR.h"
#include "config.h"

#include <glm/gtc/quaternion.hpp>
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

struct CameraUBO {
    glm::mat4 view;
    glm::mat4 proj;
};

// Joint-arena upper bound. The biggest skin in a typical asset (the oak's
// 1311-joint armature) fits comfortably; we'd grow this if a user imports a
// rig dense enough to overflow.
constexpr uint32_t kMaxJointsInArena = 4096;

constexpr const char* kSceneNodeName = "gltf_import_node";

} // namespace

GltfImportTechnique::GltfImportTechnique() {
    // Inspector parameter list — kept empty in v1 because the BakerPanel
    // owns the import-side UI. The list is referenced by GetParameters()
    // so a future "advanced" toggle (e.g. wireframe) can land here without
    // touching call sites.
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
    m_graph        = &graph;
    m_max_frames_in_flight = ctx.maxFramesInFlight;

    auto logger = spdlog::get("Render");

    // First-time setup: if the user already loaded a GLB before the technique
    // was active (e.g. switched workspaces), the asset may exist but the scene
    // node hasn't been built yet. EnsureSceneNode is idempotent.
    if (m_session.hasLoadedAsset) {
        EnsureSceneNode();
    }

    CreatePerFrameBuffers(ctx.maxFramesInFlight);

    // Binding table:
    //   binding 0: camera UBO (per-frame)
    //   binding 1: joint matrix SSBO (per-frame)
    m_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
    m_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
              .AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
              .BindUniformBufferPerFrame(0, m_camera_ubos, sizeof(CameraUBO))
              .BindStorageBufferPerFrame(1, m_joint_ssbos, m_joint_ssbo_size);
    m_bindings->Build();

    auto& meshPass = graph.AddGraphicsPass("GLB Import Skinned Mesh");
    meshPass.AcceptsItemTypes({ RenderItemType::SkinnedMesh });
    meshPass
        .SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0.05f, 0.07f, 0.1f, 1.0f)
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
            auto vk_cmd = pctx.cmd->Get();
            vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());

            VkDescriptorSet ds = m_bindings->GetSet(pctx.frameIndex)->Get();
            vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

            // Per-frame camera UBO.
            CameraUBO cam{};
            cam.view = m_camera->GetViewMatrix();
            cam.proj = m_camera->GetProjectionMatrix();
            std::memcpy(m_camera_ubos_mapped[pctx.frameIndex], &cam, sizeof(cam));

            if (!pctx.scene) return;

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
    m_camera_ubos.clear();
    m_camera_ubos_mapped.clear();
    m_joint_ssbos.clear();
    m_joint_ssbos_mapped.clear();
    m_camera_ubos.resize(frames);
    m_camera_ubos_mapped.resize(frames);
    m_joint_ssbos.resize(frames);
    m_joint_ssbos_mapped.resize(frames);

    m_joint_ssbo_size = sizeof(glm::mat4) * kMaxJointsInArena;

    for (uint32_t i = 0; i < frames; ++i) {
        m_camera_ubos[i] = VWrap::Buffer::CreateMapped(
            m_allocator,
            sizeof(CameraUBO),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_camera_ubos_mapped[i]);

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
