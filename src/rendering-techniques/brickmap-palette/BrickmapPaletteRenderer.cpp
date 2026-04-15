#include "BrickmapPaletteRenderer.h"
#include "PipelineDefaults.h"
#include "config.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cmath>

// Convert HSV (h in [0,360), s,v in [0,1]) to RGB bytes
static void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
	float c = v * s;
	float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
	float m = v - c;
	float rf, gf, bf;
	if (h < 60)       { rf = c; gf = x; bf = 0; }
	else if (h < 120) { rf = x; gf = c; bf = 0; }
	else if (h < 180) { rf = 0; gf = c; bf = x; }
	else if (h < 240) { rf = 0; gf = x; bf = c; }
	else if (h < 300) { rf = x; gf = 0; bf = c; }
	else               { rf = c; gf = 0; bf = x; }
	r = static_cast<uint8_t>((rf + m) * 255.0f);
	g = static_cast<uint8_t>((gf + m) * 255.0f);
	b = static_cast<uint8_t>((bf + m) * 255.0f);
}

// Per-axis dims kept as 3 scalars to sidestep std430 vec3 alignment rules.
struct BrickmapPaletteGeneratePC {
	int shape;
	float time;
	int volume_size_x;
	int volume_size_y;
	int volume_size_z;
};

struct BrickmapPaletteBuildPC {
	int volume_size_x;
	int volume_size_y;
	int volume_size_z;
	int brick_size;
};

// Layout matches shaders/brickmap_palette_trace.frag — std140 rules: vec3 followed
// by a scalar packs into 16 bytes; mat4 starts 16-byte aligned.
struct BrickmapPaletteTracePC {
	glm::mat4 NDCtoWorld;
	glm::vec3 cameraPos;      int maxIterations;
	glm::vec3 skyColor;       int debugColor;
	glm::vec3 sunDirection;   float sunCosHalfAngle;
	glm::vec3 sunColor;       float sunIntensity;
};

