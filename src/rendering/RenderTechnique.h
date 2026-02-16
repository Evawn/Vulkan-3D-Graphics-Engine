#pragma once

#include <string>
#include <vector>
#include <memory>
#include <vulkan/vulkan.h>
#include "Device.h"
#include "Allocator.h"
#include "CommandPool.h"
#include "CommandBuffer.h"
#include "RenderPass.h"
#include "Camera.h"

struct RenderContext {
    std::shared_ptr<VWrap::Device> device;
    std::shared_ptr<VWrap::Allocator> allocator;
    std::shared_ptr<VWrap::CommandPool> graphicsPool;
    std::shared_ptr<VWrap::CommandPool> computePool;
    std::shared_ptr<VWrap::RenderPass> renderPass;
    VkExtent2D extent;
    uint32_t maxFramesInFlight;
};

struct TechniqueParameter {
    enum Type { Float, Int, Bool, Color3, Color4, Enum };
    std::string label;
    Type type;
    void* data;
    float min = 0.0f;
    float max = 1.0f;
    std::vector<const char*> enumLabels;
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

    virtual void Init(const RenderContext& ctx) = 0;
    virtual void Shutdown() = 0;
    virtual void OnResize(VkExtent2D newExtent) = 0;

    virtual void RecordCommands(
        std::shared_ptr<VWrap::CommandBuffer> cmd,
        uint32_t frameIndex,
        std::shared_ptr<Camera> camera) = 0;

    virtual std::vector<std::string> GetShaderPaths() const = 0;
    virtual void RecreatePipeline(const RenderContext& ctx) = 0;

    virtual std::vector<TechniqueParameter>& GetParameters() {
        static std::vector<TechniqueParameter> empty;
        return empty;
    }

    virtual FrameStats GetFrameStats() const { return {}; }

    virtual void SetWireframe(bool enabled) { (void)enabled; }
    virtual bool GetWireframe() const { return false; }
};
