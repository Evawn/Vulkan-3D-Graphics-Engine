#include "AnimationBaker.h"
#include "AnimationEvaluator.h"
#include "PaletteQuantizer.h"
#include "SkinnedMeshAsset.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace voxel_bake {

bool BuildSnapshot(const AssetRegistry& assets,
                   AssetID meshId, AssetID clipId, int skinIndex,
                   BakeSourceSnapshot& out)
{
    const auto* mesh = assets.GetSkinnedMesh(meshId);
    if (!mesh || mesh->primitives.empty() || mesh->skins.empty()) return false;

    // Validate skin index — fall back to skin 0 if out of range.
    int skin = skinIndex;
    if (skin < 0 || static_cast<size_t>(skin) >= mesh->skins.size()) skin = 0;

    out = {};

    // Per-primitive vertex extraction. Pulling positions/joints/weights into
    // their own arrays lets the worker skin without dragging the full
    // SkinnedVertex layout around (saves ~24 bytes per vertex of overhead).
    out.primitives.reserve(mesh->primitives.size());
    for (const auto& p : mesh->primitives) {
        // Only emit primitives bound to the active skin — same filter the
        // SceneExtractor applies in EmitSkinnedMesh.
        if (p.skinIndex != skin) continue;
        BakeSourceSnapshot::Prim sp;
        sp.positions.reserve(p.vertices.size());
        sp.joints.reserve(p.vertices.size());
        sp.weights.reserve(p.vertices.size());
        bool anyUv = false;
        for (const auto& v : p.vertices) {
            sp.positions.push_back(v.position);
            sp.joints.push_back(v.joints);
            sp.weights.push_back(v.weights);
            if (v.uv != glm::vec2(0.0f)) anyUv = true;
        }
        if (anyUv) {
            sp.uvs.reserve(p.vertices.size());
            for (const auto& v : p.vertices) sp.uvs.push_back(v.uv);
        }
        sp.indices         = p.indices;
        sp.baseColorFactor = p.baseColorFactor;
        sp.ownerNodeIndex  = p.ownerNodeIndex;
        out.primitives.push_back(std::move(sp));
    }
    if (out.primitives.empty()) return false;

    out.skinJoints           = mesh->skins[skin].joints;
    out.inverseBindMatrices  = mesh->skins[skin].inverseBindMatrices;
    out.nodes                = mesh->nodes;            // copies name + child vector — see comment in header
    out.restTranslation      = mesh->restTranslation;
    out.restRotation         = mesh->restRotation;
    out.restScale            = mesh->restScale;
    out.activeNodeMask       = mesh->activeNodeMask;

    if (const auto* clip = assets.GetAnimationClip(clipId)) {
        out.channels     = clip->channels;
        out.clipDuration = clip->duration;
    }
    return true;
}

