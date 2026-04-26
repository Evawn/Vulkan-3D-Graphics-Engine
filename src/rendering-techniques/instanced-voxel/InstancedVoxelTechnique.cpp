#include "InstancedVoxelTechnique.h"
#include "RenderItem.h"
#include "RenderScene.h"
#include "PipelineDefaults.h"
#include "config.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <random>

namespace {
constexpr const char* kVolumeAssetName = "instanced_voxel_blade_v1";
constexpr const char* kInstanceBufferName = "instanced_voxel_instances";
constexpr const char* kSceneNodeName = "instanced_voxel_grass";
constexpr const char* kGenerateName  = "InstancedVoxel Generate";

// CPU layout must match shaders/instanced_voxel.vert::InstanceData.
// std430-compatible: 16-byte alignment for vec3+scalar pairs, vec4 for quat.
struct GpuInstance {
	glm::vec3 position;     float scale;
	glm::vec4 rotation;     // quaternion (xyz, w)
	float     animOffset;
	float     _pad0;
	float     _pad1;
	float     _pad2;
};
static_assert(sizeof(GpuInstance) == 48, "GpuInstance must be 48 bytes (std430)");

struct VolumeMetaUbo {
	int32_t sizeX, sizeY, sizeZ;
	int32_t frameCount;
};

struct GeneratePC {
	int32_t sizeX, sizeY, sizeZ;
	int32_t frameCount;
};

struct InstancedVoxelPC {
	glm::mat4 cloudWorld;
	glm::mat4 viewProj;
	glm::vec3 cameraPos;      int32_t maxIterations;
	glm::vec3 skyColor;       int32_t debugColor;
	glm::vec3 sunDirection;   float   sunCosHalfAngle;
	glm::vec3 sunColor;       float   sunIntensity;
	glm::vec3 aabbMin;        float   ambientIntensity;
	glm::vec3 aabbMax;        float   aoStrength;
	int32_t   shadowsEnabled; float   time;
	int32_t   frameCount;     int32_t _pad0;
};
static_assert(sizeof(InstancedVoxelPC) == 240, "InstancedVoxelPC must stay std140-compatible");
} // namespace

RenderTargetDesc InstancedVoxelTechnique::DescribeTargets(const RendererCaps& caps) const {
	RenderTargetDesc desc{};
	desc.color.format       = caps.swapchainFormat;
	desc.color.samples      = caps.msaaSamples;
	desc.color.needsResolve = (caps.msaaSamples != VK_SAMPLE_COUNT_1_BIT);
	desc.hasDepth     = true;
	desc.depthFormat  = caps.depthFormat;
	desc.depthSamples = caps.msaaSamples;
	return desc;
}

