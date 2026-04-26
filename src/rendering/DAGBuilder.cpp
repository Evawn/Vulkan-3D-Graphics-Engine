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

	result.executionOrder.reserve(passCount);
	result.executionOrderNodes.reserve(passCount);
	for (uint32_t nodeId : sorted) {
		size_t declIdx = nodeToDecl[nodeId];
		if (reachable[nodeId]) {
			result.executionOrder.push_back(in.declarationOrder[declIdx]);
			result.executionOrderNodes.push_back(nodeId);
		} else {
			result.declToNode[declIdx] = INVALID;
			result.prunedCount++;
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
