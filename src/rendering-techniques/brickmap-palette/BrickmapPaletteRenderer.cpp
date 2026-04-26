#include "BrickmapPaletteRenderer.h"
#include "RenderItem.h"
#include "RenderScene.h"
#include "PipelineDefaults.h"
#include "config.h"
#include <spdlog/spdlog.h>
#include <cstring>

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
	float ambientIntensity;   float aoStrength;
	int   shadowsEnabled;     int   _pad0;
};
static_assert(sizeof(BrickmapPaletteTracePC) == 144,
	"BrickmapPaletteTracePC must stay in std140 layout — 144 bytes");

RenderTargetDesc BrickmapPaletteRenderer::DescribeTargets(const RendererCaps& caps) const {
	RenderTargetDesc desc{};
	desc.color.format       = caps.swapchainFormat;
	desc.color.samples      = caps.msaaSamples;
	desc.color.needsResolve = (caps.msaaSamples != VK_SAMPLE_COUNT_1_BIT);
	desc.hasDepth     = true;
	desc.depthFormat  = caps.depthFormat;
	desc.depthSamples = caps.msaaSamples;
	return desc;
}

void BrickmapPaletteRenderer::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	const TechniqueTargets& targets)
{
	auto logger = spdlog::get("Render");
	m_device = ctx.device;
	m_allocator = ctx.allocator;
	m_graphics_pool = ctx.graphicsPool;
	m_camera = ctx.camera;
	m_lighting = ctx.lighting;
	m_start_time = std::chrono::steady_clock::now();

	m_graph  = &graph;
	m_assets = ctx.assets;
	m_world  = ctx.world;

	// First-time setup: register a procedural voxel-volume asset and wire up
	// the scene node carrying it. Subsequent graph rebuilds skip this; the
	// asset is already known to the registry and the scene node already exists.
	if (!m_volume_asset.valid()) {
		m_volume_asset = m_assets->CreateProceduralVoxelVolume(
			"brickmap_palette_volume", m_volume_size, VK_FORMAT_R8_UINT);
		if (m_world && !m_node) {
			m_node = m_world->GetRoot().AddChild("brickmap_palette_node");
			Component c{};
			c.type  = ComponentType::VoxelVolume;
			c.asset = m_volume_asset;
			m_node->AddComponent(c);
		}
	} else {
		// Procedural-volume size may have changed (.vox load → procedural
		// transition); make sure the registry reflects current state.
		auto* va = m_assets->GetVoxelVolume(m_volume_asset);
		if (va && va->isProcedural && va->size != m_volume_size) {
			m_assets->ResizeProceduralVoxelVolume(m_volume_asset, m_volume_size);
		}
	}

	// Resolve the live ImageHandle (re-allocated by the registry's
	// DeclareGraphResources earlier in this build).
	const auto* vol = m_assets->GetVoxelVolume(m_volume_asset);
	m_volume = vol ? vol->volumeImage : ImageHandle{};
	const glm::uvec3 vs = vol ? vol->size : m_volume_size;
	const glm::uvec3 grid_dim = vs / 8u;
	const uint32_t grid_cells = grid_dim.x * grid_dim.y * grid_dim.z;
	// Header is 8 uint32 (32 bytes): vs_x, vs_y, vs_z, brick_size, gd_x, gd_y, gd_z, brick_count
	VkDeviceSize brickmapSize = (8 + grid_cells + grid_cells * 128) * sizeof(uint32_t);

	logger->info("BrickmapPaletteRenderer: volume={}x{}x{}, grid={}x{}x{}, buffer={} bytes",
		vs.x, vs.y, vs.z, grid_dim.x, grid_dim.y, grid_dim.z, brickmapSize);

	m_brickmap_buffer = graph.CreateBuffer("brickmap_palette_data", {
		brickmapSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
	});

	if (!m_palette) {
		m_palette = std::make_unique<PaletteResource>(m_device, m_allocator, m_graphics_pool);
		m_palette->Create();
	}

	// Binding tables.
	m_compute_bindings = std::make_shared<BindingTable>(m_device, 1);
	m_compute_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.BindGraphStorageImage(0, m_volume);
	m_compute_bindings->Build();

	m_build_bindings = std::make_shared<BindingTable>(m_device, 1);
	m_build_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		.BindGraphStorageImage(0, m_volume)
		.BindGraphStorageBuffer(1, m_brickmap_buffer);
	m_build_bindings->Build();

	m_graphics_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_graphics_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		.BindGraphStorageBuffer(0, m_brickmap_buffer)
		.BindExternalSampledImage(1, m_palette->GetImageView(), m_palette->GetSampler(),
		                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_graphics_bindings->Build();

	// Compute pass: generate voxels into volume
	graph.AddComputePass("Brickmap Palette Generate")
		.Write(m_volume, ResourceUsage::StorageWrite)
		.SetPipeline([this]() {
			ComputePipelineDesc d;
			d.compSpvPath = std::string(config::SHADER_DIR) + "/brickmap_palette_generate.comp.spv";
			d.descriptorSetLayout = m_compute_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			r.offset = 0;
			r.size = sizeof(BrickmapPaletteGeneratePC);
			d.pushConstantRanges = { r };
			return d;
		})
		.SetRecord([this, vs](PassContext& ctx) {
			ctx.cmd->CmdBindComputePipeline(ctx.computePipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(ctx.computePipeline->GetLayout(),
				{ m_compute_bindings->GetSet(0)->Get() });
			BrickmapPaletteGeneratePC pc{};
			pc.shape = m_shape;
			auto now = std::chrono::steady_clock::now();
			pc.time = std::chrono::duration<float>(now - m_start_time).count() * m_time_scale;
			pc.volume_size_x = static_cast<int>(vs.x);
			pc.volume_size_y = static_cast<int>(vs.y);
			pc.volume_size_z = static_cast<int>(vs.z);
			vkCmdPushConstants(ctx.cmd->Get(), ctx.computePipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			ctx.cmd->CmdDispatch(cdiv(vs.x, 4), cdiv(vs.y, 4), cdiv(vs.z, 4));
		})
		.SetBindings(m_compute_bindings);

	// Compute pass: build brickmap from voxel volume
	graph.AddComputePass("Brickmap Palette Build")
		.Read(m_volume, ResourceUsage::StorageRead)
		.Write(m_brickmap_buffer, ResourceUsage::StorageWrite)
		.SetPipeline([this]() {
			ComputePipelineDesc d;
			d.compSpvPath = std::string(config::SHADER_DIR) + "/brickmap_palette_build.comp.spv";
			d.descriptorSetLayout = m_build_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			r.offset = 0;
			r.size = sizeof(BrickmapPaletteBuildPC);
			d.pushConstantRanges = { r };
			return d;
		})
		.SetRecord([this, vs, grid_dim](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();

			// Zero the brick_count atomic counter (uint32 index 7 → byte offset 28).
			vkCmdFillBuffer(vk_cmd, m_graph->GetVkBuffer(m_brickmap_buffer), 28, 4, 0);

			VkMemoryBarrier memBarrier{};
			memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			memBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(vk_cmd,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 1, &memBarrier, 0, nullptr, 0, nullptr);

			ctx.cmd->CmdBindComputePipeline(ctx.computePipeline);
			ctx.cmd->CmdBindComputeDescriptorSets(ctx.computePipeline->GetLayout(),
				{ m_build_bindings->GetSet(0)->Get() });
			BrickmapPaletteBuildPC pc{};
			pc.volume_size_x = static_cast<int>(vs.x);
			pc.volume_size_y = static_cast<int>(vs.y);
			pc.volume_size_z = static_cast<int>(vs.z);
			pc.brick_size = 8;
			vkCmdPushConstants(vk_cmd, ctx.computePipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			ctx.cmd->CmdDispatch(cdiv(grid_dim.x, 4), cdiv(grid_dim.y, 4), cdiv(grid_dim.z, 4));
		})
		.SetBindings(m_build_bindings);

	// Graphics pass: brickmap two-level DDA ray-march with palette colors.
	// Consumes BrickmapVolume items emitted by the SceneExtractor (one per
	// scene node carrying VoxelVolumeComponent). For v1 the trace shader
	// still draws a fullscreen quad — bounding-box rasterization is the
	// foliage-step migration of this technique.
	auto& tracePass = graph.AddGraphicsPass("Brickmap Palette Trace");
	tracePass.AcceptsItemTypes({ RenderItemType::BrickmapVolume });
	tracePass
		.SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(targets.resolve)
		.Read(m_brickmap_buffer, ResourceUsage::StorageRead)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/brickmap_palette_trace.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/brickmap_palette_trace.frag.spv";
			d.descriptorSetLayout = m_graphics_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			r.offset = 0;
			r.size = sizeof(BrickmapPaletteTracePC);
			d.pushConstantRanges = { r };
			d.inputAssembly = PipelineDefaults::TriangleStrip();
			d.rasterizer = PipelineDefaults::NoCullFill();
			d.depthStencil = PipelineDefaults::NoDepthTest();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this](PassContext& ctx) {
			auto vk_cmd = ctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.graphicsPipeline->Get());
			VkDescriptorSet ds = m_graphics_bindings->GetSet(ctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				ctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

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
				pc.ambientIntensity = m_lighting->ambientIntensity;
				pc.aoStrength = m_lighting->aoStrength;
				pc.shadowsEnabled = m_lighting->shadowsEnabled ? 1 : 0;
			} else {
				pc.sunDirection = glm::vec3(0.0f, 0.0f, -1.0f);
				pc.sunCosHalfAngle = 1.0f;
				pc.sunColor = glm::vec3(1.0f);
				pc.sunIntensity = 0.0f;
				pc.ambientIntensity = 1.0f;
				pc.aoStrength = 0.0f;
				pc.shadowsEnabled = 0;
			}
			pc._pad0 = 0;
			vkCmdPushConstants(vk_cmd, ctx.graphicsPipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BrickmapPaletteTracePC), &pc);

			if (!ctx.scene) return;
			// V1 BrickmapVolume items still drive a fullscreen quad — the
			// trace shader reads its bound volume from the descriptor set.
			// Future work (bounded brickmap rasterization) will swap this
			// for a bounding-box rasterized draw using item.aabb*.
			for (const auto& item : ctx.scene->Get(RenderItemType::BrickmapVolume)) {
				DrawFullscreenItem(ctx, item);
			}
		})
		.SetBindings(m_graphics_bindings);

	logger->debug("BrickmapPaletteRenderer: Initialized via RegisterPasses");
}

