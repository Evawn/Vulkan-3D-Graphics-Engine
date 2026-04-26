#include "FullscreenPass.h"
#include "RenderGraph.h"
#include "RenderItem.h"
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

	// Binding table: one COMBINED_IMAGE_SAMPLER per sampled input (graph-managed).
	fp->m_bindings = std::make_shared<BindingTable>(device, maxFramesInFlight);
	for (uint32_t i = 0; i < desc.sampledInputs.size(); ++i) {
		fp->m_bindings->AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		fp->m_bindings->BindGraphSampledImage(i, desc.sampledInputs[i], desc.sampler);
	}
	fp->m_bindings->Build();

	auto& builder = graph.AddGraphicsPass(desc.name);
	// Declare for graph introspection / debug surface. Per the plan, fullscreen
	// post-process passes don't iterate the scene — they draw an implicit FSQ
	// item directly via DrawFullscreenItem in the record callback.
	builder.AcceptsItemTypes({ RenderItemType::Fullscreen });
	builder.SetColorAttachment(desc.output, desc.loadOp, desc.storeOp,
		desc.clearColor.float32[0], desc.clearColor.float32[1],
		desc.clearColor.float32[2], desc.clearColor.float32[3]);
	for (auto h : desc.sampledInputs) builder.Read(h);

	uint32_t pushSize = desc.pushConstantSize;
	std::string fragSpv = ShaderPath(desc.fragShaderSpv);
	std::string vertSpv = std::string(config::SHADER_DIR) + "/post_fullscreen.vert.spv";
	auto bindings = fp->m_bindings;

	builder.SetPipeline([vertSpv, fragSpv, pushSize, bindings]() {
		GraphicsPipelineDesc d{};
		d.vertSpvPath = vertSpv;
		d.fragSpvPath = fragSpv;
		d.descriptorSetLayout = bindings->GetLayout();
		if (pushSize > 0) {
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			r.offset = 0;
			r.size = pushSize;
			d.pushConstantRanges.push_back(r);
		}
		d.inputAssembly = PipelineDefaults::TriangleStrip();
		d.rasterizer = PipelineDefaults::NoCullFill();
		d.depthStencil = PipelineDefaults::NoDepthTest();
		d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		return d;
	});

	builder.SetRecord([bindings, push = std::move(pushFn)](PassContext& ctx) mutable {
		ctx.cmd->CmdBindGraphicsPipeline(ctx.graphicsPipeline);
		ctx.cmd->CmdBindGraphicsDescriptorSets(
			ctx.graphicsPipeline->GetLayout(),
			{ bindings->GetSet(ctx.frameIndex)->Get() });
		if (push) push(ctx, ctx.graphicsPipeline->GetLayout());
		// Implicit FSQ item — post-process passes don't pull from the scene.
		// Routes through the same draw helper that ray-march techniques use,
		// which is the architectural payoff: one Fullscreen draw path engine-wide.
		static const RenderItem kImplicitFSQ = []{
			RenderItem r{};
			r.type = RenderItemType::Fullscreen;
			r.instanceCount = 1;
			r.firstInstance = 0;
			return r;
		}();
		DrawFullscreenItem(ctx, kImplicitFSQ);
	});

	builder.SetBindings(fp->m_bindings);

	return fp;
}
