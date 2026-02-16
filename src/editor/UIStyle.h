#pragma once

#include "imgui.h"
#include <spdlog/common.h>

namespace UIStyle {

	// --- Palette helpers (constexpr, used to derive all theme colors) ---
	constexpr ImVec4 Alpha(ImVec4 c, float a)
		{ return {c.x, c.y, c.z, a}; }
	constexpr ImVec4 Darken(ImVec4 c, float f)
		{ return {c.x * f, c.y * f, c.z * f, c.w}; }
	constexpr ImVec4 Lighten(ImVec4 c, float f)
		{ return {c.x * f < 1.f ? c.x * f : 1.f,
		          c.y * f < 1.f ? c.y * f : 1.f,
		          c.z * f < 1.f ? c.z * f : 1.f, c.w}; }

	// --- Base palette (true neutral dark — equal R=G=B, no blue cast) ---
	constexpr ImVec4 kBg        = ImVec4(0.059f, 0.059f, 0.059f, 1.0f);  // #0F0F0F
	constexpr ImVec4 kBgLight   = ImVec4(0.098f, 0.098f, 0.098f, 1.0f);  // #191919
	constexpr ImVec4 kBgLighter = ImVec4(0.145f, 0.145f, 0.145f, 1.0f);  // #252525
	constexpr ImVec4 kAccent    = ImVec4(0.259f, 0.588f, 0.980f, 1.0f);  // #4296FA
	constexpr ImVec4 kText      = ImVec4(0.850f, 0.850f, 0.850f, 1.0f);  // #D9D9D9
	constexpr ImVec4 kTextDim   = ImVec4(0.350f, 0.350f, 0.350f, 1.0f);  // #595959
	constexpr ImVec4 kBorder    = ImVec4(0.110f, 0.110f, 0.110f, 1.0f);  // #1C1C1C

	constexpr float kRounding = 2.0f;

	void Apply(float dpi_scale);
	ImVec4 GetLogLevelColor(spdlog::level::level_enum level);

}
