#include "AnimationBaker.h"
#include "AnimationEvaluator.h"
#include "PaletteQuantizer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace voxel_bake {

namespace {

// ---- BakeContext ----
//
// Per-job, per-worker scratch derived once from the IR + skin index. Holds
// everything that's invariant across pose iterations of the same job:
//
//   - rest TRS arrays (one entry per IR node) — copies of node.translation /
//     rotation / scale, parallel to meshIR->nodes
//   - active-node mask — joints + ancestors of the chosen skin (BFS-pruned
//     world-matrix walks consume this)
//   - filtered primitive index list — only primitives whose skinIndex matches
//     the job's skinIndex (other primitives belong to other skins or are
//     static; v1 skips them)
//   - flat UV arrays per filtered primitive — extracted from the IR's
//     interleaved SkinnedVertex once, then reused across every per-frame
//     VoxelizePrimitive build (UVs don't animate)
//
// Building this once per job (vs per-pose) keeps the inner pose loop
// allocation-free even for full bakes spanning hundreds of frames.

struct BakeContext {
    std::vector<glm::vec3> restT;
    std::vector<glm::quat> restR;
    std::vector<glm::vec3> restS;
    std::vector<bool>      activeMask;

    // Indices into meshIR->primitives that pass the skinIndex filter, in
    // order. Other arrays below are parallel to this list.
    std::vector<int>                       primIndices;
    std::vector<std::vector<glm::vec2>>    uvs;          // per filtered prim, length = vertexCount (empty if absent)
    std::vector<glm::vec4>                 baseColorFactors;
    std::vector<const gltf_import::Texture*> baseColorTextures;
    std::vector<gltf_import::Material::AlphaMode> alphaModes;
    std::vector<float>                     alphaCutoffs;
};

// Extract the UV stream for one IR primitive into a flat array. Returns empty
// if every UV is (0,0) — the GltfLoader's default for primitives without
// TEXCOORD_0 — so the sampler can branch on `uvs.empty()` later.
std::vector<glm::vec2> ExtractUvs(const gltf_import::Primitive& p) {
    std::vector<glm::vec2> out;
    bool any = false;
    out.reserve(p.vertices.size());
    for (const auto& v : p.vertices) {
        out.push_back(v.uv);
        if (v.uv != glm::vec2(0.0f)) any = true;
    }
    if (!any) out.clear();
    return out;
}

// Pick an IR skin to drive the bake. v1 honors the requested index; if it's
// out of range or the IR has no skins, we fall through to a synthetic
// identity skin (one joint = identity matrix) so static meshes still
// voxelize.
struct ResolvedSkin {
    int                    chosenIndex   = -1;       // -1 = synthetic identity
    std::vector<int>       joints;                   // indexes into meshIR.nodes (or singleton {0} for synthetic)
    std::vector<glm::mat4> inverseBindMatrices;
    bool                   synthetic     = false;
};

ResolvedSkin ResolveSkin(const gltf_import::MeshIR& ir, int requested) {
    ResolvedSkin r;
    if (!ir.skins.empty()) {
        r.chosenIndex = (requested >= 0 && requested < static_cast<int>(ir.skins.size()))
            ? requested : 0;
        const auto& sk = ir.skins[r.chosenIndex];
        r.joints              = sk.joints;
        r.inverseBindMatrices = sk.inverseBindMatrices;
        return r;
    }
    // Static mesh fallback: treat node 0 as the only joint, identity bind.
    r.synthetic = true;
    r.joints.push_back(0);
    r.inverseBindMatrices.push_back(glm::mat4(1.0f));
    return r;
}

// Build the active-node mask: joints + ancestors of `skin` set to true.
// Used by ComputeWorldMatricesFlat to prune the BFS to nodes whose world
// transforms feed any voxelized vertex.
void BuildActiveMask(const std::vector<gltf_import::Node>& nodes,
                     const std::vector<int>& joints,
                     std::vector<bool>& outMask)
{
    outMask.assign(nodes.size(), false);
    for (int jointNode : joints) {
        int n = jointNode;
        while (n >= 0 && n < static_cast<int>(nodes.size()) && !outMask[n]) {
            outMask[n] = true;
            n = nodes[n].parent;
        }
    }
}

BakeContext BuildContext(const gltf_import::MeshIR& ir,
                         int                        requestedSkin)
{
    BakeContext ctx;

    // Rest TRS, parallel to ir.nodes.
    ctx.restT.reserve(ir.nodes.size());
    ctx.restR.reserve(ir.nodes.size());
    ctx.restS.reserve(ir.nodes.size());
    for (const auto& n : ir.nodes) {
        ctx.restT.push_back(n.translation);
        ctx.restR.push_back(n.rotation);
        ctx.restS.push_back(n.scale);
    }

    const ResolvedSkin sk = ResolveSkin(ir, requestedSkin);
    BuildActiveMask(ir.nodes, sk.joints, ctx.activeMask);

    // Primitive filter: only keep primitives bound to the chosen skin (or
    // every primitive when the IR has no skins / synthetic identity skin
    // was used).
    for (size_t i = 0; i < ir.primitives.size(); ++i) {
        const auto& p = ir.primitives[i];
        if (!sk.synthetic && !ir.skins.empty() && p.skinIndex != sk.chosenIndex) continue;
        ctx.primIndices.push_back(static_cast<int>(i));

        ctx.uvs.push_back(ExtractUvs(p));

        glm::vec4 factor(1.0f);
        const gltf_import::Texture* tex = nullptr;
        gltf_import::Material::AlphaMode mode = gltf_import::Material::AlphaMode::Opaque;
        float cutoff = 0.5f;
        if (p.materialIndex >= 0 && p.materialIndex < static_cast<int>(ir.materials.size())) {
            const auto& m = ir.materials[p.materialIndex];
            factor = m.baseColorFactor;     // RAW factor — texture average is a runtime concern, not a bake concern
            mode   = m.alphaMode;
            cutoff = m.alphaCutoff;
            if (m.baseColorTextureIndex >= 0
             && m.baseColorTextureIndex < static_cast<int>(ir.textures.size())) {
                tex = &ir.textures[m.baseColorTextureIndex];
            }
        }
        ctx.baseColorFactors.push_back(factor);
        ctx.baseColorTextures.push_back(tex);
        ctx.alphaModes.push_back(mode);
        ctx.alphaCutoffs.push_back(cutoff);
    }
    return ctx;
}

// ---- PoseAndSkin ----
//
// Evaluate the chosen clip at `time`, compute world matrices, build joint
// matrices for the active skin, then CPU-skin every filtered primitive's
// positions into mesh-local world space. Output is parallel to
// `ctx.primIndices`: `outPosed[k]` is the deformed position array for
// `ir.primitives[ctx.primIndices[k]]`.
//
// Scratch is caller-owned (PoseScratch). For full bakes, the same scratch is
// reused across every frame call so the pose loop is allocation-free after
// the first frame.

struct PoseScratch {
    std::vector<glm::vec3> trsT;
    std::vector<glm::quat> trsR;
    std::vector<glm::vec3> trsS;
    std::vector<glm::mat4> worlds;
    std::vector<glm::mat4> jointMats;
    ResolvedSkin           skin;
    bool                   skinResolved = false;
};

bool PoseAndSkin(const gltf_import::MeshIR&          ir,
                 const BakeContext&                  ctx,
                 int                                 clipIndex,
                 int                                 requestedSkin,
                 float                                time,
                 PoseScratch&                         scratch,
                 std::vector<std::vector<glm::vec3>>& outPosed)
{
    if (ctx.primIndices.empty()) return false;

    // Resolve the skin once and cache it on scratch — same skin across every
    // pose call within a job.
    if (!scratch.skinResolved) {
        scratch.skin = ResolveSkin(ir, requestedSkin);
        scratch.skinResolved = true;
    }

    scratch.trsT = ctx.restT;
    scratch.trsR = ctx.restR;
    scratch.trsS = ctx.restS;

    // Apply animation channels if a clip was selected.
    if (clipIndex >= 0 && clipIndex < static_cast<int>(ir.animations.size())) {
        const auto& anim = ir.animations[clipIndex];
        if (!anim.channels.empty() && anim.duration > 0.0f) {
            gltf_import::EvaluateChannelsFlat(
                anim.channels, anim.duration, time,
                scratch.trsT, scratch.trsR, scratch.trsS);
        }
    }

    const std::vector<bool>* maskPtr = ctx.activeMask.empty() ? nullptr : &ctx.activeMask;
    gltf_import::ComputeWorldMatricesFlat(ir.nodes,
                                          scratch.trsT, scratch.trsR, scratch.trsS,
                                          scratch.worlds, maskPtr);

    // Mesh-node frame for skinning. Per glTF spec, a skinned vertex's world
    // position is computed in the mesh node's frame using
    //   world = inverse(meshNodeWorld) * jointWorld * inverseBindMatrix * vertexPos
    // The first filtered primitive's owner gives us the mesh node convention
    // (matches SceneExtractor's EmitSkinnedMesh).
    glm::mat4 meshNodeWorld(1.0f);
    if (!ctx.primIndices.empty()) {
        const auto& p0 = ir.primitives[ctx.primIndices.front()];
        if (p0.ownerNodeIndex >= 0
         && p0.ownerNodeIndex < static_cast<int>(scratch.worlds.size())) {
            meshNodeWorld = scratch.worlds[p0.ownerNodeIndex];
        }
    }

    gltf_import::Skin synthSkin;
    synthSkin.joints              = scratch.skin.joints;
    synthSkin.inverseBindMatrices = scratch.skin.inverseBindMatrices;
    gltf_import::ComputeJointMatrices(synthSkin, scratch.worlds, meshNodeWorld, scratch.jointMats);

    // CPU-skin every filtered primitive. Mirrors the runtime vertex shader's
    // pos' = Σ weight[i] * jointMat[joint[i]] * pos.
    static const glm::mat4 kIdent(1.0f);
    outPosed.resize(ctx.primIndices.size());
    for (size_t k = 0; k < ctx.primIndices.size(); ++k) {
        const auto& p = ir.primitives[ctx.primIndices[k]];
        auto& out = outPosed[k];
        out.resize(p.vertices.size());

        if (scratch.skin.synthetic) {
            // Static fallback: identity skin → just copy positions.
            for (size_t vi = 0; vi < p.vertices.size(); ++vi) {
                out[vi] = p.vertices[vi].position;
            }
            continue;
        }

        for (size_t vi = 0; vi < p.vertices.size(); ++vi) {
            const auto& v = p.vertices[vi];
            const auto safeMat = [&](uint32_t idx) -> const glm::mat4& {
                return (idx < scratch.jointMats.size()) ? scratch.jointMats[idx] : kIdent;
            };
            glm::mat4 skinM = v.weights.x * safeMat(v.joints.x)
                            + v.weights.y * safeMat(v.joints.y)
                            + v.weights.z * safeMat(v.joints.z)
                            + v.weights.w * safeMat(v.joints.w);
            out[vi] = glm::vec3(skinM * glm::vec4(v.position, 1.0f));
        }
    }
    return true;
}

// ---- ComputeClipWideAabb ----
//
// Sample K poses across the clip duration, union the deformed-mesh AABBs,
// pad by `padVoxels * voxelSize`. K=32 is enough for slow-moving foliage;
// faster-moving rigs may want higher K.
//
// Cancellation: checks the cancel atomic between samples. Returns the
// partial AABB if cancelled — callers will discard the bake anyway.

AabbSample ComputeClipWideAabb(const gltf_import::MeshIR& ir,
                               const BakeContext&         ctx,
                               int                        clipIndex,
                               int                        requestedSkin,
                               uint32_t                   sampleCount,
                               float                      padVoxels,
                               float                      voxelSize,
                               const std::atomic<bool>&   cancel)
{
    AabbSample box;
    if (sampleCount == 0) return box;

    PoseScratch scratch;
    std::vector<std::vector<glm::vec3>> posed;

    float duration = 0.0f;
    if (clipIndex >= 0 && clipIndex < static_cast<int>(ir.animations.size())) {
        duration = ir.animations[clipIndex].duration;
    }
    const uint32_t k = std::max<uint32_t>(1, sampleCount);

    for (uint32_t i = 0; i < k; ++i) {
        if (cancel.load(std::memory_order_relaxed)) return box;
        const float t = (duration > 0.0f && k > 1)
            ? duration * (static_cast<float>(i) / static_cast<float>(k - 1))
            : 0.0f;
        if (!PoseAndSkin(ir, ctx, clipIndex, requestedSkin, t, scratch, posed)) return box;
        for (const auto& prim : posed) {
            for (const glm::vec3& p : prim) box.Include(p);
        }
    }

    if (box.valid() && voxelSize > 0.0f) {
        const float pad = padVoxels * voxelSize;
        box.min -= glm::vec3(pad);
        box.max += glm::vec3(pad);
    }
    return box;
}

// ---- BuildVoxelizePrimitives ----
//
// Adapter from BakeContext + posed positions → VoxelizePrimitive[]. Reused
// by RunPreview and per-frame inside RunFullBake.
//
// Pointer borrowing: VoxelizePrimitive holds raw pointers into the posed
// positions array, the BakeContext's UV arrays, and the IR's textures /
// indices. All three are valid for the duration of the Voxelize() call —
// the IR shared_ptr keeps textures/indices alive, the context owns the UVs,
// and the caller owns `posedPositions`.

void BuildVoxelizePrimitives(const gltf_import::MeshIR&                ir,
                             const BakeContext&                        ctx,
                             const std::vector<std::vector<glm::vec3>>& posedPositions,
                             std::vector<VoxelizePrimitive>&            outVp)
{
    outVp.resize(ctx.primIndices.size());
    for (size_t k = 0; k < ctx.primIndices.size(); ++k) {
        const auto& p = ir.primitives[ctx.primIndices[k]];
        VoxelizePrimitive& v = outVp[k];
        v.positions        = posedPositions[k].data();
        v.vertexCount      = posedPositions[k].size();
        v.uvs              = ctx.uvs[k].empty() ? nullptr : ctx.uvs[k].data();
        v.indices          = p.indices.data();
        v.indexCount       = p.indices.size();
        v.baseColorFactor  = ctx.baseColorFactors[k];
        v.baseColorTexture = ctx.baseColorTextures[k];
        v.alphaMode        = ctx.alphaModes[k];
        v.alphaCutoff      = ctx.alphaCutoffs[k];
    }
}

// ---- Cell-budget check ----
//
// Returns the per-frame cell count, and writes `exceeded=true` if it's over
// the budget. Caller decides whether to bail or proceed.

uint64_t GridCellsForExtent(const glm::vec3& extent, float voxelSize, uint64_t budget, bool& exceeded) {
    const uint64_t cells =
          static_cast<uint64_t>(std::ceil(extent.x / voxelSize))
        * static_cast<uint64_t>(std::ceil(extent.y / voxelSize))
        * static_cast<uint64_t>(std::ceil(extent.z / voxelSize));
    exceeded = cells > budget;
    return cells;
}

} // namespace

