#include "InspectorPanel.h"
#include "UIStyle.h"
#include "FileDialog.h"
#include "post-process/PostProcessChain.h"
#include "Scene.h"
#include "CaptureSystem.h"

#include <cstdio>

void InspectorPanel::SetRenderers(
	std::vector<std::unique_ptr<RenderTechnique>>* renderers,
	size_t* activeIndex)
{
	m_renderers = renderers;
	m_active_index = activeIndex;
}

static void DrawTechniqueParameter(TechniqueParameter& param) {
	bool changed = false;
	switch (param.type) {
	case TechniqueParameter::Float: {
		const char* fmt = param.format.empty() ? "%.3f" : param.format.c_str();
		changed = ImGui::SliderFloat(param.label.c_str(), static_cast<float*>(param.data), param.min, param.max, fmt);
		break;
	}
	case TechniqueParameter::Int:
		changed = ImGui::SliderInt(param.label.c_str(), static_cast<int*>(param.data), (int)param.min, (int)param.max);
		break;
	case TechniqueParameter::Bool:
		changed = ImGui::Checkbox(param.label.c_str(), static_cast<bool*>(param.data));
		break;
	case TechniqueParameter::Color3:
		changed = ImGui::ColorEdit3(param.label.c_str(), static_cast<float*>(param.data));
		break;
	case TechniqueParameter::Color4:
		changed = ImGui::ColorEdit4(param.label.c_str(), static_cast<float*>(param.data));
		break;
	case TechniqueParameter::Vec3:
		changed = ImGui::DragFloat3(param.label.c_str(), static_cast<float*>(param.data),
		                            param.speed, param.min, param.max, "%.3f");
		break;
	case TechniqueParameter::Text: {
		const char* val = param.textValue ? param.textValue->c_str() : "";
		ImGui::TextColored(UIStyle::kTextDim, "%s: %s", param.label.c_str(), val);
		return;
	}
	case TechniqueParameter::Enum: {
		int* val = static_cast<int*>(param.data);
		if (!param.enumLabels.empty()) {
			changed = ImGui::Combo(param.label.c_str(), val, param.enumLabels.data(), (int)param.enumLabels.size());
		}
		break;
	}
	case TechniqueParameter::File: {
		if (param.filePath) {
			auto pos = param.filePath->find_last_of('/');
			std::string display = (pos != std::string::npos) ? param.filePath->substr(pos + 1) : *param.filePath;
			if (display.empty()) display = "(none)";
			ImGui::TextColored(UIStyle::kTextDim, "%s", display.c_str());
			ImGui::SameLine();
			std::string buttonLabel = "Browse##" + param.label;
			if (ImGui::Button(buttonLabel.c_str())) {
				std::string result = FileDialog::OpenFile(param.label, param.fileFilters, *param.filePath);
				if (!result.empty() && result != *param.filePath) {
					*param.filePath = result;
					if (param.onFileChanged) param.onFileChanged(result);
				}
			}
		}
		return;
	}
	case TechniqueParameter::Button: {
		const float w = ImGui::GetContentRegionAvail().x;
		if (ImGui::Button(param.label.c_str(), ImVec2(w, 0))) {
			if (param.onClicked) param.onClicked();
		}
		return;
	}
	case TechniqueParameter::Header: {
		UIStyle::SectionHeader(param.label.c_str());
		return;
	}
	}
	if (changed && param.onChanged) param.onChanged();
}

