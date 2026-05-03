#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <chrono>
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
struct SkyDescription;

struct RenderContext {
	std::shared_ptr<VWrap::Device> device;
	std::shared_ptr<VWrap::Allocator> allocator;
	std::shared_ptr<VWrap::CommandPool> graphicsPool;
	std::shared_ptr<VWrap::CommandPool> computePool;
	VkExtent2D extent;
	uint32_t maxFramesInFlight;
	std::shared_ptr<Camera> camera;
	SceneLighting*  lighting = nullptr;  // shared, non-owning; scene-owned state
	SkyDescription* sky      = nullptr;  // shared, non-owning; scene-owned state

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

	// ---- Workspace scoping ----
	//
	// Returns true if the technique should NOT appear in the Scene workspace's
	// general technique-picker UI (Inspector dropdown + Cycle Technique menu).
	// The technique is still constructed and added to the rendering system —
	// workspace-locked techniques activate only when their workspace is
	// entered (via RenderingSystem::RequestSwitchTechniqueByName from the
	// workspace switcher), so they need to be present in the list. This flag
	// is purely about hiding them from the Scene workspace's interactive
	// pickers, where they'd otherwise be confusing (the user could activate
	// the import technique without entering the import workspace, leaving
	// half-wired UI state).
	virtual bool IsScopedToWorkspace() const { return false; }

	// ---- Reload (event-driven; Application reposts ReloadTechnique back here) ----
	void SetEventSink(std::function<void(AppEvent)> sink) { m_eventSink = std::move(sink); }
	virtual void Reload(const RenderContext& ctx) { (void)ctx; }

	// ---- Logical time source ----
	// Set by RenderingSystem from CaptureSystem::GetLogicalTimeSeconds. When a
	// FixedStep-paced recording is active this advances by 1/fps per captured
	// frame; otherwise it's wall-clock time since process start. Techniques
	// driving animation should call GetTimeSeconds() rather than reading
	// std::chrono::steady_clock::now() directly so FixedStep recordings produce
	// smooth-motion video regardless of how slow the engine renders. When no
	// provider is set, GetTimeSeconds() falls back to wall-clock so tests /
	// stand-alone uses keep working.
	void   SetTimeProvider(std::function<double()> provider) { m_timeProvider = std::move(provider); }
	double GetTimeSeconds() const {
		if (m_timeProvider) return m_timeProvider();
		const auto wall = std::chrono::steady_clock::now() - m_fallbackEpoch;
		return std::chrono::duration<double>(wall).count();
	}

protected:
	// Push events back to the application — request reload, request rebuild,
	// request pipeline recreate, etc. Set once by Application after construction.
	std::function<void(AppEvent)> m_eventSink;

	// Logical-time hook + per-instance fallback origin (only used when no
	// provider is wired — keeps the no-config path useful).
	std::function<double()> m_timeProvider;
	std::chrono::steady_clock::time_point m_fallbackEpoch = std::chrono::steady_clock::now();
};
