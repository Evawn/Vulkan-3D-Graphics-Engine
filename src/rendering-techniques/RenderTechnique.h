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

class RenderGraph;
struct ImageHandle;

struct RenderContext {
	std::shared_ptr<VWrap::Device> device;
	std::shared_ptr<VWrap::Allocator> allocator;
	std::shared_ptr<VWrap::CommandPool> graphicsPool;
	std::shared_ptr<VWrap::CommandPool> computePool;
	VkExtent2D extent;
	uint32_t maxFramesInFlight;
	std::shared_ptr<Camera> camera;
};

struct TechniqueParameter {
	enum Type { Float, Int, Bool, Color3, Color4, Enum, File };
	std::string label;
	Type type;
	void* data = nullptr;
	float min = 0.0f;
	float max = 1.0f;
	std::vector<const char*> enumLabels;

	// File type fields
	std::string* filePath = nullptr;
	std::vector<std::string> fileFilters;                  // e.g. {"obj"}
	std::string fileFilterDesc;                            // e.g. "3D Models"
	std::function<void(const std::string&)> onFileChanged;
};

struct FrameStats {
	uint32_t drawCalls = 0;
	uint32_t vertices = 0;
	uint32_t indices = 0;
};

class RenderTechnique {
public:
	virtual ~RenderTechnique() = default;

	virtual std::string GetName() const = 0;

	virtual void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		ImageHandle colorTarget,
		ImageHandle depthTarget,
		ImageHandle resolveTarget) = 0;

	virtual void Shutdown() = 0;
	virtual void OnResize(VkExtent2D newExtent, RenderGraph& graph) = 0;

	virtual std::vector<std::string> GetShaderPaths() const = 0;
	virtual void RecreatePipeline(const RenderContext& ctx) = 0;

	// Called after graph.Compile() to write descriptors referencing graph-owned images.
	virtual void WriteGraphDescriptors(RenderGraph& graph) {}

	virtual std::vector<TechniqueParameter>& GetParameters() {
		static std::vector<TechniqueParameter> empty;
		return empty;
	}

	virtual FrameStats GetFrameStats() const { return {}; }

	virtual void SetWireframe(bool enabled) { (void)enabled; }
	virtual bool GetWireframe() const { return false; }

	// Deferred reload support (for file parameter changes)
	virtual bool NeedsReload() const { return false; }
	virtual void PerformReload(const RenderContext& ctx) { (void)ctx; }

	// Graph rebuild support (for resource size changes)
	virtual bool NeedsRebuild() const { return false; }
};
