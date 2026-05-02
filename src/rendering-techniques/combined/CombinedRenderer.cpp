#include "CombinedRenderer.h"
#include "Substrate.h"
#include "PipelineDefaults.h"
#include "config.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <random>

namespace {

constexpr const char* kTerrainBufferName    = "combined_terrain_brickmap";
constexpr const char* kFoliageAssetName     = "combined_foliage_volume";
constexpr const char* kFoliageBitmaskName   = "combined_foliage_bitmask";
constexpr const char* kFoliageInstancesName = "combined_foliage_instances";
constexpr const char* kSubstrateBufferName  = "combined_substrate";
constexpr const char* kComputePassName      = "Combined Foliage Generate";
constexpr const char* kSkyPassName          = "Combined Sky";
constexpr const char* kTerrainPassName      = "Combined Terrain Trace";
constexpr const char* kFoliagePassName      = "Combined Foliage Trace";

// One inch per voxel. Mirrors the value in InstancedVoxelTechnique — both
// techniques pin to the same world voxel grid (LIGHTING.md §1.1, §2). The
// CombinedRenderer's substrate query addresses terrain bricks via integer
// world voxels, so this constant is load-bearing for shadow correctness.
constexpr float kWorldVoxelSize = 0.0254f;

// CPU layout matches shaders/instanced_voxel.vert::InstanceData.
struct GpuInstance {
	glm::vec3 position;     float scale;
	glm::vec4 rotation;
	float     animOffset;
	float     _pad0;
	int32_t   yawIdx;
	float     _pad2;
};
static_assert(sizeof(GpuInstance) == 48, "GpuInstance must be 48 bytes (std430)");

struct VolumeMetaUbo {
	int32_t sizeX, sizeY, sizeZ;
	int32_t frameCount;
};

// Per-frame state shared by every graphics pass in this technique. std140
// layout: vec3+scalar pairs share 16 B; mat4 fields are 16-aligned. Trailing
// pad keeps the struct a clean 16 B multiple — easier to reason about and
// removes any ambiguity about std140 round-up rules.
struct CombinedFrameUbo {
	glm::mat4 viewProj;            // 64
	glm::mat4 ndcToWorld;          // 64
	glm::vec3 cameraPos;           int32_t maxIterations;   // 16
	glm::vec3 skyColor;            int32_t debugColor;      // 16
	glm::vec3 sunDirection;        float   sunCosHalfAngle; // 16
	glm::vec3 sunColor;            float   sunIntensity;    // 16
	float     ambientIntensity;
	float     aoStrength;
	int32_t   shadowsEnabled;
	float     time;                                          // 16
	int32_t   frameCount;
	float     worldVoxelSize;
	float     _pad0;
	float     _pad1;                                         // 16
	glm::ivec3 terrainOriginVoxel; int32_t  _pad2;           // 16
};
static_assert(sizeof(CombinedFrameUbo) == 240,
	"CombinedFrameUbo layout drift — update both combined_*_trace.frag UBO blocks to match");

// Compute pass push constant — reuses instanced_voxel_generate.comp.
struct GeneratePC {
	int32_t sizeX, sizeY, sizeZ;
	int32_t frameCount;
	int32_t numXWords;
};

// Terrain trace pass push constant — primary-trace geometry only. Shading
// state lives in the FrameUbo.
struct TerrainTracePC {
	glm::mat4  ndcToWorld;            // 64
	glm::ivec3 terrainOriginVoxel; int32_t _pad0;   // 16
};
static_assert(sizeof(TerrainTracePC) == 80,
	"TerrainTracePC must stay <= 128 B (portable PC limit)");

// Foliage trace pass push constant — same shape as InstancedVoxelTechnique's.
struct FoliageTracePC {
	glm::mat4 cloudWorld;
	glm::vec3 aabbMin; float _pad0;
	glm::vec3 aabbMax; float _pad1;
};
static_assert(sizeof(FoliageTracePC) == 96, "FoliageTracePC must stay <= 128 B");

inline uint32_t BitmaskXWords(uint32_t sizeX) { return (sizeX + 31u) / 32u; }
inline uint32_t BitmaskWordCount(glm::uvec3 size, uint32_t frameCount) {
	return BitmaskXWords(size.x) * size.y * size.z * frameCount;
}

// CPU-side brickmap occupancy probe. Walks the same buffer layout the GPU
// shader does (matches src/rendering/voxel/Brickmap.h). `v` is in terrain-
// local voxel coords (NOT world voxels); the caller subtracts the terrain
// origin before passing.
bool BrickmapVoxelSolid(const BrickmapData& bm, glm::ivec3 v) {
	if (v.x < 0 || v.y < 0 || v.z < 0) return false;
	if (v.x >= int(bm.volumeSize.x) ||
	    v.y >= int(bm.volumeSize.y) ||
	    v.z >= int(bm.volumeSize.z)) return false;

	const int bs = static_cast<int>(bm.brickSize);
	const glm::ivec3 brickCell(v.x / bs, v.y / bs, v.z / bs);
	const glm::ivec3 local(v.x - brickCell.x * bs,
	                       v.y - brickCell.y * bs,
	                       v.z - brickCell.z * bs);
	const int gridIdx = brickCell.x
	                  + brickCell.y * static_cast<int>(bm.gridDim.x)
	                  + brickCell.z * static_cast<int>(bm.gridDim.x * bm.gridDim.y);
	const uint32_t brickIndex = bm.data[8 + gridIdx];
	if (brickIndex == 0xFFFFFFFFu) return false;

	const int linear   = local.z * 64 + local.y * 8 + local.x;
	const int wordIdx  = linear / 4;
	const int byteLane = linear % 4;
	const uint32_t topCells = bm.gridDim.x * bm.gridDim.y * bm.gridDim.z;
	const uint32_t word = bm.data[8u + topCells + brickIndex * 128u + uint32_t(wordIdx)];
	const uint32_t mat = (word >> (byteLane * 8)) & 0xFFu;
	return mat != 0u;
}

// Find the topmost solid voxel of the column at terrain-local (x, y).
// Returns -1 if the column is empty. Naive walk — fine for the v1 placement
// density (dozens of columns); add brick-skipping if the count grows.
int FindTopSolidZ(const BrickmapData& bm, int tx, int ty) {
	if (tx < 0 || ty < 0 ||
	    tx >= int(bm.volumeSize.x) || ty >= int(bm.volumeSize.y)) return -1;
	for (int z = static_cast<int>(bm.volumeSize.z) - 1; z >= 0; --z) {
		if (BrickmapVoxelSolid(bm, glm::ivec3(tx, ty, z))) return z;
	}
	return -1;
}

}  // namespace

