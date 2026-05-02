#include "UIStyle.h"
#include "config.h"
#include <filesystem>
#include <cstdio>
#include <spdlog/spdlog.h>

using namespace UIStyle;

namespace {
	ImFont* s_font_header = nullptr;
	ImFont* s_font_body   = nullptr;
	ImFont* s_font_detail = nullptr;
	ImFont* s_font_mono_body = nullptr;
	ImFont* s_font_mono_detail = nullptr;
}

void UIStyle::LoadFonts(float dpi_scale) {
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();

	const std::string sans_path = std::string(config::RESOURCE_DIR) + "/fonts/Inter-Regular.ttf";
	const std::string mono_path = std::string(config::RESOURCE_DIR) + "/fonts/JetBrainsMono-Regular.ttf";
	const bool sans_exists = std::filesystem::exists(sans_path);
	const bool mono_exists = std::filesystem::exists(mono_path);

	auto load = [&](const std::string& path, bool exists, float base_size) -> ImFont* {
		float scaled = base_size * dpi_scale;
		if (exists) {
			ImFontConfig cfg;
			cfg.OversampleH = 3;
			cfg.OversampleV = 2;
			cfg.PixelSnapH  = true;
			return io.Fonts->AddFontFromFileTTF(path.c_str(), scaled, &cfg);
		}
		return io.Fonts->AddFontDefault();
	};

	// Body first — becomes ImGui default font (Fonts[0])
	s_font_body   = load(sans_path, sans_exists, kFontSizeBody);
	s_font_header = load(sans_path, sans_exists, kFontSizeHeader);
	s_font_detail = load(sans_path, sans_exists, kFontSizeDetail);

	// Monospace falls back to the sans-serif body when the font isn't shipped —
	// the layout still works, numerics just don't tabulate nicely.
	if (mono_exists) {
		s_font_mono_body   = load(mono_path, true, kFontSizeBody);
		s_font_mono_detail = load(mono_path, true, kFontSizeDetail);
	} else {
		s_font_mono_body   = s_font_body;
		s_font_mono_detail = s_font_detail;
	}

	io.FontGlobalScale = 1.0f / dpi_scale;

	if (!sans_exists)
		spdlog::get("App")->warn("Font not found: {} - using defaults", sans_path);
	if (!mono_exists)
		spdlog::get("App")->info("Mono font not found ({}); numerics will use sans body.", mono_path);
}

ImFont* UIStyle::FontHeader()     { return s_font_header; }
ImFont* UIStyle::FontBody()       { return s_font_body; }
ImFont* UIStyle::FontDetail()     { return s_font_detail; }
ImFont* UIStyle::FontMonoBody()   { return s_font_mono_body; }
ImFont* UIStyle::FontMonoDetail() { return s_font_mono_detail; }

ImVec4 UIStyle::BudgetColor(float fraction, float warnAt, float overAt) {
	if (fraction >= overAt) return kBudgetOver;
	if (fraction >= warnAt) return kBudgetWarn;
	return kBudgetGood;
}

void UIStyle::SectionHeader(const char* label) {
	ImGui::PushFont(FontHeader());
	ImGui::TextUnformatted(label);
	ImGui::PopFont();
	// Underline runs from the label's left edge to the right edge of the
	// available content region. Using GetWindowSize would push past the panel
	// when docked tightly; content-region accounts for both window padding
	// and the dock node's clip width.
	ImVec2 a = ImGui::GetItemRectMin();
	ImVec2 b = ImGui::GetItemRectMax();
	float y = b.y + 1.0f;
	float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(a.x, y), ImVec2(right, y), U32(Alpha(kAccent, 0.45f)), 1.0f);
	ImGui::Dummy(ImVec2(0, 2));
}

void UIStyle::Numeric(const char* label, const char* value) {
	ImGui::TextColored(kTextDim, "%s", label);
	ImGui::SameLine(0, 4);
	ImGui::PushFont(FontMonoBody());
	ImGui::TextUnformatted(value);
	ImGui::PopFont();
}

void UIStyle::FormatBytes(char* out, size_t outSize, uint64_t bytes) {
	const double kKB = 1024.0;
	const double kMB = 1024.0 * 1024.0;
	const double kGB = 1024.0 * 1024.0 * 1024.0;
	double v = static_cast<double>(bytes);
	if (v >= kGB)      snprintf(out, outSize, "%.2f GB", v / kGB);
	else if (v >= kMB) snprintf(out, outSize, "%.1f MB", v / kMB);
	else if (v >= kKB) snprintf(out, outSize, "%.1f KB", v / kKB);
	else               snprintf(out, outSize, "%llu B",  static_cast<unsigned long long>(bytes));
}

ImVec4 UIStyle::GetLogLevelColor(spdlog::level::level_enum level) {
	switch (level) {
	case spdlog::level::trace:    return kTextDim;
	case spdlog::level::debug:    return ImVec4(0.40f, 0.73f, 0.42f, 1.0f);  // muted green
	case spdlog::level::info:     return kText;
	case spdlog::level::warn:     return ImVec4(1.00f, 0.76f, 0.28f, 1.0f);  // amber
	case spdlog::level::err:      return ImVec4(0.94f, 0.33f, 0.31f, 1.0f);  // red
	case spdlog::level::critical: return ImVec4(1.00f, 0.17f, 0.17f, 1.0f);  // bright red
	default:                      return kText;
	}
}

