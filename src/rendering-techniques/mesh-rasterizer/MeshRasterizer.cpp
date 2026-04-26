#include "MeshRasterizer.h"
#include "RenderItem.h"
#include "PipelineDefaults.h"
#include "config.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <map>

RenderTargetDesc MeshRasterizer::DescribeTargets(const RendererCaps& caps) const {
	RenderTargetDesc desc{};
	desc.color.format       = caps.swapchainFormat;
	desc.color.samples      = caps.msaaSamples;
	desc.color.needsResolve = (caps.msaaSamples != VK_SAMPLE_COUNT_1_BIT);
	desc.hasDepth     = true;
	desc.depthFormat  = caps.depthFormat;
	desc.depthSamples = caps.msaaSamples;
	return desc;
}

void MeshRasterizer::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	const TechniqueTargets& targets)
{
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_graphics_pool = ctx.graphicsPool;
	m_camera = ctx.camera;
	m_graph = &graph;

	m_sampler = VWrap::Sampler::Create(m_device);

	m_model_path = std::string(config::ASSET_DIR) + "/models/indoor plant_02.obj";
	m_texture_path = std::string(config::ASSET_DIR) + "/textures/indoor plant_02_COL.jpg";

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

	DeclareGeometryBuffers(graph);
	CreateUniformBuffers(ctx.maxFramesInFlight);

	// Binding table: per-frame UBO at binding 0 + texture sampler at binding 1.
	m_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.BindUniformBufferPerFrame(0, m_uniform_buffers, sizeof(UniformBufferObject))
		.BindExternalSampledImage(1, m_texture_image_view, m_sampler,
		                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_bindings->Build();

	auto& meshPass = graph.AddGraphicsPass("Mesh Scene");
	meshPass.AcceptsItemTypes({ RenderItemType::Mesh });
	meshPass
		.SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(targets.resolve)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/shader_rast.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/shader_rast.frag.spv";
			d.descriptorSetLayout = m_bindings->GetLayout();

			auto binding = VWrap::Vertex::getBindingDescription();
			auto attrs = VWrap::Vertex::getAttributeDescriptions();
			d.vertexBindings = { binding };
			d.vertexAttributes = { attrs.begin(), attrs.end() };

			d.inputAssembly = PipelineDefaults::TriangleList();
			// Wireframe toggle re-enters this factory via graph.RecreatePipelines(),
			// so the rasterizer state observes the current m_wireframe value.
			d.rasterizer = PipelineDefaults::BackCullFill(m_wireframe);
			d.depthStencil = PipelineDefaults::DepthTestWrite();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			auto vk_cmd = pctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());

			VkDescriptorSet ds = m_bindings->GetSet(pctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			if (!pctx.scene) return;
			for (const auto& item : pctx.scene->Get(RenderItemType::Mesh)) {
				UpdateUniformBuffer(pctx.frameIndex, item.transform);
				DrawMeshItem(pctx, item, *m_graph);
			}
		})
		.SetBindings(m_bindings);
}

void MeshRasterizer::OnPostCompile(RenderGraph& graph) {
	(void)graph;
	if (m_pending_geometry_upload) {
		UploadGeometry();
		m_pending_geometry_upload = false;
	}
}

