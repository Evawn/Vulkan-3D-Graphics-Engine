#include "RenderGraph.h"
#include "GPUProfiler.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cassert>

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
	handle.id = static_cast<uint32_t>(m_buffers.size());

	BufferResource res;
	res.name = name;
	res.desc = desc;
	res.imported = false;
	res.usageFlags = desc.usage;
	m_buffers.push_back(std::move(res));

	return handle;
}

BufferHandle RenderGraph::ImportBuffer(const std::string& name,
                                       std::shared_ptr<VWrap::Buffer> buffer,
                                       VkDeviceSize size) {
	BufferHandle handle;
	handle.id = static_cast<uint32_t>(m_buffers.size());

	BufferResource res;
	res.name = name;
	res.imported = true;
	res.buffer = buffer;
	res.desc.size = size;
	m_buffers.push_back(std::move(res));

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

const BufferResource& RenderGraph::GetBufferResource(BufferHandle handle) const {
	assert(handle.id < m_buffers.size());
	return m_buffers[handle.id];
}

std::shared_ptr<VWrap::Buffer> RenderGraph::GetBuffer(BufferHandle handle) const {
	assert(handle.id < m_buffers.size());
	return m_buffers[handle.id].buffer;
}

VkBuffer RenderGraph::GetVkBuffer(BufferHandle handle) const {
	assert(handle.id < m_buffers.size());
	return m_buffers[handle.id].buffer->Get();
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
				bool referencesHandle = false;
				for (const auto& ca : pass.m_colorAttachments) {
					if (ca.target.id == handle.id) { referencesHandle = true; break; }
				}
				if (!referencesHandle && pass.m_hasResolve && pass.m_resolveTarget.id == handle.id) {
					referencesHandle = true;
				}
				if (referencesHandle) {
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
// RenderGraph — Introspection
// =====================================================================

GraphSnapshot RenderGraph::BuildSnapshot() const {
	GraphSnapshot snap;
	snap.images = m_images;
	snap.buffers = m_buffers;
	snap.passes.reserve(m_executionOrder.size());
	snap.barriers.reserve(m_executionOrder.size());

	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];
		PassInfo info;

		if (ref.type == PassType::Graphics) {
			const auto& pass = *m_graphicsPasses[ref.index];
			info.name = pass.GetName();
			info.type = PassType::Graphics;
			info.enabled = pass.IsEnabled();
			info.readImages = pass.m_readImages;
			info.readBuffers = pass.m_readBuffers;
			info.writeBuffers = pass.m_writeBuffers;

			PassInfo::GraphicsDetail gfx;
			for (const auto& ca : pass.m_colorAttachments) {
				gfx.colorAttachments.push_back({ ca.target, ca.load, ca.store });
				info.writeImages.push_back(ca.target);
			}
			gfx.depthTarget = pass.m_depthTarget;
			gfx.hasDepth = pass.m_hasDepth;
			gfx.depthLoad = pass.m_depthLoad;
			gfx.depthStore = pass.m_depthStore;
			gfx.resolveTarget = pass.m_resolveTarget;
			gfx.hasResolve = pass.m_hasResolve;
			if (pass.m_hasDepth)
				info.writeImages.push_back(pass.m_depthTarget);
			if (pass.m_hasResolve)
				info.writeImages.push_back(pass.m_resolveTarget);
			info.gfx = gfx;
		}
		else {
			const auto& pass = *m_computePasses[ref.index];
			info.name = pass.GetName();
			info.type = PassType::Compute;
			info.enabled = pass.IsEnabled();
			info.readImages = pass.m_readImages;
			info.readBuffers = pass.m_readBuffers;
			info.writeImages = pass.m_writeImages;
			info.writeBuffers = pass.m_writeBuffers;
		}

		snap.passes.push_back(std::move(info));

		if (i < m_barriers.size()) {
			snap.barriers.push_back({ m_barriers[i].imageBarriers, m_barriers[i].bufferBarriers });
		} else {
			snap.barriers.push_back({});
		}
	}

	return snap;
}

// =====================================================================
// RenderGraph — Compile
// =====================================================================

void RenderGraph::AccumulateUsageFlags() {
	for (auto& ref : m_executionOrder) {
		if (ref.type == PassType::Graphics) {
			auto& pass = *m_graphicsPasses[ref.index];

			// Color attachments (MRT)
			for (const auto& ca : pass.m_colorAttachments) {
				if (ca.target.id < m_images.size()) {
					m_images[ca.target.id].usageFlags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
					// If MSAA and transient, mark as transient attachment
					if (!m_images[ca.target.id].imported &&
					    m_images[ca.target.id].desc.samples != VK_SAMPLE_COUNT_1_BIT) {
						m_images[ca.target.id].usageFlags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
					}
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

			// Buffer reads/writes
			for (auto& buf : pass.m_readBuffers) {
				if (buf.id < m_buffers.size())
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			}
			for (auto& buf : pass.m_writeBuffers) {
				if (buf.id < m_buffers.size())
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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

			// Buffer reads/writes
			for (auto& buf : pass.m_readBuffers) {
				if (buf.id < m_buffers.size())
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			}
			for (auto& buf : pass.m_writeBuffers) {
				if (buf.id < m_buffers.size())
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			}
		}
	}

	// Apply user-requested extra usage flags
	for (auto& img : m_images) {
		img.usageFlags |= img.desc.extraUsage;
	}
}

void RenderGraph::AllocateTransientResources() {
	auto logger = spdlog::get("Render");

	for (auto& res : m_images) {
		if (res.imported) continue;
		if (res.image) continue;

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

	// Allocate transient buffers
	for (auto& res : m_buffers) {
		if (res.imported) continue;
		if (res.buffer) continue;

		res.buffer = VWrap::Buffer::Create(
			m_allocator,
			res.desc.size,
			res.usageFlags,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			0);

		logger->debug("RenderGraph: Allocated transient buffer '{}' (size {})",
			res.name, res.desc.size);
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

	if (IsImageReadDownstream(passOrderIndex, image))
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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

		std::vector<VkImageLayout> colorFinals;
		for (const auto& ca : pass.m_colorAttachments) {
			colorFinals.push_back(DetermineColorFinalLayout(i, ca.target));
		}
		VkImageLayout resolveFinal = pass.m_hasResolve
			? DetermineResolveFinalLayout(i, pass.m_resolveTarget)
			: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		pass.m_renderPass.reset();
		pass.CreateRenderPass(colorFinals, resolveFinal);
	}
}

void RenderGraph::CreateFramebuffers() {
	for (auto& ref : m_executionOrder) {
		if (ref.type != PassType::Graphics) continue;
		auto& pass = *m_graphicsPasses[ref.index];
		pass.CreateFramebuffer();
	}
}

void RenderGraph::BuildGraphicsPipeline(GraphicsPassBuilder& pass) {
	if (!pass.m_pipelineDescFactory) return;
	auto desc = pass.m_pipelineDescFactory();

	auto vertCode = VWrap::readFile(desc.vertSpvPath);
	auto fragCode = VWrap::readFile(desc.fragSpvPath);

	// Rebuild pointer-bearing create infos against owned storage in `desc`.
	VkPipelineVertexInputStateCreateInfo vi{};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = static_cast<uint32_t>(desc.vertexBindings.size());
	vi.pVertexBindingDescriptions = desc.vertexBindings.empty() ? nullptr : desc.vertexBindings.data();
	vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(desc.vertexAttributes.size());
	vi.pVertexAttributeDescriptions = desc.vertexAttributes.empty() ? nullptr : desc.vertexAttributes.data();

	VkPipelineDynamicStateCreateInfo ds{};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	ds.dynamicStateCount = static_cast<uint32_t>(desc.dynamicStates.size());
	ds.pDynamicStates = desc.dynamicStates.empty() ? nullptr : desc.dynamicStates.data();

	VkExtent2D extent{};
	if (!pass.m_colorAttachments.empty()) {
		const auto& res = m_images[pass.m_colorAttachments[0].target.id];
		extent = { res.desc.width, res.desc.height };
	}

	VWrap::PipelineCreateInfo info{};
	info.extent = extent;
	info.render_pass = pass.m_renderPass;
	info.descriptor_set_layout = desc.descriptorSetLayout;
	info.vertex_input_info = vi;
	info.input_assembly = desc.inputAssembly;
	info.dynamic_state = ds;
	info.rasterizer = desc.rasterizer;
	info.depth_stencil = desc.depthStencil;
	info.push_constant_ranges = desc.pushConstantRanges;
	info.subpass = 0;
	info.colorAttachmentCount = static_cast<uint32_t>(pass.m_colorAttachments.size());

	pass.m_pipeline = VWrap::Pipeline::Create(m_device, info, vertCode, fragCode);
}

void RenderGraph::BuildComputePipeline(ComputePassBuilder& pass) {
	if (!pass.m_pipelineDescFactory) return;
	auto desc = pass.m_pipelineDescFactory();
	auto code = VWrap::readFile(desc.compSpvPath);
	pass.m_pipeline = VWrap::ComputePipeline::Create(
		m_device, desc.descriptorSetLayout, desc.pushConstantRanges, code);
}

void RenderGraph::CreatePipelines() {
	for (auto& gfx : m_graphicsPasses) {
		BuildGraphicsPipeline(*gfx);
	}
	for (auto& comp : m_computePasses) {
		BuildComputePipeline(*comp);
	}
}

void RenderGraph::RecreatePipelines() {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Recreating pipelines");
	for (auto& gfx : m_graphicsPasses) {
		BuildGraphicsPipeline(*gfx);
	}
	for (auto& comp : m_computePasses) {
		BuildComputePipeline(*comp);
	}
}

void RenderGraph::ComputeBarriers() {
	m_barriers.clear();
	m_barriers.resize(m_executionOrder.size());

	// Image layout tracking
	std::vector<VkImageLayout> currentLayout(m_images.size(), VK_IMAGE_LAYOUT_UNDEFINED);

	for (size_t i = 0; i < m_images.size(); i++) {
		if (m_images[i].imported) {
			currentLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	struct WriterInfo {
		VkPipelineStageFlags stage;
		VkAccessFlags access;
	};
	std::vector<WriterInfo> lastImageWriter(m_images.size(), { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 });

	// Buffer writer tracking
	std::vector<WriterInfo> lastBufferWriter(m_buffers.size(), { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 });

	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];

		if (ref.type == PassType::Compute) {
			auto& pass = *m_computePasses[ref.index];

			// Image read barriers: ensure previous writes to the image are
			// made visible before this compute pass reads it, and transition
			// to GENERAL (the only layout compute shaders can sample/load from
			// as a storage image).
			for (const auto& img : pass.m_readImages) {
				VkImageLayout required = VK_IMAGE_LAYOUT_GENERAL;
				ImageBarrier barrier;
				barrier.image = img;
				barrier.srcStage = lastImageWriter[img.id].stage;
				barrier.dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
				barrier.srcAccess = lastImageWriter[img.id].access;
				barrier.dstAccess = VK_ACCESS_SHADER_READ_BIT;
				barrier.oldLayout = currentLayout[img.id];
				barrier.newLayout = required;
				m_barriers[i].imageBarriers.push_back(barrier);
				currentLayout[img.id] = required;
			}

			// Image write barriers
			for (const auto& img : pass.m_writeImages) {
				if (currentLayout[img.id] != VK_IMAGE_LAYOUT_GENERAL) {
					ImageBarrier barrier;
					barrier.image = img;
					barrier.srcStage = lastImageWriter[img.id].stage;
					barrier.dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
					barrier.srcAccess = lastImageWriter[img.id].access;
					barrier.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
					barrier.oldLayout = currentLayout[img.id];
					barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
					m_barriers[i].imageBarriers.push_back(barrier);
					currentLayout[img.id] = VK_IMAGE_LAYOUT_GENERAL;
				}
			}

			for (const auto& img : pass.m_writeImages) {
				lastImageWriter[img.id] = {
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_ACCESS_SHADER_WRITE_BIT
				};
				currentLayout[img.id] = VK_IMAGE_LAYOUT_GENERAL;
			}

			// Buffer write barriers
			for (const auto& buf : pass.m_writeBuffers) {
				BufferBarrier barrier;
				barrier.buffer = buf;
				barrier.srcStage = lastBufferWriter[buf.id].stage;
				barrier.dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
				barrier.srcAccess = lastBufferWriter[buf.id].access;
				barrier.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
				m_barriers[i].bufferBarriers.push_back(barrier);

				lastBufferWriter[buf.id] = {
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_ACCESS_SHADER_WRITE_BIT
				};
			}

			// Buffer read barriers
			for (const auto& buf : pass.m_readBuffers) {
				BufferBarrier barrier;
				barrier.buffer = buf;
				barrier.srcStage = lastBufferWriter[buf.id].stage;
				barrier.dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
				barrier.srcAccess = lastBufferWriter[buf.id].access;
				barrier.dstAccess = VK_ACCESS_SHADER_READ_BIT;
				m_barriers[i].bufferBarriers.push_back(barrier);
			}
		}
		else if (ref.type == PassType::Graphics) {
			auto& pass = *m_graphicsPasses[ref.index];

			// Image read barriers
			for (const auto& img : pass.m_readImages) {
				VkImageLayout required = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				if (currentLayout[img.id] != required) {
					ImageBarrier barrier;
					barrier.image = img;
					barrier.srcStage = lastImageWriter[img.id].stage;
					barrier.dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
					barrier.srcAccess = lastImageWriter[img.id].access;
					barrier.dstAccess = VK_ACCESS_SHADER_READ_BIT;
					barrier.oldLayout = currentLayout[img.id];
					barrier.newLayout = required;
					m_barriers[i].imageBarriers.push_back(barrier);
					currentLayout[img.id] = required;
				}
			}

			// Buffer read barriers (vertex/fragment shader SSBOs)
			for (const auto& buf : pass.m_readBuffers) {
				BufferBarrier barrier;
				barrier.buffer = buf;
				barrier.srcStage = lastBufferWriter[buf.id].stage;
				barrier.dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				barrier.srcAccess = lastBufferWriter[buf.id].access;
				barrier.dstAccess = VK_ACCESS_SHADER_READ_BIT;
				m_barriers[i].bufferBarriers.push_back(barrier);
			}

			// Buffer write barriers
			for (const auto& buf : pass.m_writeBuffers) {
				BufferBarrier barrier;
				barrier.buffer = buf;
				barrier.srcStage = lastBufferWriter[buf.id].stage;
				barrier.dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				barrier.srcAccess = lastBufferWriter[buf.id].access;
				barrier.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
				m_barriers[i].bufferBarriers.push_back(barrier);

				lastBufferWriter[buf.id] = {
					VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					VK_ACCESS_SHADER_WRITE_BIT
				};
			}

			for (const auto& ca : pass.m_colorAttachments) {
				if (ca.target.id < m_images.size()) {
					VkImageLayout colorFinal = DetermineColorFinalLayout(i, ca.target);
					currentLayout[ca.target.id] = colorFinal;
					lastImageWriter[ca.target.id] = {
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
					};
				}
			}

			if (pass.m_hasDepth && pass.m_depthTarget.id < m_images.size()) {
				currentLayout[pass.m_depthTarget.id] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				lastImageWriter[pass.m_depthTarget.id] = {
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
				};
			}

			if (pass.m_hasResolve && pass.m_resolveTarget.id < m_images.size()) {
				VkImageLayout resolveFinal = DetermineResolveFinalLayout(i, pass.m_resolveTarget);
				currentLayout[pass.m_resolveTarget.id] = resolveFinal;
				lastImageWriter[pass.m_resolveTarget.id] = {
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				};
			}
		}
	}
}

void RenderGraph::Compile() {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Compiling ({} passes, {} images, {} buffers)",
		m_executionOrder.size(), m_images.size(), m_buffers.size());

	AccumulateUsageFlags();
	AllocateTransientResources();
	CreateRenderPasses();
	CreateFramebuffers();
	CreatePipelines();
	ComputeBarriers();

	m_compiled = true;
	logger->info("RenderGraph: Compiled successfully");
}

// =====================================================================
// RenderGraph — Execute
// =====================================================================

void RenderGraph::Execute(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex,
                          GPUProfiler* profiler) {
	auto vk_cmd = cmd->Get();

	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];

		bool enabled = true;
		if (ref.type == PassType::Graphics)
			enabled = m_graphicsPasses[ref.index]->IsEnabled();
		else
			enabled = m_computePasses[ref.index]->IsEnabled();
		if (!enabled) continue;

		// Insert image barriers
		for (const auto& barrier : m_barriers[i].imageBarriers) {
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

		// Insert buffer barriers
		for (const auto& barrier : m_barriers[i].bufferBarriers) {
			const auto& res = m_buffers[barrier.buffer.id];

			VkBufferMemoryBarrier bufBarrier{};
			bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			bufBarrier.srcAccessMask = barrier.srcAccess;
			bufBarrier.dstAccessMask = barrier.dstAccess;
			bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufBarrier.buffer = res.buffer ? res.buffer->Get() : VK_NULL_HANDLE;
			bufBarrier.offset = 0;
			bufBarrier.size = VK_WHOLE_SIZE;

			cmd->CmdPipelineBarrier(barrier.srcStage, barrier.dstStage, {}, { bufBarrier });
		}

		if (profiler) profiler->CmdBeginPass(cmd, frameIndex, static_cast<uint32_t>(i));

		if (ref.type == PassType::Graphics) {
			auto& pass = *m_graphicsPasses[ref.index];
			const auto& firstColorRes = m_images[pass.m_colorAttachments[0].target.id];
			VkExtent2D extent = { firstColorRes.desc.width, firstColorRes.desc.height };

			std::vector<VkClearValue> clearValues;
			for (const auto& ca : pass.m_colorAttachments) {
				VkClearValue colorClear;
				colorClear.color = ca.clearColor;
				clearValues.push_back(colorClear);
			}

			if (pass.m_hasDepth) {
				VkClearValue depthClear;
				depthClear.depthStencil = pass.m_clearDepthStencil;
				clearValues.push_back(depthClear);
			}

			if (pass.m_hasResolve) {
				VkClearValue resolveClear;
				resolveClear.color = pass.m_colorAttachments[0].clearColor;
				clearValues.push_back(resolveClear);
			}

			cmd->CmdBeginRenderPass(pass.m_renderPass, pass.m_activeFramebuffer, clearValues);

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

			if (pass.m_recordFn) {
				PassContext ctx;
				ctx.cmd = cmd;
				ctx.frameIndex = frameIndex;
				ctx.extent = extent;
				ctx.graphicsPipeline = pass.m_pipeline;
				pass.m_recordFn(ctx);
			}

			vkCmdEndRenderPass(vk_cmd);
		}
		else if (ref.type == PassType::Compute) {
			auto& pass = *m_computePasses[ref.index];

			if (pass.m_recordFn) {
				VkExtent2D extent{};
				if (!pass.m_writeImages.empty()) {
					const auto& res = m_images[pass.m_writeImages[0].id];
					extent = { res.desc.width, res.desc.height };
				}

				PassContext ctx;
				ctx.cmd = cmd;
				ctx.frameIndex = frameIndex;
				ctx.extent = extent;
				ctx.computePipeline = pass.m_pipeline;
				pass.m_recordFn(ctx);
			}
		}

		if (profiler) profiler->CmdEndPass(cmd, frameIndex, static_cast<uint32_t>(i));
	}
}

// =====================================================================
// RenderGraph — Resize
// =====================================================================

void RenderGraph::Resize(VkExtent2D newExtent) {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Resizing to {}x{}", newExtent.width, newExtent.height);

	for (auto& res : m_images) {
		if (res.imported) continue;
		// Preserve 3D volumes across resize — their contents (e.g. uploaded
		// .vox data) would otherwise be wiped and there's no producer pass
		// running every frame to refill them once Generate is disabled.
		if (res.desc.imageType == VK_IMAGE_TYPE_3D) continue;
		res.image.reset();
		res.view.reset();
		res.usageFlags = 0;
	}

	for (auto& pass : m_graphicsPasses) {
		pass->m_framebufferCache.clear();
		pass->m_activeFramebuffer.reset();
		pass->m_renderPass.reset();
	}

	for (auto& res : m_images) {
		if (res.imported) continue;
		if (res.desc.imageType == VK_IMAGE_TYPE_3D) continue;
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
	m_buffers.clear();
	m_graphicsPasses.clear();
	m_computePasses.clear();
	m_executionOrder.clear();
	m_barriers.clear();
	m_compiled = false;
}
