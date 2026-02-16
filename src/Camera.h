#pragma once
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>

class Camera
{
private:
	glm::vec3 m_position;
	glm::vec3 m_up;
	glm::vec3 m_forward;
	float m_fovy, m_aspect, m_znear, m_zfar;

public:

	static std::shared_ptr<Camera> Create() {
		auto ret = std::make_shared<Camera>();
		ret->m_position = glm::vec3(0.0f, -3.0f, 0.0f);
		ret->m_up = glm::vec3(0.0f, 0.0f, 1.0f);
		ret->m_forward = glm::vec3(0.0f, 1.0f, 0.0f);
		ret->m_fovy = 45.0f;
		ret->m_aspect = 1.0f;
		ret->m_znear = 0.1f;
		ret->m_zfar = 100.0f;
		return ret;
	}

	static std::shared_ptr<Camera> Create(float fovy, float aspect, float znear, float zfar) {
		auto ret = std::make_shared<Camera>();
		ret->m_position = glm::vec3(3.0f, 3.0f, 3.0f);
		ret->m_up = glm::vec3(0.0f, 0.0f, 1.0f);
		ret->m_forward = glm::normalize(glm::vec3(-1.0f, -1.0f, -1.0f));
		ret->m_fovy = fovy;
		ret->m_aspect = aspect;
		ret->m_znear = znear;
		ret->m_zfar = zfar;
		return ret;
	}

	glm::mat4 GetViewMatrix() {
		return glm::lookAt(m_position, m_position + m_forward, m_up);
	}

	glm::mat4 GetProjectionMatrix() {
		auto mat = glm::perspective(glm::radians(m_fovy), m_aspect, m_znear, m_zfar);
		mat[1][1] *= -1;
		return mat;
	}

	glm::mat4 GetNDCtoWorldMatrix() {
		return glm::inverse(GetProjectionMatrix() * GetViewMatrix());
	}

	glm::vec3 GetPosition() {
		return m_position;
	}

	void SetForward(glm::vec3 forward) {
		m_forward = glm::normalize(forward);
	}

	/// <summary>
	/// Move the camera in the direction of the forward vector projected onto the plane normal to the up vector
	/// </summary>
	/// <param name="distance"></param>
	void MoveForward(float distance) {

		glm::vec3 upNormalized = glm::normalize(m_up); // Normalize the 'up' vector
		float dotProduct = glm::dot(m_forward, upNormalized);
		glm::vec3 projection = m_forward - dotProduct * upNormalized;

		m_position += glm::normalize(projection) * distance;
	}

	void MoveRight(float distance) {
		glm::vec3 right = glm::cross(m_forward, m_up);
		m_position += glm::normalize(right) * distance;
	}

	void MoveUp(float distance) {
		m_position += glm::normalize(m_up) * distance;
	}

	glm::vec3 GetUp() {
		return m_up;
	}

	glm::vec3 GetForward() {
		return m_forward;
	}

	void SetPosition(glm::vec3 pos) { m_position = pos; }

	float GetFOV() const { return m_fovy; }
	void SetFOV(float fov) { m_fovy = fov; }

	float GetNear() const { return m_znear; }
	float GetFar() const { return m_zfar; }
	void SetNearFar(float znear, float zfar) { m_znear = znear; m_zfar = zfar; }

	float GetAspect() const { return m_aspect; }
	void SetAspect(float aspect) { m_aspect = aspect; }
};