RenderTargetDesc CombinedRenderer::DescribeTargets(const RendererCaps& caps) const {
	RenderTargetDesc desc{};
	desc.color.format       = caps.swapchainFormat;
	desc.color.samples      = caps.msaaSamples;
	desc.color.needsResolve = (caps.msaaSamples != VK_SAMPLE_COUNT_1_BIT);
	desc.hasDepth     = true;
	desc.depthFormat  = caps.depthFormat;
	desc.depthSamples = caps.msaaSamples;
	return desc;
}

void CombinedRenderer::RegisterPasses(
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

	// First-time-ever bake. Sets m_terrain_brickmap, m_terrain_pending_upload,
	// repositions camera. Subsequent bakes are user-triggered via the inspector.
	if (m_pending_bake) {
		m_pending_bake = false;
		BakeIslandNow();
		RebuildFoliagePlacement();
	}

	// First-time setup of the foliage asset slot. The volume image gets re-
	// allocated by the registry on every graph rebuild; we re-resolve the
	// ImageHandle each pass.
	if (!m_foliage_asset.valid()) {
		m_foliage_asset = m_assets->CreateProceduralAnimatedVoxelVolume(
			kFoliageAssetName, m_foliage_size, m_foliage_frame_count, VK_FORMAT_R8_UINT);
	}
	const auto* vol = m_assets->GetVoxelVolume(m_foliage_asset);
	m_foliage_volume = vol ? vol->volumeImage : ImageHandle{};

	// ---- Buffers (graph-managed, persistent across rebuilds) ----

	BufferDesc tb{};
	tb.size     = std::max<VkDeviceSize>(64, m_terrain_brickmap.ByteSize());
	tb.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	tb.lifetime = Lifetime::Persistent;
	m_terrain_buffer = graph.CreateBuffer(kTerrainBufferName, tb);

	const uint32_t instanceCount = std::max(1u, m_foliage_instance_count);
	BufferDesc ib{};
	ib.size     = sizeof(GpuInstance) * instanceCount;
	ib.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	ib.lifetime = Lifetime::Persistent;
	m_foliage_instance_buffer = graph.CreateBuffer(kFoliageInstancesName, ib);
	m_foliage_instance_capacity = instanceCount;

	m_foliage_bitmask_word_count = BitmaskWordCount(m_foliage_size, m_foliage_frame_count);
	BufferDesc bm{};
	bm.size     = sizeof(uint32_t) * m_foliage_bitmask_word_count;
	bm.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bm.lifetime = Lifetime::Persistent;
	m_foliage_bitmask_buffer = graph.CreateBuffer(kFoliageBitmaskName, bm);

	// Substrate buffer — sized to fit the just-built foliage substrate. The
	// upstream Substrate::UpperBoundWords helper assumes the foliage grid is
	// densely packed (gridDim × pitch footprint), which holds for the
	// standalone foliage technique but NOT for surface-placed instances that
	// can spread across the entire island. Surface-placed instances produce a
	// substrate top-grid spanning the full island footprint regardless of
	// instance count, so we just size the buffer to the actual build's words
	// (with a generous headroom for the next bake's variance). RebuildFoliage-
	// Placement was called above, so m_pending_substrate_data is current.
	const size_t actualWords  = m_pending_substrate_data.size();
	const size_t headroomMult = 2;            // re-bake with different params can swing the size
	const size_t minWords     = 1024;
	m_substrate_word_capacity = static_cast<uint32_t>(
		std::max(minWords, actualWords * headroomMult));
	BufferDesc sb{};
	sb.size     = sizeof(uint32_t) * m_substrate_word_capacity;
	sb.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	sb.lifetime = Lifetime::Persistent;
	m_substrate_buffer = graph.CreateBuffer(kSubstrateBufferName, sb);

	// ---- UBOs (per frame in flight) ----
	m_meta_ubo_buffers.assign(ctx.maxFramesInFlight, nullptr);
	m_meta_ubo_mapped.assign(ctx.maxFramesInFlight, nullptr);
	m_frame_ubo_buffers.assign(ctx.maxFramesInFlight, nullptr);
	m_frame_ubo_mapped.assign(ctx.maxFramesInFlight, nullptr);
	for (uint32_t i = 0; i < ctx.maxFramesInFlight; i++) {
		m_meta_ubo_buffers[i] = VWrap::Buffer::CreateMapped(
			m_allocator, sizeof(VolumeMetaUbo),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_meta_ubo_mapped[i]);
		VolumeMetaUbo meta{};
		meta.sizeX = static_cast<int32_t>(m_foliage_size.x);
		meta.sizeY = static_cast<int32_t>(m_foliage_size.y);
		meta.sizeZ = static_cast<int32_t>(m_foliage_size.z);
		meta.frameCount = static_cast<int32_t>(m_foliage_frame_count);
		std::memcpy(m_meta_ubo_mapped[i], &meta, sizeof(meta));

		m_frame_ubo_buffers[i] = VWrap::Buffer::CreateMapped(
			m_allocator, sizeof(CombinedFrameUbo),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_frame_ubo_mapped[i]);
	}

	// ---- Palette + samplers ----
	if (!m_palette) {
		m_palette = std::make_unique<PaletteResource>(m_device, m_allocator, m_graphics_pool);
		m_palette->Create();
	}
	if (!m_volume_sampler) {
		m_volume_sampler = VWrap::Sampler::CreateNearestClamp(m_device);
	}

	// ---- Compute pass: foliage volume + bitmask generate ----
	m_compute_bindings = std::make_shared<BindingTable>(m_device, 1);
	m_compute_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_SHADER_STAGE_COMPUTE_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		 .BindGraphStorageImage(0, m_foliage_volume)
		 .BindGraphStorageBuffer(1, m_foliage_bitmask_buffer);
	m_compute_bindings->Build();

	graph.AddComputePass(kComputePassName)
		.Write(m_foliage_volume,         ResourceUsage::StorageWrite)
		.Write(m_foliage_bitmask_buffer, ResourceUsage::StorageWrite)
		.SetPipeline([this]() {
			ComputePipelineDesc d;
			d.compSpvPath = std::string(config::SHADER_DIR) + "/instanced_voxel_generate.comp.spv";
			d.descriptorSetLayout = m_compute_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			r.offset     = 0;
			r.size       = sizeof(GeneratePC);
			d.pushConstantRanges = { r };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			pctx.cmd->CmdBindComputePipeline(pctx.computePipeline);
			pctx.cmd->CmdBindComputeDescriptorSets(pctx.computePipeline->GetLayout(),
				{ m_compute_bindings->GetSet(0)->Get() });
			GeneratePC pc{};
			pc.sizeX      = static_cast<int32_t>(m_foliage_size.x);
			pc.sizeY      = static_cast<int32_t>(m_foliage_size.y);
			pc.sizeZ      = static_cast<int32_t>(m_foliage_size.z);
			pc.frameCount = static_cast<int32_t>(m_foliage_frame_count);
			pc.numXWords  = static_cast<int32_t>(BitmaskXWords(m_foliage_size.x));
			vkCmdPushConstants(pctx.cmd->Get(), pctx.computePipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			auto cdiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; };
			pctx.cmd->CmdDispatch(
				BitmaskXWords(m_foliage_size.x),
				cdiv(m_foliage_size.y, 4),
				cdiv(m_foliage_size.z * m_foliage_frame_count, 4));
		})
		.SetBindings(m_compute_bindings);

	// ---- Sky pre-pass ----
	m_sky_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_sky_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindUniformBufferPerFrame(0, m_frame_ubo_buffers, sizeof(CombinedFrameUbo));
	m_sky_bindings->Build();

	auto& skyPass = graph.AddGraphicsPass(kSkyPassName);
	skyPass
		.SetColorAttachment(targets.color, LoadOp::Clear, StoreOp::Store, 0, 0, 0, 1)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/sky_fullscreen.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/sky_fullscreen.frag.spv";
			d.descriptorSetLayout = m_sky_bindings->GetLayout();
			d.inputAssembly = PipelineDefaults::TriangleStrip();
			d.rasterizer   = PipelineDefaults::NoCullFill();
			d.depthStencil = PipelineDefaults::NoDepthTest();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			WriteFrameUbo(pctx.frameIndex);
			auto vk_cmd = pctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());
			VkDescriptorSet ds = m_sky_bindings->GetSet(pctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);
			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		})
		.SetBindings(m_sky_bindings);

	// ---- Terrain trace pass ----
	// Bindings (mirrors combined_terrain_trace.frag):
	//   0 — terrain brickmap (SSBO)
	//   1 — palette (combined image sampler)
	//   2 — frame UBO
	//   3 — foliage meta UBO  (substrate.glsl reads meta.size/frameCount)
	//   4 — foliage instance SSBO  (substrate.glsl reads ib.instances)
	//   5 — substrate SSBO
	//   6 — foliage bitmask SSBO
	m_terrain_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_terrain_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindGraphStorageBuffer(0, m_terrain_buffer)
		 .BindExternalSampledImage(1, m_palette->GetImageView(), m_palette->GetSampler(),
		                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		 .BindUniformBufferPerFrame(2, m_frame_ubo_buffers, sizeof(CombinedFrameUbo))
		 .BindUniformBufferPerFrame(3, m_meta_ubo_buffers, sizeof(VolumeMetaUbo))
		 .BindGraphStorageBuffer(4, m_foliage_instance_buffer)
		 .BindGraphStorageBuffer(5, m_substrate_buffer)
		 .BindGraphStorageBuffer(6, m_foliage_bitmask_buffer);
	m_terrain_bindings->Build();

	auto& terrainPass = graph.AddGraphicsPass(kTerrainPassName);
	terrainPass
		.SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::Store)
		.SetResolveTarget(targets.resolve)
		.Read(m_terrain_buffer,          ResourceUsage::StorageRead)
		.Read(m_foliage_instance_buffer, ResourceUsage::StorageRead)
		.Read(m_substrate_buffer,        ResourceUsage::StorageRead)
		.Read(m_foliage_bitmask_buffer,  ResourceUsage::StorageRead)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/brickmap_palette_trace.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/combined_terrain_trace.frag.spv";
			d.descriptorSetLayout = m_terrain_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			r.offset = 0;
			r.size = sizeof(TerrainTracePC);
			d.pushConstantRanges = { r };
			d.inputAssembly = PipelineDefaults::TriangleStrip();
			d.rasterizer    = PipelineDefaults::NoCullFill();
			// DepthTestWrite — terrain shader writes gl_FragDepth so foliage
			// pass (which loads this depth buffer) z-tests correctly.
			d.depthStencil  = PipelineDefaults::DepthTestWrite();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			WriteFrameUbo(pctx.frameIndex);

			auto vk_cmd = pctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());
			VkDescriptorSet ds = m_terrain_bindings->GetSet(pctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			TerrainTracePC pc{};
			pc.ndcToWorld         = m_camera->GetNDCtoWorldMatrix();
			pc.terrainOriginVoxel = m_terrain_brickmap.originVoxel;
			vkCmdPushConstants(vk_cmd, pctx.graphicsPipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		})
		.SetBindings(m_terrain_bindings);

	// ---- Foliage trace pass ----
	// Bindings (mirrors combined_foliage_trace.frag):
	//   0 — foliage instance SSBO
	//   1 — foliage volume sampler3D
	//   2 — palette
	//   3 — meta UBO
	//   4 — frame UBO
	//   6 — substrate SSBO       (slot 5 vacant — historical layout)
	//   7 — foliage bitmask SSBO
	//   8 — terrain brickmap SSBO  (NEW: substrate.glsl's terrain check)
	m_foliage_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_foliage_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindGraphStorageBuffer(0, m_foliage_instance_buffer)
		 .BindGraphSampledImage(1, m_foliage_volume, m_volume_sampler)
		 .BindExternalSampledImage(2, m_palette->GetImageView(), m_palette->GetSampler(),
		                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		 .BindUniformBufferPerFrame(3, m_meta_ubo_buffers, sizeof(VolumeMetaUbo))
		 .BindUniformBufferPerFrame(4, m_frame_ubo_buffers, sizeof(CombinedFrameUbo))
		 .BindGraphStorageBuffer(6, m_substrate_buffer)
		 .BindGraphStorageBuffer(7, m_foliage_bitmask_buffer)
		 .BindGraphStorageBuffer(8, m_terrain_buffer);
	m_foliage_bindings->Build();

	auto& foliagePass = graph.AddGraphicsPass(kFoliagePassName);
	foliagePass
		.SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Load, StoreOp::Store)
		.SetResolveTarget(targets.resolve)
		.Read(m_foliage_volume,         ResourceUsage::SampledRead)
		.Read(m_foliage_instance_buffer,ResourceUsage::StorageRead)
		.Read(m_substrate_buffer,       ResourceUsage::StorageRead)
		.Read(m_foliage_bitmask_buffer, ResourceUsage::StorageRead)
		.Read(m_terrain_buffer,         ResourceUsage::StorageRead)
		.SetPipeline([this]() {
			GraphicsPipelineDesc d{};
			d.vertSpvPath = std::string(config::SHADER_DIR) + "/instanced_voxel.vert.spv";
			d.fragSpvPath = std::string(config::SHADER_DIR) + "/combined_foliage_trace.frag.spv";
			d.descriptorSetLayout = m_foliage_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
			r.offset = 0;
			r.size = sizeof(FoliageTracePC);
			d.pushConstantRanges = { r };
			d.inputAssembly = PipelineDefaults::TriangleList();
			d.rasterizer    = PipelineDefaults::BackCullFill(false);
			d.depthStencil  = PipelineDefaults::DepthTestWrite();
			d.dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			WriteFrameUbo(pctx.frameIndex);

			auto vk_cmd = pctx.cmd->Get();
			vkCmdBindPipeline(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pctx.graphicsPipeline->Get());
			VkDescriptorSet ds = m_foliage_bindings->GetSet(pctx.frameIndex)->Get();
			vkCmdBindDescriptorSets(vk_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pctx.graphicsPipeline->GetLayout(), 0, 1, &ds, 0, nullptr);

			if (m_foliage_instance_count == 0) return;

			// Cloud anchored at world origin — the trace shader's coord
			// convention requires this. AABB in cloud-local voxels.
			FoliageTracePC pc{};
			pc.cloudWorld = glm::scale(glm::mat4(1.0f), glm::vec3(kWorldVoxelSize));
			pc.aabbMin = glm::vec3(0.0f);
			pc.aabbMax = glm::vec3(static_cast<float>(m_foliage_size.x),
			                       static_cast<float>(m_foliage_size.y),
			                       static_cast<float>(m_foliage_size.z));
			vkCmdPushConstants(vk_cmd, pctx.graphicsPipeline->GetLayout(),
				VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0, sizeof(pc), &pc);

			// 36 verts = unit cube (vertex shader generates positions from
			// gl_VertexIndex). One draw, instanceCount instances.
			vkCmdDraw(vk_cmd, 36, m_foliage_instance_count, 0, 0);
		})
		.SetBindings(m_foliage_bindings);

	// Every graph rebuild re-allocates the persistent buffers with
	// uninitialized content, so we must re-stage the terrain + foliage uploads
	// every time. The pending-data vectors are still valid from the most
	// recent BakeIslandNow / RebuildFoliagePlacement call.
	m_terrain_pending_upload = !m_terrain_brickmap.data.empty();
	m_foliage_pending_upload = !m_pending_substrate_data.empty();

	logger->info("CombinedRenderer: registered with terrain {}x{}x{} (anchor {},{},{}), foliage instances={}, substrate {} words ({} KB)",
		m_terrain_brickmap.volumeSize.x, m_terrain_brickmap.volumeSize.y, m_terrain_brickmap.volumeSize.z,
		m_terrain_brickmap.originVoxel.x, m_terrain_brickmap.originVoxel.y, m_terrain_brickmap.originVoxel.z,
		m_foliage_instance_count,
		m_pending_substrate_data.size(),
		(m_pending_substrate_data.size() * sizeof(uint32_t)) / 1024);
}

