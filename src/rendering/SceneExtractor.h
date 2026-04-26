#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

class Scene;
class SceneNode;
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
	void Extract(const Scene& scene, const AssetRegistry& assets, RenderScene& out) const;

private:
	void Visit(const SceneNode& node, const glm::mat4& parentWorld,
	           const AssetRegistry& assets, RenderScene& out) const;
};
