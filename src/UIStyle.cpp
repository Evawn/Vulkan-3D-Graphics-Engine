#include "UIStyle.h"

using namespace UIStyle;

ImVec4 UIStyle::GetLogLevelColor(spdlog::level::level_enum level) {
	switch (level) {
	case spdlog::level::trace:    return kTextDim;
	case spdlog::level::debug:    return Lighten(kAccent, 0.85f);
	case spdlog::level::info:     return Darken(kText, 0.88f);
	case spdlog::level::warn:     return ImVec4(0.95f, 0.75f, 0.20f, 1.0f);
	case spdlog::level::err:      return ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
	case spdlog::level::critical: return ImVec4(1.00f, 0.20f, 0.20f, 1.0f);
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
	c[ImGuiCol_ChildBg]               = kBg;
	c[ImGuiCol_PopupBg]               = Alpha(kBg, 0.97f);
	c[ImGuiCol_Border]                = kBorder;
	c[ImGuiCol_BorderShadow]          = Alpha(kBg, 0.0f);

	// Frames (inputs, sliders, checkboxes)
	c[ImGuiCol_FrameBg]               = kBgLight;
	c[ImGuiCol_FrameBgHovered]        = kBgLighter;
	c[ImGuiCol_FrameBgActive]         = Alpha(kAccent, 0.40f);

	// Title bar
	c[ImGuiCol_TitleBg]               = Darken(kBg, 0.70f);
	c[ImGuiCol_TitleBgActive]         = Darken(kBgLight, 0.80f);
	c[ImGuiCol_TitleBgCollapsed]      = Alpha(Darken(kBg, 0.55f), 0.70f);
	c[ImGuiCol_MenuBarBg]             = Darken(kBg, 0.80f);

	// Scrollbar
	c[ImGuiCol_ScrollbarBg]           = Alpha(kBg, 0.15f);
	c[ImGuiCol_ScrollbarGrab]         = kBgLighter;
	c[ImGuiCol_ScrollbarGrabHovered]  = Lighten(kBgLighter, 1.35f);
	c[ImGuiCol_ScrollbarGrabActive]   = Lighten(kBgLighter, 1.70f);

	// Interactive
	c[ImGuiCol_CheckMark]             = kAccent;
	c[ImGuiCol_SliderGrab]            = Alpha(kAccent, 0.80f);
	c[ImGuiCol_SliderGrabActive]      = kAccent;

	// Buttons
	c[ImGuiCol_Button]                = kBgLight;
	c[ImGuiCol_ButtonHovered]         = kBgLighter;
	c[ImGuiCol_ButtonActive]          = kAccent;

	// Headers (collapsing headers, tree nodes, selectables)
	c[ImGuiCol_Header]                = kBgLight;
	c[ImGuiCol_HeaderHovered]         = Alpha(kAccent, 0.50f);
	c[ImGuiCol_HeaderActive]          = Alpha(kAccent, 0.70f);

	// Separators
	c[ImGuiCol_Separator]             = kBorder;
	c[ImGuiCol_SeparatorHovered]      = Alpha(kAccent, 0.70f);
	c[ImGuiCol_SeparatorActive]       = kAccent;

	// Resize grip
	c[ImGuiCol_ResizeGrip]            = Alpha(kAccent, 0.15f);
	c[ImGuiCol_ResizeGripHovered]     = Alpha(kAccent, 0.50f);
	c[ImGuiCol_ResizeGripActive]      = Alpha(kAccent, 0.85f);

	// Tabs
	c[ImGuiCol_Tab]                   = Darken(kBg, 0.80f);
	c[ImGuiCol_TabHovered]            = Alpha(kAccent, 0.40f);
	c[ImGuiCol_TabActive]             = kBg;
	c[ImGuiCol_TabUnfocused]          = Darken(kBg, 0.72f);
	c[ImGuiCol_TabUnfocusedActive]    = Darken(kBg, 0.92f);

	// Docking
	c[ImGuiCol_DockingPreview]        = Alpha(kAccent, 0.40f);
	c[ImGuiCol_DockingEmptyBg]        = Darken(kBg, 0.70f);

	// Plots
	c[ImGuiCol_PlotLines]             = Lighten(kAccent, 1.1f);
	c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.90f, 0.55f, 0.25f, 1.0f);
	c[ImGuiCol_PlotHistogram]         = kAccent;
	c[ImGuiCol_PlotHistogramHovered]  = Alpha(kAccent, 0.80f);

	// Tables
	c[ImGuiCol_TableHeaderBg]         = kBgLight;
	c[ImGuiCol_TableBorderStrong]     = kBorder;
	c[ImGuiCol_TableBorderLight]      = Alpha(kBorder, 0.70f);
	c[ImGuiCol_TableRowBg]            = Alpha(kBg, 0.0f);
	c[ImGuiCol_TableRowBgAlt]         = Alpha(kText, 0.015f);

	// Misc
	c[ImGuiCol_TextSelectedBg]        = Alpha(kAccent, 0.30f);
	c[ImGuiCol_DragDropTarget]        = Alpha(kAccent, 0.90f);
	c[ImGuiCol_NavHighlight]          = kAccent;
	c[ImGuiCol_NavWindowingHighlight] = Alpha(kText, 0.60f);
	c[ImGuiCol_NavWindowingDimBg]     = Alpha(kBgLighter, 0.20f);
	c[ImGuiCol_ModalWindowDimBg]      = Alpha(kBgLighter, 0.40f);

	// ---- Sizes (compact) ----
	style.WindowPadding     = ImVec2(4, 4);
	style.FramePadding      = ImVec2(4, 2);
	style.ItemSpacing       = ImVec2(4, 2);
	style.ItemInnerSpacing  = ImVec2(3, 2);
	style.CellPadding       = ImVec2(3, 1);
	style.IndentSpacing     = 12.0f;
	style.ScrollbarSize     = 9.0f;
	style.GrabMinSize       = 7.0f;

	style.WindowRounding    = kRounding;
	style.ChildRounding     = 1.0f;
	style.FrameRounding     = kRounding;
	style.PopupRounding     = kRounding;
	style.ScrollbarRounding = kRounding;
	style.GrabRounding      = 1.0f;
	style.TabRounding       = kRounding;

	style.WindowBorderSize  = 1.0f;
	style.FrameBorderSize   = 0.0f;
	style.TabBorderSize     = 0.0f;

	style.WindowTitleAlign        = ImVec2(0, 0.5f);
	style.DockingSeparatorSize    = 1.0f;
	style.WindowMenuButtonPosition = ImGuiDir_None;

	style.ScaleAllSizes(dpi_scale);
}
