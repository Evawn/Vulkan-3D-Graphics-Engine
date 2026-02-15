#include "Pipeline.h"

namespace VWrap {

    std::shared_ptr<Pipeline> Pipeline::Create(std::shared_ptr<Device> device, const PipelineCreateInfo& create_info, const std::vector<char>& vertex_shader_code, const std::vector<char>& fragment_shader_code)
    {
        auto ret = std::make_shared<Pipeline>();
        ret->m_device = device;

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
        multisampling.minSampleShading = 1.0f; // Optional
        multisampling.pSampleMask = nullptr; // Optional
        multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
        multisampling.alphaToOneEnable = VK_FALSE; // Optional

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        colorBlending.blendConstants[0] = 0.0f; // Optional
        colorBlending.blendConstants[1] = 0.0f; // Optional
        colorBlending.blendConstants[2] = 0.0f; // Optional
        colorBlending.blendConstants[3] = 0.0f; // Optional

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        std::array<VkDescriptorSetLayout, 1> descriptor_set_layout_handles = { create_info.descriptor_set_layout->Get()};
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(descriptor_set_layout_handles.size());
        pipelineLayoutInfo.pSetLayouts = descriptor_set_layout_handles.data();

        pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(create_info.push_constant_ranges.size());
        pipelineLayoutInfo.pPushConstantRanges = create_info.push_constant_ranges.data();


        if (vkCreatePipelineLayout(device->Get(), &pipelineLayoutInfo, nullptr, &ret->m_pipeline_layout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create pipline layout!");
        }

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
        pipelineInfo.layout = ret->m_pipeline_layout;
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
        if (m_pipeline_layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(m_device->Get(), m_pipeline_layout, nullptr);
    }

}