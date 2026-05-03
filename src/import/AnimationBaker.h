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
    float              time            = 0.0f;
    float              voxelSizeWorld  = 0.05f;
    VoxColorSource     colorSource{};
    uint32_t           maxGridCells    = 512u * 512u * 512u; // ~134M cells, plan §10
    uint64_t           generation      = 0;                  // matches submitted-gen at result return
};

struct PreviewBakeResult {
    VoxFrame   frame;
    glm::vec3  worldOriginMin = glm::vec3(0.0f);   // mesh-local space
    glm::vec3  worldOriginMax = glm::vec3(0.0f);
    float      voxelSizeWorld = 0.0f;
    uint64_t   generation     = 0;                 // copy of the job's gen
    bool       budgetExceeded = false;             // grid would have exceeded maxGridCells
};

// ---- FullBakeJob (M4 — interface only in M3) ----
//
// Same shape as PreviewBakeJob but iterates [startTime, endTime] @ fps. M3
// doesn't service these — SubmitFullBake is wired so M4 just adds the worker
// branch. TakeCompletedFullBake always returns nullopt today.

struct FullBakeJob {
    BakeSourceSnapshot snapshot;
    float              startTime       = 0.0f;
    float              endTime         = 0.0f;
    float              fps             = 24.0f;
    float              voxelSizeWorld  = 0.05f;
    VoxColorSource     colorSource{};
    uint32_t           maxGridCells    = 512u * 512u * 512u;
    uint64_t           generation      = 0;
};

struct FullBakeResult {
    std::vector<VoxFrame> frames;
    glm::vec3             worldOriginMin = glm::vec3(0.0f);
    glm::vec3             worldOriginMax = glm::vec3(0.0f);
    float                 voxelSizeWorld = 0.0f;
    float                 startTime      = 0.0f;
    float                 fps            = 24.0f;
    uint64_t              generation     = 0;
    bool                  budgetExceeded = false;
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

    // M4 entry point — services nothing in M3 (the worker drops the job after
    // logging). Wired today so M4 only adds the worker branch.
    uint64_t SubmitFullBake(FullBakeJob job);

    // Main-thread polls: returns nullopt when no result waiting.
    std::optional<PreviewBakeResult> TakeCompletedPreview();
    std::optional<FullBakeResult>    TakeCompletedFullBake();

    // Status hint for the UI — does not block.
    bool IsPreviewBaking() const  { return m_previewInFlight.load(std::memory_order_relaxed); }
    bool IsFullBaking()    const  { return m_fullInFlight.load(std::memory_order_relaxed); }

private:
    void WorkerLoop();
    PreviewBakeResult RunPreview(PreviewBakeJob& job, std::atomic<bool>& cancel) const;

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
    // currently-active flag when a newer preview arrives.
    std::atomic<bool>* m_activePreviewCancel = nullptr;

    // Status flags (relaxed atomics are sufficient — UI is purely informational).
    std::atomic<bool> m_previewInFlight{false};
    std::atomic<bool> m_fullInFlight{false};

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
