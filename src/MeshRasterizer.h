#pragma once
#include "vulkan/vulkan.h"
#include "Buffer.h"
#include "Device.h"
#include "CommandPool.h"
#include "CommandBuffer.h"
#include "DescriptorSet.h"
#include "DescriptorSetLayout.h"
#include "DescriptorPool.h"
#include "RenderPass.h"
#include "Framebuffer.h"
#include "Pipeline.h"
#include "Image.h"
#include "ImageView.h"
#include "Sampler.h"
#include "Allocator.h"
#include "config.h"

#include "Camera.h"

#include "tiny_obj_loader.h"
#include <unordered_map>
#include <chrono>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// CONSTANTS -------------------------------------------------------------------------------------------------

/// <summary>
/// The path to the model to load.
/// </summary>
const std::string MODEL_PATH = std::string(config::ASSET_DIR) + "/models/viking_room.obj";

/// <summary>
/// The path to the texture to load.
/// </summary>
const std::string TEXTURE_PATH = std::string(config::ASSET_DIR) + "/textures/viking_room.png";

/// <summary>
/// Represents the contents of the uniform buffer.
/// </summary>
struct UniformBufferObject {
	glm::mat4 model;
	glm::mat4 view;
	glm::mat4 proj;
};

/// <summary>
/// Records commands to draw a mesh to a framebuffer using rasterization. 
/// Manages all resources needed to describe the operations that draw the mesh.
/// </summary>
class MeshRasterizer
{
private:

	// DATA --------------------------------------------------------------------------------------------------
	/// <summary> The indices of the index buffer. </summary>
	std::vector<uint32_t> m_indices;

	/// <summary> The vertices of the vertex buffer. </summary>
	std::vector<VWrap::Vertex> m_vertices;

	// RESOURCES ---------------------------------------------------------------------------------------------
	// DEVICE RESOURCES
	std::shared_ptr<VWrap::Device> m_device;
	std::shared_ptr<VWrap::CommandPool> m_graphics_pool;
	std::shared_ptr<VWrap::Allocator> m_allocator;

	// TEXTURES
	std::shared_ptr<VWrap::ImageView> m_texture_image_view;
	std::shared_ptr<VWrap::Image> m_texture_image;
	std::shared_ptr<VWrap::Sampler> m_sampler;

	// BUFFERS
	std::shared_ptr<VWrap::Buffer> m_vertex_buffer;
	std::shared_ptr<VWrap::Buffer> m_index_buffer;
	std::vector<std::shared_ptr<VWrap::Buffer>> m_uniform_buffers;
	std::vector<void*> m_uniform_buffers_mapped;

	// DESCRIPTORS
	std::shared_ptr<VWrap::DescriptorSetLayout> m_descriptor_set_layout;
	std::shared_ptr<VWrap::DescriptorPool> m_descriptor_pool;
	std::vector<std::shared_ptr<VWrap::DescriptorSet>> m_descriptor_sets;

	// PIPELINE
	std::shared_ptr<VWrap::Pipeline> m_pipeline;
	VkExtent2D m_extent;

	
	// CLASS FUNCTIONS ---------------------------------------------------------------------------------------
	void CreatePipeline(std::shared_ptr<VWrap::RenderPass> render_pass);
	
	void CreateDescriptors(int max_sets);

	/// <summary>
	/// Creates the vertex buffer and copies the vertex data into it.
	/// </summary>
	void CreateVertexBuffer();

	/// <summary>
	/// Creates the index buffer and copies the index data into it.
	/// </summary>
	void CreateIndexBuffer();

	/// <summary>
	/// Creates one uniform buffer for each frame in flight and maps them to host memory.
	/// </summary>
	void CreateUniformBuffers();

	/// <summary>
	/// Updates the descriptor sets with the uniform buffers and image sampler.
	/// </summary>
	void WriteDescriptors();

	/// <summary>
	/// Loads model and texture data from files, stores in CPU memory.
	/// </summary>
	void LoadModel();

public:

	/// <summary>
	/// Creates a new MeshRasterizer object.
	/// </summary>
	/// <param name="device"> The device to create everything. </param>
	/// <param name="render_pass"> Defines the pipeline. </param>
	/// <param name="graphics_pool"> The pool to load textures and buffers. Must be transfer and graphics compatible. </param>
	/// <param name="extent"> The extent of the pipeline. </param>
	/// <param name="num_frames"> The max number of frames. Defines number of descriptor sets. </param>
	/// <returns> A pointer to a new MeshRasterizer </returns>
	static std::shared_ptr<MeshRasterizer> Create(std::shared_ptr<VWrap::Allocator> allocator, std::shared_ptr<VWrap::Device> device, std::shared_ptr<VWrap::RenderPass> render_pass, std::shared_ptr<VWrap::CommandPool> graphics_pool, VkExtent2D extent, uint32_t num_frames);

	/// <summary>
	/// Records commands to the command_buffer to draw the model using rasterization.
	/// </summary>
	/// <param name="command_buffer"> The command buffer to record to. </param>
	/// <param name="frame"> Which frame-in-flight's resources to use. </param>
	void CmdDraw(std::shared_ptr<VWrap::CommandBuffer> command_buffer, uint32_t frame);


	/// <summary>
	/// Updates the uniform buffer for the given frame using perpetually-mapped memory.
	/// </summary>
	/// <param name="frame"> The index of the buffer to update. </param>
	void UpdateUniformBuffer(uint32_t frame, std::shared_ptr<Camera> camera);

	/// <summary>
	/// Updates the extent of the pipeline.
	/// </summary>
	/// <param name="extent"> The new extent. </param>
	void Resize(VkExtent2D extent) {
		m_extent = extent;
	}
};

