#include "InstancedVoxelTechnique.h"
#include "Substrate.h"
#include "RenderItem.h"
#include "RenderScene.h"
#include "PipelineDefaults.h"
#include "config.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cassert>
#include <cstring>
#include <random>

namespace {
constexpr const char* kVolumeAssetName = "instanced_voxel_blade_v1";
constexpr const char* kInstanceBufferName = "instanced_voxel_instances";
constexpr const char* kSceneNodeName = "instanced_voxel_grass";
constexpr const char* kGenerateName  = "InstancedVoxel Generate";
constexpr const char* kSkyPassName   = "InstancedVoxel Sky";
constexpr const char* kTracePassName = "InstancedVoxel Trace";

// World voxel size — the engine's canonical voxel pitch (VISION.md §1.1).
// One day this graduates out to a Scene-level concept shared with the brickmap
// pillar; today it lives here because the foliage technique is the only
// consumer that has been quantized onto the world voxel grid. The substrate
// (LIGHTING.md §3) keys off this value: every quantized instance position maps
// to integer world-voxel coordinates via this scale.
//
// The chosen value preserves the pre-quantization visual: 16-voxel-wide blades
// at 0.0125 m/voxel = 0.2 m wide, matching the previous m_grid_spacing default.
constexpr float kWorldVoxelSize = 0.0125f;

// CPU layout must match shaders/instanced_voxel.vert::InstanceData.
// std430-compatible: 16-byte alignment for vec3+scalar pairs, vec4 for quat.
struct GpuInstance {
	glm::vec3 position;     float scale;
	glm::vec4 rotation;     // quaternion (xyz, w)
	float     animOffset;
	float     _pad0;        // reserved for future speciesIndex (bindless multi-species)
	int32_t   yawIdx;       // 0..3 — Z-yaw enumeration. Same info the rotation
	                        // quaternion encodes, but pre-decoded so substrate
	                        // queries (and any future code that wants to apply a
	                        // permutation rather than rotate) can dispatch on
	                        // the integer directly. The vertex shader still
	                        // consumes the quaternion.
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
	int32_t numXWords;       // bitmask row stride along X: ceil(sizeX / 32)
};

// Number of 32-bit words per (frame, z, y) row of the bitmask.
inline uint32_t BitmaskXWords(uint32_t sizeX) {
	return (sizeX + 31u) / 32u;
}

// Total uint32 word count for an asset's bitmask: one word per
// 32-voxel chunk along X, full sizeY × sizeZ × frameCount slabs.
inline uint32_t BitmaskWordCount(glm::uvec3 size, uint32_t frameCount) {
	return BitmaskXWords(size.x) * size.y * size.z * frameCount;
}

// Per-frame UBO. Rewritten every frame; the sky pre-pass and the trace pass
// both read it. std140 layout: vec3+scalar pairs share 16 B slots; mat4
// fields are naturally 16-aligned.
struct InstancedVoxelFrameUbo {
	glm::mat4 viewProj;            // 64
	glm::mat4 ndcToWorld;          // 64 — sky pass uses this; trace pass ignores
	glm::vec3 cameraPos;           int32_t maxIterations;   // 16
	glm::vec3 skyColor;            int32_t debugColor;      // 16
	glm::vec3 sunDirection;        float   sunCosHalfAngle; // 16
	glm::vec3 sunColor;            float   sunIntensity;    // 16
	float     ambientIntensity;
	float     aoStrength;
	int32_t   shadowsEnabled;
	float     time;                                         // 16
	int32_t   frameCount;
	float     shadowBiasConstant;
	float     shadowBiasSlope;
	float     worldVoxelSize;      // engine voxel pitch; mirrored from kWorldVoxelSize
	                               // so substrate.glsl can convert world ↔ voxel    // 16
};
static_assert(sizeof(InstancedVoxelFrameUbo) == 224,
	"InstancedVoxelFrameUbo must stay std140-compatible");

// Slim per-draw push constant: just the cube-rasterization geometry. Comfortably
// under the 128-byte minimum guarantee of every conformant Vulkan implementation.
struct InstancedVoxelDrawPC {
	glm::mat4 cloudWorld;      // 64
	glm::vec3 aabbMin; float _pad0;   // 16
	glm::vec3 aabbMax; float _pad1;   // 16
};
static_assert(sizeof(InstancedVoxelDrawPC) == 96,
	"InstancedVoxelDrawPC must stay <= 128 B for portable push-constant limits");
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
	//
	// Cloud node convention (locked in by Milestone A — LIGHTING.md §2):
	// the cloud's *local* frame is the world voxel grid: instance positions
	// are integer voxel offsets, and the cloud node's TRS is the only
	// transform that scales voxels into world units. Doing it this way means
	// the substrate (Milestone C) builds its index in cloud-local voxels and
	// shaders do one floor-divide to walk it.
	if (!m_volume_asset.valid()) {
		m_volume_asset = m_assets->CreateProceduralAnimatedVoxelVolume(
			kVolumeAssetName, m_volume_size, m_frame_count, VK_FORMAT_R8_UINT);
		if (m_world && !m_node) {
			m_node = m_world->GetRoot().AddChild(kSceneNodeName);
			Component c{};
			c.type            = ComponentType::InstanceCloud;
			c.asset           = m_volume_asset;
			c.frameCount      = m_frame_count;
			// Per-instance AABB is the asset's voxel extent — integers in
			// cloud-local space. The cloud node's scale(kWorldVoxelSize)
			// converts to world units.
			c.instanceAabbMin = glm::vec3(0.0f);
			c.instanceAabbMax = glm::vec3(static_cast<float>(m_volume_size.x),
			                              static_cast<float>(m_volume_size.y),
			                              static_cast<float>(m_volume_size.z));
			m_node->AddComponent(c);
		}
	}

