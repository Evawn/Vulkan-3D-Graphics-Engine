#ifndef INSTANCE_H
#define INSTANCE_H

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vector>
#include <stdexcept>
#include <iostream>
#include <unordered_set>
#include <string>
#include <memory>


namespace VWrap {

	/// <summary>
	/// The validation layers to enable.
	/// </summary>
	const std::vector<const char*> VALIDATION_LAYERS = {
	"VK_LAYER_KHRONOS_validation"
	};

	/// <summary>
	/// The device extensions to enable.
	/// </summary>
	const std::vector<const char*> DEVICE_EXTENSIONS = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	/// <summary>
	/// Proxy function that loads the extension function vkCreateDebugUtilsMessengerEXT and then runs it.
	/// </summary>
	/// <param name="pDebugMessenger">  Where to store the callback handle. </param>
	static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
		const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
		const VkAllocationCallbacks* pAllocator,
		VkDebugUtilsMessengerEXT* pDebugMessenger);

	/// <summary>
	/// Proxy function that loads the EXT function 'vkDestroyDebugUtilsMessengerEXT' and then runs it.
	/// </summary>
	static void DestroyDebugUtilsMessengerEXT(VkInstance instance,
		VkDebugUtilsMessengerEXT debugMessenger,
		const VkAllocationCallbacks* pAllocator);


	/// <summary>
	/// Custom Debug Callback
	/// </summary>
	/// <param name="messageSeverity"> The severity of the message. Can be VERBOSE, INFO, WARNING, ERROR. </param>
	/// <param name="messageType">  The type of the message. Can be GENERAL, VALIDATION, PERFORMANCE. </param>
	/// <param name="pCallbackData"> Contains the actual message, and an array of objects relating to the message. </param>
	/// <param name="pUserData"> Allows you to pass your own data to the function. Specified in callback setup. </param>
	/// <returns> Whether or not to abort the function that trigger this callback. Should always be VK_FALSE. </returns>
	static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
		VkDebugUtilsMessageTypeFlagsEXT messageType,
		const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
		void* pUserData);

	/// <summary> Populates passed debug messenger create info. </summary>
	static void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);

	/// <summary> Gets the required extensions, including validation extensions if enabled. </returns>
	static std::vector<const char*> getRequiredExtensions(bool enableValidationLayers);

	/// <summary> Queries whether all elements of VALIDATION_LAYERS are available. </returns>
	static bool checkValidationLayerSupport();

	/// <summary> Represents a Vulkan Instance. </summary>
	class Instance {

	private:

		/// <summary>
		/// The underlying Vulkan instance handle.
		/// </summary>
		VkInstance m_instance{ VK_NULL_HANDLE };

		/// <summary>
		/// The debug messenger handle.
		/// </summary>
		VkDebugUtilsMessengerEXT m_debug_messenger{ VK_NULL_HANDLE };

	public:
		/// <summary> Creates an Instance </summary>
		static std::shared_ptr<Instance> Create(bool enableValidationLayers);

		/// <summary> Gets the Vulkan instance handle. </summary>
		VkInstance Get() const { return m_instance; }

		~Instance();

	};
}
#endif