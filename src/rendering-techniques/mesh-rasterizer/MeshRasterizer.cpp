#include "MeshRasterizer.h"
#include "PipelineDefaults.h"
#include "config.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <map>

void MeshRasterizer::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	ImageHandle colorTarget,
	ImageHandle depthTarget,
	ImageHandle resolveTarget)
{
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_extent = ctx.extent;
	m_graphics_pool = ctx.graphicsPool;
	m_camera = ctx.camera;

	auto desc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(ctx.maxFramesInFlight);
	m_descriptor_set_layout = desc.layout;
	m_descriptor_pool = desc.pool;
	m_descriptor_sets = desc.sets;

	m_sampler = VWrap::Sampler::Create(m_device);

	m_model_path = std::string(config::ASSET_DIR) + "/models/viking_room.obj";
	m_texture_path = std::string(config::ASSET_DIR) + "/textures/viking_room.png";

	auto logger = spdlog::get("Render");

	// Texture — fall back to 1x1 white placeholder if missing
	try {
		VWrap::CommandBuffer::UploadTextureToImage(m_graphics_pool, m_allocator, m_texture_image, m_texture_path.c_str());
	} catch (const std::exception& e) {
		logger->warn("Texture not found ({}), using placeholder", m_texture_path);
		CreatePlaceholderTexture();
	}
	m_texture_image_view = VWrap::ImageView::Create(m_device, m_texture_image);

	// Model — start with empty geometry if file is missing
	try {
		LoadModel();
	} catch (const std::exception& e) {
		logger->warn("Model not found ({}), waiting for user to select one", m_model_path);
	}

	if (!m_vertices.empty()) {
		CreateVertexBuffer();
		CreateIndexBuffer();
	}
	CreateUniformBuffers();
	WriteDescriptors();

	auto& pass = graph.AddGraphicsPass("Mesh Scene")
		.SetColorAttachment(colorTarget, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(depthTarget, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(resolveTarget)
		.SetRecord([this](PassContext& ctx) {
			UpdateUniformBuffer(ctx.frameIndex);

			if (m_indices.empty() || !m_vertex_buffer || !m_index_buffer) return;

			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->Get());

			std::array<VkDescriptorSet, 1> descriptorSets = { m_descriptor_sets[ctx.frameIndex]->Get() };
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->GetLayout(), 0, 1, descriptorSets.data(), 0, nullptr);

			VkBuffer vertexBuffers[] = { m_vertex_buffer->Get() };
			VkDeviceSize offsets[] = { 0 };
			vkCmdBindVertexBuffers(vk_cmd, 0, 1, vertexBuffers, offsets);
			vkCmdBindIndexBuffer(vk_cmd, m_index_buffer->Get(), 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(vk_cmd, static_cast<uint32_t>(m_indices.size()), 1, 0, 0, 0);
		});

	m_render_pass = pass.GetRenderPassPtr();
	CreatePipeline(m_render_pass);
}

void MeshRasterizer::CreateVertexBuffer() {
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

void MeshRasterizer::CreateIndexBuffer() {
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

void MeshRasterizer::CreateUniformBuffers() {
	VkDeviceSize bufferSize = sizeof(UniformBufferObject);
	auto frames = m_descriptor_sets.size();
	m_uniform_buffers.resize(frames);
	m_uniform_buffers_mapped.resize(frames);

	for (size_t i = 0; i < frames; i++) {
		m_uniform_buffers[i] = VWrap::Buffer::CreateMapped(
			m_allocator,
			bufferSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_uniform_buffers_mapped[i]);
	}
}

void MeshRasterizer::WriteDescriptors() {
	for (size_t i = 0; i < m_descriptor_sets.size(); i++) {
		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = m_uniform_buffers[i]->Get();
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(UniformBufferObject);

		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = m_texture_image_view->Get();
		imageInfo.sampler = m_sampler->Get();

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

void MeshRasterizer::LoadModel() {
	auto logger = spdlog::get("Render");
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn, err;

	// Pass the model's directory as mtl search path so tinyobj finds the .mtl
	std::string modelDir;
	auto slash = m_model_path.find_last_of('/');
	if (slash != std::string::npos)
		modelDir = m_model_path.substr(0, slash + 1);

	// Use stream-based API when we have a user-selected MTL, so tinyobj
	// resolves usemtl directives against it (bypasses broken mtllib parsing).
	if (!m_mtl_path.empty()) {
		std::ifstream objStream(m_model_path);
		std::ifstream mtlStream(m_mtl_path);
		if (!objStream.is_open()) throw std::runtime_error("Cannot open OBJ: " + m_model_path);
		if (!mtlStream.is_open()) throw std::runtime_error("Cannot open MTL: " + m_mtl_path);
		tinyobj::MaterialStreamReader mtlReader(mtlStream);
		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &objStream, &mtlReader)) {
			throw std::runtime_error(warn + err);
		}
	} else {
		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, m_model_path.c_str(), modelDir.c_str())) {
			throw std::runtime_error(warn + err);
		}
	}
	if (!warn.empty()) logger->warn("tinyobj warn: {}", warn);
	if (!err.empty()) logger->warn("tinyobj err: {}", err);

	// Resolve texture paths relative to the MTL's directory (not the OBJ's)
	std::string mtlDir = modelDir;
	if (!m_mtl_path.empty()) {
		auto mtlSlash = m_mtl_path.find_last_of('/');
		if (mtlSlash != std::string::npos)
			mtlDir = m_mtl_path.substr(0, mtlSlash + 1);
	}

	// Resolve a texture name from the MTL to an actual file path
	auto resolveTexPath = [&](const std::string& rawName) -> std::string {
		std::string texName = rawName;
		std::replace(texName.begin(), texName.end(), '\\', '/');
		std::string path = mtlDir + texName;
		if (std::ifstream(path).good()) return path;
		// Fallback: try just the filename in mtlDir or modelDir
		auto lastSlash = texName.find_last_of('/');
		if (lastSlash != std::string::npos) {
			std::string basename = texName.substr(lastSlash + 1);
			if (std::ifstream(mtlDir + basename).good()) return mtlDir + basename;
			if (std::ifstream(modelDir + basename).good()) return modelDir + basename;
			if (std::ifstream(modelDir + texName).good()) return modelDir + texName;
		}
		return path; // return original composed path even if not found
	};

	// Extract diffuse texture — prefer non-normal-map textures, then fall back to any
	std::string firstTexPath;
	for (const auto& mat : materials) {
		if (mat.diffuse_texname.empty()) continue;
		std::string resolved = resolveTexPath(mat.diffuse_texname);
		if (firstTexPath.empty()) firstTexPath = resolved;
		// Skip textures that look like normal maps
		std::string upper = mat.diffuse_texname;
		std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
		if (upper.find("_NOR") != std::string::npos || upper.find("_NORM") != std::string::npos)
			continue;
		m_texture_path = resolved;
		break;
	}
	// If all textures were normal maps, use the first one we found
	if (m_texture_path == mtlDir || m_texture_path.empty() ||
		m_texture_path.size() >= 4 && m_texture_path.compare(m_texture_path.size() - 4, 4, ".mtl") == 0) {
		if (!firstTexPath.empty()) m_texture_path = firstTexPath;
	}
	std::unordered_map<VWrap::Vertex, uint32_t> uniqueVertices{};

	for (const auto& shape : shapes) {
		size_t index_offset = 0;
		for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
			int fv = shape.mesh.num_face_vertices[f];
			int material_id = shape.mesh.material_ids[f];

			for (size_t v = 0; v < static_cast<size_t>(fv); v++) {
				tinyobj::index_t index = shape.mesh.indices[index_offset + v];
				VWrap::Vertex vertex{};

				vertex.pos = {
					attrib.vertices[3 * index.vertex_index + 0],
					attrib.vertices[3 * index.vertex_index + 1],
					attrib.vertices[3 * index.vertex_index + 2]
				};

				if (index.texcoord_index >= 0) {
					vertex.texCoord = {
						attrib.texcoords[2 * index.texcoord_index + 0],
						1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
					};
				} else {
					vertex.texCoord = { 0.0f, 0.0f };
				}

				// Use material Kd color as vertex color; falls back to white if no material
				if (material_id >= 0 && material_id < static_cast<int>(materials.size())) {
					float r = materials[material_id].diffuse[0];
					float g = materials[material_id].diffuse[1];
					float b = materials[material_id].diffuse[2];
					// Kd (0,0,0) is the tinyobj default when unspecified — treat as white
					if (r == 0.0f && g == 0.0f && b == 0.0f)
						vertex.color = { 1.0f, 1.0f, 1.0f };
					else
						vertex.color = { r, g, b };
				} else {
					vertex.color = { 1.0f, 1.0f, 1.0f };
				}

				if (uniqueVertices.count(vertex) == 0) {
					uniqueVertices[vertex] = static_cast<uint32_t>(m_vertices.size());
					m_vertices.push_back(vertex);
				}

				m_indices.push_back(uniqueVertices[vertex]);
			}
			index_offset += fv;
		}
	}
	logger->info("Finished loading model: {}", m_model_path);
}

