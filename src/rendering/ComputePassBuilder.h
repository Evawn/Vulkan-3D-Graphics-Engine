#pragma once

#include "PassBuilderBase.h"

class ComputePassBuilder : public PassBuilderBase {
public:
	ComputePassBuilder(const std::string& name, RenderGraph& graph);

	ComputePassBuilder& Read(ImageHandle resource, ResourceUsage usage = ResourceUsage::Default);
	ComputePassBuilder& Read(BufferHandle resource, ResourceUsage usage = ResourceUsage::Default);
	ComputePassBuilder& Write(ImageHandle resource, ResourceUsage usage = ResourceUsage::Default);
	ComputePassBuilder& Write(BufferHandle resource, ResourceUsage usage = ResourceUsage::Default);
	ComputePassBuilder& SetRecord(std::function<void(PassContext&)> fn);

	// Hand the graph a factory that produces the desc for this pass's compute
	// pipeline. The graph invokes the factory during Compile() and again on
	// RecreatePipelines() so SPV is always re-read.
	ComputePassBuilder& SetPipeline(std::function<ComputePipelineDesc()> descFactory);

	// Hint that this compute pass should run on a dedicated compute queue
	// concurrently with graphics work. The graph honors this only when a
	// separate compute queue family exists AND the pass has no graphics-stream
	// dependency; otherwise it logs a one-line demotion warning and runs the
	// pass on the graphics queue. Default is QueueAffinity::Graphics.
	ComputePassBuilder& SetQueueAffinity(QueueAffinity affinity);

private:
	friend class RenderGraph;

	std::vector<ImageHandle> m_writeImages;
	std::vector<BufferHandle> m_writeBuffers;
	std::vector<ResourceUsage> m_writeImageUsages;
	std::vector<ResourceUsage> m_writeBufferUsages;

	// Pipeline ownership: the graph builds the VkPipeline during Compile.
	std::function<ComputePipelineDesc()> m_pipelineDescFactory;
	std::shared_ptr<VWrap::ComputePipeline> m_pipeline;
};
