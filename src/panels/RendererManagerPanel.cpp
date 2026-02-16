#include "RendererManagerPanel.h"

void RendererManagerPanel::SetRenderers(
	std::vector<std::unique_ptr<RenderTechnique>>* renderers,
	size_t* activeIndex)
{
	m_renderers = renderers;
	m_active_index = activeIndex;
}

void RendererManagerPanel::Draw() {
	ImGui::Begin("Renderer");

	if (m_renderers && m_active_index) {
		// Renderer selection combo
		std::string currentName = (*m_renderers)[*m_active_index]->GetName();
		if (ImGui::BeginCombo("Technique", currentName.c_str())) {
			for (size_t i = 0; i < m_renderers->size(); i++) {
				bool selected = (i == *m_active_index);
				std::string name = (*m_renderers)[i]->GetName();
				if (ImGui::Selectable(name.c_str(), selected)) {
					if (i != *m_active_index && m_switch_callback) {
						m_switch_callback(i);
					}
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		ImGui::Separator();

		// Shader paths
		ImGui::Text("Shaders:");
		auto paths = (*m_renderers)[*m_active_index]->GetShaderPaths();
		for (const auto& path : paths) {
			// Show just the filename
			auto pos = path.find_last_of('/');
			std::string name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
			ImGui::BulletText("%s", name.c_str());
		}

		ImGui::Separator();

		if (ImGui::Button("Reload Shaders (F5)")) {
			if (m_reload_callback) m_reload_callback();
		}
	}

	ImGui::Separator();

	// Camera controls
	if (m_sensitivity) {
		ImGui::SliderFloat("Mouse Sensitivity", m_sensitivity, 0.01f, 2.0f, "%.3f");
	}
	if (m_speed) {
		ImGui::SliderFloat("Movement Speed", m_speed, 0.1f, 10.0f, "%.3f");
	}

	ImGui::End();
}
