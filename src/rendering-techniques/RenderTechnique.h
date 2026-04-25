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

class RenderGraph;
struct ImageHandle;
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
};

class RenderTechnique : public IInspectable {
public:
	virtual ~RenderTechnique() = default;

	// IInspectable: GetDisplayName, GetParameters

	// ---- Rendering ----
	virtual void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		ImageHandle colorTarget,
		ImageHandle depthTarget,
		ImageHandle resolveTarget) = 0;

	// Called once after graph.Compile(); resources allocated by the graph are now
	// available. BindingTable handles descriptor writes automatically — this hook
	// is for one-shot post-compile work like seeding storage buffers/images.
	virtual void OnPostCompile(RenderGraph& graph) { (void)graph; }

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
