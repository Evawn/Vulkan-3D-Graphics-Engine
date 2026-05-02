#include "Editor.h"
#include "Window.h"
#include "Log.h"
#include "UIStyle.h"
#include "Scene.h"
#include "RenderTechnique.h"
#include <spdlog/spdlog.h>
#include <cstdio>

static void check_vk_result(VkResult err) {
	if (err == 0) return;
	spdlog::get("App")->error("VkResult = {}", (int)err);
	if (err < 0) abort();
}

void Editor::InitImGui(VWrap::VulkanContext& vk,
					   std::shared_ptr<VWrap::RenderPass> renderPass,
					   Window& window) {
	m_gui = GUIRenderer::Create(vk.device);
	m_gui->SetUIState(&m_ui);
	m_scene_sampler = VWrap::Sampler::Create(vk.device);

	VWrap::QueueFamilyIndices indices = vk.physicalDevice->FindQueueFamilies();
	ImGui_ImplGlfw_InitForVulkan(window.GetRaw(), true);

	ImGui_ImplVulkan_InitInfo init_info{};
	init_info.Instance = vk.instance->Get();
	init_info.PhysicalDevice = vk.physicalDevice->Get();
	init_info.Device = vk.device->Get();
	init_info.QueueFamily = indices.graphicsFamily.value();
	init_info.Queue = vk.graphicsQueue->Get();
	init_info.PipelineCache = VK_NULL_HANDLE;
	init_info.DescriptorPool = m_gui->GetDescriptorPool()->Get();
	init_info.Subpass = 0;
	init_info.MinImageCount = vk.frameController->GetSwapchain()->Size();
	init_info.ImageCount = vk.frameController->GetSwapchain()->Size();
	init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init_info.Allocator = VK_NULL_HANDLE;
	init_info.CheckVkResultFn = check_vk_result;
	ImGui_ImplVulkan_Init(&init_info, renderPass->Get());

	float dpi_scale = window.GetContentScale();
	m_gui->LoadFonts(dpi_scale);
	ImGui_ImplVulkan_DestroyFontsTexture();
	ImGui_ImplVulkan_CreateFontsTexture();

	UIStyle::Apply();
}

void Editor::InitPanels(std::vector<std::unique_ptr<RenderTechnique>>* renderers,
						size_t* activeRendererIndex,
						std::shared_ptr<Camera> camera,
						std::shared_ptr<CameraController> cameraController,
						VWrap::VulkanContext& vk,
						Scene* scene) {
	m_scene = scene;
	m_renderers = renderers;
	m_active_renderer_index = activeRendererIndex;

	// Viewport — HUD content is refreshed each frame from the perf panel and
	// the active technique name before the viewport's Draw() runs.
	m_viewport.SetTextureID(m_scene_texture);
	m_viewport.SetUIState(&m_ui);
	m_gui->RegisterPanel("Viewport", [this]() {
		const std::string tech = (m_renderers && m_active_renderer_index && !m_renderers->empty())
			? (*m_renderers)[*m_active_renderer_index]->GetDisplayName()
			: std::string{};
		m_viewport.SetHud(m_performance.GetFps(), m_performance.GetFrameMs(), tech);
		m_viewport.Draw();
	});

	// Performance
	m_gui->RegisterPanel("Performance", [this]() { m_performance.Draw(); });

	// Memory
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice->Get(), &memProps);
	m_memory.SetAllocator(vk.allocator->Get(), memProps.memoryHeapCount);
	m_gui->RegisterPanel("Memory", [this]() {
		m_memory.Update();
		m_memory.Draw();
	});

	// Console
	m_console.SetSink(Log::GetImGuiSink());
	m_gui->RegisterPanel("Console", [this]() { m_console.Draw(); });

	// Hierarchy
	m_hierarchy.SetSelectionChangedCallback(
		[this](SceneNode* node) { m_inspector.SetSelectedNode(node); });
	m_gui->RegisterPanel("Hierarchy", [this]() { m_hierarchy.Draw(m_scene); });

	// Inspector
	m_inspector.SetRenderers(renderers, activeRendererIndex);
	m_inspector.SetCamera(camera);
	m_inspector.SetAppControls(cameraController->SensitivityPtr(), cameraController->SpeedPtr());
	m_gui->RegisterPanel("Inspector", [this]() { m_inspector.Draw(); });

	// Render Graph
	m_gui->RegisterPanel("Render Graph", [this]() { m_renderGraphPanel.Draw(); });

	// Top menu bar + bottom status bar
	m_gui->SetMenuBar  ([this]{ DrawMenuBar();   });
	m_gui->SetStatusBar([this]{ DrawStatusBar(); });
}

