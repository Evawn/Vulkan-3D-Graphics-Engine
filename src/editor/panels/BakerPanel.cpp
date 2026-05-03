#include "BakerPanel.h"
#include "GltfImportTechnique.h"
#include "FileDialog.h"
#include "UIStyle.h"

#include <algorithm>
#include <cstdio>
#include <string>

void BakerPanel::Draw() {
    if (!m_technique) {
        ImGui::TextColored(UIStyle::kTextDim, "(no GLB import technique wired)");
        return;
    }
    const auto& session = m_technique->GetSession();

    // ============================================================
    // Source
    // ============================================================
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Open .glb\xE2\x80\xA6")) {        // U+2026 …
            m_wantOpenFileDialog = true;
        }
        ImGui::SameLine();
        if (session.hasLoadedAsset) {
            ImGui::TextUnformatted(session.sourceFileName.c_str());
        } else {
            ImGui::TextColored(UIStyle::kTextDim, "(no file loaded)");
        }

        if (m_wantOpenFileDialog) {
            // Open synchronously via the native file dialog. Returns empty
            // string on cancel; the technique's LoadGlb is then a no-op.
            m_wantOpenFileDialog = false;
            std::string path = FileDialog::OpenFile(
                "Open glTF Binary",
                {"glb"},
                "");
            if (!path.empty()) {
                m_technique->LoadGlb(path);
            }
        }

        if (session.hasLoadedAsset) {
            ImGui::Text("Nodes:    %zu", session.totalNodes);
            ImGui::Text("Skins:    %zu", session.totalSkins);
            ImGui::Text("Prims:    %zu", session.totalPrimitives);
            ImGui::Text("Verts:    %zu", session.totalVertices);
            ImGui::Text("Tris:     %zu", session.totalTriangles);
            ImGui::Text("Clips:    %zu", session.clipNames.size());
        }
    }

    // ============================================================
    // Animation
    // ============================================================
    if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!session.hasLoadedAsset || session.clipNames.empty()) {
            ImGui::TextColored(UIStyle::kTextDim, "Load a .glb with animations to enable.");
        } else {
            // Clip combo
            int activeIdx = std::clamp(session.activeClipIndex, 0,
                static_cast<int>(session.clipNames.size()) - 1);
            const char* preview = session.clipNames[activeIdx].c_str();
            if (ImGui::BeginCombo("Clip", preview)) {
                for (size_t i = 0; i < session.clipNames.size(); ++i) {
                    bool sel = (static_cast<int>(i) == activeIdx);
                    char label[160];
                    std::snprintf(label, sizeof(label), "%s  (%.2fs)",
                        session.clipNames[i].c_str(), session.clipDurations[i]);
                    if (ImGui::Selectable(label, sel)) {
                        m_technique->SelectClip(static_cast<int>(i));
                    }
                }
                ImGui::EndCombo();
            }

            // Transport: play / pause / time scrub / speed
            const float duration = session.clipDurations[activeIdx];
            float currentTime = m_technique->GetTime();
            currentTime = std::clamp(currentTime, 0.0f, duration);

            if (ImGui::Button("\xE2\x8F\xAF")) {  // U+23EF play/pause
                m_technique->SetPaused(false);
            }
            ImGui::SameLine();
            if (ImGui::Button("\xE2\x8F\xB8")) {  // U+23F8 pause
                m_technique->SetPaused(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart")) {
                m_technique->SetTime(0.0f);
            }

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##time", &currentTime, 0.0f, std::max(0.001f, duration),
                "t = %.2fs"))
            {
                m_technique->SetTime(currentTime);
                m_technique->SetPaused(true);   // scrubbing implies pause
            }
            ImGui::Text("duration: %.2fs", duration);

            static float speed = 1.0f;
            if (ImGui::SliderFloat("speed", &speed, 0.0f, 4.0f, "%.2fx")) {
                m_technique->SetPlaybackSpeed(speed);
            }
        }
    }

    // ============================================================
    // Voxelization
    // ============================================================
    if (ImGui::CollapsingHeader("Voxelization", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!session.hasLoadedAsset) {
            ImGui::TextColored(UIStyle::kTextDim,
                "Load a .glb first — the voxel preview bakes the active pose.");
        } else {
            // ---- Voxel size slider (debounced re-bake on release) ----
            //
            // Logarithmic-ish range: 1mm to 1m. Most useful range for a 10-unit
            // asset (the AnimatedOak scaled into the viewport) is 1cm–10cm —
            // the slider sits there by default.
            float voxelSize = m_technique->GetVoxelSize();
            if (ImGui::SliderFloat("voxel size", &voxelSize, 0.005f, 1.0f, "%.4f m",
                ImGuiSliderFlags_Logarithmic))
            {
                m_technique->SetVoxelSize(voxelSize);
            }

            // ---- View mode toggle ----
            //
            // Pure UI state; the technique selects which pass actually draws.
            // Overlay (M6) lands when alpha-composited mesh+voxel is wired.
            int mode = static_cast<int>(m_technique->GetPreviewMode());
            const char* labels[] = { "Mesh", "Voxels" };
            for (int i = 0; i < 2; ++i) {
                if (i > 0) ImGui::SameLine();
                bool selected = (mode == i);
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button,
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(labels[i])) {
                    m_technique->SetPreviewMode(
                        i == 0 ? GltfImportTechnique::PreviewMode::Mesh
                               : GltfImportTechnique::PreviewMode::Voxels);
                }
                if (selected) ImGui::PopStyleColor();
            }

            // ---- Bake status line ----
            //
            // Three states: never-baked (drag the slider), in-flight (worker
            // is voxelizing), and complete (show grid + voxel size used).
            // budgetExceeded is sticky until the next successful bake — keeps
            // the warning visible during slider exploration.
            ImGui::Spacing();
            if (m_technique->IsBakingPreview()) {
                ImGui::TextColored(UIStyle::kTextDim, "Baking...");
            } else if (session.lastBudgetExceeded) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                    "Cell budget exceeded — increase voxel size.");
            } else if (session.hasBake) {
                ImGui::Text("Grid: %u x %u x %u  @ %.4f m",
                    session.previewVolumeSize.x,
                    session.previewVolumeSize.y,
                    session.previewVolumeSize.z,
                    session.previewVoxelSize);
            } else {
                ImGui::TextColored(UIStyle::kTextDim,
                    "No bake yet — adjust voxel size to bake.");
            }
        }
    }

    // ============================================================
    // Bake animation (M4 placeholder)
    // ============================================================
    if (ImGui::CollapsingHeader("Bake animation")) {
        ImGui::TextColored(UIStyle::kTextDim,
            "Full-animation bake + .vxa export land in M4.");
        ImGui::BeginDisabled();
        ImGui::Button("Bake animation");
        ImGui::SameLine();
        ImGui::Button("Save\xE2\x80\xA6");
        ImGui::SameLine();
        ImGui::Button("Promote to scene");
        ImGui::EndDisabled();
    }
}