void BrickmapPaletteRenderer::CreatePaletteTexture() {
	// 256-entry RGBA palette: index 0 = empty, 1-9 = shape colors
	std::array<uint8_t, 256 * 4> palette{};

	const uint8_t colors[][4] = {
		{   0,   0,   0,   0 },   // 0: empty (never sampled)
		{ 230,  60,  60, 255 },   // 1: red (sphere)
		{  60, 180,  60, 255 },   // 2: green (torus)
		{  60,  60, 230, 255 },   // 3: blue (box frame)
		{ 230, 230,  60, 255 },   // 4: yellow (cylinder)
		{ 230, 130,  60, 255 },   // 5: orange (cone)
		{ 180,  60, 230, 255 },   // 6: purple (octahedron)
		{  60, 230, 230, 255 },   // 7: cyan (gyroid)
		{ 230,  60, 180, 255 },   // 8: pink (sine blob)
		{ 180, 180, 180, 255 },   // 9: gray (menger sponge)
	};

	for (int i = 0; i < 10; i++) {
		std::memcpy(&palette[i * 4], colors[i], 4);
	}
	// Fill indices 10-255 with a full HSV rainbow (high saturation, full value)
	for (int i = 10; i < 256; i++) {
		float hue = static_cast<float>(i - 10) / 246.0f * 360.0f;
		hsvToRgb(hue, 0.85f, 0.95f, palette[i * 4 + 0], palette[i * 4 + 1], palette[i * 4 + 2]);
		palette[i * 4 + 3] = 255;
	}

	VkDeviceSize imageSize = 256 * 4;
	auto staging = VWrap::Buffer::CreateStaging(m_allocator, imageSize);
	void* data = staging->Map();
	std::memcpy(data, palette.data(), imageSize);
	staging->Unmap();

	VWrap::ImageCreateInfo info{};
	info.width = 256;
	info.height = 1;
	info.depth = 1;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	info.mip_levels = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.image_type = VK_IMAGE_TYPE_2D;

	m_palette_image = VWrap::Image::Create(m_allocator, info);

	auto cmd = VWrap::CommandBuffer::Create(m_graphics_pool);
	cmd->BeginSingle();
	cmd->CmdTransitionImageLayout(m_palette_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	cmd->CmdCopyBufferToImage(staging, m_palette_image, 256, 1, 1);
	cmd->CmdTransitionImageLayout(m_palette_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	cmd->EndAndSubmit();

	m_palette_image_view = VWrap::ImageView::Create(m_device, m_palette_image, VK_IMAGE_ASPECT_COLOR_BIT);
}

void BrickmapPaletteRenderer::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	ImageHandle colorTarget,
	ImageHandle depthTarget,
	ImageHandle resolveTarget)
{
	auto logger = spdlog::get("Render");
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_graphics_pool = ctx.graphicsPool;
	m_extent = ctx.extent;
	m_camera = ctx.camera;
	m_lighting = ctx.lighting;
	m_start_time = std::chrono::steady_clock::now();

	m_graph = &graph;
	m_needs_rebuild = false; // clear rebuild flag (we're rebuilding now)

	glm::uvec3 vs = m_volume_size;
	glm::uvec3 grid_dim = vs / 8u;
	uint32_t grid_cells = grid_dim.x * grid_dim.y * grid_dim.z;
	// Header is 8 uint32 (32 bytes): vs_x, vs_y, vs_z, brick_size, gd_x, gd_y, gd_z, brick_count
	VkDeviceSize brickmapSize = (8 + grid_cells + grid_cells * 128) * sizeof(uint32_t);

	logger->info("BrickmapPaletteRenderer: volume={}x{}x{}, grid={}x{}x{}, buffer={} bytes",
		vs.x, vs.y, vs.z, grid_dim.x, grid_dim.y, grid_dim.z, brickmapSize);

	// 3D storage image for voxel volume (R8_UINT: material index per voxel)
	// extraUsage: TRANSFER_DST for CPU-side .vox upload via staging buffer
	m_volume = graph.CreateImage("brickmap_palette_volume", {
		vs.x, vs.y, vs.z,
		VK_FORMAT_R8_UINT,
		VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_TYPE_3D,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT
	});

	// Brickmap structure buffer (dynamic layout):
	//   Header:       8 uint32 (32 bytes)
	//   Top grid:     gd_x * gd_y * gd_z uint32
	//   Brick pool:   up to that many bricks * 128 uint32
	m_brickmap_buffer = graph.CreateBuffer("brickmap_palette_data", {
		brickmapSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
	});

	// Compute pass: generate voxels into volume
	graph.AddComputePass("Brickmap Palette Generate")
		.Write(m_volume)
		.SetRecord([this, vs](PassContext& ctx) {
			ctx.cmd->CmdBindComputePipeline(m_compute_pipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(m_compute_pipeline->GetLayout(),
				{ m_compute_descriptor_set->Get() });
			BrickmapPaletteGeneratePC pc{};
			pc.shape = m_shape;
			auto now = std::chrono::steady_clock::now();
			pc.time = std::chrono::duration<float>(now - m_start_time).count() * m_time_scale;
			pc.volume_size_x = static_cast<int>(vs.x);
			pc.volume_size_y = static_cast<int>(vs.y);
			pc.volume_size_z = static_cast<int>(vs.z);
			vkCmdPushConstants(ctx.cmd->Get(), m_compute_pipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			// local_size = 4; dispatch per-axis (ceil-div so small axes get ≥1 group)
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			ctx.cmd->CmdDispatch(cdiv(vs.x, 4), cdiv(vs.y, 4), cdiv(vs.z, 4));
		});

	// Compute pass: build brickmap from voxel volume
	graph.AddComputePass("Brickmap Palette Build")
		.Read(m_volume)
		.Write(m_brickmap_buffer)
		.SetRecord([this, vs, grid_dim](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();

			// Zero the brick_count atomic counter.
			// New header layout: brick_count is at uint32 index 7 → byte offset 28.
			vkCmdFillBuffer(vk_cmd, m_brickmap_vk_buffer, 28, 4, 0);

			VkMemoryBarrier memBarrier{};
			memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(vk_cmd,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 1, &memBarrier, 0, nullptr, 0, nullptr);

			ctx.cmd->CmdBindComputePipeline(m_build_pipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(m_build_pipeline->GetLayout(),
				{ m_build_descriptor_set->Get() });
			BrickmapPaletteBuildPC pc{};
			pc.volume_size_x = static_cast<int>(vs.x);
			pc.volume_size_y = static_cast<int>(vs.y);
			pc.volume_size_z = static_cast<int>(vs.z);
			pc.brick_size = 8;
			vkCmdPushConstants(vk_cmd, m_build_pipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			// local_size = 4; dispatch per-axis grid cells (ceil-div)
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			ctx.cmd->CmdDispatch(cdiv(grid_dim.x, 4), cdiv(grid_dim.y, 4), cdiv(grid_dim.z, 4));
		});

	// Graphics pass: brickmap two-level DDA ray-march with palette colors
	auto& gfx = graph.AddGraphicsPass("Brickmap Palette Trace")
		.SetColorAttachment(colorTarget, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(depthTarget, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(resolveTarget)
		.Read(m_brickmap_buffer)
		.SetRecord([this](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphics_pipeline->Get());
			VkDescriptorSet ds = m_graphics_descriptor_sets[ctx.frameIndex]->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_graphics_pipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			BrickmapPaletteTracePC pc{};
			pc.NDCtoWorld = m_camera->GetNDCtoWorldMatrix();
			pc.cameraPos = m_camera->GetPosition();
			pc.maxIterations = m_max_iterations;
			pc.skyColor = glm::vec3(m_sky_color[0], m_sky_color[1], m_sky_color[2]);
			pc.debugColor = m_debug_color ? 1 : 0;
			if (m_lighting) {
				pc.sunDirection = m_lighting->GetSunDirection();
				pc.sunCosHalfAngle = m_lighting->GetSunCosHalfAngle();
				pc.sunColor = glm::vec3(m_lighting->sunColor[0], m_lighting->sunColor[1], m_lighting->sunColor[2]);
				pc.sunIntensity = m_lighting->sunIntensity;
			} else {
				// No lighting state — place the sun below the horizon so the disk test
				// never passes and the sky renders as pure gradient.
				pc.sunDirection = glm::vec3(0.0f, 0.0f, -1.0f);
				pc.sunCosHalfAngle = 1.0f;
				pc.sunColor = glm::vec3(1.0f);
				pc.sunIntensity = 0.0f;
			}
			vkCmdPushConstants(vk_cmd, m_graphics_pipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BrickmapPaletteTracePC), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		});

	// Generate descriptors (1 storage image)
	logger->debug("BrickmapPaletteRenderer: Creating generate descriptors...");
	auto computeDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.Build(1);
	m_compute_descriptor_layout = computeDesc.layout;
	m_compute_descriptor_pool = computeDesc.pool;
	m_compute_descriptor_set = computeDesc.sets[0];

	logger->debug("BrickmapPaletteRenderer: Creating generate pipeline...");
	CreateComputePipeline();

	// Build descriptors (storage image + storage buffer)
	logger->debug("BrickmapPaletteRenderer: Creating build descriptors...");
	auto buildDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		.Build(1);
	m_build_descriptor_layout = buildDesc.layout;
	m_build_descriptor_pool = buildDesc.pool;
	m_build_descriptor_set = buildDesc.sets[0];

	logger->debug("BrickmapPaletteRenderer: Creating build pipeline...");
	CreateBuildPipeline();

	// Graphics descriptors (per-frame: brickmap buffer + palette sampler)
	logger->debug("BrickmapPaletteRenderer: Creating graphics descriptors...");
	auto gfxDesc = DescriptorSetBuilder(m_device)
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.Build(ctx.maxFramesInFlight);
	m_graphics_descriptor_layout = gfxDesc.layout;
	m_graphics_descriptor_pool = gfxDesc.pool;
	m_graphics_descriptor_sets = gfxDesc.sets;

	m_render_pass = gfx.GetRenderPassPtr();

	logger->debug("BrickmapPaletteRenderer: Creating graphics pipeline...");
	CreateGraphicsPipeline();

	logger->debug("BrickmapPaletteRenderer: Creating sampler...");
	m_sampler = VWrap::Sampler::Create(m_device);

	logger->debug("BrickmapPaletteRenderer: Creating palette texture...");
	CreatePaletteTexture();

	logger->debug("BrickmapPaletteRenderer: Initialized via RegisterPasses");
}

void BrickmapPaletteRenderer::WriteGraphDescriptors(RenderGraph& graph) {
	auto volumeView = graph.GetImageView(m_volume);
	auto brickmapBuffer = graph.GetBuffer(m_brickmap_buffer);
	m_brickmap_vk_buffer = brickmapBuffer->Get();

	// Upload pending .vox data after graph rebuild
	if (m_pending_vox) {
		m_vox_active = true;
		graph.SetPassEnabled("Brickmap Palette Generate", false);
		UploadVolumeData(m_pending_vox->volume.data());
		UploadPalette(m_pending_vox->palette.data());
		spdlog::get("Render")->info("BrickmapPaletteRenderer: Uploaded pending .vox ({}x{}x{}, volume={}x{}x{})",
			m_pending_vox->sizeX, m_pending_vox->sizeY, m_pending_vox->sizeZ,
			m_pending_vox->volumeSize.x, m_pending_vox->volumeSize.y, m_pending_vox->volumeSize.z);
		m_pending_vox.reset();
	}

	// Generate descriptor: storage image in GENERAL layout
	{
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageView = volumeView->Get();
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = m_compute_descriptor_set->Get();
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		write.pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(m_device->Get(), 1, &write, 0, nullptr);
	}

	// Build descriptors: storage image (read) + storage buffer (write)
	{
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageView = volumeView->Get();
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = brickmapBuffer->Get();
		bufferInfo.offset = 0;
		bufferInfo.range = VK_WHOLE_SIZE;

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = m_build_descriptor_set->Get();
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[0].pImageInfo = &imageInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = m_build_descriptor_set->Get();
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].pBufferInfo = &bufferInfo;

		vkUpdateDescriptorSets(m_device->Get(), 2, writes, 0, nullptr);
	}

	// Graphics descriptors: brickmap buffer + palette texture per frame
	for (auto& ds : m_graphics_descriptor_sets) {
		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = brickmapBuffer->Get();
		bufferInfo.offset = 0;
		bufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorImageInfo paletteInfo{};
		paletteInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		paletteInfo.imageView = m_palette_image_view->Get();
		paletteInfo.sampler = m_sampler->Get();

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = ds->Get();
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[0].pBufferInfo = &bufferInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = ds->Get();
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[1].pImageInfo = &paletteInfo;

		vkUpdateDescriptorSets(m_device->Get(), 2, writes, 0, nullptr);
	}
}

void BrickmapPaletteRenderer::CreateComputePipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_palette_generate.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(BrickmapPaletteGeneratePC);

	m_compute_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_compute_descriptor_layout, { pushRange }, comp_code);
}

void BrickmapPaletteRenderer::CreateBuildPipeline() {
	auto comp_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_palette_build.comp.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(BrickmapPaletteBuildPC);

	m_build_pipeline = VWrap::ComputePipeline::Create(
		m_device, m_build_descriptor_layout, { pushRange }, comp_code);
}

void BrickmapPaletteRenderer::CreateGraphicsPipeline() {
	auto vert_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_palette_trace.vert.spv");
	auto frag_code = VWrap::readFile(std::string(config::SHADER_DIR) + "/brickmap_palette_trace.frag.spv");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(BrickmapPaletteTracePC);

	auto create_info = PipelineDefaults::FullscreenQuad(
		m_render_pass, m_graphics_descriptor_layout, m_extent, { pushRange });

	m_graphics_pipeline = VWrap::Pipeline::Create(m_device, create_info, vert_code, frag_code);
}

void BrickmapPaletteRenderer::OnResize(VkExtent2D newExtent, RenderGraph& graph) {
	m_extent = newExtent;
	WriteGraphDescriptors(graph);
}

std::vector<std::string> BrickmapPaletteRenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/brickmap_palette_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/brickmap_palette_build.comp.spv",
		std::string(config::SHADER_DIR) + "/brickmap_palette_trace.vert.spv",
		std::string(config::SHADER_DIR) + "/brickmap_palette_trace.frag.spv"
	};
}

void BrickmapPaletteRenderer::RecreatePipeline(const RenderContext& ctx) {
	m_graphics_pipeline.reset();
	m_compute_pipeline.reset();
	m_build_pipeline.reset();
	CreateComputePipeline();
	CreateBuildPipeline();
	CreateGraphicsPipeline();
}

std::vector<TechniqueParameter>& BrickmapPaletteRenderer::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Shape", TechniqueParameter::Enum, &m_shape, 0.0f, 0.0f,
				{ "Sphere", "Torus", "Box Frame", "Cylinder", "Cone", "Octahedron", "Gyroid", "Sine Blob", "Menger Sponge" } },
			{ "Time Scale", TechniqueParameter::Float, &m_time_scale, 0.0f, 5.0f },
			{ "Max Iterations", TechniqueParameter::Int, &m_max_iterations, 1.0f, 500.0f },
			{ "Sky Color", TechniqueParameter::Color3, m_sky_color },
			{ "Debug Coloring", TechniqueParameter::Bool, &m_debug_color },
		};

		TechniqueParameter voxFile;
		voxFile.label = "VOX Model";
		voxFile.type = TechniqueParameter::File;
		voxFile.filePath = &m_vox_file_path;
		voxFile.fileFilters = {"vox"};
		voxFile.fileFilterDesc = "MagicaVoxel Models";
		voxFile.onFileChanged = [this](const std::string&) {
			m_needs_reload = true;
		};
		m_parameters.push_back(std::move(voxFile));
	}
	return m_parameters;
}

