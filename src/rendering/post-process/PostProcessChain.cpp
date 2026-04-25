#include "post-process/PostProcessChain.h"
#include "RenderGraph.h"

ImageHandle PostProcessChain::Register(RenderGraph& graph, ImageHandle sceneInput, VkExtent2D extent) {
	ImageHandle current = sceneInput;
	for (auto& effect : m_effects) {
		if (!effect->IsEnabled()) continue;
		current = effect->RegisterPasses(graph, m_ctx, current, extent);
	}
	return current;
}

void PostProcessChain::OnResize(VkExtent2D newExtent, RenderGraph& graph) {
	for (auto& effect : m_effects) {
		if (!effect->IsEnabled()) continue;
		effect->OnResize(newExtent, graph);
	}
}

std::vector<std::string> PostProcessChain::GetShaderPaths() const {
	std::vector<std::string> all;
	for (auto& effect : m_effects) {
		auto paths = effect->GetShaderPaths();
		all.insert(all.end(), paths.begin(), paths.end());
	}
	return all;
}
