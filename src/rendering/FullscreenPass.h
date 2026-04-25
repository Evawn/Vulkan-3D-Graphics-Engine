#pragma once

#include "RenderGraphTypes.h"
#include "Device.h"
#include "Sampler.h"
#include "Pipeline.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "DescriptorSet.h"
#include "RenderPass.h"
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
// The graph handles barriers, framebuffer creation, and final layouts. The
// FullscreenPass handles descriptor-set wiring and pipeline construction.
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
// One instance owns the pipeline, descriptor pool/sets, and a captured
// shared_ptr to the graph's render pass. Created via Build() during the
// effect's RegisterPasses, descriptors written via WriteDescriptors() during
// WriteGraphDescriptors(), pipeline recreated via RecreatePipeline() during
// hot-reload.
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

	// Recreate the pipeline (typically called after hot-reload). Reuses the
	// render pass captured at Build() time, which is render-pass-compatible
	// with whatever the graph holds after Compile() (same attachment formats,
	// samples, and counts).
	void RecreatePipeline();

	std::shared_ptr<VWrap::RenderPass> GetRenderPass() const { return m_renderPass; }
	std::shared_ptr<VWrap::Pipeline> GetPipeline() const { return m_pipeline; }

private:
	std::shared_ptr<VWrap::Device> m_device;
	VkExtent2D m_extent{};
	uint32_t m_maxFramesInFlight = 0;

	std::vector<ImageHandle> m_inputs;
	std::shared_ptr<VWrap::Sampler> m_sampler;

	std::string m_fragShaderSpv;
	uint32_t m_pushConstantSize = 0;

	std::shared_ptr<VWrap::RenderPass> m_renderPass;
	std::shared_ptr<VWrap::DescriptorSetLayout> m_descriptorLayout;
	std::shared_ptr<VWrap::DescriptorPool> m_descriptorPool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_descriptorSets;
	std::shared_ptr<VWrap::Pipeline> m_pipeline;
};
