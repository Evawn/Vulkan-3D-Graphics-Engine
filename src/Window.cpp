#include "Window.h"
#include <stdexcept>
#include <algorithm>

std::unique_ptr<Window> Window::Create(const std::string& title) {
	auto window = std::unique_ptr<Window>(new Window());

	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

	GLFWwindow* raw_window = glfwCreateWindow(800, 600, title.c_str(), nullptr, nullptr);
	if (!raw_window) {
		throw std::runtime_error("Failed to create GLFW window");
	}
	window->m_handle = std::make_shared<GLFWwindow*>(raw_window);

	// Size to 80% of the monitor work area the OS placed us on, centered
	GLFWmonitor* monitor = GetCurrentMonitor(raw_window);
	int work_x, work_y, work_w, work_h;
	glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_w, &work_h);

	int win_w = static_cast<int>(work_w * 0.8f);
	int win_h = static_cast<int>(work_h * 0.8f);
	glfwSetWindowSize(raw_window, win_w, win_h);
	glfwSetWindowPos(raw_window, work_x + (work_w - win_w) / 2, work_y + (work_h - win_h) / 2);

	glfwSetWindowUserPointer(raw_window, window.get());
	glfwSetFramebufferSizeCallback(raw_window, glfw_FramebufferResizeCallback);
	glfwSetWindowContentScaleCallback(raw_window, glfw_ContentScaleCallback);

	glfwShowWindow(raw_window);

	return window;
}

GLFWwindow* Window::GetRaw() const {
	return *m_handle;
}

std::shared_ptr<GLFWwindow*> Window::GetHandle() const {
	return m_handle;
}

bool Window::ShouldClose() const {
	return glfwWindowShouldClose(*m_handle);
}

float Window::GetContentScale() const {
	float scale;
	glfwGetWindowContentScale(*m_handle, &scale, nullptr);
	return scale;
}

void Window::SetFramebufferResizeCallback(std::function<void()> cb) {
	m_on_resize = std::move(cb);
}

void Window::SetContentScaleCallback(std::function<void(float)> cb) {
	m_on_scale_change = std::move(cb);
}

void Window::Destroy() {
	if (m_handle && *m_handle) {
		glfwDestroyWindow(*m_handle);
		*m_handle = nullptr;
	}
	glfwTerminate();
}

Window::~Window() {
	Destroy();
}

GLFWmonitor* Window::GetCurrentMonitor(GLFWwindow* window) {
	int wx, wy, ww, wh;
	glfwGetWindowPos(window, &wx, &wy);
	glfwGetWindowSize(window, &ww, &wh);

	int best_overlap = 0;
	GLFWmonitor* best_monitor = nullptr;

	int monitor_count;
	GLFWmonitor** monitors = glfwGetMonitors(&monitor_count);

	for (int i = 0; i < monitor_count; ++i) {
		int mx, my, mw, mh;
		glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);

		int overlap_x = std::max(0, std::min(wx + ww, mx + mw) - std::max(wx, mx));
		int overlap_y = std::max(0, std::min(wy + wh, my + mh) - std::max(wy, my));
		int overlap = overlap_x * overlap_y;

		if (overlap > best_overlap) {
			best_overlap = overlap;
			best_monitor = monitors[i];
		}
	}

	return best_monitor ? best_monitor : glfwGetPrimaryMonitor();
}

void Window::glfw_FramebufferResizeCallback(GLFWwindow* window, int width, int height) {
	auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
	if (self->m_on_resize) {
		self->m_on_resize();
	}
}

void Window::glfw_ContentScaleCallback(GLFWwindow* window, float xscale, float yscale) {
	auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
	if (self->m_on_scale_change) {
		self->m_on_scale_change(xscale);
	}
}