FrameStats BrickmapPaletteRenderer::GetFrameStats() const {
	return { 3, 4, 0 };
}

void BrickmapPaletteRenderer::PerformReload(const RenderContext& ctx) {
	m_needs_reload = false;
	auto logger = spdlog::get("Render");

	if (m_vox_file_path.empty()) {
		// Switch back to procedural mode
		if (m_vox_active) {
			logger->info("BrickmapPaletteRenderer: Restoring procedural mode");
			m_vox_active = false;
			glm::uvec3 procSize(128, 128, 128);
			if (m_volume_size != procSize) {
				m_volume_size = procSize;
				m_needs_rebuild = true; // graph rebuild will re-enable generate pass
			} else {
				m_graph->SetPassEnabled("Brickmap Palette Generate", true);
				CreatePaletteTexture();
			}
		}
		return;
	}

	auto model = LoadVoxFile(m_vox_file_path);
	if (!model) {
		logger->error("BrickmapPaletteRenderer: Failed to load .vox file");
		return;
	}

	if (model->volumeSize != m_volume_size) {
		// Volume size changed — need graph rebuild
		m_volume_size = model->volumeSize;
		m_pending_vox = std::move(*model);
		m_needs_rebuild = true;
		logger->info("BrickmapPaletteRenderer: Volume resize to {}x{}x{}, rebuilding graph",
			m_volume_size.x, m_volume_size.y, m_volume_size.z);
		return;
	}

	// Same size — upload directly
	m_vox_active = true;
	m_graph->SetPassEnabled("Brickmap Palette Generate", false);

	UploadVolumeData(model->volume.data());
	UploadPalette(model->palette.data());

	// Recompile shaders + rebuild pipelines so the refreshed model is visible immediately
	RecreatePipeline(ctx);

	logger->info("BrickmapPaletteRenderer: Loaded .vox model ({}x{}x{}, volume={}x{}x{})",
		model->sizeX, model->sizeY, model->sizeZ,
		model->volumeSize.x, model->volumeSize.y, model->volumeSize.z);
}