AnimationBaker::AnimationBaker() = default;

AnimationBaker::~AnimationBaker() {
    Shutdown();
}

void AnimationBaker::Start() {
    if (m_worker.joinable()) return;
    m_shutdown.store(false, std::memory_order_relaxed);
    m_worker = std::thread([this] { WorkerLoop(); });
}

void AnimationBaker::Shutdown() {
    if (!m_worker.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_shutdown.store(true, std::memory_order_relaxed);
        if (m_activePreviewCancel) m_activePreviewCancel->store(true, std::memory_order_relaxed);
        if (m_activeFullCancel)    m_activeFullCancel   ->store(true, std::memory_order_relaxed);
    }
    m_cv.notify_all();
    m_worker.join();
}

void AnimationBaker::SetPalette(const std::array<uint8_t, 256 * 4>& palette) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_quantizer = std::make_unique<PaletteQuantizer>(palette);
}

uint64_t AnimationBaker::SubmitPreview(PreviewBakeJob job) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_activePreviewCancel) m_activePreviewCancel->store(true, std::memory_order_relaxed);
    job.generation = ++m_previewGenCounter;
    const uint64_t gen = job.generation;
    m_pendingPreview = std::move(job);
    m_completedPreview.reset();
    m_cv.notify_one();
    return gen;
}

