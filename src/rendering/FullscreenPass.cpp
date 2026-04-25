#include "FullscreenPass.h"
#include "RenderGraph.h"
#include "DescriptorSetBuilder.h"
#include "PipelineDefaults.h"
#include "Utils.h"
#include "config.h"

namespace {
	std::string ShaderPath(const std::string& spv) {
		// Already absolute? (config::SHADER_DIR-prefixed paths come through unchanged.)
		if (!spv.empty() && spv[0] == '/') return spv;
		return std::string(config::SHADER_DIR) + "/" + spv;
	}
}

std::unique_ptr<FullscreenPass> FullscreenPass::Build(
	RenderGraph& graph,
	std::shared_ptr<VWrap::Device> device,
	VkExtent2D extent,
	uint32_t maxFramesInFlight,
	const FullscreenPassDesc& desc,
	PushFn pushFn)
{
	auto fp = std::make_unique<FullscreenPass>();
	fp->m_device = device;
	fp->m_extent = extent;
	fp->m_maxFramesInFlight = maxFramesInFlight;
	fp->m_inputs = desc.sampledInputs;
	fp->m_sampler = desc.sampler;
	fp->m_fragShaderSpv = desc.fragShaderSpv;
	fp->m_pushConstantSize = desc.pushConstantSize;

	// Descriptor layout: one COMBINED_IMAGE_SAMPLER binding per input.
	{
		DescriptorSetBuilder dsb(device);
		for (uint32_t i = 0; i < desc.sampledInputs.size(); ++i) {
			dsb.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		}
		auto res = dsb.Build(maxFramesInFlight);
		fp->m_descriptorLayout = res.layout;
		fp->m_descriptorPool = res.pool;
		fp->m_descriptorSets = res.sets;
	}

	// Register the graphics pass with the graph. Capture clear color into the
	// SetColorAttachment overload that takes per-channel floats.
	auto& builder = graph.AddGraphicsPass(desc.name)
		.SetColorAttachment(desc.output, desc.loadOp, desc.storeOp,
			desc.clearColor.float32[0], desc.clearColor.float32[1],
			desc.clearColor.float32[2], desc.clearColor.float32[3]);
	for (auto h : desc.sampledInputs) builder.Read(h);

	FullscreenPass* fpRaw = fp.get();
	builder.SetRecord([fpRaw, push = std::move(pushFn)](PassContext& ctx) mutable {
		ctx.cmd->CmdBindGraphicsPipeline(fpRaw->m_pipeline);
		ctx.cmd->CmdBindGraphicsDescriptorSets(
			fpRaw->m_pipeline->GetLayout(),
			{ fpRaw->m_descriptorSets[ctx.frameIndex]->Get() });
		if (push) push(ctx, fpRaw->m_pipeline->GetLayout());
		ctx.cmd->CmdDraw(4, 1, 0, 0);
	});

	fp->m_renderPass = builder.GetRenderPassPtr();
	fp->RecreatePipeline();
	return fp;
}

void FullscreenPass::RecreatePipeline() {
	if (!m_renderPass) return;

	auto vertCode = VWrap::readFile(std::string(config::SHADER_DIR) + "/post_fullscreen.vert.spv");
	auto fragCode = VWrap::readFile(ShaderPath(m_fragShaderSpv));

	std::vector<VkPushConstantRange> ranges;
	if (m_pushConstantSize > 0) {
		VkPushConstantRange r{};
		r.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		r.offset = 0;
		r.size = m_pushConstantSize;
		ranges.push_back(r);
	}

	auto info = PipelineDefaults::FullscreenQuad(m_renderPass, m_descriptorLayout, m_extent, ranges);
	m_pipeline = VWrap::Pipeline::Create(m_device, info, vertCode, fragCode);
}

void FullscreenPass::WriteDescriptors(RenderGraph& graph) {
	for (auto& ds : m_descriptorSets) {
		// imgInfos must outlive the vkUpdateDescriptorSets call. Pre-sizing the
		// vector prevents reallocation while we hand out interior pointers.
		std::vector<VkDescriptorImageInfo> imgInfos(m_inputs.size());
		std::vector<VkWriteDescriptorSet> writes(m_inputs.size());

		for (size_t i = 0; i < m_inputs.size(); ++i) {
			auto view = graph.GetImageView(m_inputs[i]);
			imgInfos[i] = {};
			imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imgInfos[i].imageView = view->Get();
			imgInfos[i].sampler = m_sampler->Get();

			writes[i] = {};
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = ds->Get();
			writes[i].dstBinding = static_cast<uint32_t>(i);
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &imgInfos[i];
		}

		if (!writes.empty()) {
			vkUpdateDescriptorSets(m_device->Get(),
				static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
	}
}
