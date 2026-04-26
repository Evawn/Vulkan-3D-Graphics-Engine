#include "Scene.h"
#include "AssetRegistry.h"

#include <glm/gtx/euler_angles.hpp>

namespace {
// Inspector-side handle to the asset registry, set by Application during
// editor wiring. Read inside SceneNode::GetParameters() to resolve component
// asset references to display names. Static because parameter rows are
// per-node but the inspector context is per-process.
const AssetRegistry* g_inspectorAssets = nullptr;
}

void SceneNode::SetAssetRegistryForInspector(const AssetRegistry* assets) {
	g_inspectorAssets = assets;
}

Scene::Scene() {
	m_root.name = "root";
}

SceneNode* SceneNode::AddChild(std::string nm) {
	auto child = std::make_unique<SceneNode>();
	child->name = std::move(nm);
	auto* raw = child.get();
	children.push_back(std::move(child));
	// Adding a child invalidates parent's cached parameter rebuild key on
	// the *child*, but the parent's own cache is unaffected — only its
	// component count drives rebuilds.
	return raw;
}

Component& SceneNode::AddComponent(Component c) {
	components.push_back(c);
	// Force parameter cache rebuild on next GetParameters() — component count changed.
	m_paramsBuiltForComponentCount = SIZE_MAX;
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

std::string SceneNode::GetDisplayName() const {
	return name.empty() ? std::string("(unnamed node)") : name;
}

std::vector<TechniqueParameter>& SceneNode::GetParameters() {
	// Lazy build — the row vector aliases pointers into this node's members,
	// so it's stable across calls as long as the node stays at the same
	// address. Only rebuild when the component list shape changes (the rows
	// for components are appended once and never reorder).
	if (m_paramsBuiltForComponentCount == components.size()) {
		return m_params;
	}

	m_params.clear();
	m_componentTextCache.clear();
	m_componentTextCache.reserve(components.size());

	// Refresh the Euler cache from the current quaternion. We only do this on
	// rebuild (which is rare — only on component-count change). Inside a
	// continuous edit session the cached Euler is the source of truth.
	glm::extractEulerAngleXYZ(glm::mat4_cast(rotation),
		m_eulerDeg.x, m_eulerDeg.y, m_eulerDeg.z);
	m_eulerDeg = glm::degrees(m_eulerDeg);

	auto onTransformChanged = [this]() {
		// Recompose quat from edited Euler angles. Stored degrees → radians.
		glm::vec3 r = glm::radians(m_eulerDeg);
		rotation = glm::quat(glm::eulerAngleXYZ(r.x, r.y, r.z));
		MarkSubtreeDirty();
	};

	{
		TechniqueParameter p;
		p.label = "Position";
		p.type = TechniqueParameter::Vec3;
		p.data = &position;
		p.speed = 0.1f; p.min = -1e6f; p.max = 1e6f;
		p.onChanged = [this]() { MarkSubtreeDirty(); };
		m_params.push_back(std::move(p));
	}
	{
		TechniqueParameter p;
		p.label = "Rotation (deg)";
		p.type = TechniqueParameter::Vec3;
		p.data = &m_eulerDeg;
		p.speed = 1.0f; p.min = -360.0f; p.max = 360.0f;
		p.onChanged = onTransformChanged;
		m_params.push_back(std::move(p));
	}
	{
		TechniqueParameter p;
		p.label = "Scale";
		p.type = TechniqueParameter::Vec3;
		p.data = &scale;
		p.speed = 0.05f; p.min = 0.001f; p.max = 1000.0f;
		p.onChanged = [this]() { MarkSubtreeDirty(); };
		m_params.push_back(std::move(p));
	}
	{
		TechniqueParameter p;
		p.label = "Visible";
		p.type = TechniqueParameter::Bool;
		p.data = &visible;
		m_params.push_back(std::move(p));
	}

	// Component rows — read-only summary. Editing component asset references
	// is asset-browser territory (§8.2 in the design doc, deferred).
	for (const auto& c : components) {
		std::string label;
		std::string value;
		switch (c.type) {
			case ComponentType::Mesh: {
				label = "Mesh";
				if (g_inspectorAssets) {
					if (const auto* m = g_inspectorAssets->GetMesh(c.asset)) {
						value = m->name + " (" + std::to_string(m->vertices.size()) + " verts)";
					}
				}
				if (value.empty()) value = "(asset missing)";
				break;
			}
			case ComponentType::VoxelVolume: {
				label = "Voxel Volume";
				if (g_inspectorAssets) {
					if (const auto* v = g_inspectorAssets->GetVoxelVolume(c.asset)) {
						value = v->name + " (" +
							std::to_string(v->size.x) + "x" +
							std::to_string(v->size.y) + "x" +
							std::to_string(v->size.z) + ")";
						if (v->frameCount > 1) value += " x" + std::to_string(v->frameCount) + "f";
						if (v->isProcedural) value += " [procedural]";
					}
				}
				if (value.empty()) value = "(asset missing)";
				break;
			}
			case ComponentType::InstanceCloud: {
				label = "Instance Cloud";
				value = std::to_string(c.instanceCount) + " instances";
				if (g_inspectorAssets) {
					if (const auto* v = g_inspectorAssets->GetVoxelVolume(c.asset)) {
						value += " of " + v->name;
						if (v->frameCount > 1) {
							value += " (" + std::to_string(v->frameCount) + " frames)";
						}
					}
				}
				break;
			}
		}
		m_componentTextCache.push_back(std::move(value));
		TechniqueParameter p;
		p.label     = std::move(label);
		p.type      = TechniqueParameter::Text;
		p.textValue = &m_componentTextCache.back();
		m_params.push_back(std::move(p));
	}

	m_paramsBuiltForComponentCount = components.size();
	return m_params;
}
