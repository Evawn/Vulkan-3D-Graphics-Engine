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
        // Cancel any in-flight bake so the worker exits its inner Voxelize
        // loop promptly instead of finishing the bake before reading the
        // shutdown flag.
        if (m_activePreviewCancel) m_activePreviewCancel->store(true, std::memory_order_relaxed);
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
    m_pendingFull = std::move(job);
    m_cv.notify_one();
    return job.generation;
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
        PreviewBakeJob previewJob;
        bool havePreview = false;
        // Outer scope holds the lock only across the wait + pull, not the bake.
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
            // Stack-local cancellation atomic — main thread reaches into it
            // through m_activePreviewCancel when a newer preview is submitted.
            std::atomic<bool> cancel{false};
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_activePreviewCancel = &cancel;
            }

            PreviewBakeResult result = RunPreview(previewJob, cancel);

            // Publish the result iff the job wasn't cancelled (a cancelled
            // bake is partial; the main thread will get the new bake's
            // result anyway, so dropping the partial avoids flicker).
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_activePreviewCancel = nullptr;
                m_previewInFlight.store(false, std::memory_order_relaxed);
                if (!cancel.load(std::memory_order_relaxed)) {
                    m_completedPreview = std::move(result);
                }
            }
        }

        // M4 entry point — preserved here so the wiring (scheduling,
        // cancellation, completion) lands intact when M4 fills it in.
        FullBakeJob fullJob;
        bool haveFull = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_pendingFull) {
                fullJob = std::move(*m_pendingFull);
                m_pendingFull.reset();
                haveFull = true;
            }
        }
        if (haveFull) {
            if (logger) logger->info("AnimationBaker: full-bake job received (gen={}) — M4 will service.",
                fullJob.generation);
            // No-op for M3. Main thread polling TakeCompletedFullBake() gets
            // nullopt; UI shows "M4 not implemented".
        }
    }
}

