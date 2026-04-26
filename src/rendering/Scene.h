#pragma once

#include "AssetRegistry.h"
#include "SceneLighting.h"
#include "SkyDescription.h"
#include "Inspectable.h"

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
	InstanceCloud,
};

struct Component {
	ComponentType type;
	AssetID       asset;          // resolved against AssetRegistry by the extractor

	// VoxelVolume / InstanceCloud — number of animation frames packed in the
	// asset (Z-slabs of the volume image). Mesh ignores this.
	uint32_t frameCount = 1;

	// ---- InstanceCloud-only ----
	//
	// One bulk SSBO carries the per-instance state for the entire cloud:
	// position, scale, rotation, animOffset. The technique addresses entries
	// via firstInstance + gl_InstanceIndex. The buffer's lifetime is graph-
	// managed (Persistent), allocated by whoever creates the cloud (typically
	// the technique on first RegisterPasses).
	BufferHandle instanceBuffer;
	uint32_t     instanceCount    = 0;

	// Per-instance bounding box in instance-local space (i.e. the cube each
	// instance rasterizes). Typically (0,0,0) → (size.x, size.y, size.z) of
	// the asset volume so the shader's BB-rasterized cube matches the volume
	// extent. Used by the vertex shader to scale the unit cube and by future
	// frustum culling.
	glm::vec3 instanceAabbMin = glm::vec3(0.0f);
	glm::vec3 instanceAabbMax = glm::vec3(0.0f);
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

class AssetRegistry;

class SceneNode : public IInspectable {
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

	// IInspectable
	std::string                       GetDisplayName() const override;
	std::vector<TechniqueParameter>&  GetParameters() override;

	// Inspector wires this so component rows can resolve asset IDs to display
	// names. The pointer is read on every GetParameters() rebuild — set on
	// construction or before showing the inspector.
	static void SetAssetRegistryForInspector(const AssetRegistry* assets);

private:
	// Editor-cached parameter rows. Built on first GetParameters() call and
	// rebuilt whenever the component list shape changes (cached by component
	// count). Euler angles are stored directly here because deriving them from
	// the quaternion every frame would cause drift on edit.
	std::vector<TechniqueParameter> m_params;
	glm::vec3                       m_eulerDeg = glm::vec3(0.0f);
	std::vector<std::string>        m_componentTextCache;  // backing storage for Text rows
	size_t                          m_paramsBuiltForComponentCount = SIZE_MAX;
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

	SkyDescription&       GetSky()       { return m_sky; }
	const SkyDescription& GetSky() const { return m_sky; }

	std::shared_ptr<Camera> GetActiveCamera() const { return m_camera; }
	void SetActiveCamera(std::shared_ptr<Camera> cam) { m_camera = std::move(cam); }

private:
	SceneNode               m_root;
	SceneLighting           m_lighting;
	SkyDescription          m_sky;
	std::shared_ptr<Camera> m_camera;
};
