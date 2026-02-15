#include "MeshRasterizer.h"
#include "config.h"

inline void MeshRasterizer::CreateVertexBuffer() {
	VkDeviceSize bufferSize = sizeof(m_vertices[0]) * m_vertices.size();

	auto staging_buffer = VWrap::Buffer::CreateStaging(m_allocator, bufferSize);

	void* data;
	vmaMapMemory(m_allocator->Get(), staging_buffer->GetAllocation(), &data);
	memcpy(data, m_vertices.data(), (size_t)bufferSize);
	vmaUnmapMemory(m_allocator->Get(), staging_buffer->GetAllocation());

	m_vertex_buffer = VWrap::Buffer::Create(m_allocator,
		bufferSize,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		0);

	auto command_buffer = VWrap::CommandBuffer::Create(m_graphics_pool);
	command_buffer->BeginSingle();
	command_buffer->CmdCopyBuffer(staging_buffer, m_vertex_buffer, bufferSize);
	command_buffer->EndAndSubmit();
}

inline void MeshRasterizer::CreateIndexBuffer() {
	VkDeviceSize bufferSize = sizeof(m_indices[0]) * m_indices.size();

	auto staging_buffer = VWrap::Buffer::CreateStaging(m_allocator, bufferSize);

	void* data;
	vmaMapMemory(m_allocator->Get(), staging_buffer->GetAllocation(), &data);
	memcpy(data, m_indices.data(), (size_t)bufferSize);
	vmaUnmapMemory(m_allocator->Get(), staging_buffer->GetAllocation());

	m_index_buffer = VWrap::Buffer::Create(m_allocator,
		bufferSize,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		0);

	auto command_buffer = VWrap::CommandBuffer::Create(m_graphics_pool);
	command_buffer->BeginSingle();
	command_buffer->CmdCopyBuffer(staging_buffer, m_index_buffer, bufferSize);
	command_buffer->EndAndSubmit();
}

inline void MeshRasterizer::CreateUniformBuffers() {
	VkDeviceSize bufferSize = sizeof(UniformBufferObject);
	auto frames = m_descriptor_sets.size();
	m_uniform_buffers.resize(frames);
	m_uniform_buffers_mapped.resize(frames);

	for (size_t i = 0; i < frames; i++) {
		//VmaAllocationInfo alloc_info{};
		m_uniform_buffers[i] = VWrap::Buffer::CreateMapped(
			m_allocator,
			bufferSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_uniform_buffers_mapped[i]);
	}
}

inline void MeshRasterizer::WriteDescriptors() {
	for (size_t i = 0; i < m_descriptor_sets.size(); i++) {
		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = m_uniform_buffers[i]->Get();
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(UniformBufferObject);

		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = m_texture_image_view->Get();
		imageInfo.sampler = m_sampler->Get();

		// array of descriptor writes:
		std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
		descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[0].descriptorCount = 1;
		descriptorWrites[0].dstBinding = 0;
		descriptorWrites[0].dstSet = m_descriptor_sets[i]->Get();
		descriptorWrites[0].dstArrayElement = 0;
		descriptorWrites[0].pBufferInfo = &bufferInfo;
		descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

		descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[1].descriptorCount = 1;
		descriptorWrites[1].dstBinding = 1;
		descriptorWrites[1].dstSet = m_descriptor_sets[i]->Get();
		descriptorWrites[1].dstArrayElement = 0;
		descriptorWrites[1].pImageInfo = &imageInfo;
		descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

		vkUpdateDescriptorSets(m_device->Get(), static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
	}
}

inline void MeshRasterizer::LoadModel() {
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn, err;

	if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, MODEL_PATH.c_str())) {
		throw std::runtime_error(warn + err);
	}

	std::unordered_map<VWrap::Vertex, uint32_t> uniqueVertices{};

	for (const auto& shape : shapes) {
		for (const auto& index : shape.mesh.indices) {
			VWrap::Vertex vertex{};

			vertex.pos = {
				attrib.vertices[3 * index.vertex_index + 0],
				attrib.vertices[3 * index.vertex_index + 1],
				attrib.vertices[3 * index.vertex_index + 2]
			};

			vertex.texCoord = {
				attrib.texcoords[2 * index.texcoord_index + 0],
				1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
			};

			vertex.color = { 1.0f, 1.0f, 1.0f };

			if (uniqueVertices.count(vertex) == 0) {
				uniqueVertices[vertex] = static_cast<uint32_t>(m_vertices.size());
				m_vertices.push_back(vertex);
			}

			m_indices.push_back(uniqueVertices[vertex]);
		}
	}
	std::cout << "Finished loading" << std::endl;
}

std::shared_ptr<MeshRasterizer> MeshRasterizer::Create(std::shared_ptr<VWrap::Allocator> allocator, std::shared_ptr<VWrap::Device> device, std::shared_ptr<VWrap::RenderPass> render_pass, std::shared_ptr<VWrap::CommandPool> graphics_pool, VkExtent2D extent, uint32_t num_frames) {
	auto ret = std::make_shared<MeshRasterizer>();
	ret->m_device = device;
	ret->m_allocator = allocator;
	ret->m_extent = extent;
	ret->m_graphics_pool = graphics_pool;

	ret->CreateDescriptors(num_frames);
	ret->CreatePipeline(render_pass);
	ret->m_sampler = VWrap::Sampler::Create(device);

	VWrap::CommandBuffer::UploadTextureToImage(graphics_pool, allocator, ret->m_texture_image, TEXTURE_PATH.c_str());
	ret->m_texture_image_view = VWrap::ImageView::Create(device, ret->m_texture_image);

	ret->LoadModel();
	ret->CreateVertexBuffer();
	ret->CreateIndexBuffer();
	ret->CreateUniformBuffers();

	ret->WriteDescriptors();

	return ret;
}