void MeshRasterizer::ReloadModel(const std::string& newPath) {
	auto logger = spdlog::get("Render");
	m_model_path = newPath;
	m_vertices.clear();
	m_indices.clear();

	try {
		LoadModel();
	} catch (const std::exception& e) {
		logger->error("Failed to load model: {}", e.what());
		m_vertex_buffer.reset();
		m_index_buffer.reset();
		return;
	}

	m_vertex_buffer.reset();
	m_index_buffer.reset();
	CreateVertexBuffer();
	CreateIndexBuffer();

	// Reload texture — uses path from MTL if found, otherwise keeps current path
	m_texture_image.reset();
	m_texture_image_view.reset();
	try {
		VWrap::CommandBuffer::UploadTextureToImage(m_graphics_pool, m_allocator, m_texture_image, m_texture_path.c_str());
	} catch (const std::exception&) {
		logger->warn("No texture found, using placeholder");
		CreatePlaceholderTexture();
	}
	m_texture_image_view = VWrap::ImageView::Create(m_device, m_texture_image);
	WriteDescriptors();
}

void MeshRasterizer::ReloadTexture(const std::string& newPath) {
	auto logger = spdlog::get("Render");
	m_texture_path = newPath;

	// If the user picked an .mtl file, store it and trigger a full model reload.
	// LoadModel() will use the stream-based tinyobj API to resolve materials
	// from this MTL, which also handles texture path extraction and Kd colors.
	if (m_texture_path.size() >= 4 &&
		m_texture_path.compare(m_texture_path.size() - 4, 4, ".mtl") == 0)
	{
		m_mtl_path = m_texture_path;
		m_needs_reload = true;
		return;
	}

	m_texture_image.reset();
	m_texture_image_view.reset();
	try {
		VWrap::CommandBuffer::UploadTextureToImage(m_graphics_pool, m_allocator, m_texture_image, m_texture_path.c_str());
	} catch (const std::exception& e) {
		logger->error("Failed to load texture: {}", e.what());
		CreatePlaceholderTexture();
	}
	m_texture_image_view = VWrap::ImageView::Create(m_device, m_texture_image);
	WriteDescriptors();
}