void InspectorPanel::Draw() {
	ImGui::Begin("Inspector");

	if (!ImGui::BeginTabBar("##inspector_tabs", ImGuiTabBarFlags_None)) {
		ImGui::End();
		return;
	}

	// === Selected Node tab ===
	// Lights up only when something is selected; otherwise an empty-state hint
	// tells the user to pick a node from Hierarchy.
	if (ImGui::BeginTabItem("Selected")) {
		if (m_selected_node) {
			ImGui::PushID(static_cast<const void*>(m_selected_node));
			ImGui::TextColored(UIStyle::kTextDim, "Node");
			ImGui::SameLine(0, 6);
			ImGui::TextUnformatted(m_selected_node->GetDisplayName().c_str());
			ImGui::Separator();
			auto& params = m_selected_node->GetParameters();
			if (params.empty()) {
				ImGui::TextColored(UIStyle::kTextDim, "No parameters");
			} else {
				for (auto& p : params) DrawTechniqueParameter(p);
			}
			ImGui::PopID();
		} else {
			ImGui::TextColored(UIStyle::kTextDim, "Select a node in Hierarchy.");
		}
		ImGui::EndTabItem();
	}

	// === Scene tab — lighting + sky + post-process ===
	if (ImGui::BeginTabItem("Scene")) {
		if (m_lighting) {
			UIStyle::SectionHeader("Lighting");
			ImGui::SliderFloat("Sun Azimuth",      &m_lighting->sunAzimuth,   -180.0f, 180.0f, "%.1f");
			ImGui::SliderFloat("Sun Elevation",    &m_lighting->sunElevation, -20.0f,  90.0f,  "%.1f");
			ImGui::SliderFloat("Sun Angular Size", &m_lighting->sunAngularSize, 0.1f, 15.0f, "%.2f deg");
			ImGui::SliderFloat("Sun Intensity",    &m_lighting->sunIntensity, 0.0f, 5.0f, "%.2f");
			ImGui::ColorEdit3("Sun Color",         m_lighting->sunColor);
			ImGui::Separator();
			ImGui::SliderFloat("Ambient Intensity", &m_lighting->ambientIntensity, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("AO Strength",       &m_lighting->aoStrength,       0.0f, 1.0f, "%.2f");
			ImGui::Checkbox   ("Sun Shadows",       &m_lighting->shadowsEnabled);
		}

		if (m_sky) {
			UIStyle::SectionHeader("Sky");
			ImGui::ColorEdit3("Sky Color", &m_sky->color.x);
		}

		if (m_post_process) {
			UIStyle::SectionHeader("Post-Processing");
			for (size_t i = 0; i < m_post_process->GetEffectCount(); i++) {
				auto* fx = m_post_process->GetEffect(i);
				ImGui::PushID(static_cast<int>(i));
				if (ImGui::TreeNodeEx(fx->GetDisplayName().c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
					auto& params = fx->GetParameters();
					if (params.empty()) {
						ImGui::TextColored(UIStyle::kTextDim, "No parameters");
					} else {
						for (auto& p : params) DrawTechniqueParameter(p);
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTabItem();
	}

	// === Camera tab ===
	if (ImGui::BeginTabItem("Camera")) {
		if (m_camera) {
			glm::vec3 pos = m_camera->GetPosition();
			if (ImGui::DragFloat3("Position", &pos.x, 0.1f)) m_camera->SetPosition(pos);

			glm::vec3 fwd = m_camera->GetForward();
			ImGui::TextColored(UIStyle::kTextDim, "Forward  %.2f, %.2f, %.2f", fwd.x, fwd.y, fwd.z);

			float fov = m_camera->GetFOV();
			if (ImGui::SliderFloat("FOV", &fov, 1.0f, 120.0f, "%.0f")) m_camera->SetFOV(fov);

			float znear = m_camera->GetNear();
			float zfar  = m_camera->GetFar();
			bool changed = false;
			changed |= ImGui::DragFloat("Near", &znear, 0.01f, 0.001f, zfar - 0.001f, "%.3f");
			changed |= ImGui::DragFloat("Far",  &zfar,  1.0f,  znear + 0.001f, 10000.0f, "%.1f");
			if (changed) m_camera->SetNearFar(znear, zfar);

			if (ImGui::Button("Reset Camera")) {
				m_camera->SetPosition(glm::vec3(0.0f, -3.0f, 0.0f));
				m_camera->SetForward(glm::vec3(0.0f, 1.0f, 0.0f));
				m_camera->SetFOV(45.0f);
				m_camera->SetNearFar(0.001f, 10000.0f);
			}
		}

		UIStyle::SectionHeader("Controls");
		if (m_sensitivity) ImGui::SliderFloat("Sensitivity", m_sensitivity, 0.01f, 2.0f, "%.3f");
		if (m_speed)       ImGui::SliderFloat("Fly Speed",   m_speed,       0.1f, 50.0f, "%.1f");
		ImGui::EndTabItem();
	}

	// === Renderer tab — technique selection + parameters ===
	// First-frame latch: open this tab on startup. ImGuiTabItemFlags_SetSelected
	// must NOT be passed every frame or it would re-select on every draw and
	// the user could never switch tabs.
	ImGuiTabItemFlags rendererTabFlags = ImGuiTabItemFlags_None;
	if (!m_default_tab_applied) {
		rendererTabFlags |= ImGuiTabItemFlags_SetSelected;
		m_default_tab_applied = true;
	}
	if (ImGui::BeginTabItem("Renderer", nullptr, rendererTabFlags)) {
		if (m_renderers && m_active_index) {
			UIStyle::SectionHeader("Technique");
			std::string currentName = (*m_renderers)[*m_active_index]->GetDisplayName();
			if (ImGui::BeginCombo("##technique", currentName.c_str())) {
				for (size_t i = 0; i < m_renderers->size(); i++) {
					bool selected = (i == *m_active_index);
					std::string name = (*m_renderers)[i]->GetDisplayName();
					if (ImGui::Selectable(name.c_str(), selected)) {
						if (i != *m_active_index && m_switch_callback) m_switch_callback(i);
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			auto paths = (*m_renderers)[*m_active_index]->GetShaderPaths();
			for (const auto& path : paths) {
				auto pos = path.find_last_of('/');
				std::string name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
				ImGui::TextColored(UIStyle::kTextDim, "%s", name.c_str());
			}

			if (ImGui::Button("Reload Shaders (F5)")) {
				if (m_reload_callback) m_reload_callback();
			}

			UIStyle::SectionHeader("Parameters");
			auto& params = (*m_renderers)[*m_active_index]->GetParameters();
			if (params.empty()) {
				ImGui::TextColored(UIStyle::kTextDim, "No parameters");
			} else {
				for (auto& p : params) DrawTechniqueParameter(p);
			}
		}

		ImGui::EndTabItem();
	}

	// === Capture tab — screenshots + MP4 recording ===
	// Owns its own state (callbacks, last paths) and reads live recording
	// status from the CaptureSystem pointer set by Application. Recording
	// settings (pacing, back-pressure, duration cap, fps, bitrate) edit the
	// CaptureSystem's options directly via reference — no per-knob plumbing.
	if (ImGui::BeginTabItem("Capture")) {
		// --- Screenshot ---
		UIStyle::SectionHeader("Screenshot");
		if (ImGui::Button("Take Screenshot (F12)")) {
			if (m_screenshot_callback) m_screenshot_callback();
		}
		if (!m_last_screenshot_path.empty()) {
			ImGui::TextColored(UIStyle::kTextDim, "%s", m_last_screenshot_path.c_str());
		}

		// --- Recording ---
		UIStyle::SectionHeader("Recording");

		const bool recording = m_capture && m_capture->IsRecording();
		Capture::RecordingStatus status = m_capture ? m_capture->GetStatus()
		                                            : Capture::RecordingStatus{};

		// Toggle button — flips label and color based on state. We re-route
		// through the rendering event queue (callback set by Application) so
		// the actual ToggleRecording runs on the same drain pass as other
		// AppEvents — keeps lifecycle ordering consistent across the engine.
		const char* btnLabel = recording ? "Stop Recording (P)" : "Start Recording (P)";
		ImVec4 btnColor = recording ? UIStyle::kBudgetOver : UIStyle::kAccent;
		ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIStyle::Lighten(btnColor, 1.15f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  UIStyle::Darken(btnColor, 0.85f));
		if (ImGui::Button(btnLabel, ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
			if (m_toggle_recording_callback) m_toggle_recording_callback();
		}
		ImGui::PopStyleColor(3);

		// Live readout when recording. Cap is read off the live options so
		// changing it mid-recording reflects immediately in the timer label
		// (the actual cap enforcement is on the CaptureSystem side).
		if (recording && m_capture) {
			const int cap = m_capture->GetOptions().maxDurationSeconds;
			char tbuf[64];
			std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d / %02d:%02d",
				static_cast<int>(status.elapsedSeconds) / 60,
				static_cast<int>(status.elapsedSeconds) % 60,
				cap / 60, cap % 60);
			ImGui::TextColored(UIStyle::kAccent, "REC  %s", tbuf);

			char fbuf[96];
			std::snprintf(fbuf, sizeof(fbuf), "%u captured  %u dropped",
				status.framesCaptured, status.framesDropped);
			ImGui::TextColored(UIStyle::kTextDim, "%s", fbuf);
		}
		if (!m_last_recording_path.empty()) {
			ImGui::TextColored(UIStyle::kTextDim, "Last: %s", m_last_recording_path.c_str());
		}

		// --- Settings ---
		// Disabled while recording so we don't churn encoder state mid-take.
		// (Pacing / fps / bitrate would need a re-Open of the encoder.)
		UIStyle::SectionHeader("Settings");
		if (m_capture) {
			auto& opts = m_capture->GetOptions();
			const bool live = recording;
			if (live) ImGui::BeginDisabled();

			// Pacing radios
			ImGui::TextColored(UIStyle::kTextDim, "Pacing");
			int pacing = static_cast<int>(opts.pacing);
			ImGui::RadioButton("Real-time##pacing",  &pacing, static_cast<int>(Capture::Pacing::RealTime));
			ImGui::SameLine();
			ImGui::RadioButton("Fixed-step##pacing", &pacing, static_cast<int>(Capture::Pacing::FixedStep));
			opts.pacing = static_cast<Capture::Pacing>(pacing);

			// Back-pressure radios — independent of pacing for A/B testing.
			ImGui::TextColored(UIStyle::kTextDim, "Back-pressure");
			int bp = static_cast<int>(opts.backPressure);
			ImGui::RadioButton("Drop##bp",  &bp, static_cast<int>(Capture::BackPressure::Drop));
			ImGui::SameLine();
			ImGui::RadioButton("Block##bp", &bp, static_cast<int>(Capture::BackPressure::Block));
			opts.backPressure = static_cast<Capture::BackPressure>(bp);

			ImGui::SliderInt("Max Duration (s)", &opts.maxDurationSeconds, 5, 60);
			ImGui::SliderInt("Target FPS",       &opts.targetFps,           24, 120);
			ImGui::SliderInt("Bitrate (kbps)",   &opts.bitrateKbps,        500, 40000);

			if (live) ImGui::EndDisabled();
		}

		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
	ImGui::End();
}
