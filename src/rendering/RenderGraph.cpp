#include "RenderGraph.h"
#include "BindingTable.h"
#include "DAGBuilder.h"
#include "GPUProfiler.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cassert>
#include <cstring>

// File-local: lookup the per-handle usage in the parallel usage vector. Pass
// builders may have shorter usage vectors if older code paths skipped them; in
// that case, fall back to ResourceUsage::Default so existing call sites remain
// unchanged.
static ResourceUsage UsageAt(const std::vector<ResourceUsage>& v, size_t i) {
	return (i < v.size()) ? v[i] : ResourceUsage::Default;
}

// =====================================================================
// RenderGraph — Resource creation + uploads
// =====================================================================

void RenderGraph::UploadBufferData(BufferHandle handle, const void* data, size_t size,
                                   std::shared_ptr<VWrap::CommandPool> pool) {
	auto dst = GetBuffer(handle);

	auto staging = VWrap::Buffer::CreateStaging(m_allocator, size);
	void* mapped = nullptr;
	vmaMapMemory(m_allocator->Get(), staging->GetAllocation(), &mapped);
	std::memcpy(mapped, data, size);
	vmaUnmapMemory(m_allocator->Get(), staging->GetAllocation());

	auto cmd = VWrap::CommandBuffer::Create(pool);
	cmd->BeginSingle();
	cmd->CmdCopyBuffer(staging, dst, size);
	cmd->EndAndSubmit();
}

RenderGraph::RenderGraph(std::shared_ptr<VWrap::Device> device, std::shared_ptr<VWrap::Allocator> allocator)
	: m_device(device), m_allocator(allocator) {}

void RenderGraph::ConfigureAsync(const RenderGraphAsyncConfig& cfg) {
	m_asyncCfg = cfg;
	const bool hasDistinctComputeFamily = cfg.computeQueue && cfg.computeCommandPool &&
		cfg.computeQueue->GetQueueFamilyIndex() != cfg.graphicsQueueFamily;
	m_asyncAvailable = hasDistinctComputeFamily;
	if (!m_asyncAvailable) return;

	// Pre-allocate per-frame async command buffers and binary signal semaphores.
	// Reused across compiles — these don't depend on graph state, only on the
	// frames-in-flight count.
	m_asyncCmdBuffers.clear();
	m_asyncDoneSemaphores.clear();
	for (uint32_t i = 0; i < cfg.framesInFlight; i++) {
		m_asyncCmdBuffers.push_back(VWrap::CommandBuffer::Create(cfg.computeCommandPool));
		m_asyncDoneSemaphores.push_back(VWrap::Semaphore::Create(m_device));
	}
}

ImageHandle RenderGraph::CreateImage(const std::string& name, const ImageDesc& desc) {
	ImageHandle handle;
	handle.id = static_cast<uint32_t>(m_images.size());
	handle.gen = m_imageGen;

	ImageResource res;
	res.name = name;
	res.desc = desc;
	res.format = desc.format;
	res.imported = false;
	res.gen = m_imageGen;
	m_images.push_back(std::move(res));

	return handle;
}

BufferHandle RenderGraph::CreateBuffer(const std::string& name, const BufferDesc& desc) {
	BufferHandle handle;
	handle.id = static_cast<uint32_t>(m_buffers.size());
	handle.gen = m_bufferGen;

	BufferResource res;
	res.name = name;
	res.desc = desc;
	res.imported = false;
	res.gen = m_bufferGen;
	res.usageFlags = desc.usage;
	m_buffers.push_back(std::move(res));

	return handle;
}

BufferHandle RenderGraph::ImportBuffer(const std::string& name,
                                       std::shared_ptr<VWrap::Buffer> buffer,
                                       VkDeviceSize size) {
	BufferHandle handle;
	handle.id = static_cast<uint32_t>(m_buffers.size());
	handle.gen = m_bufferGen;

	BufferResource res;
	res.name = name;
	res.imported = true;
	res.gen = m_bufferGen;
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
	handle.gen = m_imageGen;

	ImageResource res;
	res.name = name;
	res.imported = true;
	res.gen = m_imageGen;
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
	m_declarationOrder.push_back({ PassType::Graphics, m_graphicsPasses.size() - 1 });
	return *m_graphicsPasses.back();
}