void InstancedVoxelTechnique::RegisterPasses(
	RenderGraph& graph,
	const RenderContext& ctx,
	const TechniqueTargets& targets)
{
	auto logger = spdlog::get("Render");

	m_device        = ctx.device;
	m_allocator     = ctx.allocator;
	m_graphics_pool = ctx.graphicsPool;
	m_camera        = ctx.camera;
	m_assets        = ctx.assets;
	m_world         = ctx.world;
	m_lighting      = ctx.lighting;
	m_sky           = ctx.sky;
	if (m_start_time.time_since_epoch().count() == 0) {
		m_start_time = std::chrono::steady_clock::now();
	}

	// First-time setup: register a procedural animated voxel asset and a
	// scene node carrying an InstanceCloudComponent. Subsequent rebuilds
	// re-resolve handles and re-create the instance buffer.
	if (!m_volume_asset.valid()) {
		m_volume_asset = m_assets->CreateProceduralAnimatedVoxelVolume(
			kVolumeAssetName, m_volume_size, m_frame_count, VK_FORMAT_R8_UINT);
		if (m_world && !m_node) {
			m_node = m_world->GetRoot().AddChild(kSceneNodeName);
			m_node->position = glm::vec3(0.0f, 0.0f, 0.0f);
			Component c{};
			c.type            = ComponentType::InstanceCloud;
			c.asset           = m_volume_asset;
			c.frameCount      = m_frame_count;
			c.instanceAabbMin = glm::vec3(0.0f);
			c.instanceAabbMax = glm::vec3(static_cast<float>(m_volume_size.x),
			                              static_cast<float>(m_volume_size.y),
			                              static_cast<float>(m_volume_size.z));
			m_node->AddComponent(c);
		}
	}

	// Resolve live handles from the registry (re-allocated on every rebuild).
	const auto* vol = m_assets->GetVoxelVolume(m_volume_asset);
	m_volume = vol ? vol->volumeImage : ImageHandle{};

	// Per-instance SSBO. Persistent so it survives resize; rebuilt on grid-dim change.
	const uint32_t instanceCount = static_cast<uint32_t>(m_grid_dim) * static_cast<uint32_t>(m_grid_dim);
	m_instance_count = instanceCount;
	BufferDesc bd{};
	bd.size     = sizeof(GpuInstance) * instanceCount;
	bd.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bd.lifetime = Lifetime::Persistent;
	m_instance_buffer = graph.CreateBuffer(kInstanceBufferName, bd);

	// Refresh the InstanceCloudComponent on the scene node — buffer handle
	// changes every rebuild because the graph bumps the gen counter.
	if (m_node && !m_node->components.empty()) {
		auto& comp = m_node->components[0];
		comp.instanceBuffer = m_instance_buffer;
		comp.instanceCount  = m_instance_count;
	}

	// Volume-meta UBO (per frame in flight, but its contents don't change).
	m_meta_buffers.assign(ctx.maxFramesInFlight, nullptr);
	m_meta_mapped.assign(ctx.maxFramesInFlight, nullptr);
	for (uint32_t i = 0; i < ctx.maxFramesInFlight; i++) {
		m_meta_buffers[i] = VWrap::Buffer::CreateMapped(
			m_allocator, sizeof(VolumeMetaUbo),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_meta_mapped[i]);
		// Fill once now — the volume size doesn't change frame-to-frame.
		VolumeMetaUbo meta{};
		meta.sizeX = static_cast<int32_t>(m_volume_size.x);
		meta.sizeY = static_cast<int32_t>(m_volume_size.y);
		meta.sizeZ = static_cast<int32_t>(m_volume_size.z);
		meta.frameCount = static_cast<int32_t>(m_frame_count);
		std::memcpy(m_meta_mapped[i], &meta, sizeof(meta));
	}

	// Palette + sampler.
	if (!m_palette) {
		m_palette = std::make_unique<PaletteResource>(m_device, m_allocator, m_graphics_pool);
		m_palette->Create();
	}
	if (!m_volume_sampler) {
		m_volume_sampler = VWrap::Sampler::CreateNearestClamp(m_device);
	}

	// ---- Compute pass: write the animated voxel asset frames once. ----
	m_compute_bindings = std::make_shared<BindingTable>(m_device, 1);
	m_compute_bindings->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)
		.BindGraphStorageImage(0, m_volume);
	m_compute_bindings->Build();

	graph.AddComputePass(kGenerateName)
		.Write(m_volume, ResourceUsage::StorageWrite)
		.SetPipeline([this]() {
			ComputePipelineDesc d;
			d.compSpvPath = std::string(config::SHADER_DIR) + "/instanced_voxel_generate.comp.spv";
			d.descriptorSetLayout = m_compute_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			r.offset = 0;
			r.size = sizeof(GeneratePC);
			d.pushConstantRanges = { r };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			pctx.cmd->CmdBindComputePipeline(pctx.computePipeline);
			pctx.cmd->CmdBindComputeDescriptorSets(pctx.computePipeline->GetLayout(),
				{ m_compute_bindings->GetSet(0)->Get() });
			GeneratePC pc{};
			pc.sizeX = static_cast<int32_t>(m_volume_size.x);
			pc.sizeY = static_cast<int32_t>(m_volume_size.y);
			pc.sizeZ = static_cast<int32_t>(m_volume_size.z);
			pc.frameCount = static_cast<int32_t>(m_frame_count);
			vkCmdPushConstants(pctx.cmd->Get(), pctx.computePipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			pctx.cmd->CmdDispatch(
				cdiv(m_volume_size.x, 4),
				cdiv(m_volume_size.y, 4),
				cdiv(m_volume_size.z * m_frame_count, 4));
		})
		.SetBindings(m_compute_bindings);

	// ---- Graphics pass: rasterize the cube per instance, DDA inside. ----
	m_graphics_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_graphics_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,        VK_SHADER_STAGE_VERTEX_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindGraphStorageBuffer(0, m_instance_buffer)
		 .BindGraphSampledImage(1, m_volume, m_volume_sampler)
		 .BindExternalSampledImage(2, m_palette->GetImageView(), m_palette->GetSampler(),
		                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		 .BindUniformBufferPerFrame(3, m_meta_buffers, sizeof(VolumeMetaUbo));
	m_graphics_bindings->Build();

	auto& drawPass = graph.AddGraphicsPass("InstancedVoxel Trace");
	drawPass.AcceptsItemTypes({ RenderItemType::InstancedVoxelMesh });
	drawPass
		.SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(targets.resolve)
		.Read(m_volume, ResourceUsage::SampledRead)
		.Read(m_instance_buffer, ResourceUsage::StorageRead)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/instanced_voxel.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/instanced_voxel.frag.spv";
			d.descriptorSetLayout = m_graphics_bindings->GetLayout();
			VkPushConstantRange r{};
			// Push constants are read in both vertex (for transforms) and fragment
			// (for trace state). Single range with combined stage flags.
			r.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			r.offset = 0;
			r.size = sizeof(InstancedVoxelPC);
			d.pushConstantRanges = { r };
			d.inputAssembly = PipelineDefaults::TriangleList();
			// Front face = CCW, back-face cull. Cube vertex order in
			// instanced_voxel.vert is set up for outward-facing CCW.
			d.rasterizer = PipelineDefaults::BackCullFill(false);
			d.depthStencil = PipelineDefaults::DepthTestWrite();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			auto vk_cmd = pctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());

			VkDescriptorSet ds = m_graphics_bindings->GetSet(pctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			if (!pctx.scene) return;

			InstancedVoxelPC pc{};
			pc.viewProj = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
			pc.cameraPos = m_camera->GetPosition();
			pc.maxIterations = m_max_iterations;
			pc.skyColor = m_sky ? m_sky->color : glm::vec3(0.529f, 0.808f, 0.922f);
			pc.debugColor = m_debug_color ? 1 : 0;
			if (m_lighting) {
				pc.sunDirection     = m_lighting->GetSunDirection();
				pc.sunCosHalfAngle  = m_lighting->GetSunCosHalfAngle();
				pc.sunColor         = glm::vec3(m_lighting->sunColor[0],
				                                m_lighting->sunColor[1],
				                                m_lighting->sunColor[2]);
				pc.sunIntensity     = m_lighting->sunIntensity;
				pc.ambientIntensity = m_lighting->ambientIntensity;
				pc.aoStrength       = m_lighting->aoStrength;
				pc.shadowsEnabled   = m_lighting->shadowsEnabled ? 1 : 0;
			} else {
				pc.sunDirection = glm::vec3(0, 0, -1);
				pc.sunCosHalfAngle = 1.0f;
				pc.sunColor = glm::vec3(1.0f);
				pc.sunIntensity = 1.0f;
				pc.ambientIntensity = 0.5f;
				pc.aoStrength = 0.0f;
				pc.shadowsEnabled = 0;
			}
			pc.frameCount = static_cast<int32_t>(m_frame_count);

			auto now = std::chrono::steady_clock::now();
			pc.time = std::chrono::duration<float>(now - m_start_time).count() * m_animation_speed;

			for (const auto& item : pctx.scene->Get(RenderItemType::InstancedVoxelMesh)) {
				pc.cloudWorld = item.transform;
				pc.aabbMin = item.aabbMin;
				pc.aabbMax = item.aabbMax;
				vkCmdPushConstants(vk_cmd, pctx.graphicsPipeline->GetLayout(),
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0, sizeof(pc), &pc);
				// 36 = unit cube triangles (vertex shader generates positions
				// from gl_VertexIndex). instanceCount instances, firstInstance
				// offsets into the bound SSBO.
				vkCmdDraw(vk_cmd, 36, item.instanceCount, 0, item.firstInstance);
			}
		})
		.SetBindings(m_graphics_bindings);

	logger->info("InstancedVoxelTechnique: registered with {}^2 = {} instances, frames={}",
		m_grid_dim, m_instance_count, m_frame_count);
}

