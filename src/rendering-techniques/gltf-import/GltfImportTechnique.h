#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "AssetRegistry.h"
#include "Scene.h"
#include "BindingTable.h"
#include "Buffer.h"
#include "Sampler.h"
#include "AnimationBaker.h"
#include "Voxelizer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

class RenderGraph;
class PaletteResource;

// ---- GltfImportTechnique ----
//
// First-class workspace technique for importing animated .glb assets,
// previewing them in the viewport, and (eventually) baking them into the
// engine's animated voxel format. This M1 implementation handles the *Mesh*
// half of the eventual mesh ↔ voxel toggle:
//
//   - User opens a .glb via the BakerPanel ("Open .glb…" button)
//   - GltfLoader parses to MeshIR; the technique converts the IR into a
//     SkinnedMeshAsset + AnimationClipAsset(s) and registers them with the
//     AssetRegistry
//   - A SceneNode is added to the world carrying a Component::SkinnedMesh
//   - The SceneExtractor evaluates the active clip each frame and emits
//     RenderItem::SkinnedMesh items the technique consumes here
//   - A skinned-vertex pipeline (skinned_mesh.vert/.frag) draws the result
//
// The "voxel" half + the bake worker land in M3-onward.
//
// Threading: LoadGlb runs on the main thread today (synchronous). The bake
// worker (M3) will be the first async work item.
//
// Coexists with the other techniques — the workspace switcher (M2) decides
// when this one is the active technique vs. the regular Scene techniques.

struct GltfImportSession {
    // Empty until the user picks a file.
    std::string sourcePath;
    std::string sourceFileName;     // basename for UI display
    AssetID     meshAsset;          // SkinnedMesh
    std::vector<AssetID> clipAssets;
    std::vector<std::string> clipNames;       // mirror of clip names for UI
    std::vector<float>       clipDurations;   // seconds, parallel to clipNames
    int         activeClipIndex = -1;          // -1 = rest pose
    bool        hasLoadedAsset  = false;

    // Stats for UI display.
    size_t totalNodes      = 0;
    size_t totalSkins      = 0;
    size_t totalPrimitives = 0;
    size_t totalVertices   = 0;
    size_t totalTriangles  = 0;

    // ---- Voxel preview (M3) ----
    //
    // The procedural animated volume holding the most recent bake. Always
    // frameCount=1 in M3; M4 will resize-in-place (same AssetID) to N frames
    // once full bakes land. Created on first GLB load; resized on every
    // completed preview bake. Invalid until the first bake completes.
    AssetID    previewVolume;

    // World-local AABB the most recent bake was sized into (mesh-local frame
    // — same coord system as the skinned-mesh vertex positions before the
    // SceneNode world transform). Used by the voxel preview pipeline to
    // rasterize a cube spanning the volume.
    glm::vec3  previewAabbMin = glm::vec3(0.0f);
    glm::vec3  previewAabbMax = glm::vec3(0.0f);

    bool       hasBake            = false;     // at least one bake has completed
    bool       lastBudgetExceeded = false;     // last submission would have exceeded the cell budget
    glm::uvec3 previewVolumeSize  = glm::uvec3(0);
    float      previewVoxelSize   = 0.0f;      // mesh-local voxel edge length the bake used
};

class GltfImportTechnique : public RenderTechnique {
public:
    // Which view the user is staring at right now. Mesh = skinned-mesh
    // pipeline; Voxels = baked-volume DDA preview. Overlay (M6) will
    // composite both on top of each other.
    enum class PreviewMode : uint8_t { Mesh, Voxels };

    GltfImportTechnique();
    ~GltfImportTechnique() override;

    std::string GetDisplayName() const override { return "GLB Import & Bake"; }

    // Hidden from the Scene workspace's technique picker / cycle hotkey —
    // only reachable via the Workspace::ImportBake switch, which routes
    // through RenderingSystem::RequestSwitchTechniqueByName.
    bool IsScopedToWorkspace() const override { return true; }

    // ---- RenderTechnique surface ----
    RenderTargetDesc DescribeTargets(const RendererCaps& caps) const override;
    void             RegisterPasses(RenderGraph& graph,
                                    const RenderContext& ctx,
                                    const TechniqueTargets& targets) override;
    void             OnPostCompile(RenderGraph& graph) override;
    std::vector<std::string> GetShaderPaths() const override;
    std::vector<TechniqueParameter>& GetParameters() override;
    FrameStats       GetFrameStats() const override;

    // ---- Workspace-facing API (driven by BakerPanel) ----
    //
    // LoadGlb queues a path for parsing. We do the actual parse + asset
    // registration on the main thread inside Reload (event-driven through the
    // event sink) so device resources for the new buffers can be (re)allocated
    // by a graph rebuild — that's the only way persistent buffers come up at
    // the right size. Calling LoadGlb posts ReloadTechnique → the next
    // ProcessEvents tick invokes our Reload() which does the work + posts
    // RebuildGraph.
    void LoadGlb(const std::string& path);

    void SelectClip(int clipIndex);
    void SetPaused(bool paused);
    void SetPlaybackSpeed(float speed);
    void SetTime(float seconds);
    float GetTime() const;

    // ---- Voxel preview controls (M3) ----
    //
    // SetVoxelSize records the slider value + a timestamp; the technique
    // submits a preview bake when the timestamp is older than the debounce
    // interval (250ms by default) so dragging doesn't queue up dozens of
    // bakes. Each new SetVoxelSize cancels any in-flight preview job.
    void  SetVoxelSize(float worldUnits);
    float GetVoxelSize() const { return m_voxelSizeRequested; }

