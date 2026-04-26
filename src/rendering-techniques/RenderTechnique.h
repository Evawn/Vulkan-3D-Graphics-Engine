#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <vulkan/vulkan.h>
#include "Device.h"
#include "Allocator.h"
#include "CommandPool.h"
#include "CommandBuffer.h"
#include "RenderPass.h"
#include "Camera.h"
#include "Inspectable.h"
#include "AppEvent.h"
#include "RenderGraphTypes.h"

class RenderGraph;
class RenderScene;
struct SceneLighting;

struct RenderContext {
	std::shared_ptr<VWrap::Device> device;
	std::shared_ptr<VWrap::Allocator> allocator;
	std::shared_ptr<VWrap::CommandPool> graphicsPool;
	std::shared_ptr<VWrap::CommandPool> computePool;
	VkExtent2D extent;
	uint32_t maxFramesInFlight;
	std::shared_ptr<Camera> camera;
	SceneLighting* lighting = nullptr;  // shared, non-owning; renderer-owned state

	// Per-frame scene the technique reads / writes. RenderingSystem owns it,
	// clears it before EmitItems(), and forwards it through both this context
	// (so RegisterPasses can capture it) and PassContext::scene (so record
	// callbacks can iterate items). The future scene graph replaces the
	// per-technique EmitItems shim with a graph-driven traversal that fills
	// this same RenderScene; nothing on the consumer side changes.
	RenderScene* scene = nullptr;
};

// Capabilities the Renderer exposes to techniques so DescribeTargets() can pick
// matching formats / sample counts without each technique hardcoding them.
struct RendererCaps {
	VkFormat              swapchainFormat = VK_FORMAT_UNDEFINED;
	VkFormat              depthFormat     = VK_FORMAT_UNDEFINED;
	VkSampleCountFlagBits msaaSamples     = VK_SAMPLE_COUNT_1_BIT;
	uint32_t              maxFramesInFlight = 1;
};

// What the technique tells the Renderer to allocate for it. The Renderer owns
// the images; the technique is handed the resulting handles via TechniqueTargets.
struct RenderTargetDesc {
	struct ColorAttachment {
		VkFormat              format       = VK_FORMAT_UNDEFINED;
		VkSampleCountFlagBits samples      = VK_SAMPLE_COUNT_1_BIT;
		bool                  needsResolve = false;   // non-1x samples → also allocate a 1x resolve target
	};
	ColorAttachment       color{};
	bool                  hasDepth     = false;
	VkFormat              depthFormat  = VK_FORMAT_UNDEFINED;
	VkSampleCountFlagBits depthSamples = VK_SAMPLE_COUNT_1_BIT;
};

// What the Renderer hands back after allocating the targets. Handles for any
// attachment the technique didn't request stay default-constructed (id == UINT32_MAX).
struct TechniqueTargets {
	ImageHandle color;
	ImageHandle resolve;       // valid only when desc.color.needsResolve
	ImageHandle depth;         // valid only when desc.hasDepth
};

class RenderTechnique : public IInspectable {
public:
	virtual ~RenderTechnique() = default;

	// IInspectable: GetDisplayName, GetParameters

	// ---- Rendering ----
	// Tell the Renderer what scene-image stack this technique needs. Called once
	// per graph build, before RegisterPasses. The Renderer allocates the images
	// and passes the handles back via RegisterPasses(...).
	virtual RenderTargetDesc DescribeTargets(const RendererCaps& caps) const = 0;

	virtual void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		const TechniqueTargets& targets) = 0;

	// Called once after graph.Compile(); resources allocated by the graph are now
	// available. BindingTable handles descriptor writes automatically — this hook
	// is for one-shot post-compile work like seeding storage buffers/images.
	virtual void OnPostCompile(RenderGraph& graph) { (void)graph; }

	// Per-frame: drop RenderItems into the scene that the passes registered by
	// this technique will consume. RenderingSystem clears the scene each frame
	// before invoking this on every active technique.
	//
	// This virtual is the seam the future scene graph will plug into — when it
	// lands, a single graph-traversal-emits-items call replaces the per-technique
	// loop in RenderingSystem::DrawFrame. Techniques that *consume* items (rather
	// than *own* their geometry) won't override this at all; they only declare
	// .AcceptsItemTypes(...) and iterate scene->Get(...) in their record callback.
	virtual void EmitItems(RenderScene& scene, const RenderContext& ctx) {
		(void)scene; (void)ctx;
	}

	// ---- Hot-reload + metrics ----
	virtual std::vector<std::string> GetShaderPaths() const = 0;
	virtual FrameStats GetFrameStats() const { return {}; }

	// ---- Reload (event-driven; Application reposts ReloadTechnique back here) ----
	void SetEventSink(std::function<void(AppEvent)> sink) { m_eventSink = std::move(sink); }
	virtual void Reload(const RenderContext& ctx) { (void)ctx; }

protected:
	// Push events back to the application — request reload, request rebuild,
	// request pipeline recreate, etc. Set once by Application after construction.
	std::function<void(AppEvent)> m_eventSink;
};
