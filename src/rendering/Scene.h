#pragma once

#include "AssetRegistry.h"
#include "SceneLighting.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Camera;

// ---- Component ----
//
// A piece of data attached to a SceneNode. Tagged-union (flat struct + type
// enum) to mirror RenderItem on the consumer side: same shape, same dispatch
// pattern. Adding a new component type later is purely additive — extend the
// enum, add the relevant fields, teach SceneExtractor to dispatch on it.
//
// v1 component set:
//   - Mesh         → emits one RenderItem::Mesh per frame
//   - VoxelVolume  → emits one RenderItem::BrickmapVolume per frame
//
// Light + Camera components are reserved for the next iteration (today, the
// Scene exposes one global SceneLighting and one active Camera directly).

enum class ComponentType : uint8_t {
	Mesh,
	VoxelVolume,
};

struct Component {
	ComponentType type;
	AssetID       asset;          // resolved against AssetRegistry by the extractor

	// VoxelVolume-only — number of animation frames packed in the asset.
	// Today this is always 1; the foliage workflow will use >1 once the
	// animated voxel asset path lands.
	uint32_t frameCount = 1;
};

// ---- SceneNode ----
//
// One node in the scene tree. Carries a local TRS transform, optional
// components, and child nodes. The cached world transform is recomputed on
// demand by the SceneExtractor (or eagerly if a future caller wants it).
//
// Children are owned via unique_ptr so node identity is stable across tree
// edits (we don't have to invalidate raw pointers when the parent's vector
// reallocates).

class SceneNode {
public:
	std::string name;

	glm::vec3 position = glm::vec3(0.0f);
	glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::vec3 scale    = glm::vec3(1.0f);

	bool visible = true;

	std::vector<Component>                    components;
	std::vector<std::unique_ptr<SceneNode>>   children;

	// Cached world transform — written by extraction. Public so future debug
	// tooling (gizmo, bounds renderer) can read it without re-traversing.
	mutable glm::mat4 cachedWorld   = glm::mat4(1.0f);
	mutable bool      worldDirty    = true;

	// Add a child node owned by this node. Returns a non-owning pointer the
	// caller can use to populate the new child further (set components, etc.).
	SceneNode* AddChild(std::string name = {});

	// Add a component to this node. Returns a reference to the stored copy
	// for chained edits.
	Component& AddComponent(Component c);

	// Local TRS → 4x4. Composed in T * R * S order (engine-standard).
	glm::mat4 LocalTransform() const;

	// Mark this node and all descendants as dirty. Call after editing
	// position / rotation / scale of a node high in the tree if you want
	// downstream nodes to recompute their world transform.
	void MarkSubtreeDirty();
};

// ---- Scene ----
//
// Top-level container of world state. Owns the root SceneNode (always identity
// transform), the scene-wide lighting state, and a non-owning reference to the
// active camera. Future iterations move SceneLighting and Camera ownership in
// here from RenderingSystem; for v1 the Scene is the authoritative storage and
// the rest of the engine reads through it.
//
// The Scene does NOT own:
//   - Asset data (that lives in AssetRegistry, addressed by AssetID)
//   - Render resources (that lives in RenderGraph)
//   - Per-frame extracted items (that lives in RenderScene)

class Scene {
public:
	Scene();

	SceneNode&       GetRoot()       { return m_root; }
	const SceneNode& GetRoot() const { return m_root; }

	SceneLighting&       GetLighting()       { return m_lighting; }
	const SceneLighting& GetLighting() const { return m_lighting; }

	std::shared_ptr<Camera> GetActiveCamera() const { return m_camera; }
	void SetActiveCamera(std::shared_ptr<Camera> cam) { m_camera = std::move(cam); }

private:
	SceneNode               m_root;
	SceneLighting           m_lighting;
	std::shared_ptr<Camera> m_camera;
};
