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

	// What queue stream the pass is asking to run on. Default Graphics; only
	// compute passes ever request AsyncCompute. The builder may demote this
	// during Build() if the device or DAG shape forbids honoring it.
	QueueAffinity queueAffinity = QueueAffinity::Graphics;
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

	// Whether the device exposes a separate compute queue family. When false,
	// every AsyncCompute affinity is silently demoted to Graphics — there is
	// no parallelism to be had on a single-queue-family device.
	bool asyncComputeAvailable = false;
};

// Cross-stream synchronization edge: a pass on stream `producerStream` writes a
// resource that a pass on stream `consumerStream` reads (or both write the same
// resource). When the streams differ, the graph emits a queue-family-ownership
// release on the producer's command buffer and an acquire on the consumer's,
// plus a semaphore signal/wait between submits.
struct CrossStreamEdge {
	uint32_t      producerNode;
	uint32_t      consumerNode;
	QueueAffinity producerStream;
	QueueAffinity consumerStream;
};

struct DAGBuildResult {
	PassDAG               dag;
	std::vector<PassRef>  executionOrder;       // pruned + topo-sorted (all streams interleaved)
	std::vector<uint32_t> executionOrderNodes;  // parallel to executionOrder; DAG node ids
	std::vector<uint32_t> declToNode;           // declOrder idx -> node id (INVALID if pruned)
	size_t                prunedCount = 0;

	// Per-pass resolved queue affinity, indexed by position in executionOrder.
	// A subset of these may have been demoted from AsyncCompute to Graphics —
	// see demotedCount + the Build() log line.
	std::vector<QueueAffinity> executionOrderAffinity;

	// Cross-stream sync edges, in producer-then-consumer topo order. Empty when
	// every pass is on the Graphics stream (the common single-queue case).
	std::vector<CrossStreamEdge> crossStreamEdges;

	// Number of AsyncCompute hints that got demoted because the pass had a
	// graphics-stream dependency, the device lacks a separate compute queue,
	// or a transitive demotion cascaded through it. Logged once by RenderGraph.
	size_t demotedCount = 0;
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
