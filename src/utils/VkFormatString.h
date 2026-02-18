#pragma once
#include <vulkan/vulkan.h>
#include <string>

namespace VkStr {

inline const char* Format(VkFormat fmt) {
	switch (fmt) {
	case VK_FORMAT_UNDEFINED:                return "UNDEFINED";
	case VK_FORMAT_R8G8B8A8_UNORM:           return "R8G8B8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_SRGB:            return "R8G8B8A8_SRGB";
	case VK_FORMAT_B8G8R8A8_UNORM:           return "B8G8R8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_SRGB:            return "B8G8R8A8_SRGB";
	case VK_FORMAT_R16G16B16A16_SFLOAT:      return "R16G16B16A16_SFLOAT";
	case VK_FORMAT_R32G32B32A32_SFLOAT:      return "R32G32B32A32_SFLOAT";
	case VK_FORMAT_D32_SFLOAT:               return "D32_SFLOAT";
	case VK_FORMAT_D32_SFLOAT_S8_UINT:       return "D32_SFLOAT_S8_UINT";
	case VK_FORMAT_D24_UNORM_S8_UINT:        return "D24_UNORM_S8_UINT";
	case VK_FORMAT_D16_UNORM:                return "D16_UNORM";
	case VK_FORMAT_R32_SFLOAT:               return "R32_SFLOAT";
	case VK_FORMAT_R8_UNORM:                 return "R8_UNORM";
	default: {
		static thread_local char buf[32];
		snprintf(buf, sizeof(buf), "VkFormat(%d)", static_cast<int>(fmt));
		return buf;
	}
	}
}

inline const char* ImageLayout(VkImageLayout layout) {
	switch (layout) {
	case VK_IMAGE_LAYOUT_UNDEFINED:                        return "UNDEFINED";
	case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:         return "COLOR_ATTACHMENT";
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT";
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:         return "SHADER_READ_ONLY";
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:             return "TRANSFER_SRC";
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:             return "TRANSFER_DST";
	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:                  return "PRESENT_SRC";
	default: {
		static thread_local char buf[32];
		snprintf(buf, sizeof(buf), "Layout(%d)", static_cast<int>(layout));
		return buf;
	}
	}
}

inline std::string PipelineStage(VkPipelineStageFlags flags) {
	if (flags == 0) return "NONE";
	std::string result;
	auto append = [&](VkPipelineStageFlags bit, const char* name) {
		if (flags & bit) {
			if (!result.empty()) result += " | ";
			result += name;
		}
	};
	append(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,             "TOP_OF_PIPE");
	append(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,           "VERTEX");
	append(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,         "FRAGMENT");
	append(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,    "EARLY_FRAG_TESTS");
	append(VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,     "LATE_FRAG_TESTS");
	append(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,  "COLOR_OUTPUT");
	append(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,          "COMPUTE");
	append(VK_PIPELINE_STAGE_TRANSFER_BIT,                "TRANSFER");
	append(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,          "BOTTOM_OF_PIPE");
	if (result.empty()) {
		char buf[32];
		snprintf(buf, sizeof(buf), "Stage(0x%x)", flags);
		return buf;
	}
	return result;
}

inline std::string ImageUsage(VkImageUsageFlags flags) {
	if (flags == 0) return "NONE";
	std::string result;
	auto append = [&](VkImageUsageFlags bit, const char* name) {
		if (flags & bit) {
			if (!result.empty()) result += " | ";
			result += name;
		}
	};
	append(VK_IMAGE_USAGE_TRANSFER_SRC_BIT,             "TRANSFER_SRC");
	append(VK_IMAGE_USAGE_TRANSFER_DST_BIT,             "TRANSFER_DST");
	append(VK_IMAGE_USAGE_SAMPLED_BIT,                   "SAMPLED");
	append(VK_IMAGE_USAGE_STORAGE_BIT,                   "STORAGE");
	append(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,          "COLOR_ATTACH");
	append(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,  "DEPTH_ATTACH");
	append(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,      "TRANSIENT");
	if (result.empty()) {
		char buf[32];
		snprintf(buf, sizeof(buf), "Usage(0x%x)", flags);
		return buf;
	}
	return result;
}

inline const char* SampleCount(VkSampleCountFlagBits samples) {
	switch (samples) {
	case VK_SAMPLE_COUNT_1_BIT:  return "1x";
	case VK_SAMPLE_COUNT_2_BIT:  return "2x";
	case VK_SAMPLE_COUNT_4_BIT:  return "4x";
	case VK_SAMPLE_COUNT_8_BIT:  return "8x";
	case VK_SAMPLE_COUNT_16_BIT: return "16x";
	default: return "?x";
	}
}

inline const char* LoadOpStr(LoadOp op) {
	switch (op) {
	case LoadOp::Clear:    return "Clear";
	case LoadOp::Load:     return "Load";
	case LoadOp::DontCare: return "DontCare";
	default: return "?";
	}
}

inline const char* StoreOpStr(StoreOp op) {
	switch (op) {
	case StoreOp::Store:    return "Store";
	case StoreOp::DontCare: return "DontCare";
	default: return "?";
	}
}

} // namespace VkStr
