#pragma once

#include "RenderGraphTypes.h"
#include <string>
#include <vector>
#include <functional>

class RenderGraph;

class PassBuilderBase {
public:
	PassBuilderBase(const std::string& name, RenderGraph& graph)
		: m_name(name), m_graph(graph) {}

	const std::string& GetName() const { return m_name; }
	bool IsEnabled() const { return m_enabled; }
	void SetEnabled(bool enabled) { m_enabled = enabled; }

protected:
	friend class RenderGraph;

	std::string m_name;
	bool m_enabled = true;
	RenderGraph& m_graph;

	std::vector<ImageHandle> m_readImages;
	std::vector<BufferHandle> m_readBuffers;
	std::function<void(PassContext&)> m_recordFn;
};
