#pragma once

#include "PassBuilderBase.h"
#include "RenderPass.h"
#include "Framebuffer.h"
#include <unordered_map>

struct ColorAttachmentInfo {
	ImageHandle target;
	LoadOp load = LoadOp::DontCare;
	StoreOp store = StoreOp::DontCare;
	VkClearColorValue clearColor{};
};

class GraphicsPassBuilder : public PassBuilderBase {
public:
	GraphicsPassBuilder(const std::string& name, RenderGraph& graph);

	// Color attachments — SetColorAttachment replaces all with a single one (backwards compat),
	// AddColorAttachment appends (for MRT).
	GraphicsPassBuilder& SetColorAttachment(
		ImageHandle target, LoadOp load, StoreOp store,
		float r = 0, float g = 0, float b = 0, float a = 1);
	GraphicsPassBuilder& AddColorAttachment(
		ImageHandle target, LoadOp load, StoreOp store,
		float r = 0, float g = 0, float b = 0, float a = 1);
	GraphicsPassBuilder& SetDepthAttachment(
		ImageHandle target, LoadOp load, StoreOp store,
		float depth = 1.0f, uint32_t stencil = 0);
	GraphicsPassBuilder& SetResolveTarget(ImageHandle target);
	GraphicsPassBuilder& Read(ImageHandle resource, ResourceUsage usage = ResourceUsage::Default);
	GraphicsPassBuilder& Read(BufferHandle resource, ResourceUsage usage = ResourceUsage::Default);
	GraphicsPassBuilder& Write(BufferHandle resource, ResourceUsage usage = ResourceUsage::Default);
	GraphicsPassBuilder& SetRecord(std::function<void(PassContext&)> fn);

	// Hand the graph a factory that produces the desc for this pass's pipeline.
	// The graph invokes the factory after Compile() (and again on hot-reload /
	// RecreatePipelines()), so closures may capture mutable technique state
	// like wireframe flags — each rebuild observes the latest values.
	GraphicsPassBuilder& SetPipeline(std::function<GraphicsPipelineDesc()> descFactory);

private:
	friend class RenderGraph;

	// Color attachments (MRT)
	std::vector<ColorAttachmentInfo> m_colorAttachments;

	// Depth attachment
	ImageHandle m_depthTarget;
	bool m_hasDepth = false;
	LoadOp m_depthLoad = LoadOp::DontCare;
	StoreOp m_depthStore = StoreOp::DontCare;
	VkClearDepthStencilValue m_clearDepthStencil{1.0f, 0};

	// Buffer writes
	std::vector<BufferHandle> m_writeBuffers;
	std::vector<ResourceUsage> m_writeBufferUsages;  // parallel to m_writeBuffers

	// Resolve target
	ImageHandle m_resolveTarget;
	bool m_hasResolve = false;

	// Created during Compile against the canonical attachment layouts.
	std::shared_ptr<VWrap::RenderPass> m_renderPass;
	std::unordered_map<VkImageView, std::shared_ptr<VWrap::Framebuffer>> m_framebufferCache;
	std::shared_ptr<VWrap::Framebuffer> m_activeFramebuffer;

	// Pipeline ownership: the graph builds the VkPipeline after CreateRenderPasses.
	// The factory is invoked each time the pipeline is built or rebuilt.
	std::function<GraphicsPipelineDesc()> m_pipelineDescFactory;
	std::shared_ptr<VWrap::Pipeline> m_pipeline;

	void CreateRenderPass(const std::vector<VkImageLayout>& colorFinalLayouts, VkImageLayout resolveFinalLayout);
	void CreateFramebuffer();
};
