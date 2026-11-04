#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/common.hpp>
#include <cmath>

// 与 retrieve 原工程一致的自由相机：yaw/pitch + 前向移动
class Camera {
public:
	glm::vec3 position{ 0.0f, 20.0f, -5.0f };
	float yaw = 0.0f;
	float pitch = 0.0f;

	glm::vec3 forward() const {
		return glm::vec3(std::cos(pitch) * std::sin(yaw),
			std::sin(pitch),
			std::cos(pitch) * std::cos(yaw));
	}

	glm::mat4 view() const {
		return glm::lookAt(position, position + forward(), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	void rotate(float dYaw, float dPitch) {
		yaw += dYaw;
		pitch = glm::clamp(pitch + dPitch, glm::radians(-89.0f), glm::radians(89.0f));
	}

	void moveForward(float amount) {
		position += forward() * amount;
	}
};
