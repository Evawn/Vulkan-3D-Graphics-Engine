#include "Instance.h"
namespace VWrap
{
    VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger) {

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");

        if (func != nullptr) {
            return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
        }
        else {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    void DestroyDebugUtilsMessengerEXT(VkInstance instance,
        VkDebugUtilsMessengerEXT debugMessenger,
        const VkAllocationCallbacks* pAllocator) {

        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");

        if (func != nullptr) {
            func(instance, debugMessenger, pAllocator);
        }
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData) {

        std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;

        return VK_FALSE;
    }

    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
        createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = nullptr;
    }

    std::vector<const char*> getRequiredExtensions(bool enableValidationLayers) {

        // Get the names of the required instance extensions from GLFW
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        // Store the names as a vector<string> for later comparison
        std::vector<std::string> glfwExtStrings;
        for (uint32_t i = 0; i < glfwExtensionCount; i++) {
            glfwExtStrings.push_back(glfwExtensions[i]);
        }

        // Get and store the available instance extensions
        uint32_t availableExtensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(availableExtensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, availableExtensions.data());

        // Output available extensions and store in set
        std::cout << "Available Extensions:\n";
        std::unordered_set<std::string> availExtStrings;
        for (const auto& ext : availableExtensions) {
            availExtStrings.insert(ext.extensionName);
            std::cout << "\t" << ext.extensionName << "\n";
        }
        // Iterate through the required extensions and make sure they are available
        for (const auto str : glfwExtStrings) {
            if (availExtStrings.count(str) < 0) {
                std::cout << "Required Extension not Available: " << str << "\n";
            }
        }

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        if (enableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        return extensions;
    }

    bool checkValidationLayerSupport() {
        uint32_t layerCount = 0;

        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for (const auto& layerName : VALIDATION_LAYERS) {
            bool found = false;
            for (const auto& layer : availableLayers) {
                if (strcmp(layer.layerName, layerName) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }

        return true;
    }

    std::shared_ptr<Instance> Instance::Create(bool enableValidationLayers) {
        auto ret = std::make_shared<Instance>();

        bool validationLayerSupported = checkValidationLayerSupport();

        if (enableValidationLayers && !validationLayerSupported) {
            throw std::runtime_error("Missing support for requested validation layers!");
        }

        // Define ApplicationInfo struct
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.apiVersion = VK_API_VERSION_1_0;

        // Define InstanceCreateInfo struct
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        // Get required Extensions
        auto extensions = getRequiredExtensions(enableValidationLayers);

        // Specify Extensions
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{ };
        // Specify Validation Layers
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(VALIDATION_LAYERS.size());
            createInfo.ppEnabledLayerNames = VALIDATION_LAYERS.data();

            populateDebugMessengerCreateInfo(debugCreateInfo);
            createInfo.pNext = &debugCreateInfo;
        }
        else {
            createInfo.enabledLayerCount = 0;
            createInfo.pNext = nullptr;
        }

        // Finally, create the vkInstance
        if (vkCreateInstance(&createInfo, nullptr, &ret->m_instance) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create instance!");
        }

        // Enable Debug Messenger
        if (enableValidationLayers) {
            VkDebugUtilsMessengerCreateInfoEXT createInfo{};
            populateDebugMessengerCreateInfo(createInfo);

            if (CreateDebugUtilsMessengerEXT(ret->m_instance, &createInfo, nullptr, &ret->m_debug_messenger) != VK_SUCCESS) {
                throw std::runtime_error("Failed to set up debug messenger.");
            }
        }

        return ret;
    }

    Instance::~Instance() {
        if (m_instance != VK_NULL_HANDLE) {
            if (m_debug_messenger != VK_NULL_HANDLE) {
                DestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger, nullptr);
            }
            vkDestroyInstance(m_instance, nullptr);
        }
    }
}