	if (m_node) {
		// Cloud-local to world: scale by the engine voxel pitch, then translate
		// so the cloud's footprint center lands at world origin (matches the
		// pre-quantization visual where the cloud was centered at (0,0,0)).
		// SceneNode composes T·R·S; we set R=identity, S=kWorldVoxelSize, T to
		// re-center.
		const float footprintVoxels = static_cast<float>(m_grid_dim) *
		                              static_cast<float>(m_blade_pitch_voxels);
		const float halfFootprintWorld = 0.5f * footprintVoxels * kWorldVoxelSize;
		m_node->position = glm::vec3(-halfFootprintWorld, -halfFootprintWorld, 0.0f);
		m_node->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		m_node->scale    = glm::vec3(kWorldVoxelSize);
		m_node->MarkSubtreeDirty();
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

	// Per-asset occupancy bitmask. Written by the generate compute pass; read
	// by the substrate's shadow query (Milestone C). Persistent — once written
	// the bits don't change frame-to-frame (the v1 asset is animation-static
	// once baked).
	m_bitmask_word_count = BitmaskWordCount(m_volume_size, m_frame_count);
	BufferDesc bm{};
	bm.size     = sizeof(uint32_t) * m_bitmask_word_count;
	bm.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bm.lifetime = Lifetime::Persistent;
	m_bitmask_buffer = graph.CreateBuffer("instanced_voxel_bitmask", bm);

	// World-grid substrate. Allocated at upper-bound size (see
	// SubstrateUpperBoundWords); the populated extent is recorded in the
	// buffer header so the shader doesn't read the trailing slack.
	m_substrate_word_capacity = InstancedVoxel::SubstrateUpperBoundWords(
		m_instance_count, m_volume_size,
		static_cast<uint32_t>(m_grid_dim),
		static_cast<uint32_t>(m_blade_pitch_voxels));
	BufferDesc sb{};
	sb.size     = sizeof(uint32_t) * m_substrate_word_capacity;
	sb.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	sb.lifetime = Lifetime::Persistent;
	m_substrate_buffer = graph.CreateBuffer("instanced_voxel_substrate", sb);

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

	// Per-frame UBO (camera/sun/sky/time/iteration). Written each frame in the
	// trace-pass record callback; both sky and trace passes read it.
	m_frame_ubo_buffers.assign(ctx.maxFramesInFlight, nullptr);
	m_frame_ubo_mapped.assign(ctx.maxFramesInFlight, nullptr);
	for (uint32_t i = 0; i < ctx.maxFramesInFlight; i++) {
		m_frame_ubo_buffers[i] = VWrap::Buffer::CreateMapped(
			m_allocator, sizeof(InstancedVoxelFrameUbo),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_frame_ubo_mapped[i]);
	}

	// Palette + samplers.
	if (!m_palette) {
		m_palette = std::make_unique<PaletteResource>(m_device, m_allocator, m_graphics_pool);
		m_palette->Create();
	}
	if (!m_volume_sampler) {
		m_volume_sampler = VWrap::Sampler::CreateNearestClamp(m_device);
	}

	// ---- Compute pass: write the animated voxel asset frames once. ----
	// Two outputs:
	//   binding 0 — the R8_UINT volume image (palette indices, one byte/voxel)
	//   binding 1 — the occupancy bitmask SSBO (one bit/voxel, packed 32-along-X)
	// Each thread owns one bitmask word along X, so dispatch is sized in
	// bitmask words (not voxels) on the X axis. local_size_x stays at 1
	// because per-word work is internally serialized over up to 32 voxels.
	m_compute_bindings = std::make_shared<BindingTable>(m_device, 1);
	m_compute_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_SHADER_STAGE_COMPUTE_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		 .BindGraphStorageImage(0, m_volume)
		 .BindGraphStorageBuffer(1, m_bitmask_buffer);
	m_compute_bindings->Build();

	graph.AddComputePass(kGenerateName)
		.Write(m_volume,         ResourceUsage::StorageWrite)
		.Write(m_bitmask_buffer, ResourceUsage::StorageWrite)
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
			pc.sizeX      = static_cast<int32_t>(m_volume_size.x);
			pc.sizeY      = static_cast<int32_t>(m_volume_size.y);
			pc.sizeZ      = static_cast<int32_t>(m_volume_size.z);
			pc.frameCount = static_cast<int32_t>(m_frame_count);
			pc.numXWords  = static_cast<int32_t>(BitmaskXWords(m_volume_size.x));
			vkCmdPushConstants(pctx.cmd->Get(), pctx.computePipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			pctx.cmd->CmdDispatch(
				BitmaskXWords(m_volume_size.x),       // local_size_x = 1
				cdiv(m_volume_size.y, 4),             // local_size_y = 4
				cdiv(m_volume_size.z * m_frame_count, 4));   // local_size_z = 4
		})
		.SetBindings(m_compute_bindings);

