#include "PassDAG.h"
#include <algorithm>
#include <queue>

uint32_t PassDAG::AddNode() {
	uint32_t id = static_cast<uint32_t>(m_in.size());
	m_in.emplace_back();
	m_out.emplace_back();
	return id;
}

void PassDAG::InsertSorted(std::vector<uint32_t>& v, uint32_t x) {
	auto it = std::lower_bound(v.begin(), v.end(), x);
	if (it == v.end() || *it != x) v.insert(it, x);
}

void PassDAG::AddEdge(uint32_t from, uint32_t to) {
	if (from == to) return;
	InsertSorted(m_out[from], to);
	InsertSorted(m_in[to], from);
}

void PassDAG::Clear() {
	m_in.clear();
	m_out.clear();
}

std::vector<bool> PassDAG::Reachable(const std::vector<uint32_t>& sinks) const {
	std::vector<bool> reachable(m_in.size(), false);
	std::vector<uint32_t> stack;
	stack.reserve(m_in.size());
	for (uint32_t s : sinks) {
		if (s < m_in.size() && !reachable[s]) {
			reachable[s] = true;
			stack.push_back(s);
		}
	}
	while (!stack.empty()) {
		uint32_t u = stack.back();
		stack.pop_back();
		for (uint32_t v : m_in[u]) {
			if (!reachable[v]) {
				reachable[v] = true;
				stack.push_back(v);
			}
		}
	}
	return reachable;
}

std::vector<uint32_t> PassDAG::TopoSort() const {
	const size_t N = m_in.size();
	std::vector<uint32_t> inDegree(N);
	for (size_t i = 0; i < N; i++) {
		inDegree[i] = static_cast<uint32_t>(m_in[i].size());
	}

	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
	for (uint32_t i = 0; i < N; i++) {
		if (inDegree[i] == 0) ready.push(i);
	}

	std::vector<uint32_t> result;
	result.reserve(N);
	while (!ready.empty()) {
		uint32_t u = ready.top();
		ready.pop();
		result.push_back(u);
		for (uint32_t v : m_out[u]) {
			if (--inDegree[v] == 0) ready.push(v);
		}
	}

	if (result.size() != N) return {};
	return result;
}
