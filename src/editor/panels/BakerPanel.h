#pragma once

#include "imgui.h"

#include <functional>

class GltfImportTechnique;
struct GltfImportSession;

// ---- BakerPanel ----
//
// The "studio" UI for the GLB Import & Bake workspace. Reads from the
// GltfImportTechnique's session and writes via its public mutators (LoadGlb,
// SelectClip, SetPaused/Speed/Time). This panel owns no engine state of its
// own — it's a thin dialog over the technique.
//
// v1 sections:
//   - Source: file picker, basename + counts
//   - Animation: clip dropdown, play/pause/scrub, speed slider
//   - Voxelization: placeholders for M3 (voxel size slider, view mode toggle)
//   - Bake animation: placeholders for M4 (range, fps, bake/save buttons)

class BakerPanel {
public:
    void SetTechnique(GltfImportTechnique* tech) { m_technique = tech; }
    void Draw();

private:
    GltfImportTechnique* m_technique = nullptr;

    // Latched UI scratch so the file dialog only opens when the user actually
    // clicks the button (not every frame the button is hovered, which would
    // happen if we drove it directly from ImGui::Button).
    bool m_wantOpenFileDialog = false;
};