	// ---- Sky pre-pass: fullscreen gradient + sun disk so non-cube pixels
	// aren't just clear color. Reuses the per-frame UBO from the trace pass.
	m_sky_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_sky_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindUniformBufferPerFrame(0, m_frame_ubo_buffers, sizeof(InstancedVoxelFrameUbo));
	m_sky_bindings->Build();

	// ---- Graphics pass: rasterize the cube per instance, DDA inside. ----
	// Vertex stage reads the frame UBO (for viewProj). Instance SSBO is read
	// in BOTH vertex (transform) and fragment (substrate's per-instance lookups).
	//   0 — instances SSBO   (vert+frag)
	//   1 — volume image     (frag, NEAREST)
	//   2 — palette image    (frag, LINEAR)
	//   3 — meta UBO         (frag)
	//   4 — frame UBO        (vert+frag)
	//   6 — substrate buffer (frag)
	//   7 — bitmask buffer   (frag)
	// Slot 5 was the shadow map, retired in Milestone D when the substrate
	// took over the shadow query. Left vacant rather than renumbered to
	// minimize churn; substrate.glsl doesn't care about absolute binding
	// numbers (it uses the buffer names).
	m_graphics_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_graphics_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindGraphStorageBuffer(0, m_instance_buffer)
		 .BindGraphSampledImage(1, m_volume, m_volume_sampler)
		 .BindExternalSampledImage(2, m_palette->GetImageView(), m_palette->GetSampler(),
		                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		 .BindUniformBufferPerFrame(3, m_meta_buffers, sizeof(VolumeMetaUbo))
		 .BindUniformBufferPerFrame(4, m_frame_ubo_buffers, sizeof(InstancedVoxelFrameUbo))
		 .BindGraphStorageBuffer(6, m_substrate_buffer)
		 .BindGraphStorageBuffer(7, m_bitmask_buffer);
	m_graphics_bindings->Build();

	// ---- Sky pre-pass (fullscreen). Runs FIRST among graphics passes so the
	// trace pass below can use LoadOp::Load and overlay the cube draws on top
	// of the gradient + sun disk.
	auto& skyPass = graph.AddGraphicsPass(kSkyPassName);
	skyPass
		.SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/sky_fullscreen.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/sky_fullscreen.frag.spv";
			d.descriptorSetLayout = m_sky_bindings->GetLayout();
			d.inputAssembly = PipelineDefaults::TriangleStrip();
			d.rasterizer = PipelineDefaults::NoCullFill();
			d.depthStencil = PipelineDefaults::NoDepthTest();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			// Sky is the first foliage pass to record this frame (the shadow
			// pass is gone since Milestone D), so it owns the per-frame UBO
			// write. WritePerFrameUbo is idempotent — the trace pass repeats
			// the call to keep correctness independent of pass ordering.
			WritePerFrameUbo(pctx.frameIndex);

			auto vk_cmd = pctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());
			VkDescriptorSet ds = m_sky_bindings->GetSet(pctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		})
		.SetBindings(m_sky_bindings);

	auto& drawPass = graph.AddGraphicsPass(kTracePassName);
	drawPass.AcceptsItemTypes({ RenderItemType::InstancedVoxelMesh });
	drawPass
		// LoadOp::Load — the sky pre-pass already wrote the gradient + sun.
		// Cube draws then overwrite covered pixels with traced voxel color or
		// `discard` (leaving the sky visible on misses).
		.SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::DontCare)
		.SetResolveTarget(targets.resolve)
		.Read(m_volume, ResourceUsage::SampledRead)
		.Read(m_instance_buffer,  ResourceUsage::StorageRead)
		.Read(m_substrate_buffer, ResourceUsage::StorageRead)
		.Read(m_bitmask_buffer,   ResourceUsage::StorageRead)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/instanced_voxel.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/instanced_voxel.frag.spv";
			d.descriptorSetLayout = m_graphics_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			r.offset = 0;
			r.size = sizeof(InstancedVoxelDrawPC);
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

			// Idempotent re-write of the per-frame UBO. The sky pre-pass already
			// wrote it earlier this frame (see WritePerFrameUbo); writing it
			// again here keeps correctness independent of pass execution order.
			WritePerFrameUbo(pctx.frameIndex);

			// ---- Per-draw push constant: just the cube geometry.
			InstancedVoxelDrawPC pc{};
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
	std::uniform_real_distribution<float> phase(0.0f, static_cast<float>(m_frame_count));
	// 4-way Z-yaw quantization. Voxel volumes are axis-aligned grids, so any
	// non-right-angle rotation forces the DDA to march diagonally through
	// partial voxels — quantizing to {0°, 90°, 180°, 270°} keeps each blade's
	// internal grid axis-aligned with the cloud frame. Each yaw is a
	// permutation+sign-flip on (x, y); the substrate's CPU AABB computation
	// (Milestone C) relies on this to enumerate touched bricks with integer
	// math.
	std::uniform_int_distribution<int> yawIdxDist(0, 3);

	const float kQuarterTurn = 1.57079632679f;  // π/2
	const int spacing = m_blade_pitch_voxels;
	for (int gy = 0; gy < m_grid_dim; ++gy) {
		for (int gx = 0; gx < m_grid_dim; ++gx) {
			GpuInstance gi{};
			// Position is an integer voxel offset in cloud-local space. The
			// cloud node's scale(kWorldVoxelSize) projects this to world. No
			// sub-voxel jitter — variety comes from per-instance yaw and
			// (post-multi-species) per-instance assets.
			const int vx = gx * spacing;
			const int vy = gy * spacing;
			gi.position = glm::vec3(static_cast<float>(vx),
			                        static_cast<float>(vy),
			                        0.0f);
			// Asset voxels = world voxels (VISION.md §1.1). The only correct
			// per-instance scale is 1; tall/short blade variants ship as
			// separate assets, not as a continuous scale knob.
			gi.scale = 1.0f;

			const int yawIdx = yawIdxDist(rng);
			float yaw = static_cast<float>(yawIdx) * kQuarterTurn;
			glm::quat q = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
			gi.rotation = glm::vec4(q.x, q.y, q.z, q.w);
			gi.yawIdx   = yawIdx;

			gi.animOffset = phase(rng);
			instances[gy * m_grid_dim + gx] = gi;
		}
	}

