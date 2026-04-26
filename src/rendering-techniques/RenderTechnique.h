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
class Scene;
class AssetRegistry;
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

	// Per-frame extracted item list. The SceneExtractor fills this from m_world
	// each frame; pass record callbacks read from it via PassContext::scene.
	// Techniques never write here — production goes through the scene tree.
	RenderScene* scene = nullptr;

	// Source-of-truth scene tree + asset storage. Techniques use these during
	// RegisterPasses / Reload to create scene nodes and register / look up
	// assets. They are stable across frames; ownership lives on RenderingSystem.
	Scene*         world  = nullptr;
	AssetRegistry* assets = nullptr;
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

	// Note: EmitItems() removed. RenderItems are now produced exclusively by
	// SceneExtractor walking the world tree. Techniques are pure consumers —
	// they declare .AcceptsItemTypes(...) and iterate ctx.scene->Get(...) in
	// their record callback. To make a technique render something, attach a
	// matching component to a SceneNode in the world (typically during the
	// technique's first RegisterPasses).

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