uint64_t AnimationBaker::SubmitFullBake(FullBakeJob job) {
    std::lock_guard<std::mutex> lk(m_mutex);
    job.generation = ++m_fullGenCounter;
    const uint64_t gen = job.generation;
    const float duration = std::max(0.0f, job.endTime - job.startTime);
    const int   total    = std::max(1, static_cast<int>(std::round(duration * job.fps)) + 1);
    m_fullFramesTotal.store(total, std::memory_order_relaxed);
    m_fullFramesDone .store(0,     std::memory_order_relaxed);
    m_pendingFull = std::move(job);
    m_completedFull.reset();
    m_cv.notify_one();
    return gen;
}

void AnimationBaker::CancelFullBake() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_activeFullCancel) m_activeFullCancel->store(true, std::memory_order_relaxed);
    m_pendingFull.reset();
}

std::optional<PreviewBakeResult> AnimationBaker::TakeCompletedPreview() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_completedPreview) return std::nullopt;
    auto out = std::move(*m_completedPreview);
    m_completedPreview.reset();
    return out;
}

std::optional<FullBakeResult> AnimationBaker::TakeCompletedFullBake() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_completedFull) return std::nullopt;
    auto out = std::move(*m_completedFull);
    m_completedFull.reset();
    return out;
}

void AnimationBaker::WorkerLoop() {
    auto logger = spdlog::get("Render");
    while (true) {
        // ---- Drain one preview job (if any) ----
        PreviewBakeJob previewJob;
        bool havePreview = false;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] {
                return m_shutdown.load(std::memory_order_relaxed)
                    || m_pendingPreview.has_value()
                    || m_pendingFull.has_value();
            });
            if (m_shutdown.load(std::memory_order_relaxed)) return;
            if (m_pendingPreview) {
                previewJob = std::move(*m_pendingPreview);
                m_pendingPreview.reset();
                havePreview = true;
                m_previewInFlight.store(true, std::memory_order_relaxed);
            }
        }

        if (havePreview) {
            std::atomic<bool> cancel{false};
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_activePreviewCancel = &cancel;
            }

            PreviewBakeResult result = RunPreview(previewJob, cancel);

            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_activePreviewCancel = nullptr;
                m_previewInFlight.store(false, std::memory_order_relaxed);
                if (!cancel.load(std::memory_order_relaxed)) {
                    m_completedPreview = std::move(result);
                }
            }
        }

        // ---- Drain one full-bake job (if any) ----
        FullBakeJob fullJob;
        bool haveFull = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_pendingFull) {
                fullJob = std::move(*m_pendingFull);
                m_pendingFull.reset();
                haveFull = true;
                m_fullInFlight.store(true, std::memory_order_relaxed);
            }
        }
        if (haveFull) {
            std::atomic<bool> cancel{false};
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_activeFullCancel = &cancel;
            }

            FullBakeResult result = RunFullBake(fullJob, cancel);

            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_activeFullCancel = nullptr;
                m_fullInFlight.store(false, std::memory_order_relaxed);
                if (!cancel.load(std::memory_order_relaxed)) {
                    m_completedFull = std::move(result);
                }
            }

            if (logger) {
                logger->info("AnimationBaker: full bake gen={} {} ({} frames @ {:.4f}m)",
                    fullJob.generation,
                    cancel.load() ? "cancelled" : "completed",
                    m_fullFramesDone.load(),
                    fullJob.voxelSizeWorld);
            }
        }
    }
}

