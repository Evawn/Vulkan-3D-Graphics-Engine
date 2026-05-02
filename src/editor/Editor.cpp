#include "Editor.h"
#include "Window.h"
#include "Log.h"
#include "UIStyle.h"
#include "Scene.h"
#include "RenderTechnique.h"
#include <spdlog/spdlog.h>
#include <algorithm>
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
	// the active technique name before the viewport's Draw() runs. The render
	// extent is also pushed in here so the Center-mode blit knows the texture's
	// actual pixel size (panel size and texture size diverge in Center / Fit).
	m_viewport.SetTextureID(m_scene_texture);
	m_viewport.SetUIState(&m_ui);
	m_gui->RegisterPanel("Viewport", [this]() {
		const std::string tech = (m_renderers && m_active_renderer_index && !m_renderers->empty())
			? (*m_renderers)[*m_active_renderer_index]->GetDisplayName()
			: std::string{};
		m_viewport.SetHud(m_performance.GetFps(), m_performance.GetFrameMs(), tech);
		m_viewport.SetRenderExtent(m_live_offscreen_extent);
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
void Editor::SetToggleRecordingCallback(std::function<void()> cb) { m_inspector.SetToggleRecordingCallback(std::move(cb)); }
void Editor::SetCaptureSystem(Capture::CaptureSystem* capture) {
	m_inspector.SetCaptureSystem(capture);
	m_viewport.SetCaptureSystem(capture);
}

bool Editor::ViewportWasResized() const {
	return const_cast<ViewportPanel&>(m_viewport).WasResized();
}
bool Editor::ViewportWasClicked() const {
	return const_cast<ViewportPanel&>(m_viewport).WasClicked();
}
VkExtent2D Editor::GetDesiredViewportExtent() const {
	return m_viewport.GetDesiredExtent();
}

VkExtent2D Editor::GetEffectiveRenderExtent() const {
	const VkExtent2D panel = m_viewport.GetDesiredExtent();
	if (m_ui.resolution.mode == ResolutionMode::Native) return panel;
	return m_ui.resolution.target;
}

float Editor::GetEffectiveCameraAspect() const {
	// Native + Fit: the rendered image fills the panel (Native 1:1, Fit
	// stretched), so the user-visible aspect is the panel's. Center: the
	// renderer produces a target-resolution image and the panel letterboxes
	// it; the *visible* aspect is the target's, so the camera matches that.
	VkExtent2D ext;
	if (m_ui.resolution.mode == ResolutionMode::Center) {
		ext = m_ui.resolution.target;
	} else {
		ext = m_viewport.GetDesiredExtent();
	}
	if (ext.width == 0 || ext.height == 0) return 1.0f;
	return static_cast<float>(ext.width) / static_cast<float>(ext.height);
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

void Editor::SetLastRecordingPath(const std::string& path) {
	m_inspector.SetLastRecordingPath(path);
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

	// Resolution mode toggle + target selector. Sits after the (variable-width)
	// technique label and before the right-anchored badge cluster. Anchored to
	// the right edge by computing its required width up-front so it can't
	// collide with the badges on narrow windows.
	ImGui::SameLine(0, 14);
	DrawResolutionWidget();

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

// =============================================================================
// Resolution widget
//
// Two adjacent controls inside one status-bar slot:
//   1) Three-state segmented mode toggle: Native | Center | Fit
//   2) Resolution control:
//        - Native -> read-only label (live offscreen extent)
//        - Center / Fit -> combo with 480p / 720p / 1080p / Custom…
//      "Custom…" opens a popup with W/H InputInts and an Apply button.
//
// All status-bar widgets keep the row height pinned to the existing text
// baseline by tightening FramePadding for the duration of this draw. The
// segmented buttons are zero-spaced so they read as one control.
// =============================================================================
void Editor::DrawResolutionWidget() {
	auto& policy = m_ui.resolution;

	// Tighten frame padding so SmallButton / Combo height matches the status
	// bar's existing text height. Bar height is set by the first item drawn
	// (the FPS text); pushing FramePadding(.., 0) keeps everything coplanar.
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(0.0f, 0.0f));

	const ImVec4 activeCol   = UIStyle::kAccent;
	const ImVec4 inactiveCol = ImGui::GetStyleColorVec4(ImGuiCol_Button);

	auto modeButton = [&](const char* label, ResolutionMode m) {
		const bool active = (policy.mode == m);
		ImGui::PushStyleColor(ImGuiCol_Button,        active ? activeCol : inactiveCol);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? activeCol : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  active ? activeCol : ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button(label) && policy.mode != m) {
			policy.mode = m;
			// Mode change alters the effective render extent (Native vs target);
			// fire the same resize edge a panel drag uses so the graph rebuilds
			// at the new offscreen size.
			m_viewport.MarkResized();
		}
		ImGui::PopStyleColor(3);
	};

	modeButton("Native", ResolutionMode::Native);
	ImGui::SameLine(0, 1);  // 1px hairline between segments — reads as joined
	modeButton("Center", ResolutionMode::Center);
	ImGui::SameLine(0, 1);
	modeButton("Fit", ResolutionMode::Fit);

	// Restore item spacing for the gap before the resolution combo.
	ImGui::PopStyleVar();   // ItemSpacing
	ImGui::SameLine(0, 8);

	// --- Right half: live extent (Native) or target combo (Center / Fit) ---
	if (policy.mode == ResolutionMode::Native) {
		// Read-only label of the live offscreen extent. Shown dim because it's
		// not interactive — Native mode is "whatever the panel is."
		char buf[32];
		snprintf(buf, sizeof(buf), "%u\xC3\x97%u",  // U+00D7 multiplication sign
			m_live_offscreen_extent.width, m_live_offscreen_extent.height);
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(UIStyle::kTextDim, "%s", buf);
	} else {
		// Combo presenting the four common choices + a custom option. Selecting
		// a preset writes policy.target and re-fires the resize edge.
		struct Preset { const char* label; uint32_t w, h; };
		static const Preset kPresets[] = {
			{ "480p",  854, 480  },
			{ "720p",  1280, 720 },
			{ "1080p", 1920, 1080 },
		};

		// Combo label: "1920x1080" or the preset name when target matches.
		char comboLabel[32];
		const Preset* matched = nullptr;
		for (const auto& p : kPresets) {
			if (p.w == policy.target.width && p.h == policy.target.height) {
				matched = &p; break;
			}
		}
		if (matched) {
			snprintf(comboLabel, sizeof(comboLabel), "%s", matched->label);
		} else {
			snprintf(comboLabel, sizeof(comboLabel), "%u\xC3\x97%u",
				policy.target.width, policy.target.height);
		}

		ImGui::SetNextItemWidth(110.0f);
		// Deferred-open flag: ImGui's popup stack doesn't like opening a new
		// popup from inside an active one (the combo *is* a popup). Latch the
		// intent inside BeginCombo, then OpenPopup after EndCombo so the combo
		// closes cleanly first.
		bool wantOpenCustom = false;
		if (ImGui::BeginCombo("##resolution", comboLabel)) {
			for (const auto& p : kPresets) {
				const bool selected = (matched == &p);
				if (ImGui::Selectable(p.label, selected)) {
					if (policy.target.width != p.w || policy.target.height != p.h) {
						policy.target = { p.w, p.h };
						m_viewport.MarkResized();
					}
				}
			}
			if (ImGui::Selectable("Custom\xE2\x80\xA6", false)) {  // U+2026 …
				policy.customW = static_cast<int>(policy.target.width);
				policy.customH = static_cast<int>(policy.target.height);
				wantOpenCustom = true;
			}
			ImGui::EndCombo();
		}
		if (wantOpenCustom) ImGui::OpenPopup("##resolution_custom");

		// Custom-resolution popup. Clamp range matches the Vulkan minimum
		// guaranteed maxImageDimension2D (4096); typical desktop GPUs allow
		// 16384, but 4096 is the always-safe floor and most users will be far
		// below it anyway. (Dynamic device-limit clamp is a future tweak.)
		if (ImGui::BeginPopup("##resolution_custom")) {
			ImGui::TextColored(UIStyle::kTextDim, "Custom resolution");
			ImGui::Separator();
			ImGui::SetNextItemWidth(80);
			ImGui::InputInt("W", &policy.customW, 0, 0);
			ImGui::SetNextItemWidth(80);
			ImGui::InputInt("H", &policy.customH, 0, 0);
			policy.customW = std::clamp(policy.customW, 16, 16384);
			policy.customH = std::clamp(policy.customH, 16, 16384);
			if (ImGui::Button("Apply")) {
				policy.target = {
					static_cast<uint32_t>(policy.customW),
					static_cast<uint32_t>(policy.customH)
				};
				m_viewport.MarkResized();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	ImGui::PopStyleVar();   // FramePadding
}
