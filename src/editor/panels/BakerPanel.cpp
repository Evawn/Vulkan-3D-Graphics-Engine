#include "BakerPanel.h"
#include "GltfImportTechnique.h"
#include "FileDialog.h"
#include "UIStyle.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
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
            // Logarithmic range: 5mm to 5m. With frame-as-array-layer packing
            // the old Z-slab packed-depth ceiling no longer bites; remaining
            // caps are frameCount <= maxImageArrayLayers (2048 on Apple
            // Silicon → 85s @ 24fps) and size.y * size.z <=
            // maxImageDimension2D (16384). The upper-end voxel size is kept
            // as a coarse-bake escape hatch for cell-budget pressure.
            float voxelSize = m_technique->GetVoxelSize();
            if (ImGui::SliderFloat("voxel size", &voxelSize, 0.005f, 5.0f, "%.4f m",
                ImGuiSliderFlags_Logarithmic))
            {
                m_technique->SetVoxelSize(voxelSize);
            }

            // ---- Color source (M5) ----
            //
            // Material vs Texture. Toggling reuses the voxel-size debounce
            // path on the technique side — there's a ~250ms wait, then a
            // re-bake. Disabled when the loaded asset has no textures
            // (radio still visible so the user can see why).
            ImGui::TextUnformatted("Color source:");
            ImGui::SameLine();
            const auto cs = m_technique->GetColorSource();
            const bool noTextures = (session.totalTextures == 0);
            ImGui::BeginDisabled(noTextures);
            if (ImGui::RadioButton("Material",
                cs == voxel_bake::VoxColorSource::Mode::MaterialBaseColor))
            {
                m_technique->SetColorSource(voxel_bake::VoxColorSource::Mode::MaterialBaseColor);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Texture",
                cs == voxel_bake::VoxColorSource::Mode::TextureSampled))
            {
                m_technique->SetColorSource(voxel_bake::VoxColorSource::Mode::TextureSampled);
            }
            ImGui::EndDisabled();
            if (noTextures) {
                ImGui::SameLine();
                ImGui::TextColored(UIStyle::kTextDim, "(no textures)");
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
                if (session.hasFullBake) {
                    ImGui::Text("Grid: %u x %u x %u  @ %.4f m  (%u frames @ %.0f fps)",
                        session.previewVolumeSize.x,
                        session.previewVolumeSize.y,
                        session.previewVolumeSize.z,
                        session.previewVoxelSize,
                        session.bakeFrameCount,
                        session.bakeFps);
                } else {
                    ImGui::Text("Grid: %u x %u x %u  @ %.4f m",
                        session.previewVolumeSize.x,
                        session.previewVolumeSize.y,
                        session.previewVolumeSize.z,
                        session.previewVoxelSize);
                }
            } else {
                ImGui::TextColored(UIStyle::kTextDim,
                    "No bake yet — adjust voxel size to bake.");
            }
        }
    }

    // ============================================================
    // Bake animation (M4)
    // ============================================================
    if (ImGui::CollapsingHeader("Bake animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool clipReady = session.hasLoadedAsset
                            && session.activeClipIndex >= 0
                            && session.activeClipIndex < static_cast<int>(session.clipDurations.size());
        if (!clipReady) {
            ImGui::TextColored(UIStyle::kTextDim,
                "Load a .glb with at least one animation clip to bake.");
        } else {
            const float clipDuration = session.clipDurations[session.activeClipIndex];

            // First-time / clip-switch initialization of the local range
            // mirror. We default to the full clip duration so the user can
            // just hit "Bake" without touching anything for the demo case.
            if (!m_bakeRangeInitialized || m_lastSeenClipIndex != session.activeClipIndex) {
                m_bakeStart = 0.0f;
                m_bakeEnd   = clipDuration;
                m_lastSeenClipIndex = session.activeClipIndex;
                m_bakeRangeInitialized = true;
            }

            // Range sliders — clamped to the clip's duration.
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("range start", &m_bakeStart, 0.0f, clipDuration, "%.2fs")) {
                m_bakeStart = std::clamp(m_bakeStart, 0.0f, m_bakeEnd);
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("range end",   &m_bakeEnd,   0.0f, clipDuration, "%.2fs")) {
                m_bakeEnd = std::clamp(m_bakeEnd, m_bakeStart, clipDuration);
            }

            // FPS dropdown — common multiples of the engine display rate.
            const float fpsOptions[] = { 12.0f, 15.0f, 24.0f, 30.0f, 48.0f, 60.0f };
            int fpsIdx = 2;     // 24 default
            for (int i = 0; i < (int)(sizeof(fpsOptions) / sizeof(fpsOptions[0])); ++i) {
                if (std::abs(fpsOptions[i] - m_bakeFps) < 0.5f) { fpsIdx = i; break; }
            }
            char fpsLabel[16];
            std::snprintf(fpsLabel, sizeof(fpsLabel), "%.0f fps", m_bakeFps);
            if (ImGui::BeginCombo("fps", fpsLabel)) {
                for (int i = 0; i < (int)(sizeof(fpsOptions) / sizeof(fpsOptions[0])); ++i) {
                    char label[16]; std::snprintf(label, sizeof(label), "%.0f", fpsOptions[i]);
                    bool sel = (i == fpsIdx);
                    if (ImGui::Selectable(label, sel)) m_bakeFps = fpsOptions[i];
                }
                ImGui::EndCombo();
            }

            // Predicted frame count (matches RunFullBake's schedule:
            // inclusive both ends, so duration*fps + 1 samples).
            const float duration  = std::max(0.0f, m_bakeEnd - m_bakeStart);
            const int   frameCount = std::max(1, static_cast<int>(std::round(duration * m_bakeFps)) + 1);
            ImGui::Text("Frames: %d  (%.2fs * %.0ffps + 1)", frameCount, duration, m_bakeFps);

            // Frame-as-array-layer pack budget. The animated volume now
            // packs each frame as one layer of a 2D-array (image extent
            // (size.x, size.y * size.z), arrayLayers = frameCount). Apple
            // Silicon caps both maxImageArrayLayers and maxImageDimension2D
            // at 2048 / 16384 respectively. We don't know size until the
            // bake computes the clip-wide AABB — show the current preview's
            // size as a proxy so the user gets an early warning. If a
            // preview hasn't run yet, hide the hint.
            constexpr uint32_t kMaxFrameCount  = 2048;
            constexpr uint32_t kMaxFrameDimYZ  = 16384;
            if (session.previewVolumeSize.z > 0) {
                const uint32_t packedYZ = session.previewVolumeSize.y * session.previewVolumeSize.z;
                const bool over = (static_cast<uint32_t>(frameCount) > kMaxFrameCount)
                               || (packedYZ > kMaxFrameDimYZ);
                ImVec4 col = over ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : UIStyle::kTextDim;
                ImGui::TextColored(col,
                    "Layers: %d / %u  YZ extent: %u (y=%u * z=%u) / %u",
                    frameCount, kMaxFrameCount,
                    packedYZ, session.previewVolumeSize.y, session.previewVolumeSize.z,
                    kMaxFrameDimYZ);
            }

            // Diagnostic readout — surfaces the disabled-button reason and
            // the live worker state without forcing the user to a terminal.
            const bool baking_dbg = m_technique->IsBakingFull();
            ImGui::TextColored(UIStyle::kTextDim,
                "[dbg] clipReady=%d duration=%.3f baking=%d done=%d total=%d",
                clipReady ? 1 : 0, duration, baking_dbg ? 1 : 0,
                m_technique->FullBakeFramesDone(), m_technique->FullBakeFramesTotal());

            ImGui::Spacing();

            // ---- Bake / Cancel buttons ----
            //
            // The "##bake" / "##cancel" suffixes give these buttons unique
            // ImGui IDs that don't collide with the CollapsingHeader's
            // "Bake animation" label hash. Without the suffix, ImGui treats
            // the header and the button as the same widget for input
            // routing, and the Button() never returns true (the click is
            // attributed to the header instead). The visible label is
            // everything before "##".
            const bool baking = baking_dbg;
            if (baking) {
                if (ImGui::Button("Cancel bake##cancel")) {
                    m_technique->CancelFullBake();
                }
                ImGui::SameLine();
                const int done  = m_technique->FullBakeFramesDone();
                const int total = std::max(1, m_technique->FullBakeFramesTotal());
                const float frac = std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f);
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
                ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
            } else {
                ImGui::BeginDisabled(!clipReady || duration <= 0.0f);
                if (ImGui::Button("Bake animation##bake")) {
                    m_technique->StartFullBake(m_bakeStart, m_bakeEnd, m_bakeFps);
                }
                ImGui::EndDisabled();
            }

            ImGui::Spacing();

            // ---- Save / Load row ----
            ImGui::BeginDisabled(!session.hasFullBake || baking);
            if (ImGui::Button("Save bake\xE2\x80\xA6")) {
                m_wantSaveDialog = true;
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(baking);
            if (ImGui::Button("Load .vxa\xE2\x80\xA6")) {
                m_wantLoadVxaDialog = true;
            }
            ImGui::EndDisabled();

            // ---- Status line ----
            //
            // Surfaces the latest message from the technique (save/load
            // success or failure, cancellation, budget overruns). Cleared
            // when a new bake starts.
            if (!session.lastBakeStatusMessage.empty()) {
                ImVec4 color = session.lastSaveSucceeded
                    ? ImVec4(0.5f, 0.85f, 0.5f, 1.0f)
                    : UIStyle::kTextDim;
                if (session.lastBudgetExceeded) color = ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
                ImGui::TextColored(color, "%s", session.lastBakeStatusMessage.c_str());
            }

            // ---- Dialog handling (deferred to next frame so ImGui can
            //      finish its current draw before we block on the OS) ----
            if (m_wantSaveDialog) {
                m_wantSaveDialog = false;
                // Default name = source basename (stem). User can rename in
                // the save dialog; we strip the .vxa extension before
                // splitting because the writer adds it back.
                std::string defaultName = std::filesystem::path(session.sourceFileName).stem().string();
                if (defaultName.empty()) defaultName = "bake";
                std::string suggested = defaultName + ".vxa";
                std::string picked = FileDialog::SaveFile(
                    "Save Voxel Animation",
                    {"vxa"},
                    suggested,
                    "");
                if (!picked.empty()) {
                    std::filesystem::path p(picked);
                    std::string dir  = p.parent_path().string();
                    std::string name = p.stem().string();
                    if (name.empty()) name = defaultName;
                    m_technique->SaveBake(dir, name);
                }
            }

            if (m_wantLoadVxaDialog) {
                m_wantLoadVxaDialog = false;
                std::string picked = FileDialog::OpenFile(
                    "Load Voxel Animation",
                    {"vxa"},
                    "");
                if (!picked.empty()) {
                    m_technique->LoadBakeFromDisk(picked);
                }
            }
        }
    }
}
