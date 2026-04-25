#pragma once

#include "PostProcessEffect.h"
#include <memory>
#include <vector>

class RenderGraph;

// Ordered pipeline of post-process effects. Owned by Renderer. Applied after the
// active RenderTechnique writes to the scene resolve image and before the UI
// pass reads the final scene for presentation.
//
// An empty or fully-disabled chain returns the input handle unchanged so the UI
// pass can always read "whatever the final scene image is" without special-casing.
class PostProcessChain {
public:
	PostProcessChain() = default;

	void SetContext(const PostProcessContext& ctx) { m_ctx = ctx; }
	const PostProcessContext& GetContext() const { return m_ctx; }

	void AddEffect(std::unique_ptr<PostProcessEffect> effect) {
		m_effects.push_back(std::move(effect));
	}

	// Returns the final image handle to feed into the UI pass.
	ImageHandle Register(RenderGraph& graph, ImageHandle sceneInput, VkExtent2D extent);

	void OnResize(VkExtent2D newExtent, RenderGraph& graph);

	size_t GetEffectCount() const { return m_effects.size(); }
	PostProcessEffect* GetEffect(size_t i) { return m_effects[i].get(); }
	const PostProcessEffect* GetEffect(size_t i) const { return m_effects[i].get(); }

	std::vector<std::string> GetShaderPaths() const;

private:
	PostProcessContext m_ctx{};
	std::vector<std::unique_ptr<PostProcessEffect>> m_effects;
};
