#include "Buffer.h"

namespace VWrap {

    std::shared_ptr<Buffer> Buffer::Create(std::shared_ptr<Allocator> allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VmaAllocationCreateFlags flags) {

        auto ret = std::make_shared<Buffer>();

        ret->m_allocator = allocator;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.usage = usage;
        bufferInfo.size = size;
        bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;

        QueueFamilyIndices indices = allocator->GetDevice()->GetPhysicalDevice()->FindQueueFamilies();
        bufferInfo.queueFamilyIndexCount = 2;
        uint32_t queueFamilyIndices[2] = { indices.graphicsFamily.value(), indices.transferFamily.value() };
        bufferInfo.pQueueFamilyIndices = queueFamilyIndices;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.requiredFlags = properties;
        allocInfo.flags = flags;

        if (vmaCreateBuffer(allocator->Get(), &bufferInfo, &allocInfo, &ret->m_buffer, &ret->m_allocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create buffer");
        }

        return ret;
    }

    std::shared_ptr<Buffer> Buffer::CreateStaging(std::shared_ptr<Allocator> allocator,
        VkDeviceSize size) {
        return Create(
            allocator, 
            size, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }

    std::shared_ptr<Buffer> Buffer::CreateMapped(std::shared_ptr<Allocator> allocator, 
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        void*& data) {

        auto ret = std::make_shared<Buffer>();
        ret->m_allocator = allocator;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.usage = usage;
        bufferInfo.size = size;
        bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;

        QueueFamilyIndices indices = allocator->GetDevice()->GetPhysicalDevice()->FindQueueFamilies();
        bufferInfo.queueFamilyIndexCount = 2;
        uint32_t queueFamilyIndices[2] = { indices.graphicsFamily.value(), indices.transferFamily.value() };
        bufferInfo.pQueueFamilyIndices = queueFamilyIndices;

        VmaAllocationCreateInfo allocCreateInfo = {};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.requiredFlags = properties;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfo;
        if (vmaCreateBuffer(allocator->Get(), &bufferInfo, &allocCreateInfo, &ret->m_buffer, &ret->m_allocation, &allocInfo) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create buffer");
        }
        data = allocInfo.pMappedData;
        return ret;
    }


	Buffer::~Buffer() {
		if (m_buffer != VK_NULL_HANDLE && m_allocation != nullptr)
            vmaDestroyBuffer(m_allocator->Get(), m_buffer, m_allocation);
	}
}