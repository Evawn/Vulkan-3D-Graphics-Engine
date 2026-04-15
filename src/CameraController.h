#pragma once

#include "Camera.h"
#include "Input.h"
#include <functional>
#include <memory>

class CameraController {
private:
	enum class Action {
		ESCAPE, MOVE_FORWARD, MOVE_BACKWARD, MOVE_LEFT, MOVE_RIGHT, MOVE_UP, MOVE_DOWN,
		RELOAD_SHADERS,
		TOGGLE_VIEWPORT_ONLY, TOGGLE_OS_FULLSCREEN
	};

	struct MoveState {
		bool up = false, down = false, left = false, right = false, forward = false, back = false;
		double dx = 0.0, dy = 0.0;
	};

	std::shared_ptr<Camera> m_camera;

	bool m_focused = false;
	float m_sensitivity = 0.5f;
	float m_speed = 5.0f;

	MoveState m_move_state;
	Context m_context;
	std::function<void()> m_reload_callback;
	std::function<void()> m_toggle_viewport_only;
	std::function<void()> m_toggle_fullscreen;
	std::function<void(bool)> m_focus_changed;

	void ParseInput(const InputQuery& query);
	void ApplyMovement(float dt);

public:
	static std::shared_ptr<CameraController> Create(std::shared_ptr<Camera> camera);

	void Update(float dt);

	bool IsFocused() const { return m_focused; }
	void SetFocused(bool focused);

	void SetReloadCallback(std::function<void()> cb) { m_reload_callback = std::move(cb); }
	void SetToggleViewportOnlyCallback(std::function<void()> cb) { m_toggle_viewport_only = std::move(cb); }
	void SetToggleFullscreenCallback(std::function<void()> cb) { m_toggle_fullscreen = std::move(cb); }
	void SetFocusChangedCallback(std::function<void(bool)> cb) { m_focus_changed = std::move(cb); }

	float* SensitivityPtr() { return &m_sensitivity; }
	float* SpeedPtr() { return &m_speed; }
	std::shared_ptr<Camera> GetCamera() const { return m_camera; }
};