namespace {

// ---- PoseAndSkin ----
//
// The shared "evaluate clip → world matrices → joint matrices → CPU-skin"
// pipeline used by RunPreview, RunFullBake, and ComputeClipWideAabb. All
// scratch buffers are caller-owned so a per-frame loop can reuse the same
// allocations across hundreds of poses (otherwise we'd thrash the heap with
// new posedPositions vectors and TRS scratch arrays for each frame).
//
// `time` is clamped to [0, clipDuration] internally — the caller can pass
// out-of-range values (e.g. the AABB sampler hands in t = i * step where the
// last sample may slightly overshoot due to float rounding).
//
// Returns true on success, false if the snapshot has no primitives / no
// joints (shouldn't normally happen post-BuildSnapshot, but guards the
// callers from divide-by-zero downstream).

struct PoseScratch {
    std::vector<glm::vec3> trsT;
    std::vector<glm::quat> trsR;
    std::vector<glm::vec3> trsS;
    std::vector<glm::mat4> worlds;
    std::vector<glm::mat4> jointMats;
};

bool PoseAndSkin(const BakeSourceSnapshot&             snapshot,
                 float                                  time,
                 PoseScratch&                           scratch,
                 std::vector<std::vector<glm::vec3>>&  outPosedPositions)
{
    if (snapshot.primitives.empty()) return false;

    // ---- TRS: start from rest pose, overlay the clip's evaluated channels.
    scratch.trsT = snapshot.restTranslation;
    scratch.trsR = snapshot.restRotation;
    scratch.trsS = snapshot.restScale;

    if (!snapshot.channels.empty() && snapshot.clipDuration > 0.0f) {
        // Channels-based evaluator avoids copying the (potentially large)
        // channel vector on each call. The full bake calls this N+K times
        // per bake (frames + AABB samples), so the original Animation-by-
        // value path was allocating tens of MB of channel data per bake on
        // dense rigs.
        gltf_import::EvaluateChannelsFlat(snapshot.channels, snapshot.clipDuration, time,
                                          scratch.trsT, scratch.trsR, scratch.trsS);
    }

    // ---- World matrices (BFS, mask-aware).
    const std::vector<bool>* maskPtr = snapshot.activeNodeMask.empty()
        ? nullptr : &snapshot.activeNodeMask;
    gltf_import::ComputeWorldMatricesFlat(snapshot.nodes,
                                          scratch.trsT, scratch.trsR, scratch.trsS,
                                          scratch.worlds, maskPtr);

    // ---- Joint matrices for the active skin (mesh-node frame is the first
    //      primitive's owner node — same convention as SceneExtractor).
    const int meshNodeIdx = snapshot.primitives.front().ownerNodeIndex;
    const glm::mat4 meshNodeWorld =
        (meshNodeIdx >= 0 && static_cast<size_t>(meshNodeIdx) < scratch.worlds.size())
        ? scratch.worlds[meshNodeIdx]
        : glm::mat4(1.0f);

    gltf_import::Skin synthSkin;
    synthSkin.joints              = snapshot.skinJoints;
    synthSkin.inverseBindMatrices = snapshot.inverseBindMatrices;
    gltf_import::ComputeJointMatrices(synthSkin, scratch.worlds, meshNodeWorld, scratch.jointMats);

    // ---- CPU-skin every primitive's positions. Mirrors the runtime
    //      vertex shader's `pos' = Σ weight[i] * jointMat[joint[i]] * pos`.
    outPosedPositions.resize(snapshot.primitives.size());
    static const glm::mat4 kIdent(1.0f);
    for (size_t pi = 0; pi < snapshot.primitives.size(); ++pi) {
        const auto& sp = snapshot.primitives[pi];
        auto& out = outPosedPositions[pi];
        out.resize(sp.positions.size());
        for (size_t vi = 0; vi < sp.positions.size(); ++vi) {
            const glm::uvec4& j = sp.joints[vi];
            const glm::vec4&  w = sp.weights[vi];
            const auto safeMat = [&](uint32_t idx) -> const glm::mat4& {
                return (idx < scratch.jointMats.size()) ? scratch.jointMats[idx] : kIdent;
            };
            glm::mat4 skinM = w.x * safeMat(j.x)
                            + w.y * safeMat(j.y)
                            + w.z * safeMat(j.z)
                            + w.w * safeMat(j.w);
            out[vi] = glm::vec3(skinM * glm::vec4(sp.positions[vi], 1.0f));
        }
    }
    return true;
}

// ---- ComputeClipWideAabb ----
//
// Sample K poses across the clip duration, union the deformed-mesh AABBs,
// and pad by `padVoxels * voxelSize`. K=32 is enough for slow-moving foliage
// (the AnimatedOak's leaves move ~1 voxel between adjacent samples). Faster-
// moving rigs may want higher K — the panel can expose this later.
//
// Cancellation: checks the cancel atomic between samples. Returns the partial
// AABB if cancelled — callers will discard the bake anyway.

AabbSample ComputeClipWideAabb(const BakeSourceSnapshot& snapshot,
                               uint32_t                  sampleCount,
                               float                     padVoxels,
                               float                     voxelSize,
                               const std::atomic<bool>&  cancel)
{
    AabbSample box;
    if (sampleCount == 0) return box;

    PoseScratch scratch;
    std::vector<std::vector<glm::vec3>> posed;

    const float duration = snapshot.clipDuration;
    const uint32_t k = std::max<uint32_t>(1, sampleCount);

    for (uint32_t i = 0; i < k; ++i) {
        if (cancel.load(std::memory_order_relaxed)) return box;
        // Static-mesh / no-clip case: one sample at t=0 is enough.
        const float t = (duration > 0.0f && k > 1)
            ? duration * (static_cast<float>(i) / static_cast<float>(k - 1))
            : 0.0f;
        if (!PoseAndSkin(snapshot, t, scratch, posed)) return box;
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

// ---- Build VoxelizePrimitive shim ----
//
// Adapter from the worker-side BakeSourceSnapshot::Prim + posed positions to
// the Voxelizer's input view. Reused by both Run* paths.

void BuildVoxelizePrimitives(const BakeSourceSnapshot&                snapshot,
                             const std::vector<std::vector<glm::vec3>>& posedPositions,
                             std::vector<VoxelizePrimitive>&            outVp)
{
    outVp.resize(snapshot.primitives.size());
    for (size_t pi = 0; pi < snapshot.primitives.size(); ++pi) {
        const auto& sp = snapshot.primitives[pi];
        VoxelizePrimitive& v = outVp[pi];
        v.positions        = posedPositions[pi].data();
        v.vertexCount      = posedPositions[pi].size();
        v.uvs              = sp.uvs.empty() ? nullptr : sp.uvs.data();
        v.indices          = sp.indices.data();
        v.indexCount       = sp.indices.size();
        v.baseColorFactor  = sp.baseColorFactor;
        v.baseColorTexture = nullptr;       // M5 hooks textures here
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
        // Cancel any in-flight bake so the worker exits its inner loop promptly
        // instead of finishing the bake before reading the shutdown flag.
        if (m_activePreviewCancel) m_activePreviewCancel->store(true, std::memory_order_relaxed);
        if (m_activeFullCancel)    m_activeFullCancel   ->store(true, std::memory_order_relaxed);
    }
    m_cv.notify_all();
    m_worker.join();
}

void AnimationBaker::SetPalette(const std::array<uint8_t, 256 * 4>& palette) {
    // Built lazily into the unique_ptr so the worker doesn't hold a stale ref
    // if the palette ever changes (it currently doesn't, but the surface is
    // cheap to keep flexible).
    std::lock_guard<std::mutex> lk(m_mutex);
    m_quantizer = std::make_unique<PaletteQuantizer>(palette);
}

uint64_t AnimationBaker::SubmitPreview(PreviewBakeJob job) {
    std::lock_guard<std::mutex> lk(m_mutex);
    // Cancel the in-flight preview, if any. The worker's atomic flips and
    // the inner Voxelize loop bails between triangles. The pending slot is
    // then replaced — the older job's snapshot is dropped.
    if (m_activePreviewCancel) m_activePreviewCancel->store(true, std::memory_order_relaxed);
    job.generation = ++m_previewGenCounter;
    m_pendingPreview = std::move(job);
    // Drop any stale completed result that hasn't been picked up — the new
    // job supersedes it, even if the main thread missed the previous tick.
    m_completedPreview.reset();
    m_cv.notify_one();
    return job.generation;
}

uint64_t AnimationBaker::SubmitFullBake(FullBakeJob job) {
    std::lock_guard<std::mutex> lk(m_mutex);
    job.generation = ++m_fullGenCounter;
    // Pre-stamp framesTotal so the panel can render "0 / N" while the worker
    // is still doing pre-bake AABB sampling. framesDone resets to 0 for the
    // new job; the worker bumps it as frames complete.
    const float duration = std::max(0.0f, job.endTime - job.startTime);
    const int   total    = std::max(1, static_cast<int>(std::round(duration * job.fps)) + 1);
    m_fullFramesTotal.store(total, std::memory_order_relaxed);
    m_fullFramesDone .store(0,     std::memory_order_relaxed);
    m_pendingFull = std::move(job);
    m_completedFull.reset();
    m_cv.notify_one();
    return job.generation;
}

void AnimationBaker::CancelFullBake() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_activeFullCancel) m_activeFullCancel->store(true, std::memory_order_relaxed);
    // Also discard any pending (not-yet-started) job — Cancel means "I do not
    // want a full bake anymore", whether or not the worker has picked it up.
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
            auto logger0 = spdlog::get("Render");
            if (logger0) logger0->info("AnimationBaker: full-bake job picked up by worker (gen={})", fullJob.generation);
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
                // Publish iff not cancelled. Cancelled bakes are useless to
                // the consumer (no frame range guarantees) and would just
                // confuse the panel; CancelFullBake's contract is "you'll
                // get nothing back, the partial work is wasted".
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
    if (job.snapshot.primitives.empty()) return result;

    // Pose + skin into mesh-local positions.
    PoseScratch scratch;
    std::vector<std::vector<glm::vec3>> posed;
    if (!PoseAndSkin(job.snapshot, job.time, scratch, posed)) return result;

    if (cancel.load(std::memory_order_relaxed)) return result;

    // Preview AABB is just the current pose (M3 behavior). The full-bake path
    // is the one that needs a stable cross-frame grid, so it uses the K-pose
    // ComputeClipWideAabb above.
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
    BuildVoxelizePrimitives(job.snapshot, posed, vp);

    VoxelizeInput in;
    in.primitives     = vp.data();
    in.primitiveCount = vp.size();
    in.worldOriginMin = box.min;
    in.worldOriginMax = box.max;
    in.voxelSizeWorld = job.voxelSizeWorld;
    in.colorSource    = job.colorSource;

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
    if (logger) logger->info(
        "RunFullBake: enter gen={} prims={} clipDur={:.3f} channels={} range=[{:.3f},{:.3f}] fps={:.1f} voxel={:.4f}",
        job.generation, job.snapshot.primitives.size(), job.snapshot.clipDuration,
        job.snapshot.channels.size(), job.startTime, job.endTime, job.fps, job.voxelSizeWorld);
    if (!m_quantizer) {
        if (logger) logger->error("RunFullBake: no quantizer — bake aborted");
        return result;
    }
    if (job.snapshot.primitives.empty()) {
        if (logger) logger->error("RunFullBake: snapshot has no primitives — bake aborted");
        return result;
    }
    if (job.fps <= 0.0f) {
        if (logger) logger->error("RunFullBake: fps={} invalid — bake aborted", job.fps);
        return result;
    }

    // ---- Frame schedule ----
    //
    // Inclusive on both endpoints: at fps=24, range [0, 1.0] yields 25 frames
    // (t = 0.0, 1/24, 2/24, ..., 1.0). That matches the user's mental model
    // — "1 second @ 24 fps" reads as 25 distinct samples, not 24 — and
    // matches what we pre-stamped into m_fullFramesTotal at SubmitFullBake.
    const float duration  = std::max(0.0f, job.endTime - job.startTime);
    const int   frameCount = std::max(1, static_cast<int>(std::round(duration * job.fps)) + 1);
    const float dt         = (frameCount > 1) ? (duration / static_cast<float>(frameCount - 1)) : 0.0f;
    m_fullFramesTotal.store(frameCount, std::memory_order_relaxed);
    m_fullFramesDone .store(0,          std::memory_order_relaxed);

    // ---- Clip-wide AABB ----
    AabbSample box = ComputeClipWideAabb(job.snapshot, job.aabbSampleCount,
                                         /*padVoxels=*/1.0f, job.voxelSizeWorld, cancel);
    if (cancel.load(std::memory_order_relaxed)) { result.cancelled = true; return result; }
    if (!box.valid()) {
        if (logger) logger->error("RunFullBake: clip-wide AABB invalid (no posed vertices?) — bake aborted");
        return result;
    }
    if (logger) logger->info(
        "RunFullBake: clip-wide AABB min=({:.3f},{:.3f},{:.3f}) max=({:.3f},{:.3f},{:.3f}), frameCount={}",
        box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z, frameCount);

    result.worldOriginMin = box.min;
    result.worldOriginMax = box.max;

    // ---- Per-frame budget check + total-bytes check ----
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

    // Snapshot the frame size so we can assert each per-frame Voxelize call
    // produces the same dimensions. Mismatch would corrupt the Z-slab packing.
    const glm::vec3 extent = box.max - box.min;
    const glm::uvec3 expectedSize = glm::uvec3(
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.x / job.voxelSizeWorld))),
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.y / job.voxelSizeWorld))),
        std::max(1u, static_cast<uint32_t>(std::ceil(extent.z / job.voxelSizeWorld))));
    result.frameSize = expectedSize;

    // Z-slab packed-depth check. The animated volume image has
    // depth = size.z * frameCount. The Vulkan spec guarantees up to 2048
    // (Apple Silicon enforces exactly 2048; many desktop GPUs go higher but
    // we cap at the spec floor for portability). Exceeding this is a hard
    // GPU-driver crash, not a soft fail, so we MUST reject the bake here
    // before TickBakeState ships it to the graph allocator.
    const uint64_t packedDepth = static_cast<uint64_t>(expectedSize.z) * static_cast<uint64_t>(frameCount);
    if (packedDepth > job.maxPackedDepth) {
        if (logger) logger->warn(
            "RunFullBake: rejected — packed depth {} (z={} * frames={}) exceeds device 3D-image limit {} "
            "(reduce fps, shorten range, or coarsen voxel size)",
            packedDepth, expectedSize.z, frameCount, job.maxPackedDepth);
        result.budgetExceeded = true;
        return result;
    }

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

        if (!PoseAndSkin(job.snapshot, t, scratch, posed)) return result;
        BuildVoxelizePrimitives(job.snapshot, posed, vp);

        VoxelizeInput in;
        in.primitives     = vp.data();
        in.primitiveCount = vp.size();
        in.worldOriginMin = box.min;
        in.worldOriginMax = box.max;
        in.voxelSizeWorld = job.voxelSizeWorld;
        in.colorSource    = job.colorSource;

        result.frames[f] = Voxelize(in, *m_quantizer, &cancel);

        // Defensive: a Voxelize() bailout (cancel mid-call) returns a partial
        // frame with size=0. Treat that as cancellation, not a successful
        // frame, so the result isn't published with an invalid frame slot.
        if (result.frames[f].size != expectedSize) {
            if (cancel.load(std::memory_order_relaxed)) {
                result.cancelled = true;
                return result;
            }
            // Genuine logic error — shouldn't happen given the AABB invariant.
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
