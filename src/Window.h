#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <memory>
#include <string>
#include <functional>

class Window {
public:
	static std::unique_ptr<Window> Create(const std::string& title = "Vulkan");

	GLFWwindow* GetRaw() const;
	std::shared_ptr<GLFWwindow*> GetHandle() const;
	bool ShouldClose() const;
	float GetContentScale() const;

	void SetFramebufferResizeCallback(std::function<void()> cb);
	void SetContentScaleCallback(std::function<void(float)> cb);

	void Destroy();
	~Window();

private:
	Window() = default;

	std::shared_ptr<GLFWwindow*> m_handle;
	std::function<void()> m_on_resize;
	std::function<void(float)> m_on_scale_change;

	static GLFWmonitor* GetCurrentMonitor(GLFWwindow* window);
	static void glfw_FramebufferResizeCallback(GLFWwindow* window, int width, int height);
	static void glfw_ContentScaleCallback(GLFWwindow* window, float xscale, float yscale);
};
