#pragma once

#include <cstdint>
#include <vector>

// Generic DAG over integer node ids. Knows nothing about Vulkan, render
// graphs, or resources — fed by DAGBuilder, queried by RenderGraph.
//
// AddEdge() dedups, so callers (e.g. DAGBuilder, which can re-emit the same
// "this writer feeds that reader" edge for every read of a binding) don't
// have to track duplicates themselves.
//
// TopoSort() uses Kahn's algorithm with a min-heap on node ids: when several
// nodes are simultaneously ready, the one with the lowest id wins. That
// stable ordering is what gives Phase-1 declaration-order parity — DAGBuilder
// allocates node ids in declaration order, so the topo sort reproduces it
// when there are no semantic reasons to reorder.

class PassDAG {
public:
	uint32_t AddNode();
	void     AddEdge(uint32_t from, uint32_t to);

	size_t   NodeCount() const { return m_in.size(); }

	const std::vector<uint32_t>& Dependencies(uint32_t id) const { return m_in[id]; }
	const std::vector<uint32_t>& Dependents  (uint32_t id) const { return m_out[id]; }

	// Returns nodes in topological order, with the lowest-id ready node visited
	// first when there's a tie. Returns the empty vector if the graph has a
	// cycle (caller should assert — a cycle here means DAGBuilder built bad
	// edges, which is a logic error, not a runtime input).
	std::vector<uint32_t> TopoSort() const;

	// Reverse reachability from a set of sink nodes. Walks in-edges (upstream)
	// from each sink, marking every visited node. Returns a per-node bitmask
	// of size NodeCount(). Used by DAGBuilder to identify passes whose output
	// flows into something that matters (the swapchain, a persistent resource,
	// an explicit sink); unreachable nodes are pruned from the execution order.
	std::vector<bool> Reachable(const std::vector<uint32_t>& sinks) const;

	void Clear();

private:
	std::vector<std::vector<uint32_t>> m_in;
	std::vector<std::vector<uint32_t>> m_out;

	static void InsertSorted(std::vector<uint32_t>& v, uint32_t x);
};
