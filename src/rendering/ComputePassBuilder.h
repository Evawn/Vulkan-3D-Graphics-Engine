#pragma once

#include "PassBuilderBase.h"

class ComputePassBuilder : public PassBuilderBase {
public:
	ComputePassBuilder(const std::string& name, RenderGraph& graph);

	ComputePassBuilder& Read(ImageHandle resource);
	ComputePassBuilder& Read(BufferHandle resource);
	ComputePassBuilder& Write(ImageHandle resource);
	ComputePassBuilder& Write(BufferHandle resource);
	ComputePassBuilder& SetRecord(std::function<void(PassContext&)> fn);

	// Hand the graph a factory that produces the desc for this pass's compute
	// pipeline. The graph invokes the factory during Compile() and again on
	// RecreatePipelines() so SPV is always re-read.
	ComputePassBuilder& SetPipeline(std::function<ComputePipelineDesc()> descFactory);

private:
	friend class RenderGraph;

	std::vector<ImageHandle> m_writeImages;
	std::vector<BufferHandle> m_writeBuffers;

	// Pipeline ownership: the graph builds the VkPipeline during Compile.
	std::function<ComputePipelineDesc()> m_pipelineDescFactory;
	std::shared_ptr<VWrap::ComputePipeline> m_pipeline;
};