void CombinedRenderer::OnPostCompile(RenderGraph& graph) {
	if (!m_assets) return;

	// Apply the terrain bake's palette. The foliage shader uses indices 5,
	// 64..95 — these survive any bake-driven palette since the bake fills the
	// full 256 entries from the same default HSV layout.
	if (m_palette) m_palette->Upload(m_terrain_brickmap.palette.data());

	if (m_terrain_pending_upload && m_graphics_pool && !m_terrain_brickmap.data.empty()) {
		graph.UploadBufferData(m_terrain_buffer,
		                       m_terrain_brickmap.data.data(),
		                       m_terrain_brickmap.ByteSize(),
		                       m_graphics_pool);
		m_terrain_pending_upload = false;
		spdlog::get("Render")->info(
			"CombinedRenderer: uploaded terrain brickmap ({} bricks, {:.2f} MB)",
			m_terrain_brickmap.brickCount,
			m_terrain_brickmap.ByteSize() / (1024.0 * 1024.0));
	}

	if (m_foliage_pending_upload && m_graphics_pool) {
		if (!m_pending_instance_bytes.empty()) {
			graph.UploadBufferData(m_foliage_instance_buffer,
			                       m_pending_instance_bytes.data(),
			                       m_pending_instance_bytes.size(),
			                       m_graphics_pool);
		}
		if (!m_pending_substrate_data.empty()) {
			graph.UploadBufferData(m_substrate_buffer,
			                       m_pending_substrate_data.data(),
			                       m_pending_substrate_data.size() * sizeof(uint32_t),
			                       m_graphics_pool);
		}
		m_foliage_pending_upload = false;
	}
}

