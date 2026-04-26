#include "ComputePassBuilder.h"
#include "RenderGraph.h"

ComputePassBuilder::ComputePassBuilder(const std::string& name, RenderGraph& graph)
	: PassBuilderBase(name, graph) {}

ComputePassBuilder& ComputePassBuilder::Read(ImageHandle resource, ResourceUsage usage) {
	m_readImages.push_back(resource);
	m_readImageUsages.push_back(usage);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::Read(BufferHandle resource, ResourceUsage usage) {
	m_readBuffers.push_back(resource);
	m_readBufferUsages.push_back(usage);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::Write(ImageHandle resource, ResourceUsage usage) {
	m_writeImages.push_back(resource);
	m_writeImageUsages.push_back(usage);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::Write(BufferHandle resource, ResourceUsage usage) {
	m_writeBuffers.push_back(resource);
	m_writeBufferUsages.push_back(usage);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::SetRecord(std::function<void(PassContext&)> fn) {
	m_recordFn = std::move(fn);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::SetPipeline(std::function<ComputePipelineDesc()> descFactory) {
	m_pipelineDescFactory = std::move(descFactory);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::SetQueueAffinity(QueueAffinity affinity) {
	m_queueAffinity = affinity;
	return *this;
}
