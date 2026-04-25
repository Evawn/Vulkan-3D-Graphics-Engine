#pragma once

#include "RenderGraphTypes.h"
#include "BindingTable.h"
#include "Device.h"
#include "Sampler.h"
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
// Just a thin convenience wrapper now: builds a BindingTable for the sampled
// inputs, registers the pass with the graph, and hands the graph the pipeline
// desc factory. Pipeline ownership lives in the graph; descriptor writes are
// auto-applied by the graph after Compile()/Resize().
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

private:
	std::shared_ptr<BindingTable> m_bindings;
};