void BrickmapPaletteRenderer::UploadVolumeData(const uint8_t* data) {
	glm::uvec3 vs = m_volume_size;
	VkDeviceSize size = static_cast<VkDeviceSize>(vs.x) * vs.y * vs.z;

	auto staging = VWrap::Buffer::CreateStaging(m_allocator, size);
	void* mapped = staging->Map();
	std::memcpy(mapped, data, size);
	staging->Unmap();

	auto volumeImage = m_graph->GetImage(m_volume);

	auto cmd = VWrap::CommandBuffer::Create(m_graphics_pool);
	cmd->BeginSingle();
	cmd->CmdTransitionImageLayout(volumeImage, VK_FORMAT_R8_UINT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	cmd->CmdCopyBufferToImage(staging, volumeImage, vs.x, vs.y, vs.z);
	cmd->CmdTransitionImageLayout(volumeImage, VK_FORMAT_R8_UINT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	cmd->EndAndSubmit();
}

void BrickmapPaletteRenderer::UploadPalette(const uint8_t* rgbaData) {
	VkDeviceSize imageSize = 256 * 4;

	auto staging = VWrap::Buffer::CreateStaging(m_allocator, imageSize);
	void* mapped = staging->Map();
	std::memcpy(mapped, rgbaData, imageSize);
	staging->Unmap();

	auto cmd = VWrap::CommandBuffer::Create(m_graphics_pool);
	cmd->BeginSingle();
	cmd->CmdTransitionImageLayout(m_palette_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	cmd->CmdCopyBufferToImage(staging, m_palette_image, 256, 1, 1);
	cmd->CmdTransitionImageLayout(m_palette_image, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	cmd->EndAndSubmit();
}
