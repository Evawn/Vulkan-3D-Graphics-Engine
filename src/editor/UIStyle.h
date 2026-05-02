#pragma once

#include "imgui.h"
#include <spdlog/common.h>

namespace UIStyle {

	// --- Palette helpers ---
	constexpr ImVec4 Alpha(ImVec4 c, float a)
		{ return {c.x, c.y, c.z, a}; }
	constexpr ImVec4 Darken(ImVec4 c, float f)
		{ return {c.x * f, c.y * f, c.z * f, c.w}; }
	constexpr ImVec4 Lighten(ImVec4 c, float f)
		{ return {c.x * f < 1.f ? c.x * f : 1.f,
		          c.y * f < 1.f ? c.y * f : 1.f,
		          c.z * f < 1.f ? c.z * f : 1.f, c.w}; }

	// --- Base palette (VSCode dark + matrix green) ---
	constexpr ImVec4 kBg        = ImVec4(0.118f, 0.118f, 0.118f, 1.0f);  // #1E1E1E  editor bg
	constexpr ImVec4 kBgLight   = ImVec4(0.145f, 0.145f, 0.149f, 1.0f);  // #252526  panels
	constexpr ImVec4 kBgLighter = ImVec4(0.176f, 0.176f, 0.188f, 1.0f);  // #2D2D30  hover
	constexpr ImVec4 kBgActive  = ImVec4(0.216f, 0.216f, 0.239f, 1.0f);  // #37373D  active/selected
	constexpr ImVec4 kAccent    = ImVec4(0.000f, 1.000f, 0.251f, 1.0f);  // #00FF40  matrix green
	constexpr ImVec4 kAccentDim = ImVec4(0.000f, 0.280f, 0.070f, 1.0f);  // #004712  dark green tint
	constexpr ImVec4 kText      = ImVec4(0.800f, 0.800f, 0.800f, 1.0f);  // #CCCCCC
	constexpr ImVec4 kTextDim   = ImVec4(0.500f, 0.500f, 0.500f, 1.0f);  // #808080
	constexpr ImVec4 kBorder    = ImVec4(0.188f, 0.188f, 0.188f, 1.0f);  // #303030

	// --- Semantic colors (budgets + render-graph) ---
	constexpr ImVec4 kBudgetGood  = ImVec4(0.31f, 0.78f, 0.47f, 1.0f);  // green
	constexpr ImVec4 kBudgetWarn  = ImVec4(1.00f, 0.76f, 0.28f, 1.0f);  // amber
	constexpr ImVec4 kBudgetOver  = ImVec4(0.94f, 0.33f, 0.31f, 1.0f);  // red
	constexpr ImVec4 kQueueGraphics = ImVec4(0.30f, 0.62f, 0.86f, 1.0f); // muted blue
	constexpr ImVec4 kQueueCompute  = ImVec4(0.40f, 0.78f, 0.40f, 1.0f); // muted green
	constexpr ImVec4 kResourceImage = ImVec4(0.85f, 0.65f, 0.30f, 1.0f); // amber-orange
	constexpr ImVec4 kResourceBuffer= ImVec4(0.65f, 0.50f, 0.85f, 1.0f); // muted purple
	constexpr ImVec4 kResourceImport= ImVec4(0.95f, 0.95f, 0.55f, 1.0f); // pale yellow

	constexpr float kRounding = 3.0f;

	// --- Font sizes (unscaled logical pixels) ---
	constexpr float kFontSizeHeader = 14.0f;
	constexpr float kFontSizeBody   = 13.0f;
	constexpr float kFontSizeDetail = 11.0f;

	// Load all semantic fonts into the ImGui atlas. Call before atlas build.
	void LoadFonts(float dpi_scale);

	// Font accessors (valid after LoadFonts + atlas build)
	ImFont* FontHeader();
	ImFont* FontBody();
	ImFont* FontDetail();
	ImFont* FontMonoBody();
	ImFont* FontMonoDetail();

	void Apply();
	ImVec4 GetLogLevelColor(spdlog::level::level_enum level);

	// Budget color for a usage fraction (good < warn < over thresholds in [0,1]).
	// Defaults: good <70%, warn <90%, over >=90%.
	ImVec4 BudgetColor(float fraction, float warnAt = 0.70f, float overAt = 0.90f);

	// Section header — bold-ish, thin accent rule under the label, full-width.
	void SectionHeader(const char* label);

	// Numeric pair render: dim label, monospace value. "FPS: 144".
	void Numeric(const char* label, const char* value);

	// Color RGBA pack helper (for ImDrawList).
	inline ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

	// Format a byte count in B / KB / MB / GB.
	void FormatBytes(char* out, size_t outSize, uint64_t bytes);
}
