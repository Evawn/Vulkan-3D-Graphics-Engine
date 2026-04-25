#pragma once

#include "RenderGraphTypes.h"
#include "Device.h"
#include "Sampler.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "DescriptorSet.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RenderGraph;

// ---- Description struct ----
//
// Captures everything a "single fullscreen quad pass" needs:
//   - one output color attachment
//   - N sampled inputs (bound to descriptor set bindings 0..N-1)
//   - one fragment shader (the vertex shader is always post_fullscreen.vert.spv)
//   - an optional push-constant block on the FRAGMENT stage
//
// The graph handles barriers, framebuffer creation, final layouts, and pipeline
// construction. The FullscreenPass handles descriptor-set wiring and forwards
// the pipeline desc to the graph.
struct FullscreenPassDesc {
	std::string name;
	ImageHandle output;
	std::vector<ImageHandle> sampledInputs;     // descriptor bindings 0..N-1
	std::shared_ptr<VWrap::Sampler> sampler;    // used for every sampled input
	std::string fragShaderSpv;                  // absolute path to .frag.spv
	uint32_t pushConstantSize = 0;              // 0 = no push block
	LoadOp loadOp = LoadOp::Clear;
	StoreOp storeOp = StoreOp::Store;
	VkClearColorValue clearColor{ { 0.0f, 0.0f, 0.0f, 1.0f } };
};

// ---- The pass itself ----
//
// Owns the descriptor layout/pool/sets and the per-frame descriptor writes for
// the fullscreen pass. Pipeline ownership lives in the graph (built post-Compile
// against the canonical render pass, rebuilt on hot-reload via
// RenderGraph::RecreatePipelines()).
class FullscreenPass {
public:
	// Per-draw callback invoked between BindDescriptorSets and Draw. Use it to
	// push constants whose values depend on per-frame state (camera, time, etc.).
	using PushFn = std::function<void(PassContext& ctx, VkPipelineLayout layout)>;

	static std::unique_ptr<FullscreenPass> Build(
		RenderGraph& graph,
		std::shared_ptr<VWrap::Device> device,
		VkExtent2D extent,
		uint32_t maxFramesInFlight,
		const FullscreenPassDesc& desc,
		PushFn pushFn);

	// Re-bind every sampled input to its current graph image view. Call this
	// from PostProcessEffect::WriteGraphDescriptors and on resize.
	void WriteDescriptors(RenderGraph& graph);

private:
	std::shared_ptr<VWrap::Device> m_device;

	std::vector<ImageHandle> m_inputs;
	std::shared_ptr<VWrap::Sampler> m_sampler;

	std::shared_ptr<VWrap::DescriptorSetLayout> m_descriptorLayout;
	std::shared_ptr<VWrap::DescriptorPool> m_descriptorPool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_descriptorSets;
};