void MeshRasterizer::CreatePipeline(std::shared_ptr<VWrap::RenderPass> render_pass)
{
	auto vert_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_rast.vert.spv");
	auto frag_shader_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/shader_rast.frag.spv");

	auto bindingDescription = VWrap::Vertex::getBindingDescription();
	auto attributeDescriptions = VWrap::Vertex::getAttributeDescriptions();

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexAttributeDescriptionCount = 3;
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;
	rasterizer.depthBiasConstantFactor = 0.0f; // Optional
	rasterizer.depthBiasClamp = 0.0f; // Optional
	rasterizer.depthBiasSlopeFactor = 0.0f; // Optional

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f; // Optional
	depthStencil.maxDepthBounds = 1.0f; // Optional
	depthStencil.stencilTestEnable = VK_FALSE;
	depthStencil.front = {}; // Optional
	depthStencil.back = {}; // Optional

	VWrap::PipelineCreateInfo create_info{};
	create_info.extent = m_extent;
	create_info.render_pass = render_pass;
	create_info.descriptor_set_layout = m_descriptor_set_layout;
	create_info.vertex_input_info = vertexInputInfo;
	create_info.input_assembly = inputAssembly;
	create_info.dynamic_state = dynamicState;
	create_info.rasterizer = rasterizer;
	create_info.depth_stencil = depthStencil;
	create_info.push_constant_ranges = {};
	create_info.subpass = 0;

	m_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_shader_code, frag_shader_code);
}

void MeshRasterizer::CreateDescriptors(int max_sets)
{
	// Create the descriptor set layout
	VkDescriptorSetLayoutBinding uboLayoutBinding{};
	uboLayoutBinding.binding = 0;
	uboLayoutBinding.descriptorCount = 1;
	uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	uboLayoutBinding.pImmutableSamplers = nullptr; // Optional - relevant for image sampling descriptors

	VkDescriptorSetLayoutBinding samplerLayoutBinding{};
	samplerLayoutBinding.binding = 1;
	samplerLayoutBinding.descriptorCount = 1;
	samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	samplerLayoutBinding.pImmutableSamplers = nullptr; // Optional - relevant for image sampling descriptors

	std::vector<VkDescriptorSetLayoutBinding> bindings = { uboLayoutBinding, samplerLayoutBinding };
	m_descriptor_set_layout = VWrap::DescriptorSetLayout::Create(m_device, bindings);

	// Create the descriptor pool
	std::vector<VkDescriptorPoolSize> poolSizes(2);
	poolSizes[0].descriptorCount = max_sets;
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[1].descriptorCount = max_sets;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	m_descriptor_pool = VWrap::DescriptorPool::Create(m_device, poolSizes, max_sets, 0);

	// Create the descriptor sets
	std::vector<std::shared_ptr<VWrap::DescriptorSetLayout>> layouts(static_cast<size_t>(max_sets), m_descriptor_set_layout);
	m_descriptor_sets = VWrap::DescriptorSet::CreateMany(m_descriptor_pool, layouts);
}

void MeshRasterizer::CmdDraw(std::shared_ptr<VWrap::CommandBuffer> command_buffer, uint32_t frame) {
	auto vk_command_buffer = command_buffer->Get();
	vkCmdBindPipeline(vk_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->Get());

	std::array<VkDescriptorSet, 1> descriptorSets = { m_descriptor_sets[frame]->Get() };
	vkCmdBindDescriptorSets(vk_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->GetLayout(), 0, 1, descriptorSets.data(), 0, nullptr);

	VkViewport viewport{};
	viewport.height = (float)m_extent.height;
	viewport.width = (float)m_extent.width;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	viewport.x = 0;
	viewport.y = 0;

	vkCmdSetViewport(vk_command_buffer, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = m_extent;
	scissor.offset = { 0,0 };

	vkCmdSetScissor(vk_command_buffer, 0, 1, &scissor);

	VkBuffer vertexBuffers[] = { m_vertex_buffer->Get() };
	VkDeviceSize offsets[] = { 0 };
	vkCmdBindVertexBuffers(vk_command_buffer, 0, 1, vertexBuffers, offsets);
	vkCmdBindIndexBuffer(vk_command_buffer, m_index_buffer->Get(), 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(vk_command_buffer, static_cast<uint32_t>(m_indices.size()), 1, 0, 0, 0);
}

void MeshRasterizer::UpdateUniformBuffer(uint32_t frame, std::shared_ptr<Camera> camera) {
	static auto startTime = std::chrono::high_resolution_clock::now();

	auto currentTime = std::chrono::high_resolution_clock::now();
	float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

	UniformBufferObject ubo{};
	ubo.model = glm::mat4(1.0f);
	ubo.view = camera->GetViewMatrix();
	ubo.proj = camera->GetProjectionMatrix();

	memcpy(m_uniform_buffers_mapped[frame], &ubo, sizeof(ubo));
}
