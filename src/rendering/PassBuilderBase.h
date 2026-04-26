#pragma once

#include "RenderGraphTypes.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

class RenderGraph;
class BindingTable;

class PassBuilderBase {
public:
	PassBuilderBase(const std::string& name, RenderGraph& graph)
		: m_name(name), m_graph(graph) {}

	const std::string& GetName() const { return m_name; }
	bool IsEnabled() const { return m_enabled; }
	void SetEnabled(bool enabled) { m_enabled = enabled; }

	// Stream the pass should run on. Default Graphics; ComputePassBuilder exposes
	// the public setter that lets a technique opt into AsyncCompute. The graph
	// resolves this at Compile time (with possible demotion) and stores the final
	// per-pass affinity alongside the execution order.
	QueueAffinity GetQueueAffinity() const { return m_queueAffinity; }

	// Attach a binding table; the graph calls bindingTable->Update(graph) after
	// Compile() and after every Resize() so descriptors stay in sync with
	// graph-allocated resources.
	void SetBindings(std::shared_ptr<BindingTable> table) { m_bindings = std::move(table); }

protected:
	friend class RenderGraph;

	std::string m_name;
	bool m_enabled = true;
	QueueAffinity m_queueAffinity = QueueAffinity::Graphics;
	RenderGraph& m_graph;

	std::vector<ImageHandle> m_readImages;
	std::vector<BufferHandle> m_readBuffers;
	std::vector<ResourceUsage> m_readImageUsages;   // parallel to m_readImages
	std::vector<ResourceUsage> m_readBufferUsages;  // parallel to m_readBuffers
	std::function<void(PassContext&)> m_recordFn;
	std::shared_ptr<BindingTable> m_bindings;
};
