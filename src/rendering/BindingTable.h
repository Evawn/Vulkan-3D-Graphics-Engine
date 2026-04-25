#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "Device.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "DescriptorSet.h"
#include "ImageView.h"
#include "Sampler.h"
#include "Buffer.h"
#include "RenderGraphTypes.h"

class RenderGraph;

// ---- BindingTable ----
//
// Declarative descriptor-set wiring. The technique describes once, in one
// place, what each binding points at; the graph re-applies the writes after
// Compile() and after every Resize() so views/buffers are always in sync.
//
// Bindings can target either:
//   - a graph-managed image/buffer (resolved at Update() time via the graph)
//   - an external image+sampler (e.g. a palette texture the technique owns)
//   - a per-frame uniform buffer set (one VkBuffer per frame in flight)
//
// The technique still writes the buffer *contents* per frame; the table only
// wires the descriptor that points at the buffer.
class BindingTable {
public:
	BindingTable(std::shared_ptr<VWrap::Device> device, uint32_t setCount);

	// ---- Layout declaration ----
	// Adds (binding, type, stages) to the descriptor-set layout. Must be called
	// for every binding that Bind*() will subsequently target.
	BindingTable& AddBinding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages);

	// ---- Bind sources ----
	BindingTable& BindGraphSampledImage(uint32_t binding, ImageHandle h,
	                                     std::shared_ptr<VWrap::Sampler> sampler);
	BindingTable& BindGraphStorageImage(uint32_t binding, ImageHandle h);
	BindingTable& BindGraphStorageBuffer(uint32_t binding, BufferHandle h);

	// External (non-graph) sampled image — the technique keeps lifetime ownership.
	BindingTable& BindExternalSampledImage(uint32_t binding,
	                                        std::shared_ptr<VWrap::ImageView> view,
	                                        std::shared_ptr<VWrap::Sampler> sampler,
	                                        VkImageLayout layout);

	// Per-frame uniform buffer: one VkBuffer per frame in flight.
	// `buffers.size()` must equal the table's setCount.
	BindingTable& BindUniformBufferPerFrame(uint32_t binding,
	                                         std::vector<std::shared_ptr<VWrap::Buffer>> buffers,
	                                         VkDeviceSize range);

	// Mutates the existing ExternalSampledImage source at `binding` without
	// rebuilding the layout/pool/sets. The new view+sampler take effect on the
	// next Update(). Used for hot-swap of technique-owned textures.
	void ReplaceExternalSampledImage(uint32_t binding,
	                                 std::shared_ptr<VWrap::ImageView> view,
	                                 std::shared_ptr<VWrap::Sampler> sampler,
	                                 VkImageLayout layout);

	// Build the descriptor layout, pool, and sets. Call once after all
	// AddBinding/Bind* calls. Update() can be called repeatedly after this.
	void Build();

	// Re-run vkUpdateDescriptorSets for every declared binding. Graph-managed
	// bindings re-resolve their views/buffers from the graph (which may have
	// recreated them on Resize).
	void Update(const RenderGraph& graph);

	std::shared_ptr<VWrap::DescriptorSetLayout> GetLayout() const { return m_layout; }
	std::shared_ptr<VWrap::DescriptorSet> GetSet(uint32_t frame) const { return m_sets[frame]; }

private:
	struct GraphSampledImage { uint32_t binding; ImageHandle handle; std::shared_ptr<VWrap::Sampler> sampler; };
	struct GraphStorageImage { uint32_t binding; ImageHandle handle; };
	struct GraphStorageBuffer { uint32_t binding; BufferHandle handle; };
	struct ExternalSampledImage { uint32_t binding; std::shared_ptr<VWrap::ImageView> view; std::shared_ptr<VWrap::Sampler> sampler; VkImageLayout layout; };
	struct UniformBufferPerFrame { uint32_t binding; std::vector<std::shared_ptr<VWrap::Buffer>> buffers; VkDeviceSize range; };

	using Source = std::variant<GraphSampledImage, GraphStorageImage, GraphStorageBuffer,
	                            ExternalSampledImage, UniformBufferPerFrame>;

	std::shared_ptr<VWrap::Device> m_device;
	uint32_t m_setCount;

	std::vector<VkDescriptorSetLayoutBinding> m_layoutBindings;
	std::vector<Source> m_sources;

	std::shared_ptr<VWrap::DescriptorSetLayout> m_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_pool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_sets;
};
