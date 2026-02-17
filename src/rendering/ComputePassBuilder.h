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

private:
	friend class RenderGraph;

	std::vector<ImageHandle> m_writeImages;
	std::vector<BufferHandle> m_writeBuffers;
};
