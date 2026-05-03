#pragma once

#include "RenderItem.h"
#include <array>
#include <cstring>
#include <glm/glm.hpp>
#include <span>
#include <vector>

// ---- RenderScene ----
//
// Frame-local materialized list of RenderItems. Owned by RenderingSystem;
// cleared at the start of every frame and re-populated either by techniques
// (today) or by a scene-graph traversal (future).
//
// Items are bucketed by RenderItemType at insertion so each pass pays only for
// the type it consumes. A pass that draws Mesh items doesn't iterate the grass
// instances; the foliage pass doesn't iterate static OBJ models.
//
// The class is intentionally trivial — it's the contract, not the algorithm.
// Sorting (depth, material), spatial culling, and per-type sub-buckets are
// future extensions that don't change the public surface.

class RenderScene {
public:
	void Add(const RenderItem& item) {
		auto idx = static_cast<size_t>(item.type);
		m_buckets[idx].push_back(item);
	}

	// Per-frame reset. Vectors keep their capacity, so steady-state cost is
	// only the placement-new of items, not allocator traffic.
	void Clear() {
		for (auto& b : m_buckets) b.clear();
		m_jointArena.clear();
	}

	std::span<const RenderItem> Get(RenderItemType type) const {
		return m_buckets[static_cast<size_t>(type)];
	}

	size_t Count(RenderItemType type) const {
		return m_buckets[static_cast<size_t>(type)].size();
	}

	size_t TotalCount() const {
		size_t n = 0;
		for (const auto& b : m_buckets) n += b.size();
		return n;
	}

	// Diagnostic helper — useful for the dev panel and debug logs.
	bool Empty() const { return TotalCount() == 0; }

	// ---- Joint matrix arena ----
	//
	// Per-frame transient storage for skinned-mesh joint matrices. The
	// SceneExtractor pushes a contiguous block per Component::SkinnedMesh,
	// and emitted RenderItem::SkinnedMesh records the (firstJoint, jointCount)
	// range. The technique uploads the entire arena to its per-frame SSBO once
	// per frame and the shader indexes via `joints[firstJoint + boneIdx]`.
	//
	// Cleared by Clear() each frame. The vector's capacity is retained so the
	// steady state is one or two memcpys plus zero allocator traffic.

	uint32_t AddJointMatrices(const glm::mat4* mats, uint32_t count) {
		const uint32_t firstIndex = static_cast<uint32_t>(m_jointArena.size());
		m_jointArena.resize(m_jointArena.size() + count);
		std::memcpy(m_jointArena.data() + firstIndex, mats, count * sizeof(glm::mat4));
		return firstIndex;
	}

	std::span<const glm::mat4> GetJoints(uint32_t firstJoint, uint32_t count) const {
		if (firstJoint + count > m_jointArena.size()) return {};
		return std::span<const glm::mat4>(m_jointArena.data() + firstJoint, count);
	}

	// Whole-arena access for techniques that upload the entire arena to GPU
	// in one shot (firstJoint becomes the per-draw offset).
	std::span<const glm::mat4> GetAllJoints() const {
		return std::span<const glm::mat4>(m_jointArena.data(), m_jointArena.size());
	}

	size_t JointArenaSize() const { return m_jointArena.size(); }

private:
	std::array<std::vector<RenderItem>, kRenderItemTypeCount> m_buckets;
	std::vector<glm::mat4> m_jointArena;
};