void BrickmapPaletteRenderer::OnPostCompile(RenderGraph& graph) {
	// AssetRegistry::UploadPending has already pushed any .vox bytes into the
	// volume image by the time we run. We just need to (a) tell the Generate
	// compute pass to stand down if we're in file-loaded mode, and (b) push
	// the .vox-supplied palette into the PaletteResource so the trace shader
	// renders with file colors.
	if (!m_assets) return;
	const auto* vol = m_assets->GetVoxelVolume(m_volume_asset);
	if (!vol) return;

	if (vol->isProcedural) {
		graph.SetPassEnabled("Brickmap Palette Generate", true);
	} else {
		graph.SetPassEnabled("Brickmap Palette Generate", false);
		if (m_palette) m_palette->Upload(vol->palette.data());
		spdlog::get("Render")->debug("BrickmapPaletteRenderer: Re-applied loaded .vox ({}x{}x{})",
			vol->size.x, vol->size.y, vol->size.z);
	}
}

std::vector<std::string> BrickmapPaletteRenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/brickmap_palette_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/brickmap_palette_build.comp.spv",
		std::string(config::SHADER_DIR) + "/brickmap_palette_trace.vert.spv",
		std::string(config::SHADER_DIR) + "/brickmap_palette_trace.frag.spv"
	};
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
			m_pending_reload = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(voxFile));
	}
	return m_parameters;
}

