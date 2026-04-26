#include "InspectorPanel.h"
#include "UIStyle.h"
#include "FileDialog.h"
#include "post-process/PostProcessChain.h"
#include "Scene.h"

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
	case TechniqueParameter::Float:
		changed = ImGui::SliderFloat(param.label.c_str(), static_cast<float*>(param.data), param.min, param.max);
		break;
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
		// Read-only text row: dim label + dim value.
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
					if (param.onFileChanged) {
						param.onFileChanged(result);
					}
				}
			}
		}
		// File handles its own change channel via onFileChanged; skip onChanged.
		return;
	}
	}
	if (changed && param.onChanged) param.onChanged();
}

void InspectorPanel::Draw() {
	ImGui::Begin("Inspector");

	// === Selected Node === (top — most context-sensitive piece)
	if (m_selected_node) {
		ImGui::PushID(static_cast<const void*>(m_selected_node));
		const std::string header = "Node: " + m_selected_node->GetDisplayName();
		if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			auto& params = m_selected_node->GetParameters();
			if (params.empty()) {
				ImGui::TextColored(UIStyle::kTextDim, "No parameters");
			} else {
				for (auto& p : params) DrawTechniqueParameter(p);
			}
		}
		ImGui::PopID();
	}

	// === Technique ===
	if (ImGui::CollapsingHeader("Technique", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (m_renderers && m_active_index) {
			std::string currentName = (*m_renderers)[*m_active_index]->GetDisplayName();
			if (ImGui::BeginCombo("##technique", currentName.c_str())) {
				for (size_t i = 0; i < m_renderers->size(); i++) {
					bool selected = (i == *m_active_index);
					std::string name = (*m_renderers)[i]->GetDisplayName();
					if (ImGui::Selectable(name.c_str(), selected)) {
						if (i != *m_active_index && m_switch_callback) {
							m_switch_callback(i);
						}
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
		}
	}

	// === Parameters ===
	if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (m_renderers && m_active_index) {
			auto& params = (*m_renderers)[*m_active_index]->GetParameters();
			if (params.empty()) {
				ImGui::TextColored(UIStyle::kTextDim, "No parameters");
			} else {
				for (auto& param : params) {
					DrawTechniqueParameter(param);
				}
			}
		}
	}

	// === Sky ===
	if (m_sky && ImGui::CollapsingHeader("Sky", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::ColorEdit3("Sky Color", &m_sky->color.x);
	}

	// === Lighting ===
	if (m_lighting && ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Sun Azimuth",   &m_lighting->sunAzimuth,   -180.0f, 180.0f, "%.1f");
		ImGui::SliderFloat("Sun Elevation", &m_lighting->sunElevation, -20.0f,  90.0f,  "%.1f");
		ImGui::SliderFloat("Sun Angular Size", &m_lighting->sunAngularSize, 0.1f, 15.0f, "%.2f deg");
		ImGui::SliderFloat("Sun Intensity", &m_lighting->sunIntensity, 0.0f, 5.0f, "%.2f");
		ImGui::ColorEdit3("Sun Color",      m_lighting->sunColor);
		ImGui::Separator();
		ImGui::SliderFloat("Ambient Intensity", &m_lighting->ambientIntensity, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("AO Strength",       &m_lighting->aoStrength,       0.0f, 1.0f, "%.2f");
		ImGui::Checkbox   ("Sun Shadows",       &m_lighting->shadowsEnabled);
	}

	// === Post-Processing ===
	if (m_post_process && ImGui::CollapsingHeader("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
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

	// === Camera ===
	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (m_camera) {
			glm::vec3 pos = m_camera->GetPosition();
			if (ImGui::DragFloat3("Position", &pos.x, 0.1f)) {
				m_camera->SetPosition(pos);
			}

			glm::vec3 fwd = m_camera->GetForward();
			ImGui::TextColored(UIStyle::kTextDim, "Forward  %.2f, %.2f, %.2f", fwd.x, fwd.y, fwd.z);

			float fov = m_camera->GetFOV();
			if (ImGui::SliderFloat("FOV", &fov, 1.0f, 120.0f, "%.0f")) {
				m_camera->SetFOV(fov);
			}

			float znear = m_camera->GetNear();
			float zfar = m_camera->GetFar();
			bool changed = false;
			changed |= ImGui::DragFloat("Near", &znear, 0.01f, 0.001f, zfar - 0.001f, "%.3f");
			changed |= ImGui::DragFloat("Far", &zfar, 1.0f, znear + 0.001f, 10000.0f, "%.1f");
			if (changed) m_camera->SetNearFar(znear, zfar);
		}

		if (m_sensitivity) {
			ImGui::SliderFloat("Sensitivity", m_sensitivity, 0.01f, 2.0f, "%.3f");
		}
		if (m_speed) {
			ImGui::SliderFloat("Fly Speed", m_speed, 0.1f, 50.0f, "%.1f");
		}

		if (m_camera && ImGui::Button("Reset Camera")) {
			m_camera->SetPosition(glm::vec3(0.0f, -3.0f, 0.0f));
			m_camera->SetForward(glm::vec3(0.0f, 1.0f, 0.0f));
			m_camera->SetFOV(45.0f);
			m_camera->SetNearFar(0.1f, 100.0f);
		}
	}

	// === Screenshot ===
	if (ImGui::CollapsingHeader("Screenshot")) {
		if (ImGui::Button("Capture")) {
			if (m_screenshot_callback) m_screenshot_callback();
		}
		if (!m_last_screenshot_path.empty()) {
			ImGui::TextColored(UIStyle::kTextDim, "%s", m_last_screenshot_path.c_str());
		}
	}

	ImGui::End();
}