void CombinedRenderer::Reload(const RenderContext& ctx) {
	(void)ctx;
	if (m_pending_bake) {
		m_pending_bake = false;
		BakeIslandNow();
		// Re-place foliage on the freshly-baked terrain BEFORE the graph
		// rebuild so the new instance buffer is sized correctly.
		RebuildFoliagePlacement();
		if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});
		return;
	}
	if (m_pending_grid_rebuild) {
		m_pending_grid_rebuild = false;
		RebuildFoliagePlacement();
		if (m_eventSink) m_eventSink({AppEventType::RebuildGraph});
	}
}

void CombinedRenderer::BakeIslandNow() {
	auto logger = spdlog::get("Render");

	const uint32_t side = static_cast<uint32_t>(std::max(8, m_terrain_size));
	m_terrain_cfg.gridSize  = glm::uvec2(side, side);
	m_terrain_cfg.maxHeight = static_cast<uint32_t>(std::max(8, m_terrain_max_height));
	m_terrain_cfg.octaves   = std::max(1, m_terrain_octaves);
	m_terrain_cfg.seed      = static_cast<uint32_t>(std::max(0, m_terrain_seed));

	const auto t0 = std::chrono::steady_clock::now();
	m_terrain_brickmap = PrimitiveFactory::BakeIslandTerrainBrickmap(m_terrain_cfg);
	const auto t1 = std::chrono::steady_clock::now();
	const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

	// Anchor terrain so its center sits at world origin. Substrate's
	// terrainOriginVoxel addresses brick (0,0,0) in world voxel coords.
	const glm::ivec3 vs(static_cast<int>(m_terrain_brickmap.volumeSize.x),
	                    static_cast<int>(m_terrain_brickmap.volumeSize.y),
	                    static_cast<int>(m_terrain_brickmap.volumeSize.z));
	m_terrain_brickmap.originVoxel = glm::ivec3(-vs.x / 2, -vs.y / 2, -vs.z / 2);
	m_terrain_pending_upload = true;
	m_terrain_baked_ever     = true;

	logger->info(
		"CombinedRenderer: baked island {}×{}×{} ({} bricks, {:.2f} MB) in {:.1f} ms; anchor=({}, {}, {})",
		m_terrain_brickmap.volumeSize.x, m_terrain_brickmap.volumeSize.y, m_terrain_brickmap.volumeSize.z,
		m_terrain_brickmap.brickCount,
		m_terrain_brickmap.ByteSize() / (1024.0 * 1024.0), ms,
		m_terrain_brickmap.originVoxel.x, m_terrain_brickmap.originVoxel.y, m_terrain_brickmap.originVoxel.z);

	// Reposition camera so the island is framed. Eye above + back, looking at
	// the terrain center.
	if (m_camera) {
		const glm::vec3 halfWorld = glm::vec3(vs) * (kWorldVoxelSize * 0.5f);
		const glm::vec3 center(0.0f);
		const glm::vec3 eye = center + glm::vec3( halfWorld.x * 1.6f,
		                                         -halfWorld.y * 1.6f,
		                                          halfWorld.z * 2.5f);
		m_camera->SetPosition(eye);
		m_camera->SetForward(center - eye);
		const float farPlane = std::max(50.0f,
			std::max({halfWorld.x, halfWorld.y, halfWorld.z}) * 8.0f);
		m_camera->SetNearFar(0.05f, farPlane);
	}
}