void Editor::Shutdown() {
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
}

void Editor::BeginFrame() { m_gui->BeginFrame(); }
void Editor::CmdDraw(std::shared_ptr<VWrap::CommandBuffer> cmd) { m_gui->CmdDraw(cmd); }

void Editor::RegisterSceneTexture(std::shared_ptr<VWrap::ImageView> resolveView) {
	m_scene_texture = ImGui_ImplVulkan_AddTexture(
		m_scene_sampler->Get(),
		resolveView->Get(),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_viewport.SetTextureID(m_scene_texture);
}

void Editor::RemoveSceneTexture() {
	if (m_scene_texture != VK_NULL_HANDLE) {
		ImGui_ImplVulkan_RemoveTexture(m_scene_texture);
		m_scene_texture = VK_NULL_HANDLE;
	}
}

void Editor::UpdateViewportTexture() {
	m_viewport.SetTextureID(m_scene_texture);
}

void Editor::SetReloadCallback(std::function<void()> cb)     { m_inspector.SetReloadCallback(std::move(cb)); }
void Editor::SetSwitchCallback(std::function<void(size_t)> cb) { m_inspector.SetSwitchCallback(std::move(cb)); }
void Editor::SetScreenshotCallback(std::function<void()> cb) { m_inspector.SetScreenshotCallback(std::move(cb)); }

bool Editor::ViewportWasResized() const {
	return const_cast<ViewportPanel&>(m_viewport).WasResized();
}
bool Editor::ViewportWasClicked() const {
	return const_cast<ViewportPanel&>(m_viewport).WasClicked();
}
VkExtent2D Editor::GetDesiredViewportExtent() const {
	return m_viewport.GetDesiredExtent();
}

void Editor::OnDpiChanged(float newScale) {
	m_gui->LoadFonts(newScale);
	ImGui_ImplVulkan_DestroyFontsTexture();
	ImGui_ImplVulkan_CreateFontsTexture();
	UIStyle::Apply();
}

void Editor::UpdateMetrics(float fps, float gpuMs, float frameMs) {
	m_performance.Update(fps, gpuMs, frameMs);
}

void Editor::SetLastScreenshotPath(const std::string& path) {
	m_inspector.SetLastScreenshotPath(path);
}

void Editor::SetGraphSnapshot(const GraphSnapshot* snapshot) {
	m_renderGraphPanel.SetSnapshot(snapshot);
	m_performance.SetGraphSnapshot(snapshot);
	m_memory.SetGraphSnapshot(snapshot);
}

void Editor::SetPerformanceMetrics(const GPUProfiler::PerformanceMetrics* metrics) {
	m_renderGraphPanel.SetMetrics(metrics);
	m_performance.SetMetrics(metrics);
}

// =============================================================================
// Menu bar
// =============================================================================

void Editor::DrawMenuBar() {
	auto cycle_technique = [this](int dir) {
		if (!m_renderers || !m_active_renderer_index || m_renderers->empty()) return;
		size_t n = m_renderers->size();
		size_t cur = *m_active_renderer_index;
		size_t next = (dir > 0) ? (cur + 1) % n : (cur + n - 1) % n;
		// Inspector holds the registered switch callback; re-use it.
		m_inspector.RequestSwitchTechnique(next);
	};

	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem("Reload Shaders", "F5")) m_inspector.RequestReload();
		ImGui::Separator();
		// Quit is delegated to GLFW via Esc; surface the binding here as an
		// affordance, but the actual quit path runs through the camera
		// controller's input handler.
		ImGui::MenuItem("Quit", "Esc", false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("View")) {
		bool vp_only = m_ui.viewport_only;
		if (ImGui::MenuItem("Viewport Only", "F1", vp_only)) {
			m_ui.viewport_only = !m_ui.viewport_only;
			m_ui.layout_dirty = true;
		}
		if (ImGui::MenuItem("Fullscreen", "F11", m_ui.os_fullscreen)) {
			// Application owns the actual GLFW toggle via the camera-controller
			// callback; we mirror state here so the checkbox renders correctly.
			// Trigger via the same path: the controller's callback flips
			// m_ui.os_fullscreen on success.
			// (No-op here; the user has F11 hotkey + the actual toggle.)
		}
		ImGui::Separator();
		if (ImGui::BeginMenu("Layout")) {
			auto entry = [&](const char* lbl, LayoutPreset p) {
				if (ImGui::MenuItem(lbl, nullptr, m_ui.layout_preset == p)) {
					m_ui.layout_preset = p;
					m_ui.layout_dirty = true;
				}
			};
			entry("Develop",      LayoutPreset::Develop);
			entry("Performance",  LayoutPreset::Performance);
			entry("Profile",      LayoutPreset::Profile);
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout")) {
				m_ui.right_panel_px = 0.0f;
				m_ui.bottom_panel_px = 0.0f;
				m_ui.layout_dirty = true;
			}
			ImGui::EndMenu();
		}
		ImGui::Separator();
		ImGui::MenuItem("HUD: Perf",       nullptr, &m_ui.hud_show_perf);
		ImGui::MenuItem("HUD: Axes",       nullptr, &m_ui.hud_show_axes);
		ImGui::MenuItem("HUD: Technique",  nullptr, &m_ui.hud_show_technique);
		ImGui::Separator();
		ImGui::SliderFloat("Scroll", &m_ui.scroll_sensitivity, 0.1f, 1.5f, "%.2f");
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Render")) {
		if (ImGui::MenuItem("Cycle Technique +", "F2")) cycle_technique(+1);
		if (ImGui::MenuItem("Cycle Technique -", nullptr)) cycle_technique(-1);
		ImGui::Separator();
		if (ImGui::MenuItem("Reload Shaders", "F5")) m_inspector.RequestReload();
		if (ImGui::MenuItem("Screenshot",     "F12")) m_inspector.RequestScreenshot();
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Help")) {
		ImGui::TextColored(UIStyle::kTextDim, "Vulkan Voxel Engine");
		ImGui::TextColored(UIStyle::kTextDim, "Diorama renderer · matrix-green build");
		ImGui::EndMenu();
	}
}

