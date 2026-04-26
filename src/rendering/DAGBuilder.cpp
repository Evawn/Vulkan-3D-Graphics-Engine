#include "DAGBuilder.h"
#include <cassert>
#include <limits>

namespace {
constexpr uint32_t INVALID = std::numeric_limits<uint32_t>::max();
static_assert(INVALID == DAGBuilder::INVALID_NODE, "INVALID sentinel divergence");
}

DAGBuildResult DAGBuilder::Build(const DAGBuildInputs& in) {
	DAGBuildResult result;
	const size_t passCount = in.declarationOrder.size();
	assert(in.passAccess.size() == passCount &&
		"DAGBuilder: declarationOrder and passAccess must be the same length");
	assert((in.imageIsSink.empty()  || in.imageIsSink.size()  == in.imageCount)  &&
		"DAGBuilder: imageIsSink must be empty or match imageCount");
	assert((in.bufferIsSink.empty() || in.bufferIsSink.size() == in.bufferCount) &&
		"DAGBuilder: bufferIsSink must be empty or match bufferCount");

	// 1. One DAG node per pass, in declaration order.
	result.declToNode.resize(passCount);
	for (size_t i = 0; i < passCount; i++) {
		result.declToNode[i] = result.dag.AddNode();
	}

	// 2. Walk passes in declaration order, emitting edges:
	//      RAW: reader depends on the most recent writer of R
	//      WAW: writer depends on the previous writer of R
	//      WAR: writer depends on every reader of R since the previous writer
	std::vector<uint32_t> lastImageWriter (in.imageCount,  INVALID);
	std::vector<uint32_t> lastBufferWriter(in.bufferCount, INVALID);
	std::vector<std::vector<uint32_t>> imageReaders (in.imageCount);
	std::vector<std::vector<uint32_t>> bufferReaders(in.bufferCount);

	for (size_t i = 0; i < passCount; i++) {
		const PassResourceAccess& access = in.passAccess[i];
		const uint32_t node = result.declToNode[i];

		// RAW edges (reads)
		for (uint32_t r : access.readImages) {
			if (r >= in.imageCount) continue;
			if (lastImageWriter[r] != INVALID) {
				result.dag.AddEdge(lastImageWriter[r], node);
			}
		}
		for (uint32_t r : access.readBuffers) {
			if (r >= in.bufferCount) continue;
			if (lastBufferWriter[r] != INVALID) {
				result.dag.AddEdge(lastBufferWriter[r], node);
			}
		}

		// WAW + WAR edges (writes)
		for (uint32_t w : access.writeImages) {
			if (w >= in.imageCount) continue;
			if (lastImageWriter[w] != INVALID) {
				result.dag.AddEdge(lastImageWriter[w], node);
			}
			for (uint32_t reader : imageReaders[w]) {
				result.dag.AddEdge(reader, node);
			}
		}
		for (uint32_t w : access.writeBuffers) {
			if (w >= in.bufferCount) continue;
			if (lastBufferWriter[w] != INVALID) {
				result.dag.AddEdge(lastBufferWriter[w], node);
			}
			for (uint32_t reader : bufferReaders[w]) {
				result.dag.AddEdge(reader, node);
			}
		}

		// Update bookkeeping AFTER edge emission so a pass that reads and
		// writes the same resource doesn't form a self-edge.
		for (uint32_t r : access.readImages) {
			if (r < in.imageCount) imageReaders[r].push_back(node);
		}
		for (uint32_t r : access.readBuffers) {
			if (r < in.bufferCount) bufferReaders[r].push_back(node);
		}
		for (uint32_t w : access.writeImages) {
			if (w >= in.imageCount) continue;
			lastImageWriter[w] = node;
			imageReaders[w].clear();
		}
		for (uint32_t w : access.writeBuffers) {
			if (w >= in.bufferCount) continue;
			lastBufferWriter[w] = node;
			bufferReaders[w].clear();
		}
	}

	// 3. Identify sink nodes — last writers of any resource flagged as a sink.
	//    If no sink flags were supplied, treat every node as reachable (no pruning).
	std::vector<bool> reachable;
	const bool pruningEnabled = !in.imageIsSink.empty() || !in.bufferIsSink.empty();
	if (pruningEnabled) {
		std::vector<uint32_t> sinkNodes;
		for (size_t i = 0; i < in.imageCount && i < in.imageIsSink.size(); i++) {
			if (in.imageIsSink[i] && lastImageWriter[i] != INVALID) {
				sinkNodes.push_back(lastImageWriter[i]);
			}
		}
		for (size_t i = 0; i < in.bufferCount && i < in.bufferIsSink.size(); i++) {
			if (in.bufferIsSink[i] && lastBufferWriter[i] != INVALID) {
				sinkNodes.push_back(lastBufferWriter[i]);
			}
		}
		reachable = result.dag.Reachable(sinkNodes);
	} else {
		reachable.assign(passCount, true);
	}

	// 4. Topo-sort, then pick out nodes that are reachable. Pruning preserves
	//    declaration-relative order because we walk topo-sort in order; in
	//    Phase 1 the sort matches declaration order so the output is a
	//    declaration-order subsequence.
	auto sorted = result.dag.TopoSort();
	assert(sorted.size() == passCount &&
		"PassDAG cycle — DAGBuilder constructed bad edges (logic bug, not user input)");

	std::vector<size_t> nodeToDecl(passCount);
	for (size_t i = 0; i < passCount; i++) {
		nodeToDecl[result.declToNode[i]] = i;
	}

	// 4a. Resolve queue affinities, by node id. Start from the user's request,
	//     then iteratively demote any AsyncCompute node whose dependencies
	//     include a Graphics-stream node, until the assignment is stable.
	//     Demotion cascades naturally because once a node demotes to Graphics,
	//     its dependents see a Graphics dependency on the next pass.
	std::vector<QueueAffinity> nodeAffinity(passCount, QueueAffinity::Graphics);
	for (size_t i = 0; i < passCount; i++) {
		uint32_t node = result.declToNode[i];
		if (in.passAccess[i].queueAffinity == QueueAffinity::AsyncCompute &&
			in.asyncComputeAvailable && reachable[node]) {
			nodeAffinity[node] = QueueAffinity::AsyncCompute;
		}
	}
	// Walk in topo order so demotions propagate forward in one pass.
	for (uint32_t nodeId : sorted) {
		if (nodeAffinity[nodeId] != QueueAffinity::AsyncCompute) continue;
		for (uint32_t dep : result.dag.Dependencies(nodeId)) {
			if (nodeAffinity[dep] == QueueAffinity::Graphics) {
				nodeAffinity[nodeId] = QueueAffinity::Graphics;
				break;
			}
		}
	}
	// Count demotions: requested AsyncCompute but resolved to Graphics.
	for (size_t i = 0; i < passCount; i++) {
		uint32_t node = result.declToNode[i];
		if (in.passAccess[i].queueAffinity == QueueAffinity::AsyncCompute &&
			nodeAffinity[node] != QueueAffinity::AsyncCompute) {
			result.demotedCount++;
		}
	}

	// 4b. Build the (pruned, sorted) execution order and parallel affinity vector.
	result.executionOrder.reserve(passCount);
	result.executionOrderNodes.reserve(passCount);
	result.executionOrderAffinity.reserve(passCount);
	for (uint32_t nodeId : sorted) {
		size_t declIdx = nodeToDecl[nodeId];
		if (reachable[nodeId]) {
			result.executionOrder.push_back(in.declarationOrder[declIdx]);
			result.executionOrderNodes.push_back(nodeId);
			result.executionOrderAffinity.push_back(nodeAffinity[nodeId]);
		} else {
			result.declToNode[declIdx] = INVALID;
			result.prunedCount++;
		}
	}

	// 4c. Cross-stream edges: every dependency that crosses queue affinities
	//     becomes a sync edge. The graph turns these into queue-family ownership
	//     transfer barriers + a semaphore signal/wait pair.
	for (uint32_t consumer : sorted) {
		if (!reachable[consumer]) continue;
		QueueAffinity consumerStream = nodeAffinity[consumer];
		for (uint32_t producer : result.dag.Dependencies(consumer)) {
			if (!reachable[producer]) continue;
			QueueAffinity producerStream = nodeAffinity[producer];
			if (producerStream != consumerStream) {
				result.crossStreamEdges.push_back({
					producer, consumer, producerStream, consumerStream
				});
			}
		}
	}

	// 5. Subsequence invariant: kept passes must appear in their original
	//    relative order. (Stronger statement: executionOrder equals
	//    declarationOrder with pruned entries removed.)
#ifndef NDEBUG
	{
		size_t declIdx = 0;
		for (size_t i = 0; i < result.executionOrder.size(); i++) {
			while (declIdx < passCount &&
				!(in.declarationOrder[declIdx].type  == result.executionOrder[i].type &&
				  in.declarationOrder[declIdx].index == result.executionOrder[i].index)) {
				declIdx++;
			}
			assert(declIdx < passCount &&
				"executionOrder contains a pass not in declarationOrder, or order was rearranged");
			declIdx++;
		}
	}
#endif

	return result;
}