    void  SetPreviewMode(PreviewMode mode);
    PreviewMode GetPreviewMode() const { return m_previewMode; }

    // Status hints for BakerPanel — all relaxed reads.
    bool  IsBakingPreview() const { return m_baker.IsPreviewBaking(); }

    // Read-only view the panel uses to render its UI rows.
    const GltfImportSession& GetSession() const { return m_session; }

    // ---- Reload ----
    // Called by RenderingSystem when ReloadTechnique fires. Picks up
    // m_pendingLoadPath if set, parses, registers, and posts RebuildGraph.
    void Reload(const RenderContext& ctx) override;

private:
    // Engine handles
    std::shared_ptr<VWrap::Device>      m_device;
    std::shared_ptr<VWrap::Allocator>   m_allocator;
    std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
    std::shared_ptr<Camera>             m_camera;
    AssetRegistry*                       m_assets = nullptr;
    Scene*                               m_world  = nullptr;
    RenderGraph*                         m_graph  = nullptr;
    uint32_t                             m_max_frames_in_flight = 1;

    // The loaded session (mesh + clips + scene node).
    GltfImportSession m_session;
    SceneNode*        m_node = nullptr;

    // Asset state set up on first RegisterPasses with valid loaded assets.
    bool m_assetsRegistered = false;

    // Pending load: the panel calls LoadGlb which writes here, posts a
    // ReloadTechnique event, and Reload() drains the path.
    std::string  m_pendingLoadPath;

    // ---- Per-frame resources ----
    //
    // m_frame_ubos hold the shared GltfImportFrameUbo (camera + sky + sun)
    // bound at slot 0 of every pipeline in this technique. m_joint_ssbos is
    // the skinned-mesh joint arena. m_voxel_draw_ubos carry voxel-preview-
    // specific per-volume metadata (camera-local position, frame index).
    std::vector<std::shared_ptr<VWrap::Buffer>> m_frame_ubos;
    std::vector<void*>                           m_frame_ubos_mapped;
    std::vector<std::shared_ptr<VWrap::Buffer>> m_joint_ssbos;
    std::vector<void*>                           m_joint_ssbos_mapped;
    VkDeviceSize                                 m_joint_ssbo_size = 0;
    std::shared_ptr<BindingTable>                m_bindings;

    // ---- Voxel preview state ----
    voxel_bake::AnimationBaker          m_baker;
    std::unique_ptr<PaletteResource>    m_palette;
    std::shared_ptr<VWrap::Sampler>     m_volume_sampler;             // NEAREST clamp — required for R8_UINT
    std::vector<std::shared_ptr<VWrap::Buffer>> m_voxel_draw_ubos;
    std::vector<void*>                  m_voxel_draw_ubos_mapped;
    std::shared_ptr<BindingTable>       m_voxel_bindings;
    std::shared_ptr<BindingTable>       m_sky_bindings;

    // Pulled from RenderContext on every RegisterPasses. Borrowed pointers —
    // owned by the Scene. Used to populate the per-frame UBO with sun/sky
    // state so the import technique reflects whatever the inspector edited
    // for the rest of the scene (LIGHTING.md: scene-wide sun is a single
    // source of truth).
    const SceneLighting*  m_lighting = nullptr;
    const SkyDescription* m_sky      = nullptr;

    PreviewMode  m_previewMode = PreviewMode::Mesh;

    // Debounce: SetVoxelSize records the new value + the wall-clock time the
    // slider last moved. Each frame's record callback checks
    // (now - lastChange) > debounceMs and submits a preview bake. New slider
    // movement resets the timestamp; a re-bake fires only after the user
    // stops dragging.
    float        m_voxelSizeRequested  = 0.05f;
    bool         m_voxelSizeDirty      = false;
    std::chrono:: steady_clock::time_point m_voxelSizeChangeAt{};
    int          m_debounceMs          = 250;

    // Cell-budget guard — the bake worker drops jobs whose grid would exceed
    // this. 512^3 ≈ 134M cells (~134MB per frame at R8_UINT). Plan §10.
    uint32_t     m_maxGridCells        = 512u * 512u * 512u;

    // The mostly-immutable initial size we use when first registering the
    // procedural volume (before any bake completes). 1^3 keeps the placeholder
    // image footprint trivial.
    static constexpr glm::uvec3 kInitialPreviewSize{1, 1, 1};

    // Pulled at the start of every record callback so debounce / poll logic
    // runs uniformly regardless of which pass we're in.
    bool m_bakerStarted = false;

    // Inspector parameters (mostly empty in v1 — the BakerPanel is the
    // primary UI surface). Kept so RenderTechnique::GetParameters() returns a
    // non-empty list when the user selects this technique in the inspector.
    std::vector<TechniqueParameter> m_parameters;

    // Internal helpers
    void   CreatePerFrameBuffers(uint32_t frames);
    void   PerformPendingLoad();      // Reload() trampoline
    void   RebuildSessionFromAsset(); // populates m_session.* counters from registered asset
    void   EnsureSceneNode();         // creates m_node + Component if absent
    void   UpdateNodeComponent();     // syncs Component fields from m_session/playback state

    // Voxel preview helpers
    void   EnsureBakerStarted();
    void   EnsurePreviewVolumeRegistered();
    void   TickBakeState();           // poll completed bakes, fire debounced submissions
    void   SubmitPreviewBake();
    void   WriteFrameUbo(uint32_t frameIndex);  // shared per-frame UBO (camera + sky + sun)
};