#ifndef NDEBUG
	// Invariant check: every instance's transform must be a permutation +
	// integer translation in cloud-local voxel space (LIGHTING.md §2). The
	// substrate's correctness depends on this — a non-integer offset or a
	// non-quantized yaw would produce world voxels that don't agree with the
	// per-brick instance lists.
	for (const auto& gi : instances) {
		const glm::vec3 fracPos = gi.position - glm::round(gi.position);
		assert(glm::dot(fracPos, fracPos) < 1e-8f && "instance position must be integer cloud voxel");
		assert(std::abs(gi.scale - 1.0f) < 1e-6f && "instance scale must be exactly 1");
		// Quaternion must be a yaw multiple of 90° around +Z: (x, y) zero,
		// (z, w) one of (0,±1), (±√2/2, √2/2). The polynomial check below
		// equivalently: |x|+|y| ≈ 0 AND z² + w² ≈ 1.
		const float zw2 = gi.rotation.z * gi.rotation.z + gi.rotation.w * gi.rotation.w;
		assert(std::abs(gi.rotation.x) + std::abs(gi.rotation.y) < 1e-6f &&
		       std::abs(zw2 - 1.0f) < 1e-6f && "instance rotation must be a Z-axis yaw");
	}
#endif

	graph.UploadBufferData(m_instance_buffer, instances.data(),
		instances.size() * sizeof(GpuInstance), m_graphics_pool);

	// Build the world-grid substrate from the just-quantized instances. The
	// substrate sees only what it needs (cloud-local voxel position + yawIdx),
	// keeping its inputs decoupled from anything else GpuInstance might grow.
	std::vector<InstancedVoxel::SubstrateInstanceInput> substrateInputs;
	substrateInputs.reserve(instances.size());
	for (const auto& gi : instances) {
		InstancedVoxel::SubstrateInstanceInput in;
		in.cloudVoxelPos = glm::ivec3(glm::round(gi.position));
		in.yawIdx        = static_cast<uint8_t>(gi.yawIdx & 0x3);
		substrateInputs.push_back(in);
	}
	auto build = InstancedVoxel::BuildFoliageSubstrate(
		substrateInputs.data(),
		static_cast<uint32_t>(substrateInputs.size()),
		m_volume_size);
	assert(build.data.size() <= m_substrate_word_capacity &&
	       "substrate upper bound was too small — recheck SubstrateUpperBoundWords");
	graph.UploadBufferData(m_substrate_buffer, build.data.data(),
		build.data.size() * sizeof(uint32_t), m_graphics_pool);
}

