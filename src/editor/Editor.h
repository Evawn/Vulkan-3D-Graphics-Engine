#pragma once

#include "GUIRenderer.h"
#include "UIState.h"
#include "ViewportPanel.h"
#include "PerformancePanel.h"
#include "MemoryPanel.h"
#include "OutputPanel.h"
#include "InspectorPanel.h"
#include "HierarchyPanel.h"
#include "RenderGraphPanel.h"
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
class Scene;

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
					VWrap::VulkanContext& vk,
					Scene* scene);

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

	// Queries
	bool ViewportWasResized() const;
	bool ViewportWasClicked() const;
	VkExtent2D GetDesiredViewportExtent() const;

	// Resolution-policy-aware render extent. Native -> panel size; Center / Fit
	// -> the chosen target. This is what Application feeds to
	// RenderingSystem::HandleViewportResize, so the entire downstream pipeline
	// (offscreen extent, every technique's DescribeTargets, the camera aspect)
	// stays driven by a single value.
	VkExtent2D GetEffectiveRenderExtent() const;
	// Aspect the camera should use, given the current policy. Native / Fit ->
	// panel aspect; Center -> target aspect (so letterboxing doesn't squish).
	float      GetEffectiveCameraAspect() const;

	// Editor mirrors the renderer's live offscreen extent so the status-bar
	// "Native" label can show what the renderer is actually producing.
	void SetLiveOffscreenExtent(VkExtent2D ext) { m_live_offscreen_extent = ext; }

	// DPI
	void OnDpiChanged(float newScale);

	// Metrics
	void UpdateMetrics(float fps, float gpuMs, float frameMs);

	// Screenshot path forwarding
	void SetLastScreenshotPath(const std::string& path);

	// Inspector wiring for lighting + sky + post-process (called once after construction)
	void SetLighting(SceneLighting* lighting) { m_inspector.SetLighting(lighting); }
	void SetSky(SkyDescription* sky)          { m_inspector.SetSky(sky); }
	void SetPostProcess(PostProcessChain* chain) { m_inspector.SetPostProcess(chain); }

	// Render Graph panel + Performance panel both consume the snapshot/metrics.
	void SetGraphSnapshot(const GraphSnapshot* snapshot);
	void SetPerformanceMetrics(const GPUProfiler::PerformanceMetrics* metrics);

	// Shared UI state (panel sizes, viewport-only mode, camera focus mirror, etc.)
	UIState* GetState() { return &m_ui; }

private:
	UIState m_ui;
	std::shared_ptr<GUIRenderer> m_gui;
	std::shared_ptr<VWrap::Sampler> m_scene_sampler;
	VkDescriptorSet m_scene_texture = VK_NULL_HANDLE;

	ViewportPanel m_viewport;
	PerformancePanel m_performance;
	MemoryPanel m_memory;
	OutputPanel m_console;
	InspectorPanel m_inspector;
	HierarchyPanel m_hierarchy;
	RenderGraphPanel m_renderGraphPanel;

	Scene* m_scene = nullptr;
	std::vector<std::unique_ptr<RenderTechnique>>* m_renderers = nullptr;
	size_t* m_active_renderer_index = nullptr;

	// Snapshot of the renderer's offscreen extent — refreshed each frame from
	// Application before the status bar draws. Decoupled from the panel size:
	// in Native they match; in Center / Fit this is the policy target.
	VkExtent2D m_live_offscreen_extent{0, 0};

	void DrawMenuBar();
	void DrawStatusBar();
	void DrawResolutionWidget();
};
