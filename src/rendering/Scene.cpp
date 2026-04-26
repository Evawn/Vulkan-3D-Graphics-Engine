#include "Scene.h"

Scene::Scene() {
	m_root.name = "root";
}

SceneNode* SceneNode::AddChild(std::string nm) {
	auto child = std::make_unique<SceneNode>();
	child->name = std::move(nm);
	auto* raw = child.get();
	children.push_back(std::move(child));
	return raw;
}

Component& SceneNode::AddComponent(Component c) {
	components.push_back(c);
	return components.back();
}

glm::mat4 SceneNode::LocalTransform() const {
	glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
	glm::mat4 r = glm::mat4_cast(rotation);
	glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
	return t * r * s;
}

void SceneNode::MarkSubtreeDirty() {
	worldDirty = true;
	for (auto& c : children) c->MarkSubtreeDirty();
}
