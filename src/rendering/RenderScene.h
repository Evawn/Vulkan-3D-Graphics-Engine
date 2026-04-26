#pragma once

#include "RenderItem.h"
#include <array>
#include <vector>
#include <span>

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

private:
	std::array<std::vector<RenderItem>, kRenderItemTypeCount> m_buckets;
};
