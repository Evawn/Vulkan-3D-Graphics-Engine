#pragma once

#include "RenderGraphTypes.h"
#include "Inspectable.h"
#include "SceneLighting.h"
#include "Device.h"
#include "Allocator.h"
#include "Camera.h"
#include <memory>
#include <string>
#include <vector>

class RenderGraph;

// Context handed to each post-process effect when it registers passes and allocates
// pipelines. Lifetime is at least as long as the effect itself — the pointers inside
// point to resources owned by the Renderer / Application.
struct PostProcessContext {
	std::shared_ptr<VWrap::Device> device;
	std::shared_ptr<VWrap::Allocator> allocator;
	VkFormat sceneFormat;
	uint32_t maxFramesInFlight;
	std::shared_ptr<Camera> camera;
	SceneLighting* lighting = nullptr;  // shared, non-owning
};

// A single post-processing effect — e.g. bloom, tonemap, lens flare. Effects own
// their own pipelines, descriptor layouts, samplers, and transient intermediate
// images. The RenderGraph handles all barriers/layout transitions; effects only
// need to declare .Read() / .SetColorAttachment() on the passes they register.
//
// Contract: RegisterPasses() takes an input scene-image handle and returns the
// handle of the final image this effect produced. The chain threads the outputs
// of one effect into the next.
class PostProcessEffect : public IInspectable {
public:
	virtual ~PostProcessEffect() = default;

	// Display name for the inspector (also exposed as GetName for legacy callers).
	virtual std::string GetName() const = 0;
	std::string GetDisplayName() const override { return GetName(); }

	// Register all passes + allocate owned resources. Called once per graph build.
	virtual ImageHandle RegisterPasses(
		RenderGraph& graph,
		const PostProcessContext& ctx,
		ImageHandle input,
		VkExtent2D extent) = 0;

	// Called when the offscreen viewport is resized. Default: no-op (BindingTable
	// re-applies descriptors automatically; effects only need to override if they
	// hold extra view-dependent state).
	virtual void OnResize(VkExtent2D newExtent, RenderGraph& graph) {
		(void)newExtent; (void)graph;
	}

	// SPV paths compiled by this effect — lets the shader hot-reload machinery
	// know what to recompile.
	virtual std::vector<std::string> GetShaderPaths() const { return {}; }

	bool IsEnabled() const { return m_enabled; }
	void SetEnabled(bool v) { m_enabled = v; }

protected:
	bool m_enabled = true;
};
