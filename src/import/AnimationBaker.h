#pragma once

#include "MeshIR.h"
#include "Voxelizer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace voxel_bake {

class PaletteQuantizer;

// ---- Job source-of-truth: the loaded MeshIR ----
//
// Bake jobs hold a `shared_ptr<const gltf_import::MeshIR>` rather than a
// copied-out flat snapshot. The IR is everything cgltf parsed from the .glb
// (nodes, skins, animations, primitives with full SkinnedVertex layout,
// materials, decoded textures), and the bake worker reads what it needs
// directly. Two reasons this is correct:
//
//   1. The IR is the natural fan-out point. Both the runtime mesh draw
//      (SkinnedMeshAsset) and the bake worker should consume the IR
//      independently — neither should be downstream of the other.
//
//   2. Lifetime is automatic. A LoadGlb mid-bake replaces the technique's
//      `m_meshIR` with a new shared_ptr, but the in-flight job's refcount
//      keeps the old IR alive until the bake finishes. Zero coordination on
//      the consumer side.
//
// Texture data lives in `meshIR->textures`; the worker reads it by reference
// (raw pointer into the IR's vector). Safe because the IR shared_ptr keeps
// the whole IR alive while the worker borrows.

// ---- PreviewBakeJob ----
//
// A single-frame voxelization. The worker:
//   1. Pose: evaluate animations[clipIndex] at `time` → per-node TRS
//   2. World matrices: BFS through meshIR->nodes (active-skin mask pruned)
//   3. Joint matrices: standard glTF skinning math
//   4. Skin: CPU-skin every primitive bound to skinIndex
//   5. AABB: union deformed positions (current pose only)
//   6. Voxelize: feeds VoxelizePrimitive[] (with texture pointers + alpha
//      config sourced from meshIR->materials / meshIR->textures) into
//      Voxelize() → VoxFrame
//
// `cancelled` flips when a newer preview job arrives — the in-flight job
// bails out at the next triangle boundary inside Voxelize() and at frame
// boundaries inside RunFullBake.

struct PreviewBakeJob {
    std::shared_ptr<const gltf_import::MeshIR> meshIR;
    int                clipIndex            = -1;     // -1 = rest pose / no clip
    int                skinIndex            = 0;
    float              time                 = 0.0f;
    float              voxelSizeWorld       = 0.05f;
    VoxColorSource     colorSource{};
    uint32_t           maxGridCellsPerFrame = 512u * 512u * 512u;
    uint64_t           generation           = 0;
};

struct PreviewBakeResult {
    VoxFrame   frame;
    glm::vec3  worldOriginMin = glm::vec3(0.0f);   // mesh-local space
    glm::vec3  worldOriginMax = glm::vec3(0.0f);
    float      voxelSizeWorld = 0.0f;
    uint64_t   generation     = 0;
    bool       budgetExceeded = false;
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
    std::shared_ptr<const gltf_import::MeshIR> meshIR;
    int                clipIndex           = -1;
    int                skinIndex           = 0;
    float              startTime           = 0.0f;
    float              endTime             = 0.0f;
    float              fps                 = 24.0f;
    float              voxelSizeWorld      = 0.05f;
    VoxColorSource     colorSource{};
    uint32_t           maxGridCellsPerFrame = 512u * 512u * 512u;
    uint64_t           maxTotalBytes        = 1ull * 1024 * 1024 * 1024;
    uint32_t           aabbSampleCount      = 32;
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
//   - Preview: replaces any in-flight preview job; latest wins.
//   - FullBake: queued separately so a long bake doesn't block previews.
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

    std::optional<PreviewBakeJob> m_pendingPreview;
    std::optional<FullBakeJob>    m_pendingFull;

    std::atomic<bool>* m_activePreviewCancel = nullptr;
    std::atomic<bool>* m_activeFullCancel    = nullptr;

    std::atomic<bool> m_previewInFlight{false};
    std::atomic<bool> m_fullInFlight{false};

    mutable std::atomic<int> m_fullFramesDone{0};
    mutable std::atomic<int> m_fullFramesTotal{0};

    std::optional<PreviewBakeResult> m_completedPreview;
    std::optional<FullBakeResult>    m_completedFull;

    std::unique_ptr<PaletteQuantizer> m_quantizer;

    uint64_t m_previewGenCounter = 0;
    uint64_t m_fullGenCounter    = 0;
};

} // namespace voxel_bake