void CombinedRenderer::RebuildFoliagePlacement() {
	auto logger = spdlog::get("Render");
	if (!m_terrain_baked_ever) {
		m_foliage_instance_count = 0;
		return;
	}

	// Auto-fit the foliage grid to the island footprint. Pitch (in world
	// voxels) = floor(island_x / grid_dim). Square footprint matches today's
	// island bake (gridSize.x == gridSize.y).
	const int gridDim = std::max(1, m_foliage_grid_dim);
	const int islandX = static_cast<int>(m_terrain_brickmap.volumeSize.x);
	const int islandY = static_cast<int>(m_terrain_brickmap.volumeSize.y);
	const int pitch   = std::max(1, std::min(islandX, islandY) / gridDim);
	m_foliage_pitch_voxels = pitch;

	std::mt19937 rng(0xFEED1337u);
	std::uniform_int_distribution<int> yawIdxDist(0, 3);
	std::uniform_real_distribution<float> phaseDist(0.0f, static_cast<float>(m_foliage_frame_count));
	std::uniform_int_distribution<int> jitterDist(-pitch / 4, pitch / 4);

	std::vector<GpuInstance> instances;
	instances.reserve(static_cast<size_t>(gridDim) * gridDim);

	const glm::ivec3 origin = m_terrain_brickmap.originVoxel;
	// Sea level + small margin in voxels. Below this z, foliage is skipped
	// (no underwater grass for v1).
	const int seaLevelZ = static_cast<int>(m_terrain_cfg.seaLevel *
	                       static_cast<float>(m_terrain_brickmap.volumeSize.z)) + 2;

	for (int gy = 0; gy < gridDim; ++gy)
	for (int gx = 0; gx < gridDim; ++gx) {
		// Cell-center placement plus a small jitter so the lattice doesn't
		// look uniform. Jitter stays within ±pitch/4 to keep cells distinct.
		int wantTx = (gx * islandX) / gridDim + (islandX / gridDim) / 2 + jitterDist(rng);
		int wantTy = (gy * islandY) / gridDim + (islandY / gridDim) / 2 + jitterDist(rng);
		wantTx = std::clamp(wantTx, 0, islandX - 1);
		wantTy = std::clamp(wantTy, 0, islandY - 1);

		const int topZ = FindTopSolidZ(m_terrain_brickmap, wantTx, wantTy);
		if (topZ < 0 || topZ < seaLevelZ) continue;

		// World voxel coords. Cloud is at world origin so cloud-local voxel
		// position = world voxel position.
		const glm::ivec3 worldVx(wantTx + origin.x,
		                         wantTy + origin.y,
		                         topZ + 1 + origin.z);

		GpuInstance gi{};
		gi.position = glm::vec3(static_cast<float>(worldVx.x),
		                        static_cast<float>(worldVx.y),
		                        static_cast<float>(worldVx.z));
		gi.scale    = 1.0f;
		const int yawIdx = yawIdxDist(rng);
		const float yaw  = static_cast<float>(yawIdx) * 1.57079632679f;
		const glm::quat q = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
		gi.rotation  = glm::vec4(q.x, q.y, q.z, q.w);
		gi.yawIdx    = yawIdx;
		gi.animOffset = phaseDist(rng);
		instances.push_back(gi);
	}

	m_foliage_instance_count = static_cast<uint32_t>(instances.size());

	if (m_foliage_instance_count == 0) {
		logger->warn("CombinedRenderer: foliage placement produced 0 instances "
		             "(no terrain columns above seaLevel + margin). Check terrain config.");
		m_pending_instance_bytes.clear();
		m_pending_substrate_data.clear();
		m_foliage_pending_upload = true;  // still upload empties so prior state doesn't linger
		return;
	}

	// Build the substrate from the placed instances.
	std::vector<Substrate::InstanceInput> substrateInputs;
	substrateInputs.reserve(instances.size());
	for (const auto& gi : instances) {
		Substrate::InstanceInput in;
		in.cloudVoxelPos = glm::ivec3(glm::round(gi.position));
		in.yawIdx        = static_cast<uint8_t>(gi.yawIdx & 0x3);
		substrateInputs.push_back(in);
	}
	auto build = Substrate::BuildFoliage(
		substrateInputs.data(),
		static_cast<uint32_t>(substrateInputs.size()),
		m_foliage_size);

	// Stash bytes for OnPostCompile to upload. We hold raw bytes (not the
	// typed GpuInstance vector) so the header doesn't have to surface the
	// std430 layout — instances live entirely inside the .cpp.
	m_pending_instance_bytes.resize(instances.size() * sizeof(GpuInstance));
	std::memcpy(m_pending_instance_bytes.data(),
	            instances.data(),
	            m_pending_instance_bytes.size());
	m_pending_substrate_data = std::move(build.data);
	m_foliage_pending_upload = true;

	logger->info("CombinedRenderer: placed {} foliage instances (grid {}×{} pitch {} voxels), substrate {} words",
		m_foliage_instance_count, gridDim, gridDim, pitch, m_pending_substrate_data.size());
}

