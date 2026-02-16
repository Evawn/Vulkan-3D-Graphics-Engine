#include "CameraController.h"
#include <glm/gtc/matrix_transform.hpp>

std::shared_ptr<CameraController> CameraController::Create(std::shared_ptr<Camera> camera) {
	auto ret = std::make_shared<CameraController>();
	ret->m_camera = camera;
	ret->m_context = {
		"Main",
		{
			{{GLFW_KEY_ESCAPE, KeyState::PRESSED}, (int)Action::ESCAPE},
			{{GLFW_KEY_W, KeyState::DOWN}, (int)Action::MOVE_FORWARD},
			{{GLFW_KEY_S, KeyState::DOWN}, (int)Action::MOVE_BACKWARD},
			{{GLFW_KEY_A, KeyState::DOWN}, (int)Action::MOVE_LEFT},
			{{GLFW_KEY_D, KeyState::DOWN}, (int)Action::MOVE_RIGHT},
			{{GLFW_KEY_SPACE, KeyState::DOWN}, (int)Action::MOVE_UP},
			{{GLFW_KEY_LEFT_SHIFT, KeyState::DOWN}, (int)Action::MOVE_DOWN},
			{{GLFW_KEY_F5, KeyState::PRESSED}, (int)Action::RELOAD_SHADERS}
		}
	};
	Input::AddContext(ret->m_context);
	return ret;
}

void CameraController::Update(float dt) {
	auto query = Input::Poll();
	ParseInput(query);

	if (m_focused) {
		ApplyMovement(dt);
	}
}

void CameraController::SetFocused(bool focused) {
	m_focused = focused;
	Input::HideCursor(focused);
	Input::CenterCursor(focused);
	if (!focused) {
		m_move_state = {};
	}
}

void CameraController::ParseInput(const InputQuery& query) {
	m_move_state = {};
	m_move_state.dx = query.dx;
	m_move_state.dy = query.dy;

	for (auto i : query.actions) {
		Action action = static_cast<Action>(i);

		switch (action) {
		case Action::ESCAPE:
			if (m_focused) {
				SetFocused(false);
			}
			break;
		case Action::MOVE_UP:
			m_move_state.up = true;
			break;
		case Action::MOVE_DOWN:
			m_move_state.down = true;
			break;
		case Action::MOVE_LEFT:
			m_move_state.left = true;
			break;
		case Action::MOVE_RIGHT:
			m_move_state.right = true;
			break;
		case Action::MOVE_FORWARD:
			m_move_state.forward = true;
			break;
		case Action::MOVE_BACKWARD:
			m_move_state.back = true;
			break;
		case Action::RELOAD_SHADERS:
			if (m_reload_callback) m_reload_callback();
			break;
		default:
			return;
		}
	}
}

void CameraController::ApplyMovement(float dt) {
	float distance = m_speed * dt;
	double mouse_sensitivity = (float)(-m_sensitivity / 100.0);

	if (m_move_state.up && !m_move_state.down) m_camera->MoveUp(distance);
	if (m_move_state.down && !m_move_state.up) m_camera->MoveUp(-distance);
	if (m_move_state.left && !m_move_state.right) m_camera->MoveRight(-distance);
	if (m_move_state.right && !m_move_state.left) m_camera->MoveRight(distance);
	if (m_move_state.forward && !m_move_state.back) m_camera->MoveForward(distance);
	if (m_move_state.back && !m_move_state.forward) m_camera->MoveForward(-distance);

	double dx = m_move_state.dx * mouse_sensitivity;
	double dy = m_move_state.dy * mouse_sensitivity;

	auto forward = m_camera->GetForward();
	auto up = m_camera->GetUp();

	auto dot = glm::dot(forward, up);
	dot = glm::clamp(dot, -1.0f, 1.0f);
	auto angle = glm::acos(dot);

	if (angle - dy < 0.001f || angle - dy > glm::pi<float>() - 0.001f) dy = 0.0f;

	auto x_rot = glm::rotate(glm::mat4(1.0f), (float)dx, up);
	auto y_rot = glm::rotate(glm::mat4(1.0f), (float)dy, glm::cross(forward, up));
	auto final_vec = x_rot * y_rot * glm::vec4(forward, 1.0f);

	m_camera->SetForward(glm::normalize(final_vec));
}
