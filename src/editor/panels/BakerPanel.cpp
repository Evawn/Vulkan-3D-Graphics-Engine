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
    // Voxelization (M3 placeholder)
    // ============================================================
    if (ImGui::CollapsingHeader("Voxelization")) {
        ImGui::TextColored(UIStyle::kTextDim,
            "Live voxel preview lands in M3.");
        ImGui::BeginDisabled();
        static float voxelSize = 0.1f;
        ImGui::SliderFloat("voxel size", &voxelSize, 0.01f, 1.0f, "%.3fm");
        const char* viewModes[] = { "Mesh", "Voxels", "Overlay" };
        static int viewMode = 0;
        ImGui::Combo("view", &viewMode, viewModes, IM_ARRAYSIZE(viewModes));
        ImGui::EndDisabled();
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