void CombinedRenderer::WriteFrameUbo(uint32_t frameIndex) {
	if (frameIndex >= m_frame_ubo_mapped.size() || !m_frame_ubo_mapped[frameIndex]) return;

	CombinedFrameUbo ubo{};
	const glm::mat4 view = m_camera->GetViewMatrix();
	const glm::mat4 proj = m_camera->GetProjectionMatrix();
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

	ubo.frameCount         = static_cast<int32_t>(m_foliage_frame_count);
	ubo.worldVoxelSize     = kWorldVoxelSize;
	ubo.terrainOriginVoxel = m_terrain_brickmap.originVoxel;
	const auto now = std::chrono::steady_clock::now();
	ubo.time = std::chrono::duration<float>(now - m_start_time).count() * m_animation_speed;

	std::memcpy(m_frame_ubo_mapped[frameIndex], &ubo, sizeof(ubo));
}

std::vector<std::string> CombinedRenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/instanced_voxel_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/instanced_voxel.vert.spv",
		std::string(config::SHADER_DIR) + "/combined_foliage_trace.frag.spv",
		std::string(config::SHADER_DIR) + "/brickmap_palette_trace.vert.spv",
		std::string(config::SHADER_DIR) + "/combined_terrain_trace.frag.spv",
		std::string(config::SHADER_DIR) + "/sky_fullscreen.vert.spv",
		std::string(config::SHADER_DIR) + "/sky_fullscreen.frag.spv",
	};
}

