#include "RenderGraph.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cassert>

// =====================================================================
// GraphicsPassBuilder
// =====================================================================

GraphicsPassBuilder::GraphicsPassBuilder(const std::string& name, RenderGraph& graph)
	: m_name(name), m_graph(graph) {}

GraphicsPassBuilder& GraphicsPassBuilder::SetColorAttachment(
	ImageHandle target, LoadOp load, StoreOp store,
	float r, float g, float b, float a)
{
	m_colorTarget = target;
	m_colorLoad = load;
	m_colorStore = store;
	m_clearColor = { { r, g, b, a } };
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

GraphicsPassBuilder& GraphicsPassBuilder::Read(ImageHandle resource) {
	m_readImages.push_back(resource);
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::Read(BufferHandle resource) {
	m_readBuffers.push_back(resource);
	return *this;
}

GraphicsPassBuilder& GraphicsPassBuilder::SetRecord(std::function<void(PassContext&)> fn) {
	m_recordFn = std::move(fn);
	return *this;
}

VkRenderPass GraphicsPassBuilder::GetRenderPass() {
	if (!m_renderPass) {
		// Use COLOR_ATTACHMENT_OPTIMAL and SHADER_READ_ONLY_OPTIMAL as defaults.
		// These will be overridden during Compile() if needed. For pipeline creation,
		// the render pass just needs compatible format/samples/subpass config.
		VkImageLayout colorFinal = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkImageLayout resolveFinal = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		// Check if color target is imported — use external layout as final
		const auto& colorRes = m_graph.GetImageResource(m_colorTarget);
		if (colorRes.imported) {
			colorFinal = colorRes.externalLayout;
		}

		CreateRenderPass(colorFinal, resolveFinal);
	}
	return m_renderPass->Get();
}

std::shared_ptr<VWrap::RenderPass> GraphicsPassBuilder::GetRenderPassPtr() {
	GetRenderPass(); // ensure created
	return m_renderPass;
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

void GraphicsPassBuilder::CreateRenderPass(VkImageLayout colorFinalLayout, VkImageLayout resolveFinalLayout) {
	auto device = m_graph.GetDevice();
	const auto& colorRes = m_graph.GetImageResource(m_colorTarget);

	std::vector<VkAttachmentDescription> attachments;
	std::vector<VkAttachmentReference> colorRefs;
	VkAttachmentReference depthRef{};
	bool hasDepthRef = false;
	VkAttachmentReference resolveRef{};
	bool hasResolveRef = false;

	// Attachment 0: Color
	VkAttachmentDescription colorAttach{};
	colorAttach.format = colorRes.desc.format;
	colorAttach.samples = colorRes.desc.samples;
	colorAttach.loadOp = ToVk(m_colorLoad);
	colorAttach.storeOp = ToVk(m_colorStore);
	colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttach.finalLayout = colorFinalLayout;
	attachments.push_back(colorAttach);

	VkAttachmentReference colorRef{};
	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorRefs.push_back(colorRef);

	uint32_t nextAttachment = 1;

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
		attachments.push_back(depthAttach);

		depthRef.attachment = nextAttachment++;
		depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		hasDepthRef = true;
	}

	// Resolve attachment (optional, auto-added when MSAA)
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
		attachments.push_back(resolveAttach);

		resolveRef.attachment = nextAttachment++;
		resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		hasResolveRef = true;
	}

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
	subpass.pColorAttachments = colorRefs.data();
	subpass.pDepthStencilAttachment = hasDepthRef ? &depthRef : nullptr;
	subpass.pResolveAttachments = hasResolveRef ? &resolveRef : nullptr;

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

	// Subpass 0 -> external (if the color/resolve is read downstream as sampled)
	if (resolveFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
	    colorFinalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
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

	m_renderPass = VWrap::RenderPass::Create(device, createInfo, colorRes.desc.samples);
}

void GraphicsPassBuilder::CreateFramebuffer() {
	auto device = m_graph.GetDevice();
	const auto& colorRes = m_graph.GetImageResource(m_colorTarget);

	std::vector<std::shared_ptr<VWrap::ImageView>> views;
	views.push_back(colorRes.view);

	if (m_hasDepth) {
		const auto& depthRes = m_graph.GetImageResource(m_depthTarget);
		views.push_back(depthRes.view);
	}

	if (m_hasResolve) {
		const auto& resolveRes = m_graph.GetImageResource(m_resolveTarget);
		views.push_back(resolveRes.view);
	}

	VkExtent2D extent = { colorRes.desc.width, colorRes.desc.height };
	auto fb = VWrap::Framebuffer::Create2D(device, m_renderPass, views, extent);
	m_framebufferCache[colorRes.view->Get()] = fb;
	m_activeFramebuffer = fb;
}

// =====================================================================
// ComputePassBuilder
// =====================================================================

ComputePassBuilder::ComputePassBuilder(const std::string& name, RenderGraph& graph)
	: m_name(name), m_graph(graph) {}

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

// =====================================================================
// RenderGraph — Resource creation
// =====================================================================

RenderGraph::RenderGraph(std::shared_ptr<VWrap::Device> device, std::shared_ptr<VWrap::Allocator> allocator)
	: m_device(device), m_allocator(allocator) {}

ImageHandle RenderGraph::CreateImage(const std::string& name, const ImageDesc& desc) {
	ImageHandle handle;
	handle.id = static_cast<uint32_t>(m_images.size());

	ImageResource res;
	res.name = name;
	res.desc = desc;
	res.format = desc.format;
	res.imported = false;
	m_images.push_back(std::move(res));

	return handle;
}

BufferHandle RenderGraph::CreateBuffer(const std::string& name, const BufferDesc& desc) {
	BufferHandle handle;
	handle.id = 0; // TODO: buffer support
	return handle;
}

ImageHandle RenderGraph::ImportImage(const std::string& name,
                                     std::shared_ptr<VWrap::ImageView> view,
                                     VkFormat format, VkImageLayout externalLayout,
                                     VkExtent2D extent) {
	ImageHandle handle;
	handle.id = static_cast<uint32_t>(m_images.size());

	ImageResource res;
	res.name = name;
	res.imported = true;
	res.view = view;
	res.format = format;
	res.externalLayout = externalLayout;
	res.desc.format = format;
	res.desc.samples = VK_SAMPLE_COUNT_1_BIT;
	res.desc.width = extent.width;
	res.desc.height = extent.height;
	res.desc.depth = 1;
	res.desc.imageType = VK_IMAGE_TYPE_2D;
	m_images.push_back(std::move(res));

	return handle;
}

// =====================================================================
// RenderGraph — Pass registration
// =====================================================================

GraphicsPassBuilder& RenderGraph::AddGraphicsPass(const std::string& name) {
	auto builder = std::make_unique<GraphicsPassBuilder>(name, *this);
	m_graphicsPasses.push_back(std::move(builder));
	m_executionOrder.push_back({ PassType::Graphics, m_graphicsPasses.size() - 1 });
	return *m_graphicsPasses.back();
}

ComputePassBuilder& RenderGraph::AddComputePass(const std::string& name) {
	auto builder = std::make_unique<ComputePassBuilder>(name, *this);
	m_computePasses.push_back(std::move(builder));
	m_executionOrder.push_back({ PassType::Compute, m_computePasses.size() - 1 });
	return *m_computePasses.back();
}

void RenderGraph::SetPassEnabled(const std::string& name, bool enabled) {
	for (auto& gfx : m_graphicsPasses) {
		if (gfx->GetName() == name) { gfx->SetEnabled(enabled); return; }
	}
	for (auto& comp : m_computePasses) {
		if (comp->GetName() == name) { comp->SetEnabled(enabled); return; }
	}
}

// =====================================================================
// RenderGraph — Resource access
// =====================================================================

const ImageResource& RenderGraph::GetImageResource(ImageHandle handle) const {
	assert(handle.id < m_images.size());
	return m_images[handle.id];
}

std::shared_ptr<VWrap::ImageView> RenderGraph::GetImageView(ImageHandle handle) const {
	return m_images[handle.id].view;
}

VkImage RenderGraph::GetVkImage(ImageHandle handle) const {
	return m_images[handle.id].image->Get();
}

std::shared_ptr<VWrap::Image> RenderGraph::GetImage(ImageHandle handle) const {
	return m_images[handle.id].image;
}

ImageDesc RenderGraph::GetImageDesc(ImageHandle handle) const {
	return m_images[handle.id].desc;
}

VkFormat RenderGraph::GetImageFormat(ImageHandle handle) const {
	return m_images[handle.id].format;
}

void RenderGraph::UpdateImport(ImageHandle handle, std::shared_ptr<VWrap::ImageView> view) {
	assert(handle.id < m_images.size());
	assert(m_images[handle.id].imported);
	m_images[handle.id].view = view;

	// Update active framebuffer for passes that reference this image (cache lookup or create)
	if (m_compiled) {
		VkImageView newView = view->Get();
		for (size_t i = 0; i < m_executionOrder.size(); i++) {
			auto& ref = m_executionOrder[i];
			if (ref.type == PassType::Graphics) {
				auto& pass = *m_graphicsPasses[ref.index];
				if (pass.m_colorTarget.id == handle.id ||
				    (pass.m_hasResolve && pass.m_resolveTarget.id == handle.id)) {
					auto it = pass.m_framebufferCache.find(newView);
					if (it != pass.m_framebufferCache.end()) {
						pass.m_activeFramebuffer = it->second;
					} else {
						pass.CreateFramebuffer();
					}
				}
			}
		}
	}
}

// =====================================================================
// RenderGraph — Compile
// =====================================================================

void RenderGraph::AccumulateUsageFlags() {
	for (auto& ref : m_executionOrder) {
		if (ref.type == PassType::Graphics) {
			auto& pass = *m_graphicsPasses[ref.index];

			// Color attachment
			if (pass.m_colorTarget.id < m_images.size()) {
				m_images[pass.m_colorTarget.id].usageFlags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
				// If MSAA and transient, mark as transient attachment
				if (!m_images[pass.m_colorTarget.id].imported &&
				    m_images[pass.m_colorTarget.id].desc.samples != VK_SAMPLE_COUNT_1_BIT) {
					m_images[pass.m_colorTarget.id].usageFlags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
				}
			}

			// Depth attachment
			if (pass.m_hasDepth && pass.m_depthTarget.id < m_images.size()) {
				m_images[pass.m_depthTarget.id].usageFlags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}

			// Resolve target
			if (pass.m_hasResolve && pass.m_resolveTarget.id < m_images.size()) {
				m_images[pass.m_resolveTarget.id].usageFlags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			}

			// Sampled reads
			for (auto& img : pass.m_readImages) {
				if (img.id < m_images.size())
					m_images[img.id].usageFlags |= VK_IMAGE_USAGE_SAMPLED_BIT;
			}
		}
		else if (ref.type == PassType::Compute) {
			auto& pass = *m_computePasses[ref.index];

			for (auto& img : pass.m_readImages) {
				if (img.id < m_images.size())
					m_images[img.id].usageFlags |= VK_IMAGE_USAGE_STORAGE_BIT;
			}
			for (auto& img : pass.m_writeImages) {
				if (img.id < m_images.size())
					m_images[img.id].usageFlags |= VK_IMAGE_USAGE_STORAGE_BIT;
			}
		}
	}
}

void RenderGraph::AllocateTransientImages() {
	auto logger = spdlog::get("Render");

	for (auto& res : m_images) {
		if (res.imported) continue;
		if (res.image) continue; // Already allocated (shouldn't happen after Clear)

		VWrap::ImageCreateInfo info{};
		info.width = res.desc.width;
		info.height = res.desc.height;
		info.depth = res.desc.depth;
		info.format = res.desc.format;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = res.usageFlags;
		info.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		info.mip_levels = 1;
		info.samples = res.desc.samples;
		info.image_type = res.desc.imageType;

		// Resolve images also need SAMPLED_BIT for ImGui + TRANSFER_SRC_BIT for screenshots
		if (res.desc.samples == VK_SAMPLE_COUNT_1_BIT &&
		    (res.usageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
		    (res.usageFlags & VK_IMAGE_USAGE_SAMPLED_BIT)) {
			info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}

		res.image = VWrap::Image::Create(m_allocator, info);

		VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		if (res.usageFlags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		res.view = VWrap::ImageView::Create(m_device, res.image, aspect);

		logger->debug("RenderGraph: Allocated transient image '{}' ({}x{}, format {})",
			res.name, res.desc.width, res.desc.height, static_cast<int>(res.desc.format));
	}
}

bool RenderGraph::IsImageReadDownstream(size_t afterOrderIndex, ImageHandle image) const {
	for (size_t i = afterOrderIndex + 1; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];
		if (ref.type == PassType::Graphics) {
			const auto& pass = *m_graphicsPasses[ref.index];
			for (const auto& r : pass.m_readImages) {
				if (r.id == image.id) return true;
			}
		} else {
			const auto& pass = *m_computePasses[ref.index];
			for (const auto& r : pass.m_readImages) {
				if (r.id == image.id) return true;
			}
		}
	}
	return false;
}

VkImageLayout RenderGraph::DetermineColorFinalLayout(size_t passOrderIndex, ImageHandle image) const {
	const auto& res = m_images[image.id];

	// If read downstream as sampled input
	if (IsImageReadDownstream(passOrderIndex, image))
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// If imported, use external layout
	if (res.imported)
		return res.externalLayout;

	return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

VkImageLayout RenderGraph::DetermineResolveFinalLayout(size_t passOrderIndex, ImageHandle image) const {
	const auto& res = m_images[image.id];

	if (IsImageReadDownstream(passOrderIndex, image))
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	if (res.imported)
		return res.externalLayout;

	return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

void RenderGraph::CreateRenderPasses() {
	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];
		if (ref.type != PassType::Graphics) continue;

		auto& pass = *m_graphicsPasses[ref.index];

		VkImageLayout colorFinal = DetermineColorFinalLayout(i, pass.m_colorTarget);
		VkImageLayout resolveFinal = pass.m_hasResolve
			? DetermineResolveFinalLayout(i, pass.m_resolveTarget)
			: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		// Recreate render pass with correct final layouts
		pass.m_renderPass.reset();
		pass.CreateRenderPass(colorFinal, resolveFinal);
	}
}

void RenderGraph::CreateFramebuffers() {
	for (auto& ref : m_executionOrder) {
		if (ref.type != PassType::Graphics) continue;
		auto& pass = *m_graphicsPasses[ref.index];
		pass.CreateFramebuffer();
	}
}

void RenderGraph::ComputeBarriers() {
	m_barriers.clear();
	m_barriers.resize(m_executionOrder.size());

	// Track current layout of each image
	std::vector<VkImageLayout> currentLayout(m_images.size(), VK_IMAGE_LAYOUT_UNDEFINED);

	// Imported images start at their external layout
	for (size_t i = 0; i < m_images.size(); i++) {
		if (m_images[i].imported) {
			currentLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED; // Swapchain images start UNDEFINED each frame
		}
	}

	// Track the last writer pass and its type for each image
	struct WriterInfo {
		VkPipelineStageFlags stage;
		VkAccessFlags access;
	};
	std::vector<WriterInfo> lastWriter(m_images.size(), { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 });

	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];

		if (ref.type == PassType::Compute) {
			auto& pass = *m_computePasses[ref.index];

			// For compute writes: transition to GENERAL if needed
			for (const auto& img : pass.m_writeImages) {
				if (currentLayout[img.id] != VK_IMAGE_LAYOUT_GENERAL) {
					ImageBarrier barrier;
					barrier.image = img;
					barrier.srcStage = lastWriter[img.id].stage;
					barrier.dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
					barrier.srcAccess = lastWriter[img.id].access;
					barrier.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
					barrier.oldLayout = currentLayout[img.id];
					barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
					m_barriers[i].push_back(barrier);
					currentLayout[img.id] = VK_IMAGE_LAYOUT_GENERAL;
				}
			}

			// After compute, update writer info
			for (const auto& img : pass.m_writeImages) {
				lastWriter[img.id] = {
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_ACCESS_SHADER_WRITE_BIT
				};
				currentLayout[img.id] = VK_IMAGE_LAYOUT_GENERAL;
			}
		}
		else if (ref.type == PassType::Graphics) {
			auto& pass = *m_graphicsPasses[ref.index];

			// For sampled reads: transition from current layout to SHADER_READ_ONLY if needed
			for (const auto& img : pass.m_readImages) {
				VkImageLayout required = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				if (currentLayout[img.id] != required) {
					ImageBarrier barrier;
					barrier.image = img;
					barrier.srcStage = lastWriter[img.id].stage;
					barrier.dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
					barrier.srcAccess = lastWriter[img.id].access;
					barrier.dstAccess = VK_ACCESS_SHADER_READ_BIT;
					barrier.oldLayout = currentLayout[img.id];
					barrier.newLayout = required;
					m_barriers[i].push_back(barrier);
					currentLayout[img.id] = required;
				}
			}

			// After graphics pass, update layouts for attachments
			// The render pass handles transitions internally, so we just track final state.
			if (pass.m_colorTarget.id < m_images.size()) {
				// The render pass finalLayout determines the layout after the pass
				// We use the same logic as CreateRenderPass to determine this
				VkImageLayout colorFinal = DetermineColorFinalLayout(i, pass.m_colorTarget);
				currentLayout[pass.m_colorTarget.id] = colorFinal;
				lastWriter[pass.m_colorTarget.id] = {
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				};
			}

			if (pass.m_hasDepth && pass.m_depthTarget.id < m_images.size()) {
				currentLayout[pass.m_depthTarget.id] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				lastWriter[pass.m_depthTarget.id] = {
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
				};
			}

			if (pass.m_hasResolve && pass.m_resolveTarget.id < m_images.size()) {
				VkImageLayout resolveFinal = DetermineResolveFinalLayout(i, pass.m_resolveTarget);
				currentLayout[pass.m_resolveTarget.id] = resolveFinal;
				lastWriter[pass.m_resolveTarget.id] = {
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				};
			}
		}
	}
}

void RenderGraph::Compile() {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Compiling ({} passes, {} images)",
		m_executionOrder.size(), m_images.size());

	AccumulateUsageFlags();
	AllocateTransientImages();
	CreateRenderPasses();
	CreateFramebuffers();
	ComputeBarriers();

	m_compiled = true;
	logger->info("RenderGraph: Compiled successfully");
}