void MeshRasterizer::PerformReload(const RenderContext& ctx) {
	// Process texture first — if it's an MTL, ReloadTexture stores m_mtl_path
	// and sets m_needs_reload so the model reload below picks it up.
	if (m_needs_texture_reload) {
		m_needs_texture_reload = false;
		ReloadTexture(m_texture_path);
	}
	if (m_needs_reload) {
		m_needs_reload = false;
		ReloadModel(m_model_path);
	}
}

void MeshRasterizer::CreatePlaceholderTexture() {
	uint8_t white[] = { 255, 255, 255, 255 };
	VkDeviceSize imageSize = 4;

	auto staging = VWrap::Buffer::CreateStaging(m_allocator, imageSize);
	void* data;
	vmaMapMemory(m_allocator->Get(), staging->GetAllocation(), &data);
	memcpy(data, white, imageSize);
	vmaUnmapMemory(m_allocator->Get(), staging->GetAllocation());

	VWrap::ImageCreateInfo info{};
	info.width = 1;
	info.height = 1;
	info.depth = 1;
	info.format = VK_FORMAT_R8G8B8A8_SRGB;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	info.mip_levels = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.image_type = VK_IMAGE_TYPE_2D;
	m_texture_image = VWrap::Image::Create(m_allocator, info);

	auto cmd = VWrap::CommandBuffer::Create(m_graphics_pool);
	cmd->BeginSingle();
	cmd->CmdTransitionImageLayout(m_texture_image, VK_FORMAT_R8G8B8A8_SRGB,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	cmd->CmdCopyBufferToImage(staging, m_texture_image, 1, 1, 1);
	cmd->CmdTransitionImageLayout(m_texture_image, VK_FORMAT_R8G8B8A8_SRGB,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	cmd->EndAndSubmit();
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

	VWrap::PipelineCreateInfo create_info{};
	create_info.extent = m_extent;
	create_info.render_pass = render_pass;
	create_info.descriptor_set_layout = m_descriptor_set_layout;
	create_info.vertex_input_info = vertexInputInfo;
	create_info.input_assembly = PipelineDefaults::TriangleList();
	create_info.dynamic_state = PipelineDefaults::DynamicViewportScissor();
	create_info.rasterizer = PipelineDefaults::BackCullFill(m_wireframe);
	create_info.depth_stencil = PipelineDefaults::DepthTestWrite();
	create_info.push_constant_ranges = {};
	create_info.subpass = 0;

	m_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_shader_code, frag_shader_code);
}

void MeshRasterizer::UpdateUniformBuffer(uint32_t frame) {
	m_accumulated_rotation += m_rotation_speed * 0.016f;

	// OBJ convention is Y-up; engine is Z-up — rotate -90° around X to correct
	static const glm::mat4 yUpToZUp = glm::rotate(glm::mat4(1.0f),
		glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	UniformBufferObject ubo{};
	ubo.model = glm::rotate(glm::mat4(1.0f), m_accumulated_rotation, glm::vec3(0.0f, 0.0f, 1.0f));
	ubo.model = glm::scale(ubo.model, glm::vec3(m_model_scale));
	ubo.model = ubo.model * yUpToZUp;
	ubo.view = m_camera->GetViewMatrix();
	ubo.proj = m_camera->GetProjectionMatrix();

	memcpy(m_uniform_buffers_mapped[frame], &ubo, sizeof(ubo));
}

std::vector<std::string> MeshRasterizer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/shader_rast.vert.spv",
		std::string(config::SHADER_DIR) + "/shader_rast.frag.spv"
	};
}

