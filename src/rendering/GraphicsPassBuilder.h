#pragma once

#include "PassBuilderBase.h"
#include "RenderPass.h"
#include "Framebuffer.h"
#include <unordered_map>

class GraphicsPassBuilder : public PassBuilderBase {
public:
	GraphicsPassBuilder(const std::string& name, RenderGraph& graph);

	GraphicsPassBuilder& SetColorAttachment(
		ImageHandle target, LoadOp load, StoreOp store,
		float r = 0, float g = 0, float b = 0, float a = 1);
	GraphicsPassBuilder& SetDepthAttachment(
		ImageHandle target, LoadOp load, StoreOp store,
		float depth = 1.0f, uint32_t stencil = 0);
	GraphicsPassBuilder& SetResolveTarget(ImageHandle target);
	GraphicsPassBuilder& Read(ImageHandle resource);
	GraphicsPassBuilder& Read(BufferHandle resource);
	GraphicsPassBuilder& SetRecord(std::function<void(PassContext&)> fn);

	VkRenderPass GetRenderPass();
	std::shared_ptr<VWrap::RenderPass> GetRenderPassPtr();

private:
	friend class RenderGraph;

	// Color attachment
	ImageHandle m_colorTarget;
	LoadOp m_colorLoad = LoadOp::DontCare;
	StoreOp m_colorStore = StoreOp::DontCare;
	VkClearColorValue m_clearColor{};

	// Depth attachment
	ImageHandle m_depthTarget;
	bool m_hasDepth = false;
	LoadOp m_depthLoad = LoadOp::DontCare;
	StoreOp m_depthStore = StoreOp::DontCare;
	VkClearDepthStencilValue m_clearDepthStencil{1.0f, 0};

	// Resolve target
	ImageHandle m_resolveTarget;
	bool m_hasResolve = false;

	// Created lazily by GetRenderPass / during Compile
	std::shared_ptr<VWrap::RenderPass> m_renderPass;
	std::unordered_map<VkImageView, std::shared_ptr<VWrap::Framebuffer>> m_framebufferCache;
	std::shared_ptr<VWrap::Framebuffer> m_activeFramebuffer;

	void CreateRenderPass(VkImageLayout colorFinalLayout, VkImageLayout resolveFinalLayout);
	void CreateFramebuffer();
};