FrameStats BrickmapPaletteRenderer::GetFrameStats() const {
	return { 3, 4, 0 };
}

void BrickmapPaletteRenderer::Reload(const RenderContext& ctx) {
	(void)ctx;
	if (!m_pending_reload) return;
	m_pending_reload = false;
	auto logger = spdlog::get("Render");
	if (!m_assets) return;

	if (m_vox_file_path.empty()) {
		// Switch back to procedural mode. Resize the asset back to the
		// procedural default (128^3) and rebuild the graph so the new size
		// takes effect.
		const auto* vol = m_assets->GetVoxelVolume(m_volume_asset);
		if (vol && !vol->isProcedural) {
			logger->info("BrickmapPaletteRenderer: Restoring procedural mode");
			VoxModel empty{};  // discarded data; the procedural conversion below ignores it
			(void)empty;
			// We can't toggle isProcedural directly through ReplaceVoxelVolume
			// (it always sets isProcedural=false). Instead, replace the asset
			// slot with a fresh procedural one at the desired size. The old
			// asset's image will be re-allocated as part of the next graph
			// rebuild via DeclareGraphResources; the AssetID stays valid since
			// it just indexes into m_volumes.
			//
			// In v1 this is simpler: just call ResizeProceduralVoxelVolume on
			// the asset (it asserts isProcedural). Force isProcedural=true
			// directly via the registry's mutable getter — small carve-out
			// while we get more general "asset mode swap" later.
			auto* mutVol = m_assets->GetVoxelVolume(m_volume_asset);
			if (mutVol) {
				mutVol->isProcedural = true;
				mutVol->data.clear();
				mutVol->needsUpload = false;
				m_volume_size = glm::uvec3(128, 128, 128);
				mutVol->size = m_volume_size;
				if (m_palette) m_palette->RestoreDefault();
			}
			if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});
		}
		return;
	}

	auto model = LoadVoxFile(m_vox_file_path);
	if (!model) {
		logger->error("BrickmapPaletteRenderer: Failed to load .vox file");
		return;
	}
	const glm::uvec3 newSize = model->volumeSize;
	m_volume_size = newSize;

	// Hand the .vox bytes to the registry; UploadPending pushes them into the
	// volume image during the next graph rebuild.
	const bool sizeChanged = m_assets->ReplaceVoxelVolume(m_volume_asset,
		std::move(*model), m_vox_file_path);

	logger->info("BrickmapPaletteRenderer: Loaded .vox model (volume={}x{}x{}, sizeChanged={})",
		newSize.x, newSize.y, newSize.z, sizeChanged);

	// Always request a graph rebuild — the volume image needs re-allocation
	// for size changes, and the post-compile hook does the palette + pass-enable
	// swap that switches us into file-loaded mode.
	if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});
}