void InstancedVoxelTechnique::OnPostCompile(RenderGraph& graph) {
	// Upload per-instance SSBO. The graph buffer was just allocated by Compile().
	RebuildInstanceData(graph);

	// Generate-pass runs once per rebuild; we don't need to re-run it every
	// frame (the volume contents are static within a graph build). Disable
	// after the first execution by re-enabling on rebuild via PostCompile. The
	// graph executes the pass on the *current* compile cycle then we leave it
	// disabled until the next rebuild.
	// V1 simplification: leave the pass enabled — re-running every frame is
	// 16*32*16*8 / 64 = 1024 dispatches × cheap shader. Re-enabling /
	// disabling adds ergonomics complexity for negligible perf win at v1.
	(void)graph;
}

void InstancedVoxelTechnique::RebuildInstanceData(RenderGraph& graph) {
	std::vector<GpuInstance> instances;
	instances.resize(m_instance_count);

	std::mt19937 rng(0xC0FFEE);
	std::uniform_real_distribution<float> jitter(-0.05f, 0.05f);
	std::uniform_real_distribution<float> phase(0.0f, static_cast<float>(m_frame_count));
	std::uniform_real_distribution<float> spinDist(-0.4f, 0.4f);

	const float origin = -0.5f * (m_grid_dim - 1) * m_grid_spacing;
	for (int gy = 0; gy < m_grid_dim; ++gy) {
		for (int gx = 0; gx < m_grid_dim; ++gx) {
			GpuInstance gi{};
			gi.position = glm::vec3(
				origin + gx * m_grid_spacing + jitter(rng),
				origin + gy * m_grid_spacing + jitter(rng),
				0.0f);
			// Visually: each blade is taller than wide. Scale shrinks the
			// 16-wide cube to ~0.4 units; the per-axis aabb on the component
			// already gives the cube a tall (32) dim, so the blade reads
			// vertically.
			gi.scale = m_grid_spacing / static_cast<float>(m_volume_size.x);
			gi.scale *= 1.0f + jitter(rng) * 4.0f;

			// Random Z-spin so blades don't all face the same way.
			float yaw = spinDist(rng) * 6.28318f;
			glm::quat q = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
			gi.rotation = glm::vec4(q.x, q.y, q.z, q.w);

			gi.animOffset = phase(rng);
			instances[gy * m_grid_dim + gx] = gi;
		}
	}

	graph.UploadBufferData(m_instance_buffer, instances.data(),
		instances.size() * sizeof(GpuInstance), m_graphics_pool);
}

