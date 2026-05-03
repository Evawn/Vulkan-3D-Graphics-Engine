#pragma once

#include "AssetRegistry.h"
#include "MeshIR.h"
#include "Voxelizer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace voxel_bake {

class PaletteQuantizer;

// ---- BakeSourceSnapshot ----
//
// Self-contained slice of an animated asset that the worker thread can
// voxelize without ever touching the AssetRegistry. We snapshot at job
// submission so a concurrent LoadGlb (which may push_back into
// AssetRegistry::m_skinnedMeshes and invalidate raw pointers) can't race the
// worker. Cost is one shallow vector-copy per debounce tick (~250ms apart);
// memory peak is roughly the size of one skinned-mesh asset.
//
// Per-primitive: positions are pre-extracted from the SkinnedVertex array so
// the worker doesn't carry the full vertex layout — it only needs positions
// + joints + weights for skinning, plus indices for triangle lookup.

struct BakeSourceSnapshot {
    struct Prim {
        std::vector<glm::vec3>   positions;        // mesh-local rest-pose positions
        std::vector<glm::vec2>   uvs;              // empty if absent — M5 will require non-empty
        std::vector<glm::uvec4>  joints;
        std::vector<glm::vec4>   weights;
        std::vector<uint32_t>    indices;
        glm::vec4                baseColorFactor = glm::vec4(1.0f);
        int                      ownerNodeIndex  = -1;
    };
    std::vector<Prim> primitives;

    std::vector<int>                            skinJoints;
    std::vector<glm::mat4>                      inverseBindMatrices;

    std::vector<gltf_import::Node>              nodes;
    std::vector<glm::vec3>                      restTranslation;
    std::vector<glm::quat>                      restRotation;
    std::vector<glm::vec3>                      restScale;
    std::vector<bool>                           activeNodeMask;

    std::vector<gltf_import::AnimationChannel>  channels;
    float                                        clipDuration = 0.0f;
};

// Build a snapshot from the registered assets. Returns false if the mesh
// asset is invalid; clip can be invalid (snapshot will be rest-pose).
bool BuildSnapshot(const AssetRegistry& assets,
                   AssetID meshId, AssetID clipId, int skinIndex,
                   BakeSourceSnapshot& out);

// ---- PreviewBakeJob ----
//
// A single-frame voxelization request. The worker:
//   1. Evaluates `snapshot.channels` at `time` → poses the skeleton
//   2. CPU-skins each primitive's positions → mesh-local world positions
//   3. Computes the bake AABB (current pose only — M3) and pads
//   4. Voxelizes into a VoxFrame
//   5. Stashes the result; main thread polls via TakeCompletedPreview
//
// `cancelled` flips when a newer preview job arrives — the in-flight job
// bails out at the next triangle boundary inside Voxelize().

struct PreviewBakeJob {
    BakeSourceSnapshot snapshot;
    float              time                 = 0.0f;
    float              voxelSizeWorld       = 0.05f;
    VoxColorSource     colorSource{};
    uint32_t           maxGridCellsPerFrame = 512u * 512u * 512u; // ~134M cells, plan §10
    uint64_t           generation           = 0;                  // matches submitted-gen at result return
};

struct PreviewBakeResult {
    VoxFrame   frame;
    glm::vec3  worldOriginMin = glm::vec3(0.0f);   // mesh-local space
    glm::vec3  worldOriginMax = glm::vec3(0.0f);
    float      voxelSizeWorld = 0.0f;
    uint64_t   generation     = 0;                 // copy of the job's gen
    bool       budgetExceeded = false;             // grid would have exceeded maxGridCellsPerFrame
};

// ---- FullBakeJob ----
//
// Iterates [startTime, endTime] in 1/fps steps and produces one VoxFrame per
// step. The grid (size + origin) is held constant across every frame using a
// clip-wide AABB sampled at K poses before the per-frame loop starts — that's
// what guarantees the per-frame outputs can be Z-slab-packed into one volume
// image without resizing each frame.
//
// Two cell-budget layers:
//   - maxGridCellsPerFrame guards the per-frame grid (same units as the
//     preview budget); a clip-wide AABB whose grid would exceed this rejects
//     the whole bake before the loop starts.
//   - maxTotalBytes guards `frameCount * cells * bytesPerCell` (R8_UINT == 1B).
//     Cheap insurance against a "60 seconds @ 24 fps @ 256³" mistake silently
//     allocating ~20 GB of voxel data on the worker thread.

struct FullBakeJob {
    BakeSourceSnapshot snapshot;
    float              startTime           = 0.0f;
    float              endTime             = 0.0f;
    float              fps                 = 24.0f;
    float              voxelSizeWorld      = 0.05f;
    VoxColorSource     colorSource{};
    uint32_t           maxGridCellsPerFrame = 512u * 512u * 512u;       // ~134M cells
    uint64_t           maxTotalBytes        = 1ull * 1024 * 1024 * 1024; // 1 GB
    // Vulkan 3D-image depth cap. Animated volumes pack frames as Z-slabs
    // (image.depth = size.z * frameCount), and the spec guarantees 2048 as
    // the minimum supported maxImageDimension3D. Apple Silicon enforces this
    // exactly; cross-platform code stays safe by capping at the spec floor
    // rather than querying device limits at job-submission time. Lifting
    // this requires a packing-scheme change (e.g. sampler2DArray) that
    // ripples through every animated-voxel consumer.
    uint32_t           maxPackedDepth       = 2048;
    uint32_t           aabbSampleCount      = 32;                        // K poses for clip-wide AABB
    uint64_t           generation           = 0;
};