PreviewBakeResult AnimationBaker::RunPreview(PreviewBakeJob& job, std::atomic<bool>& cancel) const {
    PreviewBakeResult result{};
    result.generation     = job.generation;
    result.voxelSizeWorld = job.voxelSizeWorld;

    if (!m_quantizer) return result;
    if (job.snapshot.primitives.empty()) return result;

    // ---- Pose the skeleton ----
    //
    // EvaluateClipFlat consumes a gltf_import::Animation; we synthesize one
    // on the stack borrowing the snapshot's channels (matches the pattern
    // SceneExtractor uses).
    std::vector<glm::vec3> trsT = job.snapshot.restTranslation;
    std::vector<glm::quat> trsR = job.snapshot.restRotation;
    std::vector<glm::vec3> trsS = job.snapshot.restScale;

    if (!job.snapshot.channels.empty() && job.snapshot.clipDuration > 0.0f) {
        gltf_import::Animation proxy;
        proxy.duration = job.snapshot.clipDuration;
        proxy.channels = std::move(job.snapshot.channels);
        gltf_import::EvaluateClipFlat(proxy, job.time, trsT, trsR, trsS);
    }

    if (cancel.load(std::memory_order_relaxed)) return result;

    // ---- World matrices + joint matrices ----
    std::vector<glm::mat4> worlds;
    const std::vector<bool>* maskPtr = job.snapshot.activeNodeMask.empty()
        ? nullptr : &job.snapshot.activeNodeMask;
    gltf_import::ComputeWorldMatricesFlat(job.snapshot.nodes, trsT, trsR, trsS, worlds, maskPtr);

    if (cancel.load(std::memory_order_relaxed)) return result;

    // Mesh-node frame (same convention as SceneExtractor::EmitSkinnedMesh):
    // first primitive's owner node defines the local frame of the bake.
    int meshNodeIdx = job.snapshot.primitives.front().ownerNodeIndex;
    const glm::mat4 meshNodeWorld =
        (meshNodeIdx >= 0 && static_cast<size_t>(meshNodeIdx) < worlds.size())
        ? worlds[meshNodeIdx]
        : glm::mat4(1.0f);

    gltf_import::Skin synthSkin;
    synthSkin.joints              = job.snapshot.skinJoints;
    synthSkin.inverseBindMatrices = job.snapshot.inverseBindMatrices;

    std::vector<glm::mat4> jointMats;
    gltf_import::ComputeJointMatrices(synthSkin, worlds, meshNodeWorld, jointMats);

    if (cancel.load(std::memory_order_relaxed)) return result;

    // ---- CPU-skin every primitive's positions ----
    //
    // posedPositions[primIdx] holds the mesh-node-local-frame world positions
    // for that primitive. The voxelizer reads from these directly via the
    // VoxelizePrimitive shim below. Match the runtime vertex shader's
    // formula: pos' = Σ weight[i] * jointMat[joint[i]] * pos.
    std::vector<std::vector<glm::vec3>> posedPositions(job.snapshot.primitives.size());
    for (size_t pi = 0; pi < job.snapshot.primitives.size(); ++pi) {
        const auto& sp = job.snapshot.primitives[pi];
        auto& out = posedPositions[pi];
        out.resize(sp.positions.size());
        for (size_t vi = 0; vi < sp.positions.size(); ++vi) {
            const glm::uvec4& j = sp.joints[vi];
            const glm::vec4&  w = sp.weights[vi];
            // Guard each joint index — the mask filtering / multi-skin
            // combinations occasionally leave dangling joints when the
            // primitive's vertices reference joints outside the active skin.
            const auto safeMat = [&](uint32_t idx) -> const glm::mat4& {
                static const glm::mat4 kIdent(1.0f);
                return (idx < jointMats.size()) ? jointMats[idx] : kIdent;
            };
            glm::mat4 skin = w.x * safeMat(j.x)
                           + w.y * safeMat(j.y)
                           + w.z * safeMat(j.z)
                           + w.w * safeMat(j.w);
            out[vi] = glm::vec3(skin * glm::vec4(sp.positions[vi], 1.0f));
        }
        if (cancel.load(std::memory_order_relaxed)) return result;
    }

    // ---- Compute the bake AABB (current pose only — M3) ----
    //
    // M4 will sample K poses across the clip duration and union the AABBs to
    // get a stable cross-frame grid. M3 just covers the pose we just skinned.
    AabbSample box;
    for (const auto& posed : posedPositions) {
        for (const glm::vec3& p : posed) box.Include(p);
    }
    if (!box.valid()) return result;

    // Pad by one voxel on every side so triangles flush against the AABB
    // boundary still get full triangle-AABB coverage tests.
    const float padScale = 1.0f;
    box.min -= glm::vec3(job.voxelSizeWorld * padScale);
    box.max += glm::vec3(job.voxelSizeWorld * padScale);

    result.worldOriginMin = box.min;
    result.worldOriginMax = box.max;

    // Cell-budget guard. If the slider would produce more than maxGridCells,
    // we abandon the bake and surface the issue to the panel via the result
    // flag — the panel keeps the previous bake on screen and warns.
    const glm::vec3 extent = box.max - box.min;
    const uint64_t cells =
          static_cast<uint64_t>(std::ceil(extent.x / job.voxelSizeWorld))
        * static_cast<uint64_t>(std::ceil(extent.y / job.voxelSizeWorld))
        * static_cast<uint64_t>(std::ceil(extent.z / job.voxelSizeWorld));
    if (cells > job.maxGridCells) {
        result.budgetExceeded = true;
        return result;
    }

    // ---- Voxelize ----
    std::vector<VoxelizePrimitive> vp(job.snapshot.primitives.size());
    for (size_t pi = 0; pi < job.snapshot.primitives.size(); ++pi) {
        const auto& sp = job.snapshot.primitives[pi];
        VoxelizePrimitive& v = vp[pi];
        v.positions       = posedPositions[pi].data();
        v.vertexCount     = posedPositions[pi].size();
        v.uvs             = sp.uvs.empty() ? nullptr : sp.uvs.data();
        v.indices         = sp.indices.data();
        v.indexCount      = sp.indices.size();
        v.baseColorFactor = sp.baseColorFactor;
        v.baseColorTexture = nullptr;       // M5: snapshot the texture too and bind here
    }

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

} // namespace voxel_bake
