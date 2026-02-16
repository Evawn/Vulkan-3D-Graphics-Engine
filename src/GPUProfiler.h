#pragma once
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include "Device.h"
#include "CommandBuffer.h"
#include <chrono>
#include <spdlog/spdlog.h>

class GPUProfiler
{
private:

	std::vector<VkQueryPool> m_query_pools;
	std::vector<bool> m_pool_ready;
	std::shared_ptr<VWrap::Device> m_device;
	float m_timestamp_period = 1.0f;

	std::chrono::steady_clock::time_point m_start_time;
	float m_fps;
	uint32_t m_frame_count = 0;

public:

	static std::shared_ptr<GPUProfiler> Create(std::shared_ptr<VWrap::Device> device, uint32_t num_frames) {
		auto ret = std::make_shared<GPUProfiler>();
		ret->m_device = device;
		ret->m_query_pools.resize(num_frames);
		ret->m_pool_ready.resize(num_frames, false);

		VkQueryPoolCreateInfo queryPoolInfo = {};
		queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		queryPoolInfo.queryCount = 2;

		for (auto& pool : ret->m_query_pools) {
			vkCreateQueryPool(device->Get(), &queryPoolInfo, nullptr, &pool);
		}

		VkPhysicalDeviceProperties deviceProperties;
		vkGetPhysicalDeviceProperties(device->GetPhysicalDevice()->Get(), &deviceProperties);
		ret->m_timestamp_period = deviceProperties.limits.timestampPeriod;
		ret->m_start_time = std::chrono::high_resolution_clock::now();

		return ret;
	}

	void CmdBegin(std::shared_ptr<VWrap::CommandBuffer> buffer, uint32_t frame) {

		m_frame_count++;
		auto current_time = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> elapsed_time = current_time - m_start_time;
		if (elapsed_time.count() >= 500) {
			m_fps = m_frame_count * 2.0f;
			m_frame_count = 0;
			m_start_time = current_time;
		}

		vkCmdResetQueryPool(buffer->Get(), m_query_pools[frame], 0, 2);
		vkCmdWriteTimestamp(buffer->Get(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_query_pools[frame], 0);
	}

	void CmdEnd(std::shared_ptr<VWrap::CommandBuffer> buffer, uint32_t frame) {
		vkCmdWriteTimestamp(buffer->Get(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pools[frame], 1);
		m_pool_ready[frame] = true;
	}

	struct PerformanceMetrics {
		float fps, render_time;
	};

	PerformanceMetrics GetMetrics(uint32_t frame) {
		if (!m_pool_ready[frame]) {
			return PerformanceMetrics(m_fps, 0.0f);
		}

		uint64_t data[4]; // [timestamp0, avail0, timestamp1, avail1]
		vkGetQueryPoolResults(m_device->Get(),
			m_query_pools[frame],
			0, 2,
			sizeof(data),
			data, sizeof(uint64_t) * 2,
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

		float timeTakenMilliseconds = 0.0f;
		if (data[1] && data[3]) {
			uint64_t timeTakenNanoseconds = data[2] - data[0];
			timeTakenMilliseconds = timeTakenNanoseconds * m_timestamp_period * 1e-6f;
		}

		return PerformanceMetrics(m_fps, timeTakenMilliseconds);
	}

	~GPUProfiler() {
		for(auto pool : m_query_pools)
			if(pool != VK_NULL_HANDLE) vkDestroyQueryPool(m_device->Get(), pool, nullptr);
	}
};