// ---- FullBakeResult ----
//
// Per-frame voxel data + the constant grid metadata used across every frame.
// The main thread concatenates frames[i].indices Z-sequentially into one byte
// blob and uploads via VoxelVolumeAsset::data. Frames are guaranteed to all
// share the same `size`; the worker fails the bake otherwise.
//
// `budgetExceeded` (per-frame or total) and `cancelled` (user-cancelled mid
// bake) are sticky on the result — the main thread surfaces the reason in
// the panel rather than just dropping the result silently.

struct FullBakeResult {
    std::vector<VoxFrame> frames;
    glm::vec3             worldOriginMin = glm::vec3(0.0f);
    glm::vec3             worldOriginMax = glm::vec3(0.0f);
    glm::uvec3            frameSize      = glm::uvec3(0);
    float                 voxelSizeWorld = 0.0f;
    float                 startTime      = 0.0f;
    float                 fps            = 24.0f;
    uint64_t              generation     = 0;
    bool                  budgetExceeded = false;
    bool                  cancelled      = false;
};

// ---- AnimationBaker ----
//
// Owns one worker thread. Two job slots:
//   - Preview (M3): replaces any in-flight preview job; latest wins.
//   - FullBake (M4): queued separately so a long bake doesn't block previews.
//
// Lifecycle: Start() spins up the thread; Shutdown() signals + joins. Both
// are idempotent; OK to call Start once and Shutdown once across the
// lifetime of the GltfImportTechnique that owns this baker.

class AnimationBaker {
public:
    AnimationBaker();
    ~AnimationBaker();

    AnimationBaker(const AnimationBaker&) = delete;
    AnimationBaker& operator=(const AnimationBaker&) = delete;

    void Start();
    void Shutdown();

    // Bind the palette the bakes quantize against. Must be called before any
    // job is submitted; calling again with a different palette is OK but only
    // affects future jobs (no preemption of in-flight). Held by reference;
    // the palette object must outlive the baker.
    void SetPalette(const std::array<uint8_t, 256 * 4>& palette);

    // Submit / replace the preview job. Generation increments per call; the
    // returned value is the gen the worker will tag onto the result.
    uint64_t SubmitPreview(PreviewBakeJob job);

    // Submit a full-animation bake. Replaces any pending (not-yet-started) full
    // bake; an already-running full bake continues to completion (call
    // CancelFullBake first if you want to interrupt it).
    uint64_t SubmitFullBake(FullBakeJob job);

    // Cancel the in-flight full bake (if any). The worker bails out between
    // frames — the partial result is discarded. Idempotent; safe to call when
    // nothing is baking.
    void CancelFullBake();

    // Main-thread polls: returns nullopt when no result waiting.
    std::optional<PreviewBakeResult> TakeCompletedPreview();
    std::optional<FullBakeResult>    TakeCompletedFullBake();

    // Status hints for the UI — relaxed reads, do not block.
    bool IsPreviewBaking() const  { return m_previewInFlight.load(std::memory_order_relaxed); }
    bool IsFullBaking()    const  { return m_fullInFlight.load(std::memory_order_relaxed); }

    // Per-frame progress on the in-flight (or just-finished) full bake.
    // Both return 0 when no full bake has run this session. `framesDone` lags
    // by ~one frame on a busy worker — fine for a progress bar.
    int  FullBakeFramesDone()  const { return m_fullFramesDone.load(std::memory_order_relaxed); }
    int  FullBakeFramesTotal() const { return m_fullFramesTotal.load(std::memory_order_relaxed); }

private:
    void WorkerLoop();
    PreviewBakeResult RunPreview(PreviewBakeJob& job, std::atomic<bool>& cancel) const;
    FullBakeResult    RunFullBake(FullBakeJob& job, std::atomic<bool>& cancel) const;

    // Worker thread + signaling.
    std::thread             m_worker;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool>       m_shutdown{false};

    // Pending submissions. Latest preview wins (cancellation atomic flips on
    // the in-flight job inside the worker before we replace).
    std::optional<PreviewBakeJob> m_pendingPreview;
    std::optional<FullBakeJob>    m_pendingFull;

    // In-flight cancellation flags. The worker reads these by reference from
    // a stack-local copy of the active job; the main thread flips the
    // currently-active flag when (a) a newer preview arrives or (b) the user
    // hits Cancel on the full bake. Held under m_mutex so the main thread
    // never dereferences a stack-local that has gone out of scope.
    std::atomic<bool>* m_activePreviewCancel = nullptr;
    std::atomic<bool>* m_activeFullCancel    = nullptr;

    // Status flags (relaxed atomics are sufficient — UI is purely informational).
    std::atomic<bool> m_previewInFlight{false};
    std::atomic<bool> m_fullInFlight{false};

    // Per-frame progress on the in-flight full bake. Read by the panel each
    // frame to render the progress bar. Worker writes framesDone after every
    // completed frame; framesTotal is stamped at job start. `mutable` because
    // RunFullBake is logically const (no observable AnimationBaker state
    // changes from the *consumer's* point of view) but writes to these
    // atomics for UI hand-off.
    mutable std::atomic<int> m_fullFramesDone{0};
    mutable std::atomic<int> m_fullFramesTotal{0};

    // Completed results awaiting main-thread pickup.
    std::optional<PreviewBakeResult> m_completedPreview;
    std::optional<FullBakeResult>    m_completedFull;

    // Quantizer — late-bound (palette set after construction); held as
    // unique_ptr so we can rebuild it if the palette ever changes.
    std::unique_ptr<PaletteQuantizer> m_quantizer;

    // Generation counters (separate for preview vs full).
    uint64_t m_previewGenCounter = 0;
    uint64_t m_fullGenCounter    = 0;
};

} // namespace voxel_bake