std::vector<TechniqueParameter>& CombinedRenderer::GetParameters() {
	if (m_parameters.empty()) {
		m_parameters.push_back({ "Trace", TechniqueParameter::Header });
		m_parameters.push_back({ "Animation Speed", TechniqueParameter::Float, &m_animation_speed, 0.0f, 30.0f });
		m_parameters.push_back({ "Max Iterations",  TechniqueParameter::Int,   &m_max_iterations, 1.0f, 1024.0f });
		m_parameters.push_back({ "Debug Coloring",  TechniqueParameter::Bool,  &m_debug_color });
		m_parameters.push_back({ "Enable Shadows",  TechniqueParameter::Bool,  &m_shadows_enabled });

		m_parameters.push_back({ "Foliage", TechniqueParameter::Header });
		TechniqueParameter gridDim;
		gridDim.label = "Grid Side";
		gridDim.type  = TechniqueParameter::Int;
		gridDim.data  = &m_foliage_grid_dim;
		gridDim.min   = 1.0f;
		gridDim.max   = 64.0f;
		gridDim.onChanged = [this]() {
			m_pending_grid_rebuild = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(gridDim));

		m_parameters.push_back({ "Island Terrain", TechniqueParameter::Header });
		m_parameters.push_back({ "Size",         TechniqueParameter::Int,   &m_terrain_size,       8.0f, 8192.0f });
		m_parameters.push_back({ "Max Height",   TechniqueParameter::Int,   &m_terrain_max_height, 8.0f, 512.0f });
		m_parameters.push_back({ "Noise Scale",  TechniqueParameter::Float, &m_terrain_cfg.noiseScale,    0.001f, 0.05f });
		m_parameters.push_back({ "Octaves",      TechniqueParameter::Int,   &m_terrain_octaves,    1.0f,   8.0f });
		m_parameters.push_back({ "Lacunarity",   TechniqueParameter::Float, &m_terrain_cfg.lacunarity,    1.5f,   3.0f });
		m_parameters.push_back({ "Gain",         TechniqueParameter::Float, &m_terrain_cfg.gain,          0.2f,   0.8f });
		m_parameters.push_back({ "Island Radius",  TechniqueParameter::Float, &m_terrain_cfg.islandRadius,  0.05f, 0.95f });
		m_parameters.push_back({ "Island Falloff", TechniqueParameter::Float, &m_terrain_cfg.islandFalloff, 0.01f, 0.6f });
		m_parameters.push_back({ "Sea Level",    TechniqueParameter::Float, &m_terrain_cfg.seaLevel,      0.0f,  0.6f });
		m_parameters.push_back({ "Beach Width",  TechniqueParameter::Float, &m_terrain_cfg.beachWidth,    0.0f,  0.2f });
		m_parameters.push_back({ "Seed",         TechniqueParameter::Int,   &m_terrain_seed,       0.0f,   1000000.0f });

		TechniqueParameter bake;
		bake.label = "Bake Island";
		bake.type  = TechniqueParameter::Button;
		bake.onClicked = [this]() {
			m_pending_bake = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(bake));
	}
	return m_parameters;
}

FrameStats CombinedRenderer::GetFrameStats() const {
	// Sky (1 draw) + terrain (1 fullscreen) + foliage (1 instanced cube).
	const uint32_t cubeVerts = 36 * m_foliage_instance_count;
	return { 3, 4 + 4 + cubeVerts, 0 };
}
