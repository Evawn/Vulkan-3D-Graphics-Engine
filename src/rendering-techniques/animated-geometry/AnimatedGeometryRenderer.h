#pragma once

#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "BindingTable.h"
#include "PaletteResource.h"
#include "SceneLighting.h"
#include <chrono>
#include <memory>
#include <glm/glm.hpp>

class RenderGraph;

// Sibling technique to BrickmapPaletteRenderer. Performs single-level DDA
// against a flat 3D R8_UINT volume (palette-indexed material per voxel) and
// reuses the same lighting / sky / corner-AO helpers as the brickmap pipeline.
//
// Scaffolded as the foundation for future "instanced animated voxel geometry"
// (grass blades, foliage frames, etc.). For now it just renders one procedural
// animated pattern in a 128^3 volume so the shared lighting path stays exercised.
class AnimatedGeometryRenderer : public RenderTechnique {
private:
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<Camera> m_camera;

	// Graph-managed 128^3 3D storage image (R8_UINT material indices).
	ImageHandle m_volume;

	// Shared 256-entry palette texture + sampler.
	std::unique_ptr<PaletteResource> m_palette;

	// NEAREST sampler used to read the integer volume via texelFetch. Required
	// because R8_UINT does not support linear filtering and the palette's
	// LINEAR sampler can't be reused.
	std::shared_ptr<VWrap::Sampler> m_volume_sampler;

	// Descriptor wiring is owned by BindingTables; the graph re-applies them on
	// Compile() and on Resize(). Pipelines are also graph-owned.
	std::shared_ptr<BindingTable> m_compute_bindings;
	std::shared_ptr<BindingTable> m_graphics_bindings;

	// Tunable parameters.
	int m_pattern = 0;
	int m_max_iterations = 250;
	float m_sky_color[3] = { 0.529f, 0.808f, 0.922f };
	bool m_debug_color = false;
	float m_time_scale = 1.0f;
	std::chrono::steady_clock::time_point m_start_time;

	glm::uvec3 m_volume_size = glm::uvec3(128, 128, 128);

	const SceneLighting* m_lighting = nullptr;

	std::vector<TechniqueParameter> m_parameters;

public:
	std::string GetDisplayName() const override { return "Animated Geometry Renderer"; }

	RenderTargetDesc DescribeTargets(const RendererCaps& caps) const override;

	void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		const TechniqueTargets& targets) override;

	// Drops one Fullscreen item per frame so the trace pass has something to
	// iterate. This technique is the placeholder for the future foliage
	// renderer, which will instead emit InstancedVoxelMesh items here.
	void EmitItems(RenderScene& scene, const RenderContext& ctx) override;

	std::vector<std::string> GetShaderPaths() const override;

	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
};
