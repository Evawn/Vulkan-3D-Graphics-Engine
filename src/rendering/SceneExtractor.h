#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include "MeshIR.h"

#include <vector>

class Scene;
class SceneNode;
struct Component;
class AssetRegistry;
class RenderScene;

// ---- SceneExtractor ----
//
// Per-frame producer: walks the Scene tree, resolves components against the
// AssetRegistry, and drops RenderItems into a RenderScene. The extractor is
// the *only* code path that fills the RenderScene — techniques never write to
// it, only read from it.
//
// v1 component mapping:
//   - ComponentType::Mesh         → RenderItemType::Mesh
//   - ComponentType::VoxelVolume  → RenderItemType::BrickmapVolume
//   - ComponentType::SkinnedMesh  → N × RenderItemType::SkinnedMesh + joint
//                                    matrices appended to RenderScene's arena
//
// The class is intentionally trivial — it's the seam, not an algorithm. Future
// work that lands here without changing the public surface:
//   - Frustum / occlusion culling (filter step before per-component emit)
//   - LOD selection (component fields choose which asset variant to emit)
//   - Incremental extraction (walk only dirty subtrees)
//   - Sorting / batching (sort emitted items by material, depth, etc.)

class SceneExtractor {
public:
	// Walk the scene tree, fill `out`. `out` is cleared by the caller (today,
	// RenderingSystem::DrawFrame); the extractor only adds.
	//
	// `dt` is forwarded to skinned-mesh playback so Component::currentTime
	// advances at playbackSpeed each frame. Pass 0.0f to leave time untouched
	// (useful when Application has its own scrub clock).
	void Extract(const Scene& scene, const AssetRegistry& assets, RenderScene& out, float dt);

	// Legacy entry point kept for callers that don't yet pass dt (no skinned
	// playback advancement; useful in tests).
	void Extract(const Scene& scene, const AssetRegistry& assets, RenderScene& out) {
		Extract(scene, assets, out, 0.0f);
	}

private:
	void Visit(SceneNode& node, const glm::mat4& parentWorld,
	           const AssetRegistry& assets, RenderScene& out, float dt);
	void EmitSkinnedMesh(Component& comp, const glm::mat4& world,
	                     const AssetRegistry& assets, RenderScene& out, float dt);

	// Reusable per-frame scratch — kept as members so the per-component cost
	// stays at zero allocations after warm-up. The TRS scratch arrays are
	// memcpy'd from the asset's rest pose each frame (cheap; flat POD), then
	// the active clip overwrites the channels it touches.
	std::vector<glm::vec3> m_scratchTrans;
	std::vector<glm::quat> m_scratchRot;
	std::vector<glm::vec3> m_scratchScale;
	std::vector<glm::mat4> m_scratchWorlds;
	std::vector<glm::mat4> m_scratchJoints;
};
