#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <stdexcept>
#include <memory>
#include "Instance.h"

namespace VWrap {

	/// <summary>
	/// Represents a Vulkan surface.
	/// </summary>
	class Surface {

	private:

		/// <summary> The surface handle. </summary>
		VkSurfaceKHR m_surface{ VK_NULL_HANDLE };

		/// <summary> The Instance associated with this surface. </summary>
		std::shared_ptr<Instance> m_instance;

		/// <summary> The GLFW window associated with this surface. </summary>
		std::shared_ptr<GLFWwindow*> m_window;

	public:

		/// <summary>
		/// Creates a new surface attached to the given window.
		/// </summary>
		static std::shared_ptr<Surface> Create(std::shared_ptr<Instance> instance, std::shared_ptr<GLFWwindow*> window) {
			auto ret = std::make_shared<Surface>();
			ret->m_instance = instance;
			ret->m_window = window;

			if (glfwCreateWindowSurface(instance->Get(), *window, nullptr, &ret->m_surface) != VK_SUCCESS) {
				throw std::runtime_error("Failed to create window surface!");
			}
			return ret;
		}

		/// <summary>
		/// Returns the surface handle.
		/// </summary>
		VkSurfaceKHR Get() const {
			return m_surface;
		}

		/// <summary>
		/// Gets the GLFW Window associated with this surface.
		/// </summary>
		std::shared_ptr<GLFWwindow*> GetWindow() const {
			return m_window;
		}

		~Surface() {
			if (m_surface != VK_NULL_HANDLE) {
				vkDestroySurfaceKHR(m_instance->Get(), m_surface, nullptr);
			}
		}
	};
}