PreviewBakeResult AnimationBaker::RunPreview(PreviewBakeJob& job, std::atomic<bool>& cancel) const {
    PreviewBakeResult result{};
    result.generation     = job.generation;
    result.voxelSizeWorld = job.voxelSizeWorld;

    if (!m_quantizer) return result;
    if (!job.meshIR)  return result;
    const auto& ir = *job.meshIR;
    if (ir.primitives.empty()) return result;

    BakeContext ctx = BuildContext(ir, job.skinIndex);
    if (ctx.primIndices.empty()) return result;

    PoseScratch scratch;
    std::vector<std::vector<glm::vec3>> posed;
    if (!PoseAndSkin(ir, ctx, job.clipIndex, job.skinIndex, job.time, scratch, posed)) return result;
    if (cancel.load(std::memory_order_relaxed)) return result;

    AabbSample box;
    for (const auto& prim : posed) {
        for (const glm::vec3& p : prim) box.Include(p);
    }
    if (!box.valid()) return result;
    box.min -= glm::vec3(job.voxelSizeWorld);
    box.max += glm::vec3(job.voxelSizeWorld);

    result.worldOriginMin = box.min;
    result.worldOriginMax = box.max;

    bool budgetExceeded = false;
    GridCellsForExtent(box.max - box.min, job.voxelSizeWorld,
                       job.maxGridCellsPerFrame, budgetExceeded);
    if (budgetExceeded) {
        result.budgetExceeded = true;
        return result;
    }

    std::vector<VoxelizePrimitive> vp;
    BuildVoxelizePrimitives(ir, ctx, posed, vp);

    VoxelizeInput in;
    in.primitives     = vp.data();
    in.primitiveCount = vp.size();
    in.worldOriginMin = box.min;
    in.worldOriginMax = box.max;
    in.voxelSizeWorld = job.voxelSizeWorld;
    in.colorSource    = job.colorSource;
    in.samplesPerVoxel = job.samplesPerVoxel;

    result.frame = Voxelize(in, *m_quantizer, &cancel);
    return result;
}

