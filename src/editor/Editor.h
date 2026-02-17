#pragma once

#include "GUIRenderer.h"
#include "ViewportPanel.h"
#include "MetricsPanel.h"
#include "OutputPanel.h"
#include "InspectorPanel.h"
#include "VulkanContext.h"
#include "Sampler.h"
#include "ImageView.h"
#include "Camera.h"
#include "CameraController.h"
#include "RenderTechnique.h"

#include <memory>
#include <functional>
#include <vector>

class Window;

class Editor {
public:
	Editor() = default;

	// Phase 1: ImGui backend initialization (needs VulkanContext + Window)
	void InitImGui(VWrap::VulkanContext& vk,
				   std::shared_ptr<VWrap::RenderPass> renderPass,
				   Window& window);

	// Phase 2: Panel construction and wiring (needs camera, renderers, etc.)
	void InitPanels(std::vector<std::unique_ptr<RenderTechnique>>* renderers,
					size_t* activeRendererIndex,
					std::shared_ptr<Camera> camera,
					std::shared_ptr<CameraController> cameraController,
					VWrap::VulkanContext& vk);

	void Shutdown();

	// Per-frame
	void BeginFrame();
	void CmdDraw(std::shared_ptr<VWrap::CommandBuffer> cmd);

	// Scene texture bridge (Renderer output -> ImGui viewport)
	void RegisterSceneTexture(std::shared_ptr<VWrap::ImageView> resolveView);
	void RemoveSceneTexture();
	void UpdateViewportTexture();

	// Event callbacks (set by Application)
	void SetReloadCallback(std::function<void()> cb);
	void SetSwitchCallback(std::function<void(size_t)> cb);
	void SetScreenshotCallback(std::function<void()> cb);
	void SetWireframeCallback(std::function<void()> cb);

	// Queries
	bool ViewportWasResized() const;
	bool ViewportWasClicked() const;
	VkExtent2D GetDesiredViewportExtent() const;

	// DPI
	void OnDpiChanged(float newScale);

	// Metrics
	void UpdateMetrics(float fps, float gpuMs, float frameMs);

	// Screenshot path forwarding
	void SetLastScreenshotPath(const std::string& path);

private:
	std::shared_ptr<GUIRenderer> m_gui;
	std::shared_ptr<VWrap::Sampler> m_scene_sampler;
	VkDescriptorSet m_scene_texture = VK_NULL_HANDLE;

	ViewportPanel m_viewport;
	MetricsPanel m_metrics;
	OutputPanel m_output;
	InspectorPanel m_inspector;
};
