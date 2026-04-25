#pragma once
#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "BindingTable.h"
#include "Buffer.h"
#include "Image.h"
#include "ImageView.h"
#include "Sampler.h"
#include "config.h"

#include "tiny_obj_loader.h"
#include <unordered_map>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct UniformBufferObject {
	glm::mat4 model;
	glm::mat4 view;
	glm::mat4 proj;
};

class MeshRasterizer : public RenderTechnique
{
private:
	std::vector<uint32_t> m_indices;
	std::vector<VWrap::Vertex> m_vertices;

	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<VWrap::Allocator> m_allocator;
	std::shared_ptr<Camera> m_camera;

	std::shared_ptr<VWrap::ImageView> m_texture_image_view;
	std::shared_ptr<VWrap::Image> m_texture_image;
	std::shared_ptr<VWrap::Sampler> m_sampler;

	std::shared_ptr<VWrap::Buffer> m_vertex_buffer;
	std::shared_ptr<VWrap::Buffer> m_index_buffer;
	std::vector<std::shared_ptr<VWrap::Buffer>> m_uniform_buffers;
	std::vector<void*> m_uniform_buffers_mapped;

	std::shared_ptr<BindingTable> m_bindings;

	// Pipeline owned by the graph; rebuilt on hot-reload + wireframe toggle.
	RenderGraph* m_graph = nullptr;
	VkExtent2D m_extent;

	// Tunable parameters
	float m_rotation_speed = 0.0f;
	float m_model_scale = 1.0f;
	bool m_wireframe = false;
	float m_accumulated_rotation = 0.0f;

	// File paths (dynamic, exposed as File parameters)
	std::string m_model_path;
	std::string m_texture_path;
	std::string m_mtl_path;  // User-selected MTL (may differ from OBJ's mtllib)
	bool m_needs_reload = false;
	bool m_needs_texture_reload = false;

	std::vector<TechniqueParameter> m_parameters;

	void CreateVertexBuffer();
	void CreateIndexBuffer();
	void CreateUniformBuffers(uint32_t frames);
	void WriteDescriptors();
	void LoadModel();
	void ReloadModel(const std::string& newPath);
	void ReloadTexture(const std::string& newPath);
	void CreatePlaceholderTexture();
	void UpdateUniformBuffer(uint32_t frame);

public:
	std::string GetName() const override { return "Mesh Rasterizer"; }

	void RegisterPasses(
		RenderGraph& graph,
		const RenderContext& ctx,
		ImageHandle colorTarget,
		ImageHandle depthTarget,
		ImageHandle resolveTarget) override;

	void Shutdown() override {}
	void OnResize(VkExtent2D newExtent, RenderGraph& graph) override { m_extent = newExtent; }

	std::vector<std::string> GetShaderPaths() const override;
	void RecreatePipeline(const RenderContext& ctx) override;

	std::vector<TechniqueParameter>& GetParameters() override;
	FrameStats GetFrameStats() const override;
	void SetWireframe(bool enabled) override;
	bool GetWireframe() const override { return m_wireframe; }

	bool NeedsReload() const override { return m_needs_reload || m_needs_texture_reload; }
	void PerformReload(const RenderContext& ctx) override;
};
