#pragma once

#include "imgui.h"

#include <functional>
#include <string>

class GltfImportTechnique;
struct GltfImportSession;

// ---- BakerPanel ----
//
// The "studio" UI for the GLB Import & Bake workspace. Reads from the
// GltfImportTechnique's session and writes via its public mutators (LoadGlb,
// SelectClip, SetPaused/Speed/Time, StartFullBake, SaveBake, LoadBakeFromDisk).
// This panel owns no engine state of its own — it's a thin dialog over the
// technique. UI scratch (latched-button flags, slider mirrors) lives here.
//
// Sections:
//   - Source: file picker, basename + counts
//   - Animation: clip dropdown, play/pause/scrub, speed slider
//   - Voxelization: voxel size slider, view mode toggle, status
//   - Bake animation: range + fps, bake button, progress, save / load .vxa

class BakerPanel {
public:
    void SetTechnique(GltfImportTechnique* tech) { m_technique = tech; }
    void Draw();

private:
    GltfImportTechnique* m_technique = nullptr;

    // Latched UI scratch so file dialogs open only on the click frame, not
    // every frame the button is hovered. ImGui::Button is edge-triggered, but
    // mixing it with synchronous file dialogs needs a one-frame defer so the
    // panel can finish rendering before the dialog blocks.
    bool m_wantOpenFileDialog = false;
    bool m_wantSaveDialog     = false;
    bool m_wantLoadVxaDialog  = false;

    // Bake-controls UI mirror. The session holds the authoritative range +
    // fps the in-flight job is using; these are user-editable until they
    // hit "Bake".
    float m_bakeStart = 0.0f;
    float m_bakeEnd   = 0.0f;
    float m_bakeFps   = 24.0f;
    bool  m_bakeRangeInitialized = false;     // first-load: copy from clip duration
    int   m_lastSeenClipIndex    = -1;        // detect clip switch → re-init range
};
