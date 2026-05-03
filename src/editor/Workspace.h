#pragma once

#include <cstdint>
#include <string>

// ---- Workspace ----
//
// A Workspace is a higher-level UI mode than a RenderTechnique. Each
// workspace declares which render technique drives the viewport, which
// panels are visible, and which top-level menu items / hotkeys apply.
//
// v1 has two workspaces:
//   - Scene       : current behaviour. Full technique picker, all panels
//                   visible. Hierarchy + Inspector are the canonical world
//                   editor.
//   - ImportBake  : viewport is locked to the GltfImportTechnique, BakerPanel
//                   is the primary panel, Hierarchy is hidden (irrelevant
//                   while authoring an import). The Scene workspace's world
//                   state is preserved when entering Import; switching back
//                   restores the active technique that was selected in Scene.
//
// Editor::SetWorkspace handles the transition: it stashes the current Scene
// technique index, asks RenderingSystem to switch to the workspace's locked
// technique (resolved by display-name match), and rebuilds the panel layout.

enum class Workspace : uint8_t {
    Scene,
    ImportBake,
};

struct WorkspaceConfig {
    const char* displayName;             // shown in tab strip / menu
    bool        showsHierarchy;
    bool        showsInspector;
    bool        showsBaker;
    bool        showsRenderGraph;
    bool        showsPerformance;
    bool        showsMemory;
    bool        showsConsole;

    // When non-empty, RenderingSystem switches to the technique with this
    // display name on workspace entry. Empty leaves the active technique
    // alone (the Scene workspace doesn't lock — the user picks via inspector).
    const char* lockedTechniqueName;
};

// Static config table — single source of truth for what each workspace shows.
// The Scene workspace uses the existing default panel set; ImportBake hides
// the Hierarchy + RenderGraph panels because they're irrelevant to a single-
// asset import session.
inline WorkspaceConfig WorkspaceConfigOf(Workspace ws) {
    switch (ws) {
        case Workspace::ImportBake:
            return WorkspaceConfig{
                .displayName        = "Import & Bake",
                .showsHierarchy     = false,
                .showsInspector     = true,
                .showsBaker         = true,
                .showsRenderGraph   = false,
                .showsPerformance   = true,
                .showsMemory        = false,
                .showsConsole       = true,
                .lockedTechniqueName = "GLB Import & Bake",
            };
        case Workspace::Scene:
        default:
            return WorkspaceConfig{
                .displayName        = "Scene",
                .showsHierarchy     = true,
                .showsInspector     = true,
                .showsBaker         = false,
                .showsRenderGraph   = true,
                .showsPerformance   = true,
                .showsMemory        = true,
                .showsConsole       = true,
                .lockedTechniqueName = "",
            };
    }
}