FullBakeResult AnimationBaker::RunFullBake(FullBakeJob& job, std::atomic<bool>& cancel) const {
    FullBakeResult result{};
    result.generation     = job.generation;
    result.voxelSizeWorld = job.voxelSizeWorld;
    result.startTime      = job.startTime;
    result.fps            = job.fps;

    auto logger = spdlog::get("Render");
    if (!m_quantizer) return result;
    if (!job.meshIR)  return result;
    const auto& ir = *job.meshIR;
    if (ir.primitives.empty()) return result;
    if (job.fps <= 0.0f) return result;

    // Build context once — same skin across every frame.
    BakeContext ctx = BuildContext(ir, job.skinIndex);
    if (ctx.primIndices.empty()) return result;

    // ---- Frame schedule ----
    // Inclusive on both endpoints: at fps=24, range [0, 1.0] yields 25 frames
    // (t = 0.0, 1/24, 2/24, ..., 1.0).
    const float duration  = std::max(0.0f, job.endTime - job.startTime);
    const int   frameCount = std::max(1, static_cast<int>(std::round(duration * job.fps)) + 1);
    const float dt         = (frameCount > 1) ? (duration / static_cast<float>(frameCount - 1)) : 0.0f;
    m_fullFramesTotal.store(frameCount, std::memory_order_relaxed);
    m_fullFramesDone .store(0,          std::memory_order_relaxed);

    // ---- Clip-wide AABB ----
    AabbSample box = ComputeClipWideAabb(ir, ctx, job.clipIndex, job.skinIndex,
                                         job.aabbSampleCount,
                                         /*padVoxels=*/1.0f, job.voxelSizeWorld, cancel);
    if (cancel.load(std::memory_order_relaxed)) { result.cancelled = true; return result; }
    if (!box.valid()) return result;

    result.worldOriginMin = box.min;
    result.worldOriginMax = box.max;

    // ---- Budget checks ----
    bool perFrameExceeded = false;
    const uint64_t cellsPerFrame = GridCellsForExtent(
        box.max - box.min, job.voxelSizeWorld, job.maxGridCellsPerFrame, perFrameExceeded);
    if (perFrameExceeded) {
        if (logger) logger->warn(
            "AnimationBaker: full bake rejected — per-frame cells {} exceeds budget {}",
            cellsPerFrame, job.maxGridCellsPerFrame);
        result.budgetExceeded = true;
        return result;
    }
    const uint64_t totalBytes = cellsPerFrame * static_cast<uint64_t>(frameCount);
    if (totalBytes > job.maxTotalBytes) {
        if (logger) logger->warn(
            "AnimationBaker: full bake rejected — total {} bytes ({} frames * {} cells) exceeds budget {}",
            totalBytes, frameCount, cellsPerFrame, job.maxTotalBytes);
        result.budgetExceeded = true;
        return result;
    }

    const glm::vec3 extent = box.max - box.min;
    const glm::uvec3 expectedSize = glm::uvec3(
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.x / job.voxelSizeWorld))),
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.y / job.voxelSizeWorld))),
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.z / job.voxelSizeWorld))));
    result.frameSize = expectedSize;

    // ---- Per-frame loop ----
    PoseScratch scratch;
    std::vector<std::vector<glm::vec3>> posed;
    std::vector<VoxelizePrimitive> vp;
    result.frames.resize(frameCount);

    for (int f = 0; f < frameCount; ++f) {
        if (cancel.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        const float t = job.startTime + dt * static_cast<float>(f);

        if (!PoseAndSkin(ir, ctx, job.clipIndex, job.skinIndex, t, scratch, posed)) return result;
        BuildVoxelizePrimitives(ir, ctx, posed, vp);

        VoxelizeInput in;
        in.primitives     = vp.data();
        in.primitiveCount = vp.size();
        in.worldOriginMin = box.min;
        in.worldOriginMax = box.max;
        in.voxelSizeWorld = job.voxelSizeWorld;
        in.colorSource    = job.colorSource;
        in.samplesPerVoxel = job.samplesPerVoxel;

        result.frames[f] = Voxelize(in, *m_quantizer, &cancel);

        if (result.frames[f].size != expectedSize) {
            if (cancel.load(std::memory_order_relaxed)) {
                result.cancelled = true;
                return result;
            }
            if (logger) logger->error(
                "AnimationBaker: frame {} produced size ({},{},{}) != expected ({},{},{}); aborting bake",
                f, result.frames[f].size.x, result.frames[f].size.y, result.frames[f].size.z,
                expectedSize.x, expectedSize.y, expectedSize.z);
            return result;
        }
        m_fullFramesDone.store(f + 1, std::memory_order_relaxed);
    }

    return result;
}

} // namespace voxel_bake
