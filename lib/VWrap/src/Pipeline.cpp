#include "Pipeline.h"

namespace VWrap {

    std::shared_ptr<Pipeline> Pipeline::Create(std::shared_ptr<Device> device, const PipelineCreateInfo& create_info, const std::vector<char>& vertex_shader_code, const std::vector<char>& fragment_shader_code)
    {
        // Build a fresh PipelineLayout from the descriptor_set_layout + push ranges
        // in create_info. Callers that want to share a layout across pipelines should
        // use the overload below.
        auto layout = PipelineLayout::Create(device, create_info.descriptor_set_layout, create_info.push_constant_ranges);
        return Create(std::move(device), std::move(layout), create_info, vertex_shader_code, fragment_shader_code);
    }

    std::shared_ptr<Pipeline> Pipeline::Create(std::shared_ptr<Device> device, std::shared_ptr<PipelineLayout> layout, const PipelineCreateInfo& create_info, const std::vector<char>& vertex_shader_code, const std::vector<char>& fragment_shader_code)
    {
        auto ret = std::make_shared<Pipeline>();
        ret->m_device = device;
        ret->m_pipeline_layout = std::move(layout);

        VkShaderModule vertShaderModule = CreateShaderModule(device, vertex_shader_code);
        VkShaderModule fragShaderModule = CreateShaderModule(device, fragment_shader_code);

        VkPipelineShaderStageCreateInfo vertShaderStageCreateInfo{};
        vertShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageCreateInfo.module = vertShaderModule;
        vertShaderStageCreateInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageCreateInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageCreateInfo{};
        fragShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageCreateInfo.module = fragShaderModule;
        fragShaderStageCreateInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageCreateInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageCreateInfo, fragShaderStageCreateInfo };

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.height = (float)create_info.extent.height;
        viewport.width = (float)create_info.extent.width;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.extent = create_info.extent;
        scissor.offset = { 0,0 };

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.pScissors = &scissor;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.viewportCount = 1;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = create_info.render_pass->GetSamples();
        multisampling.minSampleShading = 1.0f;
        multisampling.pSampleMask = nullptr;
        multisampling.alphaToCoverageEnable = VK_FALSE;
        multisampling.alphaToOneEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState defaultBlendAttachment{};
        defaultBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        defaultBlendAttachment.blendEnable = VK_FALSE;
        defaultBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        defaultBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        defaultBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        defaultBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        defaultBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        defaultBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(
            create_info.colorAttachmentCount, defaultBlendAttachment);

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.logicOp = VK_LOGIC_OP_COPY;
        colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
        colorBlending.pAttachments = colorBlendAttachments.data();
        colorBlending.blendConstants[0] = 0.0f;
        colorBlending.blendConstants[1] = 0.0f;
        colorBlending.blendConstants[2] = 0.0f;
        colorBlending.blendConstants[3] = 0.0f;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &create_info.vertex_input_info;
        pipelineInfo.pInputAssemblyState = &create_info.input_assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &create_info.rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &create_info.depth_stencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &create_info.dynamic_state;
        pipelineInfo.layout = ret->m_pipeline_layout->Get();
        pipelineInfo.renderPass = create_info.render_pass->Get();
        pipelineInfo.subpass = create_info.subpass;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineInfo.basePipelineIndex = -1;

        if (vkCreateGraphicsPipelines(device->Get(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &ret->m_pipeline) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create graphics pipeline!");
        }

        vkDestroyShaderModule(device->Get(), vertShaderModule, nullptr);
        vkDestroyShaderModule(device->Get(), fragShaderModule, nullptr);

        return ret;
    }

    VkShaderModule Pipeline::CreateShaderModule(std::shared_ptr<Device> device, const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device->Get(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create shader module!");
        }
        return shaderModule;
    }

    Pipeline::~Pipeline() {
        if (m_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device->Get(), m_pipeline, nullptr);
        // m_pipeline_layout is a shared_ptr — destruction is handled by PipelineLayout itself.
    }

}