void InstancedVoxelTechnique::WritePerFrameUbo(uint32_t frameIndex) {
	if (frameIndex >= m_frame_ubo_mapped.size() || !m_frame_ubo_mapped[frameIndex]) return;

	InstancedVoxelFrameUbo ubo{};
	glm::mat4 view = m_camera->GetViewMatrix();
	glm::mat4 proj = m_camera->GetProjectionMatrix();
	ubo.viewProj   = proj * view;
	ubo.ndcToWorld = glm::inverse(ubo.viewProj);
	ubo.cameraPos  = m_camera->GetPosition();
	ubo.maxIterations = m_max_iterations;
	ubo.skyColor   = m_sky ? m_sky->color : glm::vec3(0.529f, 0.808f, 0.922f);
	ubo.debugColor = m_debug_color ? 1 : 0;

	if (m_lighting) {
		ubo.sunDirection     = m_lighting->GetSunDirection();
		ubo.sunCosHalfAngle  = m_lighting->GetSunCosHalfAngle();
		ubo.sunColor         = glm::vec3(m_lighting->sunColor[0],
		                                 m_lighting->sunColor[1],
		                                 m_lighting->sunColor[2]);
		ubo.sunIntensity     = m_lighting->sunIntensity;
		ubo.ambientIntensity = m_lighting->ambientIntensity;
		ubo.aoStrength       = m_lighting->aoStrength;
		// Shader-side gate is the AND of the technique-local toggle and the
		// global lighting toggle — either one off disables shadow sampling.
		ubo.shadowsEnabled   = (m_shadows_enabled && m_lighting->shadowsEnabled) ? 1 : 0;
	} else {
		ubo.sunDirection     = glm::vec3(0, 0, -1);
		ubo.sunCosHalfAngle  = 1.0f;
		ubo.sunColor         = glm::vec3(1.0f);
		ubo.sunIntensity     = 1.0f;
		ubo.ambientIntensity = 0.5f;
		ubo.aoStrength       = 0.0f;
		ubo.shadowsEnabled   = m_shadows_enabled ? 1 : 0;
	}

	ubo.frameCount         = static_cast<int32_t>(m_frame_count);
	ubo.shadowBiasConstant = m_shadow_bias_constant;
	ubo.shadowBiasSlope    = m_shadow_bias_slope;
	ubo.worldVoxelSize     = kWorldVoxelSize;
	auto now = std::chrono::steady_clock::now();
	ubo.time = std::chrono::duration<float>(now - m_start_time).count() * m_animation_speed;

	std::memcpy(m_frame_ubo_mapped[frameIndex], &ubo, sizeof(ubo));
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
		std::string(config::SHADER_DIR) + "/sky_fullscreen.vert.spv",
		std::string(config::SHADER_DIR) + "/sky_fullscreen.frag.spv",
	};
}