void UIStyle::Apply() {
	ImGuiStyle& style = ImGui::GetStyle();
	ImVec4* c = style.Colors;

	// Text
	c[ImGuiCol_Text]                  = kText;
	c[ImGuiCol_TextDisabled]          = kTextDim;

	// Backgrounds
	c[ImGuiCol_WindowBg]              = kBg;
	c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_PopupBg]               = ImVec4(0.110f, 0.110f, 0.114f, 0.98f);
	c[ImGuiCol_Border]                = kBorder;
	c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

	// Frames (inputs, sliders, checkboxes)
	c[ImGuiCol_FrameBg]               = kBgLight;
	c[ImGuiCol_FrameBgHovered]        = kBgLighter;
	c[ImGuiCol_FrameBgActive]         = kBgActive;

	// Title bar
	c[ImGuiCol_TitleBg]               = kBg;
	c[ImGuiCol_TitleBgActive]         = kBgLight;
	c[ImGuiCol_TitleBgCollapsed]      = kBg;
	c[ImGuiCol_MenuBarBg]             = kBgLight;

	// Scrollbar
	c[ImGuiCol_ScrollbarBg]           = Alpha(kBg, 0.5f);
	c[ImGuiCol_ScrollbarGrab]         = kBgLighter;
	c[ImGuiCol_ScrollbarGrabHovered]  = kTextDim;
	c[ImGuiCol_ScrollbarGrabActive]   = Alpha(kAccent, 0.6f);

	// Interactive
	c[ImGuiCol_CheckMark]             = kAccent;
	c[ImGuiCol_SliderGrab]            = Alpha(kAccent, 0.7f);
	c[ImGuiCol_SliderGrabActive]      = kAccent;

	// Buttons — dark green hover, bright green active
	c[ImGuiCol_Button]                = kBgLight;
	c[ImGuiCol_ButtonHovered]         = kAccentDim;
	c[ImGuiCol_ButtonActive]          = Alpha(kAccent, 0.75f);

	// Headers (collapsing headers, tree nodes, selectables)
	c[ImGuiCol_Header]                = kBgLight;
	c[ImGuiCol_HeaderHovered]         = kBgLighter;
	c[ImGuiCol_HeaderActive]          = kAccentDim;

	// Separators
	c[ImGuiCol_Separator]             = kBorder;
	c[ImGuiCol_SeparatorHovered]      = Alpha(kAccent, 0.5f);
	c[ImGuiCol_SeparatorActive]       = kAccent;

	// Resize grip
	c[ImGuiCol_ResizeGrip]            = Alpha(kBgLighter, 0.5f);
	c[ImGuiCol_ResizeGripHovered]     = Alpha(kAccent, 0.5f);
	c[ImGuiCol_ResizeGripActive]      = Alpha(kAccent, 0.8f);

	// Tabs
	c[ImGuiCol_Tab]                   = kBg;
	c[ImGuiCol_TabHovered]            = kBgLighter;
	c[ImGuiCol_TabActive]             = kBgLight;
	c[ImGuiCol_TabUnfocused]          = kBg;
	c[ImGuiCol_TabUnfocusedActive]    = kBgLight;

	// Docking
	c[ImGuiCol_DockingPreview]        = Alpha(kAccent, 0.4f);
	c[ImGuiCol_DockingEmptyBg]        = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);

	// Plots
	c[ImGuiCol_PlotLines]             = kAccent;
	c[ImGuiCol_PlotLinesHovered]      = Lighten(kAccent, 1.2f);
	c[ImGuiCol_PlotHistogram]         = Alpha(kAccent, 0.8f);
	c[ImGuiCol_PlotHistogramHovered]  = kAccent;

	// Tables
	c[ImGuiCol_TableHeaderBg]         = kBgLight;
	c[ImGuiCol_TableBorderStrong]     = kBorder;
	c[ImGuiCol_TableBorderLight]      = Alpha(kBorder, 0.5f);
	c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt]         = Alpha(kBgLight, 0.5f);

	// Misc
	c[ImGuiCol_TextSelectedBg]        = Alpha(kAccent, 0.20f);
	c[ImGuiCol_DragDropTarget]        = kAccent;
	c[ImGuiCol_NavHighlight]          = kAccent;
	c[ImGuiCol_NavWindowingHighlight] = Alpha(kText, 0.7f);
	c[ImGuiCol_NavWindowingDimBg]     = Alpha(kBg, 0.6f);
	c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);

	// ---- Sizes (VSCode-compact) ----
	style.WindowPadding     = ImVec2(8, 6);
	style.FramePadding      = ImVec2(6, 4);
	style.ItemSpacing       = ImVec2(6, 4);
	style.ItemInnerSpacing  = ImVec2(4, 4);
	style.CellPadding       = ImVec2(4, 2);
	style.IndentSpacing     = 16.0f;
	style.ScrollbarSize     = 12.0f;
	style.GrabMinSize       = 8.0f;

	style.WindowRounding    = kRounding;
	style.ChildRounding     = 2.0f;
	style.FrameRounding     = kRounding;
	style.PopupRounding     = kRounding;
	style.ScrollbarRounding = kRounding;
	style.GrabRounding      = 2.0f;
	style.TabRounding       = kRounding;

	style.WindowBorderSize  = 1.0f;
	style.FrameBorderSize   = 0.0f;
	style.TabBorderSize     = 0.0f;

	style.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
	style.DockingSeparatorSize    = 1.0f;
	style.WindowMenuButtonPosition = ImGuiDir_None;
}