ComputePassBuilder& RenderGraph::AddComputePass(const std::string& name) {
	auto builder = std::make_unique<ComputePassBuilder>(name, *this);
	m_computePasses.push_back(std::move(builder));
	m_declarationOrder.push_back({ PassType::Compute, m_computePasses.size() - 1 });
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

// MarkSink defined after AssertImageHandle / AssertBufferHandle below.

// =====================================================================
// RenderGraph — Resource access
// =====================================================================

// Validates handle.id is in-range AND handle.gen matches the resource's gen.
// A mismatched gen means the handle outlived a Clear() — fail loudly so the
// regression surfaces immediately instead of silently aliasing a new resource.
static inline void AssertImageHandle(ImageHandle h, const std::vector<ImageResource>& images) {
	assert(h.id < images.size() && "ImageHandle index out of range");
	assert(images[h.id].gen == h.gen && "ImageHandle stale (graph was rebuilt)");
}
static inline void AssertBufferHandle(BufferHandle h, const std::vector<BufferResource>& buffers) {
	assert(h.id < buffers.size() && "BufferHandle index out of range");
	assert(buffers[h.id].gen == h.gen && "BufferHandle stale (graph was rebuilt)");
}

const ImageResource& RenderGraph::GetImageResource(ImageHandle handle) const {
	AssertImageHandle(handle, m_images);
	return m_images[handle.id];
}

std::shared_ptr<VWrap::ImageView> RenderGraph::GetImageView(ImageHandle handle) const {
	AssertImageHandle(handle, m_images);
	return m_images[handle.id].view;
}

VkImage RenderGraph::GetVkImage(ImageHandle handle) const {
	AssertImageHandle(handle, m_images);
	return m_images[handle.id].image->Get();
}

std::shared_ptr<VWrap::Image> RenderGraph::GetImage(ImageHandle handle) const {
	AssertImageHandle(handle, m_images);
	return m_images[handle.id].image;
}

ImageDesc RenderGraph::GetImageDesc(ImageHandle handle) const {
	AssertImageHandle(handle, m_images);
	return m_images[handle.id].desc;
}

VkFormat RenderGraph::GetImageFormat(ImageHandle handle) const {
	AssertImageHandle(handle, m_images);
	return m_images[handle.id].format;
}

const BufferResource& RenderGraph::GetBufferResource(BufferHandle handle) const {
	AssertBufferHandle(handle, m_buffers);
	return m_buffers[handle.id];
}

std::shared_ptr<VWrap::Buffer> RenderGraph::GetBuffer(BufferHandle handle) const {
	AssertBufferHandle(handle, m_buffers);
	return m_buffers[handle.id].buffer;
}

VkBuffer RenderGraph::GetVkBuffer(BufferHandle handle) const {
	AssertBufferHandle(handle, m_buffers);
	return m_buffers[handle.id].buffer->Get();
}

void RenderGraph::MarkSink(ImageHandle handle) {
	AssertImageHandle(handle, m_images);
	m_explicitImageSinks.push_back(handle.id);
}

void RenderGraph::MarkSink(BufferHandle handle) {
	AssertBufferHandle(handle, m_buffers);
	m_explicitBufferSinks.push_back(handle.id);
}

void RenderGraph::UpdateImport(ImageHandle handle, std::shared_ptr<VWrap::ImageView> view) {
	AssertImageHandle(handle, m_images);
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
			info.acceptedItemTypes = pass.GetAcceptedItemTypes();

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
			info.acceptedItemTypes = pass.GetAcceptedItemTypes();
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

			// Image reads
			for (size_t r = 0; r < pass.m_readImages.size(); r++) {
				const auto& img = pass.m_readImages[r];
				if (img.id >= m_images.size()) continue;
				ResourceUsage usage = UsageAt(pass.m_readImageUsages, r);
				m_images[img.id].usageFlags |= (usage == ResourceUsage::StorageRead)
					? VK_IMAGE_USAGE_STORAGE_BIT
					: VK_IMAGE_USAGE_SAMPLED_BIT;
			}

			// Buffer reads
			for (size_t r = 0; r < pass.m_readBuffers.size(); r++) {
				const auto& buf = pass.m_readBuffers[r];
				if (buf.id >= m_buffers.size()) continue;
				ResourceUsage usage = UsageAt(pass.m_readBufferUsages, r);
				switch (usage) {
				case ResourceUsage::UniformRead:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
					break;
				case ResourceUsage::VertexBuffer:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
					break;
				case ResourceUsage::IndexBuffer:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
					break;
				case ResourceUsage::IndirectArg:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
					break;
				default:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
					break;
				}
			}
			for (auto& buf : pass.m_writeBuffers) {
				if (buf.id < m_buffers.size())
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			}
		}
		else if (ref.type == PassType::Compute) {
			auto& pass = *m_computePasses[ref.index];

			for (size_t r = 0; r < pass.m_readImages.size(); r++) {
				const auto& img = pass.m_readImages[r];
				if (img.id >= m_images.size()) continue;
				ResourceUsage usage = UsageAt(pass.m_readImageUsages, r);
				m_images[img.id].usageFlags |= (usage == ResourceUsage::SampledRead)
					? VK_IMAGE_USAGE_SAMPLED_BIT
					: VK_IMAGE_USAGE_STORAGE_BIT;
			}
			for (size_t w = 0; w < pass.m_writeImages.size(); w++) {
				const auto& img = pass.m_writeImages[w];
				if (img.id < m_images.size())
					m_images[img.id].usageFlags |= VK_IMAGE_USAGE_STORAGE_BIT;
			}

			// Buffer reads
			for (size_t r = 0; r < pass.m_readBuffers.size(); r++) {
				const auto& buf = pass.m_readBuffers[r];
				if (buf.id >= m_buffers.size()) continue;
				ResourceUsage usage = UsageAt(pass.m_readBufferUsages, r);
				switch (usage) {
				case ResourceUsage::UniformRead:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
					break;
				case ResourceUsage::IndirectArg:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
					break;
				default:
					m_buffers[buf.id].usageFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
					break;
				}
			}
			for (size_t w = 0; w < pass.m_writeBuffers.size(); w++) {
				const auto& buf = pass.m_writeBuffers[w];
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
	// Forward BFS over the DAG from the producer node. Visits only descendants
	// that depend on this pass (via any resource); for each live descendant,
	// check whether image is in its read set. Pruned descendants are visited
	// (their dependents may still be alive) but their reads are skipped since
	// they don't actually run.
	if (afterOrderIndex >= m_executionOrderNodes.size()) return false;
	uint32_t startNode = m_executionOrderNodes[afterOrderIndex];

	std::vector<bool> visited(m_dag.NodeCount(), false);
	std::vector<uint32_t> stack;
	stack.reserve(m_dag.NodeCount());
	stack.push_back(startNode);
	visited[startNode] = true;

	while (!stack.empty()) {
		uint32_t u = stack.back();
		stack.pop_back();
		for (uint32_t v : m_dag.Dependents(u)) {
			if (visited[v]) continue;
			visited[v] = true;
			stack.push_back(v);
			if (!m_nodeAlive[v]) continue;   // pruned — doesn't actually run

			size_t declIdx = m_nodeToDecl[v];
			const auto& ref = m_declarationOrder[declIdx];
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
	// Walk m_executionOrder so pruned passes don't get pipelines built against
	// null render passes (CreateRenderPasses also skips pruned passes).
	for (const auto& ref : m_executionOrder) {
		if (ref.type == PassType::Graphics) {
			BuildGraphicsPipeline(*m_graphicsPasses[ref.index]);
		} else {
			BuildComputePipeline(*m_computePasses[ref.index]);
		}
	}
}

void RenderGraph::UpdateBindings() {
	for (auto& gfx : m_graphicsPasses) {
		if (gfx->m_bindings) gfx->m_bindings->Update(*this);
	}
	for (auto& comp : m_computePasses) {
		if (comp->m_bindings) comp->m_bindings->Update(*this);
	}
}

void RenderGraph::RecreatePipelines() {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Recreating pipelines");
	for (const auto& ref : m_executionOrder) {
		if (ref.type == PassType::Graphics) {
			BuildGraphicsPipeline(*m_graphicsPasses[ref.index]);
		} else {
			BuildComputePipeline(*m_computePasses[ref.index]);
		}
	}
}

// Map a (usage, pass-type) pair to image (layout, access, stage). Returns the
// legacy values when usage==Default so unmigrated call sites are unchanged.
static void ImageReadParams(ResourceUsage usage, PassType pass,
                            VkImageLayout& outLayout, VkAccessFlags& outAccess,
                            VkPipelineStageFlags& outStage) {
	if (pass == PassType::Compute) {
		switch (usage) {
		case ResourceUsage::SampledRead:
			outLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			outAccess = VK_ACCESS_SHADER_READ_BIT;
			outStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			return;
		case ResourceUsage::StorageRead:
		case ResourceUsage::Default:
		default:
			outLayout = VK_IMAGE_LAYOUT_GENERAL;
			outAccess = VK_ACCESS_SHADER_READ_BIT;
			outStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			return;
		}
	}
	// Graphics
	switch (usage) {
	case ResourceUsage::StorageRead:
		outLayout = VK_IMAGE_LAYOUT_GENERAL;
		outAccess = VK_ACCESS_SHADER_READ_BIT;
		outStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		return;
	case ResourceUsage::SampledRead:
	case ResourceUsage::Default:
	default:
		outLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		outAccess = VK_ACCESS_SHADER_READ_BIT;
		outStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		return;
	}
}

static void BufferReadParams(ResourceUsage usage, PassType pass,
                             VkAccessFlags& outAccess, VkPipelineStageFlags& outStage) {
	if (pass == PassType::Compute) {
		outAccess = VK_ACCESS_SHADER_READ_BIT;
		outStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		if (usage == ResourceUsage::IndirectArg) {
			outAccess = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
			outStage = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
		}
		return;
	}
	switch (usage) {
	case ResourceUsage::VertexBuffer:
		outAccess = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
		outStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		return;
	case ResourceUsage::IndexBuffer:
		outAccess = VK_ACCESS_INDEX_READ_BIT;
		outStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		return;
	case ResourceUsage::IndirectArg:
		outAccess = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
		outStage = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
		return;
	case ResourceUsage::UniformRead:
		outAccess = VK_ACCESS_UNIFORM_READ_BIT;
		outStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		return;
	case ResourceUsage::StorageRead:
	case ResourceUsage::SampledRead:
	case ResourceUsage::Default:
	default:
		outAccess = VK_ACCESS_SHADER_READ_BIT;
		outStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		return;
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
	std::vector<WriterInfo> lastBufferWriter(m_buffers.size(), { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0 });

	// Drop "sentinel-source" barriers: if a resource has not yet been written
	// in this frame, there's no producer to sync against, so emitting a barrier
	// from TOP_OF_PIPE/0 is wasted work.
	std::vector<bool> imageWritten(m_images.size(), false);
	std::vector<bool> bufferWritten(m_buffers.size(), false);

	// ---- Cross-stream ownership tracking ----
	// Queue family ownership is per-resource and changes whenever a pass on a
	// different stream uses a resource a previous stream produced. We track
	// (a) the current owner stream, (b) the m_executionOrder index of the
	// producer-most-recently-on-that-stream — this is where the release barrier
	// gets appended when the next consumer is on a different stream.
	std::vector<QueueAffinity> imageOwnerStream(m_images.size(), QueueAffinity::Graphics);
	std::vector<QueueAffinity> bufferOwnerStream(m_buffers.size(), QueueAffinity::Graphics);
	std::vector<size_t>        imageLastProducerIdx(m_images.size(), SIZE_MAX);
	std::vector<size_t>        bufferLastProducerIdx(m_buffers.size(), SIZE_MAX);

	const uint32_t graphicsQF = m_asyncCfg.graphicsQueueFamily;
	const uint32_t asyncQF    = (m_asyncAvailable && m_asyncCfg.computeQueue)
		? m_asyncCfg.computeQueue->GetQueueFamilyIndex()
		: graphicsQF;
	auto qfFor = [&](QueueAffinity s) {
		return (s == QueueAffinity::AsyncCompute) ? asyncQF : graphicsQF;
	};

	// Helper: emits a cross-stream ownership transfer for image `img.id`.
	// Returns true iff the access was handled (caller should skip its normal
	// barrier path); false iff no transfer was needed (no producer or same
	// stream — caller proceeds with the legacy intra-stream path).
	auto tryEmitImageOwnershipTransfer = [&](
		ImageHandle img, size_t consumerIdx, QueueAffinity consumerStream,
		VkImageLayout neededLayout, VkPipelineStageFlags consumerStage,
		VkAccessFlags consumerAccess) -> bool
	{
		if (!imageWritten[img.id]) return false;            // first use this frame; no transfer
		if (imageOwnerStream[img.id] == consumerStream) return false;  // intra-stream
		const size_t producerIdx = imageLastProducerIdx[img.id];
		if (producerIdx == SIZE_MAX) return false;          // owner without producer (shouldn't happen)

		// Release on producer's command buffer (executes after producer pass body).
		ImageBarrier release{};
		release.image      = img;
		release.srcStage   = lastImageWriter[img.id].stage;
		release.srcAccess  = lastImageWriter[img.id].access;
		release.dstStage   = 0;        // ignored on release half of an ownership transfer
		release.dstAccess  = 0;
		release.oldLayout  = currentLayout[img.id];
		release.newLayout  = neededLayout;
		release.srcQueueFamily = qfFor(imageOwnerStream[img.id]);
		release.dstQueueFamily = qfFor(consumerStream);
		m_barriers[producerIdx].imageReleaseBarriers.push_back(release);

		// Acquire on consumer's command buffer (executes before consumer pass body).
		ImageBarrier acquire{};
		acquire.image      = img;
		acquire.srcStage   = 0;        // ignored on acquire half
		acquire.srcAccess  = 0;
		acquire.dstStage   = consumerStage;
		acquire.dstAccess  = consumerAccess;
		acquire.oldLayout  = currentLayout[img.id];
		acquire.newLayout  = neededLayout;
		acquire.srcQueueFamily = qfFor(imageOwnerStream[img.id]);
		acquire.dstQueueFamily = qfFor(consumerStream);
		m_barriers[consumerIdx].imageBarriers.push_back(acquire);

		// Update tracker — consumer now owns it; layout is whatever the consumer asked for.
		currentLayout[img.id]       = neededLayout;
		imageOwnerStream[img.id]    = consumerStream;
		// imageLastProducerIdx unchanged: consumer hasn't written, just acquired.
		// lastImageWriter unchanged: producer's write info still defines visibility.
		return true;
	};

	auto tryEmitBufferOwnershipTransfer = [&](
		BufferHandle buf, size_t consumerIdx, QueueAffinity consumerStream,
		VkPipelineStageFlags consumerStage, VkAccessFlags consumerAccess) -> bool
	{
		if (!bufferWritten[buf.id]) return false;
		if (bufferOwnerStream[buf.id] == consumerStream) return false;
		const size_t producerIdx = bufferLastProducerIdx[buf.id];
		if (producerIdx == SIZE_MAX) return false;

		BufferBarrier release{};
		release.buffer    = buf;
		release.srcStage  = lastBufferWriter[buf.id].stage;
		release.srcAccess = lastBufferWriter[buf.id].access;
		release.dstStage  = 0;
		release.dstAccess = 0;
		release.srcQueueFamily = qfFor(bufferOwnerStream[buf.id]);
		release.dstQueueFamily = qfFor(consumerStream);
		m_barriers[producerIdx].bufferReleaseBarriers.push_back(release);

		BufferBarrier acquire{};
		acquire.buffer    = buf;
		acquire.srcStage  = 0;
		acquire.srcAccess = 0;
		acquire.dstStage  = consumerStage;
		acquire.dstAccess = consumerAccess;
		acquire.srcQueueFamily = qfFor(bufferOwnerStream[buf.id]);
		acquire.dstQueueFamily = qfFor(consumerStream);
		m_barriers[consumerIdx].bufferBarriers.push_back(acquire);

		bufferOwnerStream[buf.id] = consumerStream;
		return true;
	};

	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		const auto& ref = m_executionOrder[i];
		const QueueAffinity passStream = m_executionOrderStreams[i];

		if (ref.type == PassType::Compute) {
			auto& pass = *m_computePasses[ref.index];

			// Image reads
			for (size_t r = 0; r < pass.m_readImages.size(); r++) {
				const auto& img = pass.m_readImages[r];
				ResourceUsage usage = UsageAt(pass.m_readImageUsages, r);
				VkImageLayout required;
				VkAccessFlags dstAccess;
				VkPipelineStageFlags dstStage;
				ImageReadParams(usage, PassType::Compute, required, dstAccess, dstStage);

				if (tryEmitImageOwnershipTransfer(img, i, passStream,
						required, dstStage, dstAccess)) {
					continue;
				}

				bool layoutMatches = (currentLayout[img.id] == required);
				bool noProducer = !imageWritten[img.id];
				if (layoutMatches && noProducer) continue;

				ImageBarrier barrier;
				barrier.image = img;
				barrier.srcStage = imageWritten[img.id] ? lastImageWriter[img.id].stage
				                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				barrier.dstStage = dstStage;
				barrier.srcAccess = imageWritten[img.id] ? lastImageWriter[img.id].access : 0;
				barrier.dstAccess = dstAccess;
				barrier.oldLayout = currentLayout[img.id];
				barrier.newLayout = required;
				m_barriers[i].imageBarriers.push_back(barrier);
				currentLayout[img.id] = required;
			}

			// Image writes — emit transition to GENERAL only if layout differs.
			for (const auto& img : pass.m_writeImages) {
				if (tryEmitImageOwnershipTransfer(img, i, passStream,
						VK_IMAGE_LAYOUT_GENERAL,
						VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_SHADER_WRITE_BIT)) {
					// fall through; the acquire transitioned to GENERAL.
				} else if (currentLayout[img.id] != VK_IMAGE_LAYOUT_GENERAL) {
					ImageBarrier barrier;
					barrier.image = img;
					barrier.srcStage = imageWritten[img.id] ? lastImageWriter[img.id].stage
					                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
					barrier.dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
					barrier.srcAccess = imageWritten[img.id] ? lastImageWriter[img.id].access : 0;
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
				imageWritten[img.id] = true;
				currentLayout[img.id] = VK_IMAGE_LAYOUT_GENERAL;
				imageOwnerStream[img.id] = passStream;
				imageLastProducerIdx[img.id] = i;
			}

			// Buffer writes
			for (size_t w = 0; w < pass.m_writeBuffers.size(); w++) {
				const auto& buf = pass.m_writeBuffers[w];
				if (tryEmitBufferOwnershipTransfer(buf, i, passStream,
						VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_SHADER_WRITE_BIT)) {
					// acquire emitted
				} else if (bufferWritten[buf.id]) {
					BufferBarrier barrier;
					barrier.buffer = buf;
					barrier.srcStage = lastBufferWriter[buf.id].stage;
					barrier.dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
					barrier.srcAccess = lastBufferWriter[buf.id].access;
					barrier.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
					m_barriers[i].bufferBarriers.push_back(barrier);
				}

				lastBufferWriter[buf.id] = {
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_ACCESS_SHADER_WRITE_BIT
				};
				bufferWritten[buf.id] = true;
				bufferOwnerStream[buf.id] = passStream;
				bufferLastProducerIdx[buf.id] = i;
			}

			// Buffer reads
			for (size_t r = 0; r < pass.m_readBuffers.size(); r++) {
				const auto& buf = pass.m_readBuffers[r];

				ResourceUsage usage = UsageAt(pass.m_readBufferUsages, r);
				VkAccessFlags dstAccess;
				VkPipelineStageFlags dstStage;
				BufferReadParams(usage, PassType::Compute, dstAccess, dstStage);

				if (tryEmitBufferOwnershipTransfer(buf, i, passStream, dstStage, dstAccess)) {
					continue;
				}
				if (!bufferWritten[buf.id]) continue;  // no producer to sync against

				BufferBarrier barrier;
				barrier.buffer = buf;
				barrier.srcStage = lastBufferWriter[buf.id].stage;
				barrier.dstStage = dstStage;
				barrier.srcAccess = lastBufferWriter[buf.id].access;
				barrier.dstAccess = dstAccess;
				m_barriers[i].bufferBarriers.push_back(barrier);
			}
		}
		else if (ref.type == PassType::Graphics) {
			auto& pass = *m_graphicsPasses[ref.index];

			// Image reads
			for (size_t r = 0; r < pass.m_readImages.size(); r++) {
				const auto& img = pass.m_readImages[r];
				ResourceUsage usage = UsageAt(pass.m_readImageUsages, r);
				VkImageLayout required;
				VkAccessFlags dstAccess;
				VkPipelineStageFlags dstStage;
				ImageReadParams(usage, PassType::Graphics, required, dstAccess, dstStage);

				if (tryEmitImageOwnershipTransfer(img, i, passStream,
						required, dstStage, dstAccess)) {
					continue;
				}

				bool layoutMatches = (currentLayout[img.id] == required);
				bool noProducer = !imageWritten[img.id];
				if (layoutMatches && noProducer) continue;

				ImageBarrier barrier;
				barrier.image = img;
				barrier.srcStage = imageWritten[img.id] ? lastImageWriter[img.id].stage
				                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				barrier.dstStage = dstStage;
				barrier.srcAccess = imageWritten[img.id] ? lastImageWriter[img.id].access : 0;
				barrier.dstAccess = dstAccess;
				barrier.oldLayout = currentLayout[img.id];
				barrier.newLayout = required;
				m_barriers[i].imageBarriers.push_back(barrier);
				currentLayout[img.id] = required;
			}

			// Buffer reads (vertex/fragment shader SSBOs / UBOs / vertex+index)
			for (size_t r = 0; r < pass.m_readBuffers.size(); r++) {
				const auto& buf = pass.m_readBuffers[r];

				ResourceUsage usage = UsageAt(pass.m_readBufferUsages, r);
				VkAccessFlags dstAccess;
				VkPipelineStageFlags dstStage;
				BufferReadParams(usage, PassType::Graphics, dstAccess, dstStage);

				if (tryEmitBufferOwnershipTransfer(buf, i, passStream, dstStage, dstAccess)) {
					continue;
				}
				if (!bufferWritten[buf.id]) continue;

				BufferBarrier barrier;
				barrier.buffer = buf;
				barrier.srcStage = lastBufferWriter[buf.id].stage;
				barrier.dstStage = dstStage;
				barrier.srcAccess = lastBufferWriter[buf.id].access;
				barrier.dstAccess = dstAccess;
				m_barriers[i].bufferBarriers.push_back(barrier);
			}

			// Buffer writes
			for (const auto& buf : pass.m_writeBuffers) {
				const VkPipelineStageFlags writeStage =
					VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				if (tryEmitBufferOwnershipTransfer(buf, i, passStream,
						writeStage, VK_ACCESS_SHADER_WRITE_BIT)) {
					// acquired
				} else if (bufferWritten[buf.id]) {
					BufferBarrier barrier;
					barrier.buffer = buf;
					barrier.srcStage = lastBufferWriter[buf.id].stage;
					barrier.dstStage = writeStage;
					barrier.srcAccess = lastBufferWriter[buf.id].access;
					barrier.dstAccess = VK_ACCESS_SHADER_WRITE_BIT;
					m_barriers[i].bufferBarriers.push_back(barrier);
				}

				lastBufferWriter[buf.id] = { writeStage, VK_ACCESS_SHADER_WRITE_BIT };
				bufferWritten[buf.id] = true;
				bufferOwnerStream[buf.id] = passStream;
				bufferLastProducerIdx[buf.id] = i;
			}

			// Render-pass color/depth/resolve writes are emitted via the render
			// pass's attachment description (initialLayout=UNDEFINED → finalLayout).
			// Memory dependencies for prior writers are subsumed by render-pass
			// implicit subpass dependencies; for cross-stream, we still need a
			// queue-family ownership acquire BEFORE BeginRenderPass so the render
			// pass executes on the right family. Layout transitions stay with the
			// render pass — emit the acquire with oldLayout=newLayout (pure
			// ownership transfer; the render pass's UNDEFINED initial layout
			// discards prior contents).
			auto graphicsAttachmentAcquire = [&](ImageHandle img,
				VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
				if (!imageWritten[img.id]) return;
				if (imageOwnerStream[img.id] == passStream) return;
				const size_t producerIdx = imageLastProducerIdx[img.id];
				if (producerIdx == SIZE_MAX) return;

				ImageBarrier release{};
				release.image          = img;
				release.srcStage       = lastImageWriter[img.id].stage;
				release.srcAccess      = lastImageWriter[img.id].access;
				release.dstStage       = 0;
				release.dstAccess      = 0;
				release.oldLayout      = currentLayout[img.id];
				release.newLayout      = currentLayout[img.id];   // ownership-only
				release.srcQueueFamily = qfFor(imageOwnerStream[img.id]);
				release.dstQueueFamily = qfFor(passStream);
				m_barriers[producerIdx].imageReleaseBarriers.push_back(release);

				ImageBarrier acquire{};
				acquire.image          = img;
				acquire.srcStage       = 0;
				acquire.srcAccess      = 0;
				acquire.dstStage       = dstStage;
				acquire.dstAccess      = dstAccess;
				acquire.oldLayout      = currentLayout[img.id];
				acquire.newLayout      = currentLayout[img.id];
				acquire.srcQueueFamily = qfFor(imageOwnerStream[img.id]);
				acquire.dstQueueFamily = qfFor(passStream);
				m_barriers[i].imageBarriers.push_back(acquire);

				imageOwnerStream[img.id] = passStream;
			};

			for (const auto& ca : pass.m_colorAttachments) {
				if (ca.target.id < m_images.size()) {
					graphicsAttachmentAcquire(ca.target,
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
					VkImageLayout colorFinal = DetermineColorFinalLayout(i, ca.target);
					currentLayout[ca.target.id] = colorFinal;
					lastImageWriter[ca.target.id] = {
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
					};
					imageWritten[ca.target.id] = true;
					imageOwnerStream[ca.target.id] = passStream;
					imageLastProducerIdx[ca.target.id] = i;
				}
			}

			if (pass.m_hasDepth && pass.m_depthTarget.id < m_images.size()) {
				graphicsAttachmentAcquire(pass.m_depthTarget,
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
				currentLayout[pass.m_depthTarget.id] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				lastImageWriter[pass.m_depthTarget.id] = {
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
				};
				imageWritten[pass.m_depthTarget.id] = true;
				imageOwnerStream[pass.m_depthTarget.id] = passStream;
				imageLastProducerIdx[pass.m_depthTarget.id] = i;
			}

			if (pass.m_hasResolve && pass.m_resolveTarget.id < m_images.size()) {
				graphicsAttachmentAcquire(pass.m_resolveTarget,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
				VkImageLayout resolveFinal = DetermineResolveFinalLayout(i, pass.m_resolveTarget);
				currentLayout[pass.m_resolveTarget.id] = resolveFinal;
				lastImageWriter[pass.m_resolveTarget.id] = {
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				};
				imageWritten[pass.m_resolveTarget.id] = true;
				imageOwnerStream[pass.m_resolveTarget.id] = passStream;
				imageLastProducerIdx[pass.m_resolveTarget.id] = i;
			}
		}
	}
}

void RenderGraph::Compile() {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Compiling ({} passes, {} images, {} buffers)",
		m_declarationOrder.size(), m_images.size(), m_buffers.size());

	// Project each pass's read/write sets down to plain resource ids and hand
	// them to DAGBuilder. RenderGraph is a friend of the pass builders so this
	// is the only place that needs to peek at their private members; DAGBuilder
	// stays decoupled from the pass-builder types.
	DAGBuildInputs dagInputs;
	dagInputs.declarationOrder = m_declarationOrder;
	dagInputs.imageCount  = m_images.size();
	dagInputs.bufferCount = m_buffers.size();
	dagInputs.passAccess.reserve(m_declarationOrder.size());

	for (const auto& ref : m_declarationOrder) {
		PassResourceAccess acc;
		if (ref.type == PassType::Graphics) {
			const auto& pass = *m_graphicsPasses[ref.index];
			acc.queueAffinity = pass.GetQueueAffinity();  // always Graphics for graphics passes
			for (const auto& img : pass.m_readImages)  acc.readImages.push_back(img.id);
			for (const auto& buf : pass.m_readBuffers) acc.readBuffers.push_back(buf.id);
			for (const auto& ca  : pass.m_colorAttachments) acc.writeImages.push_back(ca.target.id);
			if (pass.m_hasDepth)   acc.writeImages.push_back(pass.m_depthTarget.id);
			if (pass.m_hasResolve) acc.writeImages.push_back(pass.m_resolveTarget.id);
			for (const auto& buf : pass.m_writeBuffers) acc.writeBuffers.push_back(buf.id);
		} else {
			const auto& pass = *m_computePasses[ref.index];
			acc.queueAffinity = pass.GetQueueAffinity();
			for (const auto& img : pass.m_readImages)   acc.readImages.push_back(img.id);
			for (const auto& buf : pass.m_readBuffers)  acc.readBuffers.push_back(buf.id);
			for (const auto& img : pass.m_writeImages)  acc.writeImages.push_back(img.id);
			for (const auto& buf : pass.m_writeBuffers) acc.writeBuffers.push_back(buf.id);
		}
		dagInputs.passAccess.push_back(std::move(acc));
	}
	dagInputs.asyncComputeAvailable = m_asyncAvailable;

	// Sink classification: imported and Persistent resources are implicit
	// sinks (their output flows somewhere outside the graph or persists across
	// frames). Plus any explicit MarkSink calls.
	dagInputs.imageIsSink.assign(m_images.size(), false);
	for (size_t i = 0; i < m_images.size(); i++) {
		if (m_images[i].imported) dagInputs.imageIsSink[i] = true;
		if (m_images[i].desc.lifetime == Lifetime::Persistent) dagInputs.imageIsSink[i] = true;
	}
	for (uint32_t id : m_explicitImageSinks) {
		if (id < dagInputs.imageIsSink.size()) dagInputs.imageIsSink[id] = true;
	}
	dagInputs.bufferIsSink.assign(m_buffers.size(), false);
	for (size_t i = 0; i < m_buffers.size(); i++) {
		if (m_buffers[i].imported) dagInputs.bufferIsSink[i] = true;
		if (m_buffers[i].desc.lifetime == Lifetime::Persistent) dagInputs.bufferIsSink[i] = true;
	}
	for (uint32_t id : m_explicitBufferSinks) {
		if (id < dagInputs.bufferIsSink.size()) dagInputs.bufferIsSink[id] = true;
	}

	auto dagResult = DAGBuilder::Build(dagInputs);
	if (dagResult.prunedCount > 0) {
		logger->info("RenderGraph: Pruned {} unreachable pass(es)", dagResult.prunedCount);
	}
	if (dagResult.demotedCount > 0) {
		logger->info("RenderGraph: Demoted {} AsyncCompute pass(es) to graphics queue (no async-capable device or graphics-stream dependency)",
			dagResult.demotedCount);
	}
	m_dag                    = std::move(dagResult.dag);
	m_dagDeclToNode          = std::move(dagResult.declToNode);
	m_executionOrder         = std::move(dagResult.executionOrder);
	m_executionOrderNodes    = std::move(dagResult.executionOrderNodes);
	m_executionOrderStreams  = std::move(dagResult.executionOrderAffinity);

	// Did anything land on the async stream this build?
	m_hasAsyncWork = false;
	for (auto a : m_executionOrderStreams) {
		if (a == QueueAffinity::AsyncCompute) { m_hasAsyncWork = true; break; }
	}

	// Build inverse mapping (node id -> decl idx) and the alive bitset, used by
	// IsImageReadDownstream's forward-BFS traversal to skip pruned descendants.
	m_nodeToDecl.assign(m_dag.NodeCount(), 0);
	for (size_t i = 0; i < m_dagDeclToNode.size(); i++) {
		uint32_t node = m_dagDeclToNode[i];
		if (node != DAGBuilder::INVALID_NODE) m_nodeToDecl[node] = i;
	}
	m_nodeAlive.assign(m_dag.NodeCount(), false);
	for (uint32_t n : m_executionOrderNodes) m_nodeAlive[n] = true;

	AccumulateUsageFlags();
	AllocateTransientResources();
	CreateRenderPasses();
	CreateFramebuffers();
	CreatePipelines();
	ComputeBarriers();
	UpdateBindings();

	m_compiled = true;
	logger->info("RenderGraph: Compiled successfully");
}

// =====================================================================
// RenderGraph — Execute
// =====================================================================

// Helper: build a VkImageMemoryBarrier from our ImageBarrier descriptor.
static VkImageMemoryBarrier MakeImageBarrier(const ImageBarrier& b, const ImageResource& res) {
	VkImageMemoryBarrier imgBarrier{};
	imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgBarrier.srcAccessMask = b.srcAccess;
	imgBarrier.dstAccessMask = b.dstAccess;
	imgBarrier.oldLayout = b.oldLayout;
	imgBarrier.newLayout = b.newLayout;
	imgBarrier.srcQueueFamilyIndex = b.srcQueueFamily;
	imgBarrier.dstQueueFamilyIndex = b.dstQueueFamily;
	imgBarrier.image = res.image ? res.image->Get() : VK_NULL_HANDLE;
	imgBarrier.subresourceRange.aspectMask =
		(res.usageFlags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			? VK_IMAGE_ASPECT_DEPTH_BIT
			: VK_IMAGE_ASPECT_COLOR_BIT;
	imgBarrier.subresourceRange.baseMipLevel = 0;
	imgBarrier.subresourceRange.levelCount = 1;
	imgBarrier.subresourceRange.baseArrayLayer = 0;
	imgBarrier.subresourceRange.layerCount = 1;
	return imgBarrier;
}

static VkBufferMemoryBarrier MakeBufferBarrier(const BufferBarrier& b, const BufferResource& res) {
	VkBufferMemoryBarrier bufBarrier{};
	bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bufBarrier.srcAccessMask = b.srcAccess;
	bufBarrier.dstAccessMask = b.dstAccess;
	bufBarrier.srcQueueFamilyIndex = b.srcQueueFamily;
	bufBarrier.dstQueueFamilyIndex = b.dstQueueFamily;
	bufBarrier.buffer = res.buffer ? res.buffer->Get() : VK_NULL_HANDLE;
	bufBarrier.offset = 0;
	bufBarrier.size = VK_WHOLE_SIZE;
	return bufBarrier;
}

void RenderGraph::RecordStream(std::shared_ptr<VWrap::CommandBuffer> cmd,
                               uint32_t frameIndex, GPUProfiler* profiler,
                               QueueAffinity targetStream) {
	auto vk_cmd = cmd->Get();

	for (size_t i = 0; i < m_executionOrder.size(); i++) {
		if (m_executionOrderStreams[i] != targetStream) continue;

		const auto& ref = m_executionOrder[i];

		bool enabled = true;
		if (ref.type == PassType::Graphics)
			enabled = m_graphicsPasses[ref.index]->IsEnabled();
		else
			enabled = m_computePasses[ref.index]->IsEnabled();
		if (!enabled) continue;

		// Pre-pass barriers: intra-stream + cross-stream acquires.
		for (const auto& barrier : m_barriers[i].imageBarriers) {
			VkImageMemoryBarrier imgBarrier = MakeImageBarrier(barrier, m_images[barrier.image.id]);
			// Acquire halves of an ownership transfer specify srcStage=0; substitute
			// TOP_OF_PIPE_BIT on the source mask so vkCmdPipelineBarrier accepts it.
			VkPipelineStageFlags srcStage = barrier.srcStage ? barrier.srcStage : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			VkPipelineStageFlags dstStage = barrier.dstStage ? barrier.dstStage : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			cmd->CmdPipelineBarrier(srcStage, dstStage, { imgBarrier });
		}
		for (const auto& barrier : m_barriers[i].bufferBarriers) {
			VkBufferMemoryBarrier bufBarrier = MakeBufferBarrier(barrier, m_buffers[barrier.buffer.id]);
			VkPipelineStageFlags srcStage = barrier.srcStage ? barrier.srcStage : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			VkPipelineStageFlags dstStage = barrier.dstStage ? barrier.dstStage : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			cmd->CmdPipelineBarrier(srcStage, dstStage, {}, { bufBarrier });
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
				ctx.scene = m_scene;
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
				ctx.scene = m_scene;
				pass.m_recordFn(ctx);
			}
		}

		if (profiler) profiler->CmdEndPass(cmd, frameIndex, static_cast<uint32_t>(i));

		// Post-pass barriers: cross-stream releases (only populated for passes
		// that produce resources consumed on a different queue stream).
		for (const auto& barrier : m_barriers[i].imageReleaseBarriers) {
			VkImageMemoryBarrier imgBarrier = MakeImageBarrier(barrier, m_images[barrier.image.id]);
			VkPipelineStageFlags srcStage = barrier.srcStage ? barrier.srcStage : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			VkPipelineStageFlags dstStage = barrier.dstStage ? barrier.dstStage : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			cmd->CmdPipelineBarrier(srcStage, dstStage, { imgBarrier });
		}
		for (const auto& barrier : m_barriers[i].bufferReleaseBarriers) {
			VkBufferMemoryBarrier bufBarrier = MakeBufferBarrier(barrier, m_buffers[barrier.buffer.id]);
			VkPipelineStageFlags srcStage = barrier.srcStage ? barrier.srcStage : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			VkPipelineStageFlags dstStage = barrier.dstStage ? barrier.dstStage : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			cmd->CmdPipelineBarrier(srcStage, dstStage, {}, { bufBarrier });
		}
	}
}

void RenderGraph::Execute(std::shared_ptr<VWrap::CommandBuffer> cmd, uint32_t frameIndex,
                          GPUProfiler* profiler) {
	m_graphicsQueueWait.semaphores.clear();
	m_graphicsQueueWait.stages.clear();

	if (m_hasAsyncWork && m_asyncAvailable && frameIndex < m_asyncCmdBuffers.size()) {
		auto asyncCb = m_asyncCmdBuffers[frameIndex];

		// Reset + begin. The previous frame's signal of this slot's semaphore
		// has already been consumed by the previous graphics submit's wait,
		// and the in-flight fence (held by FrameController) guarantees the
		// previous async cb has retired before we re-record this one.
		vkResetCommandBuffer(asyncCb->Get(), 0);
		asyncCb->Begin();
		// Async cb intentionally skips per-pass profiler timestamps: the query
		// pool is reset by the graphics cb (which executes after the async one
		// signals + the graphics submit waits, then runs reset → write). Any
		// timestamps written from the async queue would be wiped before the
		// host could read them. Re-enable when the profiler grows host-side
		// vkResetQueryPool support.
		RecordStream(asyncCb, frameIndex, /*profiler=*/nullptr, QueueAffinity::AsyncCompute);
		asyncCb->End();

		VkSubmitInfo asyncSubmit{};
		asyncSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		VkCommandBuffer asyncCbHandle = asyncCb->Get();
		asyncSubmit.commandBufferCount = 1;
		asyncSubmit.pCommandBuffers = &asyncCbHandle;
		VkSemaphore signalSem = m_asyncDoneSemaphores[frameIndex]->Get();
		asyncSubmit.signalSemaphoreCount = 1;
		asyncSubmit.pSignalSemaphores = &signalSem;
		if (vkQueueSubmit(m_asyncCfg.computeQueue->Get(), 1, &asyncSubmit, VK_NULL_HANDLE) != VK_SUCCESS) {
			throw std::runtime_error("RenderGraph: async-compute submit failed");
		}

		// Conservative wait stage: any consumer-shader / transfer / draw-indirect
		// stage on the graphics queue. Lets graphics fixed-function setup proceed
		// in parallel with async work, only stalling at the consumption boundary.
		m_graphicsQueueWait.semaphores.push_back(signalSem);
		m_graphicsQueueWait.stages.push_back(
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
			VK_PIPELINE_STAGE_TRANSFER_BIT |
			VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
	}

	RecordStream(cmd, frameIndex, profiler, QueueAffinity::Graphics);
}

// =====================================================================
// RenderGraph — Resize
// =====================================================================

void RenderGraph::Resize(VkExtent2D newExtent) {
	auto logger = spdlog::get("Render");
	logger->info("RenderGraph: Resizing to {}x{}", newExtent.width, newExtent.height);

	for (auto& res : m_images) {
		if (res.imported) continue;
		// Persistent resources keep their VkImage across Resize — used for data
		// that no producer pass refills (e.g. uploaded .vox volumes).
		if (res.desc.lifetime == Lifetime::Persistent) continue;
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
		if (res.desc.lifetime == Lifetime::Persistent) continue;
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
	m_declarationOrder.clear();
	m_executionOrder.clear();
	m_executionOrderNodes.clear();
	m_executionOrderStreams.clear();
	m_hasAsyncWork = false;
	m_dag.Clear();
	m_dagDeclToNode.clear();
	m_nodeToDecl.clear();
	m_nodeAlive.clear();
	m_explicitImageSinks.clear();
	m_explicitBufferSinks.clear();
	m_barriers.clear();
	m_graphicsQueueWait.semaphores.clear();
	m_graphicsQueueWait.stages.clear();
	m_compiled = false;

	// Bump generation counters so any handle that survives this Clear() fails
	// the gen check in subsequent Get*() calls.
	m_imageGen++;
	m_bufferGen++;
}
