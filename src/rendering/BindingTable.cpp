#include "BindingTable.h"
#include "RenderGraph.h"
#include <cassert>

BindingTable::BindingTable(std::shared_ptr<VWrap::Device> device, uint32_t setCount)
	: m_device(device), m_setCount(setCount) {}

BindingTable& BindingTable::AddBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
	VkDescriptorSetLayoutBinding b{};
	b.binding = binding;
	b.descriptorCount = 1;
	b.descriptorType = type;
	b.stageFlags = stages;
	m_layoutBindings.push_back(b);
	return *this;
}

BindingTable& BindingTable::BindGraphSampledImage(uint32_t binding, ImageHandle h,
                                                   std::shared_ptr<VWrap::Sampler> sampler) {
	m_sources.push_back(GraphSampledImage{ binding, h, std::move(sampler) });
	return *this;
}

BindingTable& BindingTable::BindGraphStorageImage(uint32_t binding, ImageHandle h) {
	m_sources.push_back(GraphStorageImage{ binding, h });
	return *this;
}

BindingTable& BindingTable::BindGraphStorageBuffer(uint32_t binding, BufferHandle h) {
	m_sources.push_back(GraphStorageBuffer{ binding, h });
	return *this;
}

BindingTable& BindingTable::BindExternalSampledImage(uint32_t binding,
                                                      std::shared_ptr<VWrap::ImageView> view,
                                                      std::shared_ptr<VWrap::Sampler> sampler,
                                                      VkImageLayout layout) {
	m_sources.push_back(ExternalSampledImage{ binding, std::move(view), std::move(sampler), layout });
	return *this;
}

BindingTable& BindingTable::BindUniformBufferPerFrame(uint32_t binding,
                                                       std::vector<std::shared_ptr<VWrap::Buffer>> buffers,
                                                       VkDeviceSize range) {
	assert(buffers.size() == m_setCount && "per-frame buffer count must match setCount");
	m_sources.push_back(UniformBufferPerFrame{ binding, std::move(buffers), range });
	return *this;
}

void BindingTable::ReplaceExternalSampledImage(uint32_t binding,
                                                std::shared_ptr<VWrap::ImageView> view,
                                                std::shared_ptr<VWrap::Sampler> sampler,
                                                VkImageLayout layout) {
	for (auto& src : m_sources) {
		if (auto* p = std::get_if<ExternalSampledImage>(&src); p && p->binding == binding) {
			p->view = std::move(view);
			p->sampler = std::move(sampler);
			p->layout = layout;
			return;
		}
	}
	assert(false && "ReplaceExternalSampledImage: binding not found");
}

void BindingTable::Build() {
	m_layout = VWrap::DescriptorSetLayout::Create(m_device, m_layoutBindings);

	std::vector<VkDescriptorPoolSize> poolSizes;
	for (const auto& b : m_layoutBindings) {
		poolSizes.push_back({ b.descriptorType, m_setCount });
	}
	m_pool = VWrap::DescriptorPool::Create(m_device, poolSizes, m_setCount, 0);

	std::vector<std::shared_ptr<VWrap::DescriptorSetLayout>> layouts(m_setCount, m_layout);
	m_sets = VWrap::DescriptorSet::CreateMany(m_pool, layouts);
}

void BindingTable::Update(const RenderGraph& graph) {
	// Build all VkWriteDescriptorSets for every set, hand off in one batched
	// vkUpdateDescriptorSets call. Image/buffer infos are stored in vectors
	// pre-sized so interior pointers stay stable.
	for (uint32_t setIdx = 0; setIdx < m_setCount; setIdx++) {
		std::vector<VkDescriptorImageInfo> imageInfos;
		std::vector<VkDescriptorBufferInfo> bufferInfos;
		// Reserve enough so emplace_back never reallocates underneath the
		// VkWriteDescriptorSet pointers.
		imageInfos.reserve(m_sources.size());
		bufferInfos.reserve(m_sources.size());

		std::vector<VkWriteDescriptorSet> writes;
		writes.reserve(m_sources.size());

		auto setHandle = m_sets[setIdx]->Get();

		for (const auto& src : m_sources) {
			VkWriteDescriptorSet w{};
			w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w.dstSet = setHandle;
			w.descriptorCount = 1;
			w.dstArrayElement = 0;

			if (auto p = std::get_if<GraphSampledImage>(&src)) {
				VkDescriptorImageInfo info{};
				info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				info.imageView = graph.GetImageView(p->handle)->Get();
				info.sampler = p->sampler->Get();
				imageInfos.push_back(info);
				w.dstBinding = p->binding;
				w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				w.pImageInfo = &imageInfos.back();
			}
			else if (auto p = std::get_if<GraphStorageImage>(&src)) {
				VkDescriptorImageInfo info{};
				info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				info.imageView = graph.GetImageView(p->handle)->Get();
				imageInfos.push_back(info);
				w.dstBinding = p->binding;
				w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				w.pImageInfo = &imageInfos.back();
			}
			else if (auto p = std::get_if<GraphStorageBuffer>(&src)) {
				VkDescriptorBufferInfo info{};
				info.buffer = graph.GetBuffer(p->handle)->Get();
				info.offset = 0;
				info.range = VK_WHOLE_SIZE;
				bufferInfos.push_back(info);
				w.dstBinding = p->binding;
				w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				w.pBufferInfo = &bufferInfos.back();
			}
			else if (auto p = std::get_if<ExternalSampledImage>(&src)) {
				VkDescriptorImageInfo info{};
				info.imageLayout = p->layout;
				info.imageView = p->view->Get();
				info.sampler = p->sampler->Get();
				imageInfos.push_back(info);
				w.dstBinding = p->binding;
				w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				w.pImageInfo = &imageInfos.back();
			}
			else if (auto p = std::get_if<UniformBufferPerFrame>(&src)) {
				VkDescriptorBufferInfo info{};
				info.buffer = p->buffers[setIdx]->Get();
				info.offset = 0;
				info.range = p->range;
				bufferInfos.push_back(info);
				w.dstBinding = p->binding;
				w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				w.pBufferInfo = &bufferInfos.back();
			}

			writes.push_back(w);
		}

		if (!writes.empty()) {
			vkUpdateDescriptorSets(m_device->Get(),
				static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
	}
}