void MeshRasterizer::RecreatePipeline(const RenderContext& ctx) {
	m_pipeline.reset();
	CreatePipeline(m_render_pass);
}

std::vector<TechniqueParameter>& MeshRasterizer::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Rotation Speed", TechniqueParameter::Float, &m_rotation_speed, 0.0f, 10.0f },
			{ "Model Scale", TechniqueParameter::Float, &m_model_scale, 0.1f, 5.0f },
		};

		TechniqueParameter modelFile;
		modelFile.label = "Model";
		modelFile.type = TechniqueParameter::File;
		modelFile.filePath = &m_model_path;
		modelFile.fileFilters = {"obj"};
		modelFile.fileFilterDesc = "OBJ Models";
		modelFile.onFileChanged = [this](const std::string&) {
			m_needs_reload = true;
		};
		m_parameters.push_back(std::move(modelFile));

		TechniqueParameter texFile;
		texFile.label = "Texture";
		texFile.type = TechniqueParameter::File;
		texFile.filePath = &m_texture_path;
		texFile.fileFilters = {"png", "jpg", "jpeg", "bmp", "tga", "mtl"};
		texFile.fileFilterDesc = "Images";
		texFile.onFileChanged = [this](const std::string&) {
			m_needs_texture_reload = true;
		};
		m_parameters.push_back(std::move(texFile));
	}
	return m_parameters;
}

FrameStats MeshRasterizer::GetFrameStats() const {
	return { 1, static_cast<uint32_t>(m_vertices.size()), static_cast<uint32_t>(m_indices.size()) };
}

void MeshRasterizer::SetWireframe(bool enabled) {
	m_wireframe = enabled;
}
