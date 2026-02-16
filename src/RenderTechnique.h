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
    std::shared_ptr<VWrap::RenderPass> renderPass;
    VkExtent2D extent;
    uint32_t maxFramesInFlight;
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
};
