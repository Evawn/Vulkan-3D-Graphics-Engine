#include "UIStyle.h"

using namespace UIStyle;

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

void UIStyle::Apply(float dpi_scale) {
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

	// ---- Sizes (compact) ----
	style.WindowPadding     = ImVec2(4, 3);
	style.FramePadding      = ImVec2(4, 2);
	style.ItemSpacing       = ImVec2(4, 2);
	style.ItemInnerSpacing  = ImVec2(3, 2);
	style.CellPadding       = ImVec2(3, 1);
	style.IndentSpacing     = 12.0f;
	style.ScrollbarSize     = 10.0f;
	style.GrabMinSize       = 6.0f;

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

	style.ScaleAllSizes(dpi_scale);
}