void MeshRasterizer::EmitItems(RenderScene& scene, const RenderContext& ctx) {
	(void)ctx;
	// Single static mesh today. Skip emission entirely if the OBJ failed to load
	// — the pass record callback then iterates an empty bucket and the draw is
	// suppressed naturally (no special-casing in the pass).
	if (m_indices.empty() || m_vertex_buffer.id == UINT32_MAX) return;

	m_accumulated_rotation += m_rotation_speed * 0.016f;

	// OBJ convention is Y-up; engine is Z-up — rotate -90° around X to correct.
	static const glm::mat4 yUpToZUp = glm::rotate(glm::mat4(1.0f),
		glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	glm::mat4 model = glm::rotate(glm::mat4(1.0f), m_accumulated_rotation, glm::vec3(0.0f, 0.0f, 1.0f));
	model = glm::scale(model, glm::vec3(m_model_scale));
	model = model * yUpToZUp;

	RenderItem item{};
	item.type           = RenderItemType::Mesh;
	item.vertexBuffer   = m_vertex_buffer;
	item.indexBuffer    = m_index_buffer;
	item.indexCount     = static_cast<uint32_t>(m_indices.size());
	item.firstIndex     = 0;
	item.vertexOffset   = 0;
	item.instanceCount  = 1;
	item.firstInstance  = 0;
	item.transform      = model;
	scene.Add(item);
}

void MeshRasterizer::DeclareGeometryBuffers(RenderGraph& graph) {
	if (m_vertices.empty()) {
		m_vertex_buffer = {};
		m_index_buffer = {};
		return;
	}

	const VkDeviceSize vbSize = sizeof(m_vertices[0]) * m_vertices.size();
	const VkDeviceSize ibSize = sizeof(m_indices[0])  * m_indices.size();

	BufferDesc vbDesc{};
	vbDesc.size = vbSize;
	vbDesc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	vbDesc.lifetime = Lifetime::Persistent;
	m_vertex_buffer = graph.CreateBuffer("mesh_vertices", vbDesc);

	BufferDesc ibDesc{};
	ibDesc.size = ibSize;
	ibDesc.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	ibDesc.lifetime = Lifetime::Persistent;
	m_index_buffer = graph.CreateBuffer("mesh_indices", ibDesc);

	m_pending_geometry_upload = true;
}

void MeshRasterizer::UploadGeometry() {
	if (m_vertices.empty() || !m_graph) return;
	const VkDeviceSize vbSize = sizeof(m_vertices[0]) * m_vertices.size();
	const VkDeviceSize ibSize = sizeof(m_indices[0])  * m_indices.size();
	m_graph->UploadBufferData(m_vertex_buffer, m_vertices.data(), vbSize, m_graphics_pool);
	m_graph->UploadBufferData(m_index_buffer,  m_indices.data(),  ibSize, m_graphics_pool);
}

void MeshRasterizer::CreateUniformBuffers(uint32_t frames) {
	VkDeviceSize bufferSize = sizeof(UniformBufferObject);
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
	// Texture-reload entry point: rebind the (new) image view through the
	// existing BindingTable and re-run vkUpdateDescriptorSets.
	if (!m_bindings || !m_graph) return;
	m_bindings->ReplaceExternalSampledImage(1, m_texture_image_view, m_sampler,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_bindings->Update(*m_graph);
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
		return;
	}

	// Geometry sizes likely changed — request a graph rebuild so the persistent
	// buffers are re-declared at the new size. OnPostCompile will then upload.
	if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});

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
		m_pending_model_reload = true;
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

void MeshRasterizer::Reload(const RenderContext& ctx) {
	// Process texture first — if it's an MTL, ReloadTexture stores m_mtl_path
	// and flips m_pending_model_reload so the model reload below picks it up.
	if (m_pending_texture_reload) {
		m_pending_texture_reload = false;
		ReloadTexture(m_texture_path);
	}
	if (m_pending_model_reload) {
		m_pending_model_reload = false;
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


void MeshRasterizer::UpdateUniformBuffer(uint32_t frame, const glm::mat4& itemTransform) {
	UniformBufferObject ubo{};
	ubo.model = itemTransform;
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

std::vector<TechniqueParameter>& MeshRasterizer::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Rotation Speed", TechniqueParameter::Float, &m_rotation_speed, 0.0f, 10.0f },
			{ "Model Scale", TechniqueParameter::Float, &m_model_scale, 0.1f, 5.0f },
		};

		// Wireframe toggle: re-evaluates the pipeline factory (which closes over
		// m_wireframe) so the rasterizer state observes the new value.
		TechniqueParameter wireframe;
		wireframe.label = "Wireframe";
		wireframe.type = TechniqueParameter::Bool;
		wireframe.data = &m_wireframe;
		wireframe.onChanged = [this]() {
			if (m_eventSink) m_eventSink({AppEventType::RecreatePipelines});
		};
		m_parameters.push_back(std::move(wireframe));

		TechniqueParameter modelFile;
		modelFile.label = "Model";
		modelFile.type = TechniqueParameter::File;
		modelFile.filePath = &m_model_path;
		modelFile.fileFilters = {"obj"};
		modelFile.fileFilterDesc = "OBJ Models";
		modelFile.onFileChanged = [this](const std::string&) {
			m_pending_model_reload = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(modelFile));

		TechniqueParameter texFile;
		texFile.label = "Texture";
		texFile.type = TechniqueParameter::File;
		texFile.filePath = &m_texture_path;
		texFile.fileFilters = {"png", "jpg", "jpeg", "bmp", "tga", "mtl"};
		texFile.fileFilterDesc = "Images";
		texFile.onFileChanged = [this](const std::string&) {
			m_pending_texture_reload = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(texFile));
	}
	return m_parameters;
}

FrameStats MeshRasterizer::GetFrameStats() const {
	return { 1, static_cast<uint32_t>(m_vertices.size()), static_cast<uint32_t>(m_indices.size()) };
}