std::vector<TechniqueParameter>& InstancedVoxelTechnique::GetParameters() {
	if (m_parameters.empty()) {
		// Lead with the headline lighting toggle so it's the first thing under
		// the technique's "Parameters" header, not buried in the global
		// Lighting section. Technique-local field; AND'd with SceneLighting's
		// global Sun Shadows toggle in the shader.
		m_parameters.push_back({ "Shadows", TechniqueParameter::Header });
		m_parameters.push_back({ "Enable Shadows",     TechniqueParameter::Bool,  &m_shadows_enabled });
		m_parameters.push_back({ "Shadow Bias Const",  TechniqueParameter::Float, &m_shadow_bias_constant, 0.0f, 0.5f });
		m_parameters.push_back({ "Shadow Bias Slope",  TechniqueParameter::Float, &m_shadow_bias_slope,    0.0f, 1.0f });

		m_parameters.push_back({ "Trace", TechniqueParameter::Header });
		m_parameters.push_back({ "Animation Speed", TechniqueParameter::Float, &m_animation_speed, 0.0f, 30.0f });
		m_parameters.push_back({ "Max Iterations",  TechniqueParameter::Int,   &m_max_iterations, 1.0f, 256.0f });
		m_parameters.push_back({ "Debug Coloring",  TechniqueParameter::Bool,  &m_debug_color });
		TechniqueParameter gridDim;
		gridDim.label = "Grid Side";
		gridDim.type  = TechniqueParameter::Int;
		gridDim.data  = &m_grid_dim;
		gridDim.min   = 1.0f;
		gridDim.max   = 128.0f;
		gridDim.onChanged = [this]() {
			m_pending_grid_rebuild = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(gridDim));
	}
	return m_parameters;
}

FrameStats InstancedVoxelTechnique::GetFrameStats() const {
	// Sky pre-pass (1 draw, 4 verts) + trace pass (1 draw × instanceCount).
	// The shadow pass is gone since Milestone D — shadows are resolved via
	// a substrate DDA inside the trace fragment shader, no extra rasterization.
	const uint32_t cubeVerts = 36 * m_instance_count;
	return { 2, 4 + cubeVerts, 0 };
}
