#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "AssetRegistry.h"
#include "Scene.h"
#include "BindingTable.h"
#include "Buffer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

class RenderGraph;

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
};

class GltfImportTechnique : public RenderTechnique {
public:
    GltfImportTechnique();

    std::string GetDisplayName() const override { return "GLB Import & Bake"; }

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

    // Per-frame resources.
    std::vector<std::shared_ptr<VWrap::Buffer>> m_camera_ubos;
    std::vector<void*>                           m_camera_ubos_mapped;
    std::vector<std::shared_ptr<VWrap::Buffer>> m_joint_ssbos;
    std::vector<void*>                           m_joint_ssbos_mapped;
    VkDeviceSize                                 m_joint_ssbo_size = 0;
    std::shared_ptr<BindingTable>                m_bindings;

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
};
