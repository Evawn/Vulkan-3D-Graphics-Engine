#include "GraphicsPassBuilder.h"
#include "RenderGraph.h"

GraphicsPassBuilder::GraphicsPassBuilder(const std::string& name, RenderGraph& graph)
	: PassBuilderBase(name, graph) {}

GraphicsPassBuilder& GraphicsPassBuilder::SetColorAttachment(
	ImageHandle target, LoadOp load, StoreOp store,
	float r, float g, float b, float a)
{
	m_colorAttachments.clear();
	m_colorAttachments.push_back({ target, load, store, { { r, g, b, a } } });
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::AddColorAttachment(
	ImageHandle target, LoadOp load, StoreOp store,
	float r, float g, float b, float a)
{
	m_colorAttachments.push_back({ target, load, store, { { r, g, b, a } } });
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetDepthAttachment(
	ImageHandle target, LoadOp load, StoreOp store,
	float depth, uint32_t stencil)
{
	m_depthTarget = target;
	m_hasDepth = true;
	m_depthLoad = load;
	m_depthStore = store;
	m_clearDepthStencil = { depth, stencil };
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetResolveTarget(ImageHandle target) {
	m_resolveTarget = target;
	m_hasResolve = true;
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::Read(ImageHandle resource, ResourceUsage usage) {
	m_readImages.push_back(resource);
	m_readImageUsages.push_back(usage);
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::Read(BufferHandle resource, ResourceUsage usage) {
	m_readBuffers.push_back(resource);
	m_readBufferUsages.push_back(usage);
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::Write(BufferHandle resource, ResourceUsage usage) {
	m_writeBuffers.push_back(resource);
	m_writeBufferUsages.push_back(usage);
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetRecord(std::function<void(PassContext&)> fn) {
	m_recordFn = std::move(fn);
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetPipeline(std::function<GraphicsPipelineDesc()> descFactory) {
	m_pipelineDescFactory = std::move(descFactory);
	return *this;
}

static VkAttachmentLoadOp ToVk(LoadOp op) {
	switch (op) {
	case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
	case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	}
	return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

static VkAttachmentStoreOp ToVk(StoreOp op) {
	switch (op) {
	case StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
	case StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
	}
	return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

void GraphicsPassBuilder::CreateRenderPass(
	const std::vector<VkImageLayout>& colorFinalLayouts,
	VkImageLayout resolveFinalLayout)
{
	auto device = m_graph.GetDevice();

	std::vector<VkAttachmentDescription> attachments;
	std::vector<VkAttachmentReference> colorRefs;
	VkAttachmentReference depthRef{};
	bool hasDepthRef = false;

	// N color attachments
	for (size_t c = 0; c < m_colorAttachments.size(); c++) {
		const auto& ca = m_colorAttachments[c];
		const auto& colorRes = m_graph.GetImageResource(ca.target);

		VkAttachmentDescription colorAttach{};
		colorAttach.format = colorRes.desc.format;
		colorAttach.samples = colorRes.desc.samples;
		colorAttach.loadOp = ToVk(ca.load);
		colorAttach.storeOp = ToVk(ca.store);
		colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttach.finalLayout = colorFinalLayouts[c];

		uint32_t attachIdx = static_cast<uint32_t>(attachments.size());
		attachments.push_back(colorAttach);

		VkAttachmentReference ref{};
		ref.attachment = attachIdx;
		ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorRefs.push_back(ref);
	}

	// Depth attachment (optional)
	if (m_hasDepth) {
		const auto& depthRes = m_graph.GetImageResource(m_depthTarget);
		VkAttachmentDescription depthAttach{};
		depthAttach.format = depthRes.desc.format;
		depthAttach.samples = depthRes.desc.samples;
		depthAttach.loadOp = ToVk(m_depthLoad);
		depthAttach.storeOp = ToVk(m_depthStore);
		depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		depthRef.attachment = static_cast<uint32_t>(attachments.size());
		depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments.push_back(depthAttach);
		hasDepthRef = true;
	}

	// Resolve attachment — resolves the first color attachment only.
	// Vulkan requires pResolveAttachments to have colorAttachmentCount entries if non-null.
	std::vector<VkAttachmentReference> resolveRefs;
	if (m_hasResolve) {
		const auto& resolveRes = m_graph.GetImageResource(m_resolveTarget);
		VkAttachmentDescription resolveAttach{};
		resolveAttach.format = resolveRes.desc.format;
		resolveAttach.samples = VK_SAMPLE_COUNT_1_BIT;
		resolveAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		resolveAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		resolveAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		resolveAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		resolveAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		resolveAttach.finalLayout = resolveFinalLayout;

		uint32_t resolveAttachIdx = static_cast<uint32_t>(attachments.size());
		attachments.push_back(resolveAttach);

		// Build resolve refs: first color attachment resolves, rest are VK_ATTACHMENT_UNUSED
		resolveRefs.resize(colorRefs.size(), { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED });
		resolveRefs[0].attachment = resolveAttachIdx;
		resolveRefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
	subpass.pColorAttachments = colorRefs.data();
	subpass.pDepthStencilAttachment = hasDepthRef ? &depthRef : nullptr;
	subpass.pResolveAttachments = m_hasResolve ? resolveRefs.data() : nullptr;

	std::vector<VkSubpassDependency> dependencies;

	// External -> subpass 0
	VkSubpassDependency startDep{};
	startDep.srcSubpass = VK_SUBPASS_EXTERNAL;
	startDep.dstSubpass = 0;
	startDep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	startDep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	startDep.srcAccessMask = 0;
	startDep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	if (m_hasDepth) {
		startDep.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		startDep.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		startDep.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	dependencies.push_back(startDep);

	// Subpass 0 -> external (if any color/resolve is read downstream as sampled)
	bool needsEndDep = (resolveFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (!needsEndDep) {
		for (const auto& layout : colorFinalLayouts) {
			if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
				needsEndDep = true;
				break;
			}
		}
	}
	if (needsEndDep) {
		VkSubpassDependency endDep{};
		endDep.srcSubpass = 0;
		endDep.dstSubpass = VK_SUBPASS_EXTERNAL;
		endDep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		endDep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		endDep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		endDep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies.push_back(endDep);
	}

	VkRenderPassCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	createInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	createInfo.pAttachments = attachments.data();
	createInfo.subpassCount = 1;
	createInfo.pSubpasses = &subpass;
	createInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
	createInfo.pDependencies = dependencies.data();

	VkSampleCountFlagBits samples = m_colorAttachments.empty()
		? VK_SAMPLE_COUNT_1_BIT
		: m_graph.GetImageResource(m_colorAttachments[0].target).desc.samples;
	m_renderPass = VWrap::RenderPass::Create(device, createInfo, samples);
}

void GraphicsPassBuilder::CreateFramebuffer() {
	auto device = m_graph.GetDevice();
	const auto& firstColorRes = m_graph.GetImageResource(m_colorAttachments[0].target);

	std::vector<std::shared_ptr<VWrap::ImageView>> views;

	// All color attachment views
	for (const auto& ca : m_colorAttachments) {
		const auto& colorRes = m_graph.GetImageResource(ca.target);
		views.push_back(colorRes.view);
	}

	if (m_hasDepth) {
		const auto& depthRes = m_graph.GetImageResource(m_depthTarget);
		views.push_back(depthRes.view);
	}

	if (m_hasResolve) {
		const auto& resolveRes = m_graph.GetImageResource(m_resolveTarget);
		views.push_back(resolveRes.view);
	}

	VkExtent2D extent = { firstColorRes.desc.width, firstColorRes.desc.height };
	auto fb = VWrap::Framebuffer::Create2D(device, m_renderPass, views, extent);
	m_framebufferCache[firstColorRes.view->Get()] = fb;
	m_activeFramebuffer = fb;
}
