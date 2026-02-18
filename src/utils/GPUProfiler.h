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
	uint32_t m_num_frames = 0;
	uint32_t m_pass_count = 0;

	std::chrono::steady_clock::time_point m_start_time;
	float m_fps = 0.0f;
	uint32_t m_frame_count = 0;

	uint32_t QueryCount() const { return 2 + m_pass_count * 2; }

	void DestroyPools() {
		for (auto pool : m_query_pools)
			if (pool != VK_NULL_HANDLE) vkDestroyQueryPool(m_device->Get(), pool, nullptr);
		m_query_pools.clear();
		m_pool_ready.clear();
	}

	void CreatePools() {
		m_query_pools.resize(m_num_frames, VK_NULL_HANDLE);
		m_pool_ready.resize(m_num_frames, false);

		VkQueryPoolCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = QueryCount();

		for (auto& pool : m_query_pools)
			vkCreateQueryPool(m_device->Get(), &info, nullptr, &pool);
	}

public:

	static std::shared_ptr<GPUProfiler> Create(std::shared_ptr<VWrap::Device> device, uint32_t num_frames) {
		auto ret = std::make_shared<GPUProfiler>();
		ret->m_device = device;
		ret->m_num_frames = num_frames;
		ret->CreatePools();

		VkPhysicalDeviceProperties deviceProperties;
		vkGetPhysicalDeviceProperties(device->GetPhysicalDevice()->Get(), &deviceProperties);
		ret->m_timestamp_period = deviceProperties.limits.timestampPeriod;
		ret->m_start_time = std::chrono::high_resolution_clock::now();

		return ret;
	}

	void SetPassCount(uint32_t count) {
		if (count == m_pass_count) return;
		vkDeviceWaitIdle(m_device->Get());
		DestroyPools();
		m_pass_count = count;
		CreatePools();
	}

	// ---- Whole-frame timing ----

	void CmdBegin(std::shared_ptr<VWrap::CommandBuffer> buffer, uint32_t frame) {
		m_frame_count++;
		auto current_time = std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> elapsed_time = current_time - m_start_time;
		if (elapsed_time.count() >= 500) {
			m_fps = m_frame_count * 2.0f;
			m_frame_count = 0;
			m_start_time = current_time;
		}

		vkCmdResetQueryPool(buffer->Get(), m_query_pools[frame], 0, QueryCount());
		vkCmdWriteTimestamp(buffer->Get(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_query_pools[frame], 0);
	}

	void CmdEnd(std::shared_ptr<VWrap::CommandBuffer> buffer, uint32_t frame) {
		vkCmdWriteTimestamp(buffer->Get(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pools[frame], 1);
		m_pool_ready[frame] = true;
	}

	// ---- Per-pass timing ----

	void CmdBeginPass(std::shared_ptr<VWrap::CommandBuffer> buffer, uint32_t frame, uint32_t passIndex) {
		if (passIndex >= m_pass_count) return;
		uint32_t queryIndex = 2 + passIndex * 2;
		vkCmdWriteTimestamp(buffer->Get(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_query_pools[frame], queryIndex);
	}

	void CmdEndPass(std::shared_ptr<VWrap::CommandBuffer> buffer, uint32_t frame, uint32_t passIndex) {
		if (passIndex >= m_pass_count) return;
		uint32_t queryIndex = 2 + passIndex * 2 + 1;
		vkCmdWriteTimestamp(buffer->Get(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pools[frame], queryIndex);
	}

	// ---- Results ----

	struct PerformanceMetrics {
		float fps = 0.0f;
		float render_time = 0.0f;
		std::vector<float> passTimesMs;
	};

	PerformanceMetrics GetMetrics(uint32_t frame) {
		PerformanceMetrics metrics;
		metrics.fps = m_fps;

		if (!m_pool_ready[frame])
			return metrics;

		uint32_t totalQueries = QueryCount();
		// Each query has [timestamp, availability]
		std::vector<uint64_t> data(totalQueries * 2, 0);
		vkGetQueryPoolResults(m_device->Get(),
			m_query_pools[frame],
			0, totalQueries,
			data.size() * sizeof(uint64_t),
			data.data(), sizeof(uint64_t) * 2,
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

		// Whole-frame time (queries 0 and 1)
		if (data[1] && data[3]) {
			uint64_t delta = data[2] - data[0];
			metrics.render_time = delta * m_timestamp_period * 1e-6f;
		}

		// Per-pass times
		metrics.passTimesMs.resize(m_pass_count, 0.0f);
		for (uint32_t p = 0; p < m_pass_count; p++) {
			uint32_t beginIdx = (2 + p * 2) * 2;     // index into data array (each query = 2 uint64)
			uint32_t endIdx   = (2 + p * 2 + 1) * 2;
			uint64_t beginAvail = data[beginIdx + 1];
			uint64_t endAvail   = data[endIdx + 1];
			if (beginAvail && endAvail) {
				uint64_t delta = data[endIdx] - data[beginIdx];
				metrics.passTimesMs[p] = delta * m_timestamp_period * 1e-6f;
			}
		}

		return metrics;
	}

	~GPUProfiler() {
		DestroyPools();
	}
};