// =============================================================================
// Status bar — fixed-slot layout
//
// Each readout is anchored to an absolute window-X coordinate via SameLine(x)
// so digit-width changes don't cascade. Values are EMA-smoothed before
// display so the numbers don't tick on every frame; the underlying perf data
// is unchanged and PerformancePanel still graphs the raw stream.
//
// Slot widths are tuned to the worst-case content (e.g. "999.99 ms" in mono
// detail font), with a small breathing margin. If you add a new readout, give
// it its own slot rather than stacking onto a neighbor — the cascade is
// exactly what stable layout was supposed to prevent.
// =============================================================================

void Editor::DrawStatusBar() {
	// EMA-smooth the displayed frame/gpu values. ~150ms half-life at 60Hz so
	// the readout follows actual change but doesn't flicker on per-frame noise.
	// FPS arrives pre-smoothed at 2Hz from GPUProfiler, so it gets a much
	// gentler EMA just to catch the half-second jumps.
	static float disp_frame = 0.0f, disp_gpu = 0.0f, disp_fps = 0.0f;
	const float a_ms  = 0.08f;
	const float a_fps = 0.20f;
	disp_frame = disp_frame * (1.0f - a_ms)  + m_performance.GetFrameMs() * a_ms;
	disp_gpu   = disp_gpu   * (1.0f - a_ms)  + m_performance.GetGpuMs()   * a_ms;
	disp_fps   = disp_fps   * (1.0f - a_fps) + m_performance.GetFps()     * a_fps;

	const size_t errs  = m_console.GetErrorCount();
	const size_t warns = m_console.GetWarnCount();

	auto pushMono = []{ ImGui::PushFont(UIStyle::FontMonoDetail()); };
	auto popMono  = []{ ImGui::PopFont(); };

	// Vertical centering: align the first item's text-baseline to the window
	// frame so all items sit at the same y.
	ImGui::AlignTextToFramePadding();

	// --- Slot positions (absolute X within the status bar window) ---
	// Each slot reserves enough room for the worst-case rendering of its
	// label + value pair. Tweak in one place; the rest of the layout stays
	// pinned because subsequent SameLine() calls use absolute X.
	const float slot_fps    = 4.0f;
	const float slot_frame  = 78.0f;
	const float slot_gpu    = 188.0f;
	const float slot_vram   = 290.0f;
	const float slot_allocs = 396.0f;
	const float slot_tech   = 482.0f;

	// FPS
	ImGui::SetCursorPosX(slot_fps);
	ImGui::TextColored(UIStyle::kTextDim, "fps");
	ImGui::SameLine(0, 4);
	pushMono();
	ImGui::TextColored(
		UIStyle::BudgetColor(16.6f / std::max(0.001f, disp_frame), 0.5f, 0.9f),
		"%4.0f", disp_fps);
	popMono();

	// frame ms
	ImGui::SameLine(slot_frame);
	ImGui::TextColored(UIStyle::kTextDim, "frame");
	ImGui::SameLine(0, 4);
	pushMono(); ImGui::Text("%6.2f", disp_frame); popMono();
	ImGui::SameLine(0, 2);
	ImGui::TextColored(UIStyle::kTextDim, "ms");

	// gpu ms
	ImGui::SameLine(slot_gpu);
	ImGui::TextColored(UIStyle::kTextDim, "gpu");
	ImGui::SameLine(0, 4);
	pushMono(); ImGui::Text("%6.2f", disp_gpu); popMono();
	ImGui::SameLine(0, 2);
	ImGui::TextColored(UIStyle::kTextDim, "ms");

	// VRAM (graph-resource estimate)
	ImGui::SameLine(slot_vram);
	ImGui::TextColored(UIStyle::kTextDim, "vram");
	ImGui::SameLine(0, 4);
	char vram_buf[32];
	UIStyle::FormatBytes(vram_buf, sizeof(vram_buf), m_memory.GetTotalBytes());
	pushMono(); ImGui::TextUnformatted(vram_buf); popMono();

	// Allocations
	ImGui::SameLine(slot_allocs);
	ImGui::TextColored(UIStyle::kTextDim, "allocs");
	ImGui::SameLine(0, 4);
	pushMono(); ImGui::Text("%5u", m_memory.GetAllocationCount()); popMono();

	// Technique — left-anchored at slot_tech, free-flowing right (variable text
	// length, but the badges to its right are anchored to the *window's*
	// right edge, so technique-name length doesn't shift them either).
	ImGui::SameLine(slot_tech);
	if (m_renderers && m_active_renderer_index && !m_renderers->empty()) {
		ImGui::TextColored(UIStyle::kTextDim, "tech");
		ImGui::SameLine(0, 4);
		ImGui::TextUnformatted((*m_renderers)[*m_active_renderer_index]->GetDisplayName().c_str());
	}

	// --- Right-anchored badges ---
	// Anchor against the window's full width (NOT GetContentRegionAvail) so
	// the cumulative width of the left-side widgets above doesn't decide where
	// the badges land. The badges always sit `pad` from the window's right.
	const float pad = 8.0f;
	const float win_w = ImGui::GetWindowSize().x;
	float right_x = win_w - pad;

	auto right_align = [&](const char* text) {
		ImVec2 ts = ImGui::CalcTextSize(text);
		right_x -= ts.x;
		ImGui::SameLine(right_x);
	};

	if (errs == 0 && warns == 0) {
		const char* lbl = "OK";
		right_align(lbl);
		ImGui::TextColored(UIStyle::kBudgetGood, "%s", lbl);
	} else {
		// Render right-to-left: warnings first (rightmost), then errors.
		if (warns > 0) {
			char buf[24]; snprintf(buf, sizeof(buf), "%zuW", warns);
			right_align(buf);
			ImGui::TextColored(UIStyle::kBudgetWarn, "%s", buf);
			right_x -= 8.0f; // gap
		}
		if (errs > 0) {
			char buf[24]; snprintf(buf, sizeof(buf), "%zuE", errs);
			right_align(buf);
			ImGui::TextColored(UIStyle::kBudgetOver, "%s", buf);
		}
	}
}