void InstancedVoxelTechnique::Reload(const RenderContext& ctx) {
	(void)ctx;
	if (m_pending_grid_rebuild && m_eventSink) {
		m_pending_grid_rebuild = false;
		m_eventSink({AppEventType::RebuildGraph});
	}
}

std::vector<std::string> InstancedVoxelTechnique::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/instanced_voxel.vert.spv",
		std::string(config::SHADER_DIR) + "/instanced_voxel.frag.spv",
		std::string(config::SHADER_DIR) + "/instanced_voxel_generate.comp.spv",
	};
}

std::vector<TechniqueParameter>& InstancedVoxelTechnique::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters = {
			{ "Animation Speed", TechniqueParameter::Float, &m_animation_speed, 0.0f, 30.0f },
			{ "Max Iterations",  TechniqueParameter::Int,   &m_max_iterations, 1.0f, 256.0f },
			{ "Debug Coloring",  TechniqueParameter::Bool,  &m_debug_color },
		};
		TechniqueParameter gridDim;
		gridDim.label = "Grid Side";
		gridDim.type  = TechniqueParameter::Int;
		gridDim.data  = &m_grid_dim;
		gridDim.min   = 1.0f;
		gridDim.max   = 64.0f;
		gridDim.onChanged = [this]() {
			m_pending_grid_rebuild = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(gridDim));
	}
	return m_parameters;
}

FrameStats InstancedVoxelTechnique::GetFrameStats() const {
	// One draw call per InstancedVoxelMesh item × instances per item.
	return { 1, 36 * m_instance_count, 0 };
}
