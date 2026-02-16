#pragma once
#include "RenderTechnique.h"
#include "RenderGraph.h"
#include "Buffer.h"
#include "DescriptorSet.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "Pipeline.h"
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

const std::string MODEL_PATH = std::string(config::ASSET_DIR) + "/models/viking_room.obj";
const std::string TEXTURE_PATH = std::string(config::ASSET_DIR) + "/textures/viking_room.png";

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

	std::shared_ptr<VWrap::DescriptorSetLayout> m_descriptor_set_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_descriptor_pool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_descriptor_sets;

	std::shared_ptr<VWrap::Pipeline> m_pipeline;
	VkExtent2D m_extent;

	std::shared_ptr<VWrap::RenderPass> m_render_pass;

	// Tunable parameters
	float m_rotation_speed = 0.0f;
	float m_model_scale = 1.0f;
	bool m_wireframe = false;
	float m_accumulated_rotation = 0.0f;

	std::vector<TechniqueParameter> m_parameters;

	void CreatePipeline(std::shared_ptr<VWrap::RenderPass> render_pass);
	void CreateDescriptors(int max_sets);
	void CreateVertexBuffer();
	void CreateIndexBuffer();
	void CreateUniformBuffers();
	void WriteDescriptors();
	void LoadModel();
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
};
