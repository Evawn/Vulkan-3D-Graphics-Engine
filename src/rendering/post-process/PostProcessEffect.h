#pragma once

#include "RenderGraphTypes.h"
#include "RenderTechnique.h"
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
class PostProcessEffect {
public:
	virtual ~PostProcessEffect() = default;

	virtual std::string GetName() const = 0;

	// Register all passes + allocate owned resources. Called once per graph build.
	virtual ImageHandle RegisterPasses(
		RenderGraph& graph,
		const PostProcessContext& ctx,
		ImageHandle input,
		VkExtent2D extent) = 0;

	// Called after graph.Compile(), when graph-allocated image views are available.
	virtual void WriteGraphDescriptors(RenderGraph& graph) {}

	// Called when the offscreen viewport is resized. Descriptors usually need a
	// re-write because graph image views are recreated.
	virtual void OnResize(VkExtent2D newExtent, RenderGraph& graph) { (void)newExtent; WriteGraphDescriptors(graph); }

	// Exposed for ImGui — same mechanism as RenderTechnique.
	virtual std::vector<TechniqueParameter>& GetParameters() {
		static std::vector<TechniqueParameter> empty; return empty;
	}

	// SPV paths compiled by this effect — lets the shader hot-reload machinery
	// know what to recompile. Matches RenderTechnique::GetShaderPaths().
	virtual std::vector<std::string> GetShaderPaths() const { return {}; }

	// Recreate pipelines after a hot-reload. Optional override.
	virtual void RecreatePipelines() {}

	bool IsEnabled() const { return m_enabled; }
	void SetEnabled(bool v) { m_enabled = v; }

protected:
	bool m_enabled = true;
};