// =====================================================================
// RenderGraph — Execute
// =====================================================================

void RenderGraph::Execute(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex) {
	auto vk_cmd = cmd->Get();

	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];

		// Check enabled state
		bool enabled = true;
		if (ref.type == PassType::Graphics)
			enabled = m_graphicsPasses[ref.index]->IsEnabled();
		else
			enabled = m_computePasses[ref.index]->IsEnabled();
		if (!enabled) continue;

		// Insert barriers
		for (const auto& barrier : m_barriers[i]) {
			const auto& res = m_images[barrier.image.id];

			VkImageMemoryBarrier imgBarrier{};
			imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imgBarrier.srcAccessMask = barrier.srcAccess;
			imgBarrier.dstAccessMask = barrier.dstAccess;
			imgBarrier.oldLayout = barrier.oldLayout;
			imgBarrier.newLayout = barrier.newLayout;
			imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imgBarrier.image = res.image ? res.image->Get() : VK_NULL_HANDLE;
			imgBarrier.subresourceRange.aspectMask =
				(res.usageFlags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
					? VK_IMAGE_ASPECT_DEPTH_BIT
					: VK_IMAGE_ASPECT_COLOR_BIT;
			imgBarrier.subresourceRange.baseMipLevel = 0;
			imgBarrier.subresourceRange.levelCount = 1;
			imgBarrier.subresourceRange.baseArrayLayer = 0;
			imgBarrier.subresourceRange.layerCount = 1;

			cmd->CmdPipelineBarrier(barrier.srcStage, barrier.dstStage, { imgBarrier });
		}

		if (ref.type == PassType::Graphics) {
			auto& pass = *m_graphicsPasses[ref.index];
			const auto& colorRes = m_images[pass.m_colorTarget.id];
			VkExtent2D extent = { colorRes.desc.width, colorRes.desc.height };

			// Build clear values
			std::vector<VkClearValue> clearValues;
			VkClearValue colorClear;
			colorClear.color = pass.m_clearColor;
			clearValues.push_back(colorClear);

			if (pass.m_hasDepth) {
				VkClearValue depthClear;
				depthClear.depthStencil = pass.m_clearDepthStencil;
				clearValues.push_back(depthClear);
			}

			if (pass.m_hasResolve) {
				VkClearValue resolveClear;
				resolveClear.color = pass.m_clearColor;
				clearValues.push_back(resolveClear);
			}

			cmd->CmdBeginRenderPass(pass.m_renderPass, pass.m_activeFramebuffer, clearValues);

			// Set viewport and scissor
			VkViewport viewport{};
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = static_cast<float>(extent.width);
			viewport.height = static_cast<float>(extent.height);
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vkCmdSetViewport(vk_cmd, 0, 1, &viewport);

			VkRect2D scissor{};
			scissor.offset = { 0, 0 };
			scissor.extent = extent;
			vkCmdSetScissor(vk_cmd, 0, 1, &scissor);

			// Call record callback
			if (pass.m_recordFn) {
				PassContext ctx;
				ctx.cmd = cmd;
				ctx.frameIndex = frameIndex;
				ctx.extent = extent;
				pass.m_recordFn(ctx);
			}

			vkCmdEndRenderPass(vk_cmd);
		}
		else if (ref.type == PassType::Compute) {
			auto& pass = *m_computePasses[ref.index];

			if (pass.m_recordFn) {
				// Determine extent from first write image
				VkExtent2D extent{};
				if (!pass.m_writeImages.empty()) {
					const auto& res = m_images[pass.m_writeImages[0].id];
					extent = { res.desc.width, res.desc.height };
				}

				PassContext ctx;
				ctx.cmd = cmd;
				ctx.frameIndex = frameIndex;
				ctx.extent = extent;
				pass.m_recordFn(ctx);
			}
		}
	}
}

// =====================================================================
// RenderGraph — Resize
// =====================================================================

void RenderGraph::Resize(VkExtent2D newExtent) {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Resizing to {}x{}", newExtent.width, newExtent.height);

	// Destroy transient resources
	for (auto& res : m_images) {
		if (res.imported) continue;
		res.image.reset();
		res.view.reset();
		res.usageFlags = 0;
	}

	// Destroy framebuffers
	for (auto& pass : m_graphicsPasses) {
		pass->m_framebufferCache.clear();
		pass->m_activeFramebuffer.reset();
		pass->m_renderPass.reset();
	}

	// Update dimensions for screen-sized transient images
	for (auto& res : m_images) {
		if (res.imported) continue;
		res.desc.width = newExtent.width;
		res.desc.height = newExtent.height;
	}

	m_compiled = false;
	Compile();
}

// =====================================================================
// RenderGraph — Clear
// =====================================================================

void RenderGraph::Clear() {
	m_images.clear();
	m_graphicsPasses.clear();
	m_computePasses.clear();
	m_executionOrder.clear();
	m_barriers.clear();
	m_compiled = false;
}
