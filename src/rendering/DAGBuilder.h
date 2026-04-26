#pragma once

#include "PassDAG.h"
#include "RenderGraphTypes.h"
#include <cstdint>
#include <vector>

// Per-pass read/write sets, projected down to plain resource ids. RenderGraph
// extracts these from the PassBuilders (it has friend access) and hands them
// to DAGBuilder so the builder doesn't need to know about pass-builder
// internals.
struct PassResourceAccess {
	std::vector<uint32_t> readImages;
	std::vector<uint32_t> readBuffers;
	std::vector<uint32_t> writeImages;
	std::vector<uint32_t> writeBuffers;
};

struct DAGBuildInputs {
	// Passes in declaration order. Length must match passAccess.
	std::vector<PassRef>            declarationOrder;
	std::vector<PassResourceAccess> passAccess;
	size_t imageCount  = 0;
	size_t bufferCount = 0;

	// Per-resource flag: when true, the resource's *last writer* counts as a
	// sink node — its output flows somewhere the engine cares about (the
	// swapchain, a persistent buffer, an explicitly-marked target). Used for
	// pruning unreachable passes. Length must match imageCount / bufferCount.
	// If both vectors are empty, pruning is disabled and every pass is kept.
	std::vector<bool> imageIsSink;
	std::vector<bool> bufferIsSink;
};

struct DAGBuildResult {
	PassDAG               dag;
	std::vector<PassRef>  executionOrder;       // pruned + topo-sorted
	std::vector<uint32_t> executionOrderNodes;  // parallel to executionOrder; DAG node ids
	std::vector<uint32_t> declToNode;           // declOrder idx -> node id (INVALID if pruned)
	size_t                prunedCount = 0;
};

// Walks declarationOrder once, emitting RAW / WAW / WAR edges between passes
// based on which resources they read and write. The resulting DAG is then
// topo-sorted (lowest-id-wins on ties), pruned against the sink set, and
// returned as executionOrder.
//
// Subsequence invariant: executionOrder is a subsequence of declarationOrder
// (i.e. pruning may drop entries but never reorder them). The builder asserts
// this internally — if it ever fires, the construction logic broke.
class DAGBuilder {
public:
	static DAGBuildResult Build(const DAGBuildInputs& in);

	// Sentinel value used in declToNode for pruned passes.
	static constexpr uint32_t INVALID_NODE = 0xFFFFFFFFu;
};
