#include "ComputePassBuilder.h"
#include "RenderGraph.h"

ComputePassBuilder::ComputePassBuilder(const std::string& name, RenderGraph& graph)
	: PassBuilderBase(name, graph) {}

ComputePassBuilder& ComputePassBuilder::Read(ImageHandle resource) {
	m_readImages.push_back(resource);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::Read(BufferHandle resource) {
	m_readBuffers.push_back(resource);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::Write(ImageHandle resource) {
	m_writeImages.push_back(resource);
	return *this;
}

ComputePassBuilder& ComputePassBuilder::Write(BufferHandle resource) {
	m_writeBuffers.push_back(resource);
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
