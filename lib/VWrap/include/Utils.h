#pragma once
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>
#include <fstream>
#include <glm/glm.hpp>
#include <array>
#include <memory>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

namespace VWrap {

	/// <summary>
	/// A struct containing the indices of the queue families that support the operations we need.
	/// </summary>
	struct QueueFamilyIndices {

		/// <summary> The family for graphics operations. </summary>
		std::optional<uint32_t> graphicsFamily;

		/// <summary> The family for presentation operations. </summary>
		std::optional<uint32_t> presentFamily;

		/// <summary> The family for transfer operations. </summary>
		std::optional<uint32_t> transferFamily;

		/// <summary> Gets whether or not the queue families are available. </summary>
		bool isComplete() {
			return graphicsFamily.has_value() && presentFamily.has_value() && transferFamily.has_value();
		}
	};

	/// <summary> A struct containing the support details of the swapchain. </summary>
	struct SwapchainSupportDetails {

		/// <summary> The capabilities of the swapchain. </summary>
		VkSurfaceCapabilitiesKHR capabilities;

		/// <summary>  The supported surface formats.  </summary>
		std::vector<VkSurfaceFormatKHR> formats;

		/// <summary> The supported presentation modes. </summary>
		std::vector<VkPresentModeKHR> presentModes;
	};

	/// <summary>
	/// A struct containing the vertex input binding description and attribute descriptions.
	/// </summary>
	struct Vertex {
		glm::vec3 pos;
		glm::vec3 color;
		glm::vec2 texCoord;

		/// <summary> Gets the binding description for the vertex. </summary>
		static VkVertexInputBindingDescription getBindingDescription() {
			VkVertexInputBindingDescription bindingDescription{};
			bindingDescription.binding = 0;
			bindingDescription.stride = sizeof(Vertex);
			bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

			return bindingDescription;
		}

		/// <summary> Gets the attribute descriptions for the vertex. </summary>
		static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
			std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};

			attributeDescriptions[0].binding = 0;
			attributeDescriptions[0].location = 0;
			attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
			attributeDescriptions[0].offset = offsetof(Vertex, pos);

			attributeDescriptions[1].binding = 0;
			attributeDescriptions[1].location = 1;
			attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
			attributeDescriptions[1].offset = offsetof(Vertex, color);

			attributeDescriptions[2].binding = 0;
			attributeDescriptions[2].location = 2;
			attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
			attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

			return attributeDescriptions;
		}

		/// <summary> Gets whether or not the vertex is equal to another vertex. </summary>
		bool operator==(const Vertex& other) const {
			return pos == other.pos && color == other.color && texCoord == other.texCoord;
		}
	};

	/// <summary>
	/// Reads the file at 'filename' and returns a byte array representing its contents.
	/// </summary>
	/// <param name="filename"> The location from which to read. </param>
	/// <returns> The binary contents of the file at 'filename'</returns>
	static std::vector<char> readFile(const std::string& filename) {
		std::ifstream file(filename, std::ios::ate | std::ios::binary);

		if (!file.is_open()) {
			throw std::runtime_error("failed to open file!");
		}
		size_t fileSize = (size_t)file.tellg();
		std::vector<char> buffer(fileSize);

		file.seekg(0);
		file.read(buffer.data(), fileSize);
		file.close();

		return buffer;
	}

	/// <summary>
	/// Whether the given format has a stencil component.
	/// </summary>
	static bool HasStencilComponent(VkFormat format)
	{
		return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
	}

	/// <summary>
	/// finds the VkFormat among the given candidates that is supported by the device and has the given features.
	/// </summary>
	static VkFormat FindSupportedFormat(VkPhysicalDevice device, const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features)
	{
		for (VkFormat format : candidates) {
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties(device, format, &props);

			if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
				return format;
			}
			else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
				return format;
			}
		}

		throw std::runtime_error("failed to find supported format!");
	}

	/// <summary>
	/// finds the depth format that is supported by the device.
	/// </summary>
	static VkFormat FindDepthFormat(VkPhysicalDevice device)
	{
		return FindSupportedFormat(
			device,
			{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT },
			VK_IMAGE_TILING_OPTIMAL,
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
		);
	}

	struct PushConstantBlock {
		glm::mat4 NDCtoWorld;
		glm::vec3 cameraPos;
	};
}

namespace std {
	/// <summary>
	/// Defines a hash function for a VWrap::Vertex
	/// </summary>
	template<> struct hash<VWrap::Vertex> {
		size_t operator()(VWrap::Vertex const& vertex) const {
			return ((hash<glm::vec3>()(vertex.pos) ^ (hash<glm::vec3>()(vertex.color) << 1)) >> 1) ^ (hash<glm::vec2>()(vertex.texCoord) << 1);
		}
	};
}