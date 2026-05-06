#include "CombinedRenderer.h"
#include "BrickGrid.h"
#include "EnginePaths.h"
#include "PipelineDefaults.h"
#include "VoxAnimFormat.h"
#include "config.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <system_error>

namespace {

constexpr const char* kTerrainBufferName       = "combined_terrain_brickmap";
constexpr const char* kFoliageAssetName        = "combined_foliage_volume";
constexpr const char* kFoliageBitmaskName      = "combined_foliage_bitmask";   // asset-space (per-frame) bitmask
constexpr const char* kFoliageInstancesName    = "combined_foliage_instances";
constexpr const char* kShadowBrickmapName      = "combined_shadow_brickmap";
constexpr const char* kInstanceBricksName      = "combined_instance_bricks";
constexpr const char* kFoliageGenPassName      = "Combined Foliage Generate";
constexpr const char* kFoliageBitmaskPassName  = "Combined Foliage Bitmask Fill";
constexpr const char* kShadowWritePassName     = "Combined Shadow Foliage Write";
constexpr const char* kSkyPassName             = "Combined Sky";
constexpr const char* kTerrainPassName         = "Combined Terrain Trace";
constexpr const char* kFoliagePassName         = "Combined Foliage Trace";

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
// pad keeps the struct a clean 16 B multiple.
struct CombinedFrameUbo {
	glm::mat4 viewProj;            // 64
	glm::mat4 ndcToWorld;          // 64
	glm::vec3 cameraPos;           int32_t maxIterations;        // 16  (primary trace cap)
	glm::vec3 skyColor;            int32_t debugColor;            // 16
	glm::vec3 sunDirection;        float   sunCosHalfAngle;       // 16
	glm::vec3 sunColor;            float   sunIntensity;          // 16
	float     ambientIntensity;
	float     aoStrength;
	int32_t   shadowsEnabled;
	float     time;                                                // 16
	int32_t   frameCount;
	float     worldVoxelSize;
	int32_t   maxShadowBrickSteps;                                 // shadow trace outer cap
	float     _ubo_pad1;                                           // 16
};
static_assert(sizeof(CombinedFrameUbo) == 224,
	"CombinedFrameUbo layout drift — update both combined_*_trace.frag UBO blocks to match");

// Compute pass push constant — reuses instanced_voxel_generate.comp.
struct GeneratePC {
	int32_t sizeX, sizeY, sizeZ;
	int32_t frameCount;
	int32_t numXWords;
};

// Push constant for shadow_foliage_write.comp. Mirrors that shader's `PC`
// block layout exactly (32 bytes).
struct ShadowWritePC {
	int32_t assetSizeX, assetSizeY, assetSizeZ;
	int32_t frameCount;
	float   time;
	int32_t numXWords;
	int32_t _pad0;
	int32_t _pad1;
};
static_assert(sizeof(ShadowWritePC) == 32,
	"ShadowWritePC layout drift — update shadow_foliage_write.comp PC block");

// Terrain trace pass push constant — primary-trace geometry only. Shading
// state lives in the FrameUbo. The new shadow brickmap subsumes the old
// per-draw `terrainOriginVoxel` plumbing — shadow lookups read the brickmap
// header directly.
struct TerrainTracePC {
	glm::mat4 ndcToWorld;            // 64
};
static_assert(sizeof(TerrainTracePC) == 64,
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
//
// Returns 0 (empty) for any out-of-range query OR for an unallocated brick.
// Otherwise returns the palette index byte at v — 0 still means "no voxel
// here" by palette convention. The bool form is just a non-zero check on
// the same lookup.
uint8_t BrickmapVoxelMaterial(const BrickmapData& bm, glm::ivec3 v) {
	if (v.x < 0 || v.y < 0 || v.z < 0) return 0;
	if (v.x >= int(bm.volumeSize.x) ||
	    v.y >= int(bm.volumeSize.y) ||
	    v.z >= int(bm.volumeSize.z)) return 0;

	const int bs = static_cast<int>(bm.brickSize);
	const glm::ivec3 brickCell(v.x / bs, v.y / bs, v.z / bs);
	const glm::ivec3 local(v.x - brickCell.x * bs,
	                       v.y - brickCell.y * bs,
	                       v.z - brickCell.z * bs);
	const int gridIdx = brickCell.x
	                  + brickCell.y * static_cast<int>(bm.gridDim.x)
	                  + brickCell.z * static_cast<int>(bm.gridDim.x * bm.gridDim.y);
	const uint32_t brickIndex = bm.data[8 + gridIdx];
	if (brickIndex == 0xFFFFFFFFu) return 0;

	const int linear   = local.z * 64 + local.y * 8 + local.x;
	const int wordIdx  = linear / 4;
	const int byteLane = linear % 4;
	const uint32_t topCells = bm.gridDim.x * bm.gridDim.y * bm.gridDim.z;
	const uint32_t word = bm.data[8u + topCells + brickIndex * 128u + uint32_t(wordIdx)];
	return static_cast<uint8_t>((word >> (byteLane * 8)) & 0xFFu);
}

bool BrickmapVoxelSolid(const BrickmapData& bm, glm::ivec3 v) {
	return BrickmapVoxelMaterial(bm, v) != 0u;
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

CombinedRenderer::CombinedRenderer() {
	// Default the foliage VXA path to the convention cache location so the
	// Promote-to-scene workflow auto-loads with no further configuration.
	// Users can clear / reroute this via the inspector's File parameter.
	m_foliage_vxa_path = engine_paths::GetPromotedFoliagePath().string();

	// Seed the derived grid params from defaults so the inspector readouts
	// are populated before the first bake/load.
	RecomputeFoliageGrid();
}

void CombinedRenderer::RecomputeFoliageGrid() {
	// Grid Size and Density are independent user inputs; this just refreshes
	// the derived values shown in the inspector readout. Pitch is informative
	// only — placement uses m_foliage_grid_dim directly.
	m_asset_footprint_voxels = std::max<uint32_t>(m_foliage_size.x, m_foliage_size.y);
	const int gridDim = std::max(1, m_foliage_grid_dim);
	const uint32_t islandSize = static_cast<uint32_t>(std::max(1, m_terrain_size));
	const uint32_t pitch = std::max<uint32_t>(1u, islandSize / static_cast<uint32_t>(gridDim));
	m_foliage_pitch_voxels = static_cast<int>(pitch);

	char buf[160];
	std::snprintf(buf, sizeof(buf),
		"pitch=%u vx, footprint=%u vx", pitch, m_asset_footprint_voxels);
	m_foliage_grid_status = buf;
}

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
	m_graph         = &graph;
	if (m_start_time_seconds < 0.0) {
		m_start_time_seconds = GetTimeSeconds();
	}

	// First-time-ever bake. Sets m_terrain_brickmap, m_terrain_pending_upload,
	// repositions camera. Subsequent bakes are user-triggered via the inspector.
	if (m_pending_bake) {
		m_pending_bake = false;
		BakeIslandNow();
		RebuildFoliagePlacement();
	}

	// First-time foliage asset setup, with optional .vxa auto-load. If the
	// "Foliage VXA Path" param points at a real file, we parse it BEFORE
	// creating the procedural slot so the slot is declared at the file's
	// size — that single declaration is what the graph builds the image
	// against. (Resizing after CreateProceduralAnimatedVoxelVolume would
	// leave the just-declared image at the wrong size for one rebuild.)
	if (!m_foliage_asset.valid()) {
		std::optional<voxel_bake::LoadedVxa> firstLoad;
		if (!m_foliage_vxa_path.empty()) {
			std::error_code ec;
			if (std::filesystem::exists(m_foliage_vxa_path, ec) && !ec) {
				firstLoad = voxel_bake::LoadVxa(m_foliage_vxa_path);
			}
		}
		if (firstLoad) {
			m_foliage_size        = firstLoad->manifest.size;
			m_foliage_frame_count = firstLoad->manifest.frameCount;
			m_use_baked_foliage   = true;
			std::error_code ec;
			auto t = std::filesystem::last_write_time(m_foliage_vxa_path, ec);
			if (!ec) m_foliage_vxa_loaded_mtime = t;
			// First-time auto-load matches the Promote-on-demand UX: refresh
			// the readout from the new size and drop a single instance at
			// island center until the user dials density up.
			RecomputeFoliageGrid();
			m_foliage_grid_dim = 1;
		}
		m_foliage_asset = m_assets->CreateProceduralAnimatedVoxelVolume(
			kFoliageAssetName, m_foliage_size, m_foliage_frame_count, VK_FORMAT_R8_UINT);
		if (firstLoad) {
			if (auto* slot = m_assets->GetVoxelVolume(m_foliage_asset)) {
				slot->data        = std::move(firstLoad->framesData);
				slot->palette     = firstLoad->palette;
				slot->needsUpload = true;
			}
			m_pending_grid_rebuild = true;
		}
	} else if (m_use_baked_foliage && !m_foliage_vxa_path.empty()) {
		// Subsequent rebuilds: detect external file changes (re-promote, hand-
		// edit). The actual resize/reload is deferred to Reload() — mutating
		// asset size inside RegisterPasses would race with the in-flight
		// graph build's already-completed DeclareGraphResources.
		std::error_code ec;
		if (std::filesystem::exists(m_foliage_vxa_path, ec) && !ec) {
			auto t = std::filesystem::last_write_time(m_foliage_vxa_path, ec);
			if (!ec && t != m_foliage_vxa_loaded_mtime) {
				m_pending_foliage_load = true;
				if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
			}
		}
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

	// Shadow occupancy brickmap — sized to fit the just-built shadow brickmap.
	// Surface-placed foliage spreads across the full island footprint, so the
	// top-level grid is large; the brick pool is bounded by the union of
	// terrain bricks + foliage instance bricks. We size to the actual build's
	// word count with a 2× headroom for re-bake variance.
	const size_t actualShadowWords = m_pending_shadow_data.size();
	const size_t shadowHeadroom    = 2;
	const size_t minShadowWords    = 1024;
	m_shadow_word_capacity = static_cast<uint32_t>(
		std::max(minShadowWords, actualShadowWords * shadowHeadroom));
	BufferDesc sbm{};
	sbm.size     = sizeof(uint32_t) * m_shadow_word_capacity;
	// The shadow_foliage_write compute writes (atomicOr) into this buffer
	// each frame; vkCmdFillBuffer (transfer) clears the dynamic pool first.
	sbm.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
	             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	sbm.lifetime = Lifetime::Persistent;
	m_shadow_buffer = graph.CreateBuffer(kShadowBrickmapName, sbm);

	// Instance-bricks SSBO — one entry per (instance, world-brick) pair.
	// Drives the shadow_foliage_write compute's dispatch shape.
	const uint32_t instanceBrickUpper = std::max<uint32_t>(
		ShadowBrickmap::UpperBoundInstanceBricks(
			std::max(1u, m_foliage_instance_count), m_foliage_size),
		1u);
	m_instance_brick_capacity = std::max<uint32_t>(
		instanceBrickUpper,
		static_cast<uint32_t>(m_pending_instance_bricks.size()));
	BufferDesc ibb{};
	ibb.size     = sizeof(ShadowBrickmap::InstanceBrick) *
	               std::max<uint32_t>(m_instance_brick_capacity, 1u);
	ibb.usage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	ibb.lifetime = Lifetime::Persistent;
	m_instance_bricks_buffer = graph.CreateBuffer(kInstanceBricksName, ibb);

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

	// Foliage volume + bitmask population. Two mutually-exclusive paths share
	// the same dispatch shape (32 voxels-along-X per thread, sizeY/4 by
	// (sizeZ*frameCount)/4) and the same descriptor layout — what differs is
	// the shader and the volume's read/write usage:
	//   - Procedural: foliage-gen writes BOTH volume + bitmask in one pass
	//     (free side-effect of knowing which voxels it generated).
	//   - Baked from .vxa: the volume bytes were CPU-uploaded; bitmask-fill
	//     reads the volume and derives the bitmask. The foliage-gen pass
	//     would clobber the loaded volume and is gated off.
	const bool useBakedFoliage = m_use_baked_foliage;
	const char* foliageComputeShader = useBakedFoliage
		? "/voxel_bitmask_fill.comp.spv"
		: "/instanced_voxel_generate.comp.spv";
	const char* foliageComputePassName = useBakedFoliage
		? kFoliageBitmaskPassName
		: kFoliageGenPassName;

	auto& foliageComputePass = graph.AddComputePass(foliageComputePassName);
	if (useBakedFoliage) {
		// Baked path: read volume, write bitmask only.
		foliageComputePass
			.Read (m_foliage_volume,         ResourceUsage::StorageRead)
			.Write(m_foliage_bitmask_buffer, ResourceUsage::StorageWrite);
	} else {
		// Procedural path: write both.
		foliageComputePass
			.Write(m_foliage_volume,         ResourceUsage::StorageWrite)
			.Write(m_foliage_bitmask_buffer, ResourceUsage::StorageWrite);
	}
	foliageComputePass
		.SetPipeline([this, foliageComputeShader]() {
			ComputePipelineDesc d;
			d.compSpvPath = std::string(config::SHADER_DIR) + foliageComputeShader;
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

	// ---- Shadow foliage write compute pass ----
	//
	// Each frame: clear the shadow brickmap's dynamic pool, then dispatch
	// one workgroup per (instance, world-brick) entry. Each workgroup is
	// 8x8x8 = 512 threads (one per voxel inside the brick); threads atomicOr
	// foliage occupancy bits into the dynamic pool from the asset bitmask
	// the previous compute pass just wrote. See docs/SHADOW-BRICKS.md §5.2.
	m_shadow_write_bindings = std::make_shared<BindingTable>(m_device, 1);
	m_shadow_write_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		 .AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		 .AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT)
		 .BindGraphStorageBuffer(0, m_shadow_buffer)
		 .BindGraphStorageBuffer(1, m_instance_bricks_buffer)
		 .BindGraphStorageBuffer(2, m_foliage_instance_buffer)
		 .BindGraphStorageBuffer(3, m_foliage_bitmask_buffer);
	m_shadow_write_bindings->Build();

	graph.AddComputePass(kShadowWritePassName)
		// Read the asset bitmask the foliage-generate pass just produced.
		.Read(m_foliage_bitmask_buffer,  ResourceUsage::StorageRead)
		.Read(m_foliage_instance_buffer, ResourceUsage::StorageRead)
		.Read(m_instance_bricks_buffer,  ResourceUsage::StorageRead)
		// Write the shadow brickmap's dynamic pool. Declared as
		// StorageWrite even though the dynamic pool is also TRANSFER_WRITE
		// inside the body — the graph's pre-pass barrier transitions to
		// STORAGE_WRITE; an internal barrier inside SetRecord handles the
		// fill→dispatch handoff.
		.Write(m_shadow_buffer,          ResourceUsage::StorageWrite)
		.SetPipeline([this]() {
			ComputePipelineDesc d;
			d.compSpvPath = std::string(config::SHADER_DIR) + "/shadow_foliage_write.comp.spv";
			d.descriptorSetLayout = m_shadow_write_bindings->GetLayout();
			VkPushConstantRange r{};
			r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			r.offset     = 0;
			r.size       = sizeof(ShadowWritePC);
			d.pushConstantRanges = { r };
			return d;
		})
		.SetRecord([this](PassContext& pctx) {
			// Nothing to write — no foliage placed, or topology hasn't
			// emitted any instance-brick entries. The graph zero-allocates
			// the buffer, so leaving the dynamic pool untouched is safe.
			if (m_pending_instance_brick_count == 0 ||
			    m_foliage_instance_count == 0 ||
			    m_shadow_dynamic_pool_size_bytes == 0) {
				return;
			}

			auto vk_cmd = pctx.cmd->Get();
			VkBuffer shadowBuf = m_graph->GetVkBuffer(m_shadow_buffer);

			// 1. Clear the dynamic pool. vkCmdFillBuffer writes the 32-bit
			// pattern (0u) — std430 layout means one bit-pool word per
			// uint32, so 0 == "no bits set" exactly.
			vkCmdFillBuffer(vk_cmd, shadowBuf,
				m_shadow_dynamic_pool_offset_bytes,
				m_shadow_dynamic_pool_size_bytes,
				0u);

			// 2. Barrier: fill (TRANSFER_WRITE) → dispatch (SHADER access).
			// The graph's pre-pass barrier already transitioned the buffer
			// to STORAGE_WRITE; this one fences the in-pass fill against
			// the in-pass dispatch.
			VkBufferMemoryBarrier bar{};
			bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
			bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bar.buffer = shadowBuf;
			bar.offset = m_shadow_dynamic_pool_offset_bytes;
			bar.size   = m_shadow_dynamic_pool_size_bytes;
			vkCmdPipelineBarrier(vk_cmd,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0,
				0, nullptr,
				1, &bar,
				0, nullptr);

			// 3. Dispatch — one workgroup per (instance, brick) entry,
			// 8×8×8 threads per workgroup (one per voxel in the brick).
			pctx.cmd->CmdBindComputePipeline(pctx.computePipeline);
			pctx.cmd->CmdBindComputeDescriptorSets(pctx.computePipeline->GetLayout(),
				{ m_shadow_write_bindings->GetSet(0)->Get() });

			ShadowWritePC pc{};
			pc.assetSizeX = static_cast<int32_t>(m_foliage_size.x);
			pc.assetSizeY = static_cast<int32_t>(m_foliage_size.y);
			pc.assetSizeZ = static_cast<int32_t>(m_foliage_size.z);
			pc.frameCount = static_cast<int32_t>(m_foliage_frame_count);
			const double now = GetTimeSeconds();
			pc.time       = static_cast<float>(now - m_start_time_seconds) * m_animation_speed;
			pc.numXWords  = static_cast<int32_t>(BitmaskXWords(m_foliage_size.x));
			vkCmdPushConstants(vk_cmd, pctx.computePipeline->GetLayout(),
				VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

			pctx.cmd->CmdDispatch(m_pending_instance_brick_count, 1, 1);
		})
		.SetBindings(m_shadow_write_bindings);

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
	//   0 — terrain palette brickmap (SSBO, primary trace only)
	//   1 — palette texture
	//   2 — frame UBO
	//   3 — shadow occupancy brickmap (SSBO, shadow trace only)
	m_terrain_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_terrain_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindGraphStorageBuffer(0, m_terrain_buffer)
		 .BindExternalSampledImage(1, m_palette->GetImageView(), m_palette->GetSampler(),
		                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		 .BindUniformBufferPerFrame(2, m_frame_ubo_buffers, sizeof(CombinedFrameUbo))
		 .BindGraphStorageBuffer(3, m_shadow_buffer);
	m_terrain_bindings->Build();

	auto& terrainPass = graph.AddGraphicsPass(kTerrainPassName);
	terrainPass
		.SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Clear, StoreOp::Store)
		.SetResolveTarget(targets.resolve)
		.Read(m_terrain_buffer,  ResourceUsage::StorageRead)
		.Read(m_shadow_buffer,   ResourceUsage::StorageRead)
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
			pc.ndcToWorld = m_camera->GetNDCtoWorldMatrix();
			vkCmdPushConstants(vk_cmd, pctx.graphicsPipeline->GetLayout(),
				VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

			vkCmdDraw(vk_cmd, 4, 1, 0, 0);
		})
		.SetBindings(m_terrain_bindings);

	// ---- Foliage trace pass ----
	// Bindings (mirrors combined_foliage_trace.frag):
	//   0 — foliage instance SSBO  (vertex shader; primary trace)
	//   1 — foliage volume sampler3D
	//   2 — palette
	//   3 — foliage meta UBO
	//   4 — frame UBO
	//   5 — shadow occupancy brickmap (SSBO, shadow trace only)
	m_foliage_bindings = std::make_shared<BindingTable>(m_device, ctx.maxFramesInFlight);
	m_foliage_bindings
		->AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		 .AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT)
		 .BindGraphStorageBuffer(0, m_foliage_instance_buffer)
		 .BindGraphSampledImage(1, m_foliage_volume, m_volume_sampler)
		 .BindExternalSampledImage(2, m_palette->GetImageView(), m_palette->GetSampler(),
		                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		 .BindUniformBufferPerFrame(3, m_meta_ubo_buffers, sizeof(VolumeMetaUbo))
		 .BindUniformBufferPerFrame(4, m_frame_ubo_buffers, sizeof(CombinedFrameUbo))
		 .BindGraphStorageBuffer(5, m_shadow_buffer);
	m_foliage_bindings->Build();

	auto& foliagePass = graph.AddGraphicsPass(kFoliagePassName);
	foliagePass
		.SetColorAttachment(targets.color, LoadOp::Load, StoreOp::Store, 0, 0, 0, 1)
		.SetDepthAttachment(targets.depth, LoadOp::Load, StoreOp::Store)
		.SetResolveTarget(targets.resolve)
		.Read(m_foliage_volume,          ResourceUsage::SampledRead)
		.Read(m_foliage_instance_buffer, ResourceUsage::StorageRead)
		.Read(m_shadow_buffer,           ResourceUsage::StorageRead)
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
	// uninitialized content, so we must re-stage the terrain + foliage +
	// shadow brickmap uploads every time.
	m_terrain_pending_upload = !m_terrain_brickmap.data.empty();
	m_foliage_pending_upload = !m_pending_instance_bytes.empty();
	m_shadow_pending_upload  = !m_pending_shadow_data.empty();

	logger->info("CombinedRenderer: registered terrain {}x{}x{} (anchor {},{},{}), foliage={} instances, "
	             "shadow brickmap {} words ({:.2f} MB), {} instance-bricks",
		m_terrain_brickmap.volumeSize.x, m_terrain_brickmap.volumeSize.y, m_terrain_brickmap.volumeSize.z,
		m_terrain_brickmap.originVoxel.x, m_terrain_brickmap.originVoxel.y, m_terrain_brickmap.originVoxel.z,
		m_foliage_instance_count,
		m_pending_shadow_data.size(),
		(m_pending_shadow_data.size() * sizeof(uint32_t)) / (1024.0 * 1024.0),
		m_pending_instance_bricks.size());
}

void CombinedRenderer::OnPostCompile(RenderGraph& graph) {
	if (!m_assets) return;

	// Apply the terrain bake's palette. PrimitiveFactory::BuildIslandPalette
	// seeds from BuildDefaultPalette() before overlaying the four terrain
	// materials (1..4), so foliage's expected indices (5 sod, 64..95 green
	// band) stay populated. See docs/COMBINED-FOLIAGE-BLACK-BUG.md for the
	// failure mode this avoids.
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
		m_foliage_pending_upload = false;
	}

	if (m_shadow_pending_upload && m_graphics_pool && !m_pending_shadow_data.empty()) {
		graph.UploadBufferData(m_shadow_buffer,
		                       m_pending_shadow_data.data(),
		                       m_pending_shadow_data.size() * sizeof(uint32_t),
		                       m_graphics_pool);

		if (!m_pending_instance_bricks.empty()) {
			graph.UploadBufferData(m_instance_bricks_buffer,
			                       m_pending_instance_bricks.data(),
			                       m_pending_instance_bricks.size() *
			                           sizeof(ShadowBrickmap::InstanceBrick),
			                       m_graphics_pool);
		}
		m_pending_instance_brick_count = static_cast<uint32_t>(m_pending_instance_bricks.size());
		m_shadow_pending_upload = false;

		spdlog::get("Render")->info(
			"CombinedRenderer: uploaded shadow brickmap ({:.2f} MB), {} instance-bricks",
			m_pending_shadow_data.size() * sizeof(uint32_t) / (1024.0 * 1024.0),
			m_pending_instance_bricks.size());
	}
}

void CombinedRenderer::Reload(const RenderContext& ctx) {
	(void)ctx;

	// Foliage .vxa load (defer-from-onFileChanged or mtime detection in
	// RegisterPasses). Apply the file before bake/grid rebuilds so foliage
	// placement uses the new asset footprint.
	if (m_pending_foliage_load) {
		m_pending_foliage_load = false;
		auto logger = spdlog::get("Render");
		auto loaded = voxel_bake::LoadVxa(m_foliage_vxa_path);
		if (loaded && m_assets && m_foliage_asset.valid()) {
			m_foliage_size        = loaded->manifest.size;
			m_foliage_frame_count = loaded->manifest.frameCount;
			m_use_baked_foliage   = true;
			std::error_code ec;
			auto t = std::filesystem::last_write_time(m_foliage_vxa_path, ec);
			if (!ec) m_foliage_vxa_loaded_mtime = t;

			m_assets->ResizeProceduralVoxelVolume(m_foliage_asset,
				m_foliage_size, m_foliage_frame_count);
			if (auto* slot = m_assets->GetVoxelVolume(m_foliage_asset)) {
				slot->data        = std::move(loaded->framesData);
				slot->palette     = loaded->palette;
				slot->needsUpload = true;
			}
			// Refresh the inspector readout off the new asset size, but
			// then force grid_dim = 1 so Promote-to-scene drops a single
			// instance at island center (per island-scene.md §4.5/§5.3).
			// User dragging Density will repopulate.
			RecomputeFoliageGrid();
			m_foliage_grid_dim = 1;
			m_pending_grid_rebuild = true;

			if (logger) logger->info(
				"CombinedRenderer: loaded foliage .vxa: {}×{}×{} ({} frames) from {}",
				m_foliage_size.x, m_foliage_size.y, m_foliage_size.z,
				m_foliage_frame_count, m_foliage_vxa_path);
		} else if (logger) {
			logger->warn("CombinedRenderer: failed to load .vxa: {}", m_foliage_vxa_path);
		}
	}

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

	// Island size feeds into the foliage pitch divisor — refresh derived grid.
	RecomputeFoliageGrid();

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

	const int islandX = static_cast<int>(m_terrain_brickmap.volumeSize.x);
	const int islandY = static_cast<int>(m_terrain_brickmap.volumeSize.y);
	const glm::ivec3 origin = m_terrain_brickmap.originVoxel;
	const int seaLevelZ = static_cast<int>(m_terrain_cfg.seaLevel *
	                       static_cast<float>(m_terrain_brickmap.volumeSize.z)) + 2;

	// ---- Single-instance placement (Promote-to-scene drop) ----
	//
	// When the foliage grid is 1×1 we explicitly want the asset centered
	// on the island, sunk into the ground a few voxels so a tree's trunk
	// reads as planted instead of hovering. Falls back to the highest
	// passing column if dead-center is below sea level (extreme falloff
	// settings).
	if (m_foliage_grid_dim == 1) {
		constexpr int kPromotedSinkVoxels = 3;

		int cx = islandX / 2;
		int cy = islandY / 2;
		int topZ = FindTopSolidZ(m_terrain_brickmap, cx, cy);
		if (topZ < 0 || topZ < seaLevelZ) {
			// Fallback — scan all columns for the highest one above sea level.
			int bestZ = -1, bestX = cx, bestY = cy;
			for (int y = 0; y < islandY; ++y)
			for (int x = 0; x < islandX; ++x) {
				int z = FindTopSolidZ(m_terrain_brickmap, x, y);
				if (z > bestZ) { bestZ = z; bestX = x; bestY = y; }
			}
			cx = bestX; cy = bestY; topZ = bestZ;
			if (logger) logger->warn(
				"CombinedRenderer: island center below sea — promoted asset placed at "
				"highest column ({}, {}, z={})", cx, cy, topZ);
		}

		std::vector<GpuInstance> instances;
		if (topZ >= seaLevelZ) {
			// Baked .vxa files store voxels in glTF Y-up convention (the bake
			// only rotates the rendered mesh via the SceneNode transform; the
			// volume bytes are mesh-local). The foliage trace shader treats
			// the volume as Z-up, so we apply the same Y-up→Z-up correction
			// per-instance — the fragment shader already inverts vInstRot
			// when DDA-ing the camera ray back into volume-local space, so
			// this rotates the rendered asset in world without skewing the
			// volume sampling. Procedural assets are already Z-up; identity.
			const glm::quat baseRot = m_use_baked_foliage
				? glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f))
				: glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			GpuInstance gi{};
			gi.position = glm::vec3(
				static_cast<float>(cx + origin.x),
				static_cast<float>(cy + origin.y),
				static_cast<float>(topZ + 1 + origin.z - kPromotedSinkVoxels));
			gi.scale     = std::max(0.05f, m_density);
			gi.rotation  = glm::vec4(baseRot.x, baseRot.y, baseRot.z, baseRot.w);
			gi.yawIdx    = 0;
			gi.animOffset = 0.0f;
			instances.push_back(gi);
		}

		m_foliage_instance_count = static_cast<uint32_t>(instances.size());
		if (m_foliage_instance_count > 0) {
			m_pending_instance_bytes.resize(instances.size() * sizeof(GpuInstance));
			std::memcpy(m_pending_instance_bytes.data(), instances.data(),
			            m_pending_instance_bytes.size());
		} else {
			m_pending_instance_bytes.clear();
		}
		m_foliage_pitch_voxels = islandX;  // single instance — pitch is meaningless; report island size
		m_foliage_pending_upload = true;
		RebuildShadowBrickmap();
		if (logger) logger->info("CombinedRenderer: placed {} promoted foliage instance "
		                         "at island center ({}, {}, {})",
		                         m_foliage_instance_count, cx, cy, topZ);
		return;
	}

	// ---- Grid placement (default) ----
	//
	// Auto-fit the foliage grid to the island footprint. Pitch (in world
	// voxels) = floor(island_x / grid_dim). Square footprint matches today's
	// island bake (gridSize.x == gridSize.y).
	const int gridDim = std::max(1, m_foliage_grid_dim);
	const int pitch   = std::max(1, std::min(islandX, islandY) / gridDim);
	m_foliage_pitch_voxels = pitch;

	std::mt19937 rng(0xFEED1337u);
	std::uniform_int_distribution<int> yawIdxDist(0, 3);
	std::uniform_real_distribution<float> phaseDist(0.0f, static_cast<float>(m_foliage_frame_count));
	std::uniform_int_distribution<int> jitterDist(-pitch / 4, pitch / 4);

	std::vector<GpuInstance> instances;
	instances.reserve(static_cast<size_t>(gridDim) * gridDim);

	// Surface-aware placement guards (island-scene.md §5.2). Together they
	// keep instances on inland plateau-ish terrain instead of the beach band
	// or cliff edges. Thresholds are conservative defaults; they could be
	// promoted to inspector params later if the user wants to tune.
	constexpr int kInlandMarginVoxels = 4;
	constexpr int kMaxSlopeVoxels     = 6;
	const int inlandThresholdZ = static_cast<int>(
		(m_terrain_cfg.seaLevel + m_terrain_cfg.beachWidth) *
		static_cast<float>(m_terrain_brickmap.volumeSize.z))
		+ kInlandMarginVoxels;

	for (int gy = 0; gy < gridDim; ++gy)
	for (int gx = 0; gx < gridDim; ++gx) {
		// Cell-center placement plus a small jitter so the lattice doesn't
		// look uniform. Jitter stays within ±pitch/4 to keep cells distinct.
		int wantTx = (gx * islandX) / gridDim + (islandX / gridDim) / 2 + jitterDist(rng);
		int wantTy = (gy * islandY) / gridDim + (islandY / gridDim) / 2 + jitterDist(rng);
		wantTx = std::clamp(wantTx, 0, islandX - 1);
		wantTy = std::clamp(wantTy, 0, islandY - 1);

		const int topZ = FindTopSolidZ(m_terrain_brickmap, wantTx, wantTy);
		// (1) Beach exclusion — column must be firmly above the beach band.
		if (topZ < 0 || topZ < inlandThresholdZ) continue;

		// (2) Slope guard — reject cliff edges where a cubic asset would
		// visibly hover over the gradient. OOB neighbors contribute zero
		// delta so map edges aren't doubly penalized by criterion (1).
		auto neighborDelta = [&](int nx, int ny) {
			int nz = FindTopSolidZ(m_terrain_brickmap, nx, ny);
			return (nz < 0) ? 0 : std::abs(nz - topZ);
		};
		const int maxDelta = std::max({
			neighborDelta(wantTx,     wantTy + 1),
			neighborDelta(wantTx,     wantTy - 1),
			neighborDelta(wantTx + 1, wantTy),
			neighborDelta(wantTx - 1, wantTy),
		});
		if (maxDelta > kMaxSlopeVoxels) continue;

		// (3) Material check — the surface voxel must be grass. Cheap re-
		// confirmation that we're on inland terrain (after the beach color-
		// gradient lands in §6.2 this becomes load-bearing — the elevation
		// threshold alone isn't a perfect proxy for "is grass band").
		if (BrickmapVoxelMaterial(m_terrain_brickmap,
		                          glm::ivec3(wantTx, wantTy, topZ))
		    != TerrainMaterials::Grass) continue;

		// World voxel coords. Cloud is at world origin so cloud-local voxel
		// position = world voxel position.
		const glm::ivec3 worldVx(wantTx + origin.x,
		                         wantTy + origin.y,
		                         topZ + 1 + origin.z);

		GpuInstance gi{};
		gi.position = glm::vec3(static_cast<float>(worldVx.x),
		                        static_cast<float>(worldVx.y),
		                        static_cast<float>(worldVx.z));
		gi.scale    = std::max(0.05f, m_density);
		const int yawIdx = yawIdxDist(rng);
		const float yaw  = static_cast<float>(yawIdx) * 1.57079632679f;
		// Compose: Y-up→Z-up base (only for baked foliage; procedural is
		// already Z-up) followed by per-instance yaw around world Z.
		// Order matters — yaw applied AFTER the up-axis correction operates
		// in world coords, which is what we want for "rotate the upright
		// tree around its vertical axis."
		const glm::quat yawQ = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
		const glm::quat baseRot = m_use_baked_foliage
			? glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f))
			: glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		const glm::quat q = yawQ * baseRot;
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
		m_foliage_pending_upload = true;
		RebuildShadowBrickmap();         // terrain-only shadow brickmap
		return;
	}

	// Stash GpuInstance bytes for OnPostCompile to upload. Holding raw bytes
	// (not the typed vector) keeps the header from having to surface the
	// std430 layout.
	m_pending_instance_bytes.resize(instances.size() * sizeof(GpuInstance));
	std::memcpy(m_pending_instance_bytes.data(),
	            instances.data(),
	            m_pending_instance_bytes.size());
	m_foliage_pending_upload = true;

	// Rebuild the shadow brickmap. Topology = terrain bricks ∪ instance
	// max-AABB bricks; static pool = terrain bits; dynamic pool starts
	// zero (shadow_foliage_write fills it per frame).
	RebuildShadowBrickmap();

	logger->info("CombinedRenderer: placed {} foliage instances (grid {}×{} pitch {} voxels)",
		m_foliage_instance_count, gridDim, gridDim, pitch);
}

void CombinedRenderer::RebuildShadowBrickmap() {
	auto logger = spdlog::get("Render");

	// Pull instance inputs out of m_pending_instance_bytes if the foliage
	// placement just ran; otherwise produce a terrain-only brickmap.
	std::vector<ShadowBrickmap::InstanceInput> sbInputs;
	const uint32_t instCount = m_foliage_instance_count;
	if (instCount > 0 && !m_pending_instance_bytes.empty()) {
		const GpuInstance* gpuInst =
			reinterpret_cast<const GpuInstance*>(m_pending_instance_bytes.data());
		sbInputs.reserve(instCount);
		for (uint32_t i = 0; i < instCount; ++i) {
			ShadowBrickmap::InstanceInput in{};
			in.worldVoxelPos = glm::ivec3(glm::round(gpuInst[i].position));
			in.yawIdx        = static_cast<uint8_t>(gpuInst[i].yawIdx & 0x3);
			sbInputs.push_back(in);
		}
	}

	auto build = ShadowBrickmap::BuildShadowBrickmap(
		m_terrain_brickmap,
		sbInputs.data(),
		static_cast<uint32_t>(sbInputs.size()),
		m_foliage_size);

	m_pending_shadow_data     = std::move(build.data);
	m_pending_instance_bricks = std::move(build.instanceBricks);
	m_shadow_pending_upload   = true;

	const uint64_t dynamicPoolWordOffset = build.dynamicPoolBase;
	const uint64_t dynamicPoolWordCount  =
		uint64_t(build.header.brickCount) * BrickGrid::kBitmaskWordsPerBrick;
	m_shadow_dynamic_pool_offset_bytes = dynamicPoolWordOffset * sizeof(uint32_t);
	m_shadow_dynamic_pool_size_bytes   = dynamicPoolWordCount  * sizeof(uint32_t);

	if (logger) {
		logger->info(
			"CombinedRenderer: built shadow brickmap — gridDim {}×{}×{} ({} bricks), "
			"static {} words / dynamic {} words, instance-bricks {}",
			build.header.gridDim.x, build.header.gridDim.y, build.header.gridDim.z,
			build.header.brickCount,
			dynamicPoolWordCount, dynamicPoolWordCount,
			m_pending_instance_bricks.size());
	}
}

void CombinedRenderer::LoadFoliageFromVxa(const std::string& path) {
	auto logger = spdlog::get("Render");

	// Empty path → revert to procedural mode. Resize the slot back to the
	// procedural defaults so the next graph rebuild reallocates the volume
	// image at that size; clear data so UploadVolume short-circuits and the
	// foliage-gen compute pass takes over filling it.
	if (path.empty()) {
		m_foliage_vxa_path.clear();
		m_foliage_vxa_loaded_mtime = std::filesystem::file_time_type{};
		m_use_baked_foliage = false;
		m_foliage_size = glm::uvec3(16, 32, 16);
		m_foliage_frame_count = 8;
		if (m_assets && m_foliage_asset.valid()) {
			m_assets->ResizeProceduralVoxelVolume(m_foliage_asset,
				m_foliage_size, m_foliage_frame_count);
			if (auto* slot = m_assets->GetVoxelVolume(m_foliage_asset)) {
				slot->data.clear();
				slot->needsUpload = false;
			}
		}
		m_foliage_vxa_path = path;  // canonicalize empty
		// Procedural mode: re-derive grid from density so the user's last
		// density setting governs the layout instead of "1" leftover from
		// the previous baked-mode promote.
		RecomputeFoliageGrid();
		m_pending_grid_rebuild = true;
		if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		return;
	}

	// Defer the actual file work to Reload(). Mutating asset size inline
	// would race with the in-flight rebuild's already-completed
	// DeclareGraphResources; routing through Reload's pending flag puts the
	// resize in a clean "between rebuilds" slot, and Reload posts
	// RebuildGraph to make the new size visible.
	m_foliage_vxa_path = path;
	m_pending_foliage_load = true;
	if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
	if (logger) logger->info("CombinedRenderer: queued foliage .vxa load: {}", path);
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

	ubo.frameCount          = static_cast<int32_t>(m_foliage_frame_count);
	ubo.worldVoxelSize      = kWorldVoxelSize;
	ubo.maxShadowBrickSteps = m_max_shadow_brick_steps;
	const double now = GetTimeSeconds();
	ubo.time = static_cast<float>(now - m_start_time_seconds) * m_animation_speed;

	std::memcpy(m_frame_ubo_mapped[frameIndex], &ubo, sizeof(ubo));
}

std::vector<std::string> CombinedRenderer::GetShaderPaths() const {
	return {
		std::string(config::SHADER_DIR) + "/instanced_voxel_generate.comp.spv",
		std::string(config::SHADER_DIR) + "/voxel_bitmask_fill.comp.spv",
		std::string(config::SHADER_DIR) + "/shadow_foliage_write.comp.spv",
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
		m_parameters.push_back({ "Animation Speed",        TechniqueParameter::Float, &m_animation_speed,        0.0f,    30.0f });
		m_parameters.push_back({ "Max Primary Iterations", TechniqueParameter::Int,   &m_max_iterations,         1.0f,  1024.0f });
		m_parameters.push_back({ "Max Shadow Brick Steps", TechniqueParameter::Int,   &m_max_shadow_brick_steps, 1.0f,  1024.0f });
		m_parameters.push_back({ "Debug Coloring",         TechniqueParameter::Bool,  &m_debug_color });
		m_parameters.push_back({ "Enable Shadows",         TechniqueParameter::Bool,  &m_shadows_enabled });

		m_parameters.push_back({ "Foliage", TechniqueParameter::Header });

		// Foliage VXA Path — the file-based contract with
		// GltfImportTechnique's Promote-to-scene workflow. Defaults to the
		// convention cache path so first-time promote auto-loads. Empty
		// path or onFileChanged with empty input reverts to procedural.
		TechniqueParameter vxaPath;
		vxaPath.label          = "Foliage VXA Path";
		vxaPath.type           = TechniqueParameter::File;
		vxaPath.filePath       = &m_foliage_vxa_path;
		vxaPath.fileFilters    = { "vxa" };
		vxaPath.fileFilterDesc = "Animated voxel asset";
		vxaPath.onFileChanged  = [this](const std::string& p) {
			LoadFoliageFromVxa(p);
		};
		m_parameters.push_back(std::move(vxaPath));

		// Grid Size: per-axis instance count. Promote-to-scene resets this
		// to 1 (single asset at island center); user drags up to populate.
		TechniqueParameter gridDim;
		gridDim.label = "Grid Size";
		gridDim.type  = TechniqueParameter::Int;
		gridDim.data  = &m_foliage_grid_dim;
		gridDim.min   = 1.0f;
		gridDim.max   = 128.0f;
		gridDim.onChanged = [this]() {
			RecomputeFoliageGrid();
			m_pending_grid_rebuild = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(gridDim));

		// Density: per-instance render scale multiplier (gi.scale = m_density).
		// Independent of grid size — tunes how visually filled the grid feels
		// without changing the instance count.
		TechniqueParameter density;
		density.label  = "Density";
		density.type   = TechniqueParameter::Float;
		density.data   = &m_density;
		density.min    = 0.05f;
		density.max    = 4.0f;
		density.format = "%.2f";
		density.onChanged = [this]() {
			m_pending_grid_rebuild = true;
			if (m_eventSink) m_eventSink({AppEventType::ReloadTechnique});
		};
		m_parameters.push_back(std::move(density));

		// Read-only display row: derived pitch + asset footprint.
		TechniqueParameter gridStatus;
		gridStatus.label     = "Layout";
		gridStatus.type      = TechniqueParameter::Text;
		gridStatus.textValue = &m_foliage_grid_status;
		m_parameters.push_back(std::move(gridStatus));

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
	// Graphics draws: sky (1) + terrain (1 fullscreen) + foliage (1 instanced
	// cube). Compute dispatches happen too but aren't counted in FrameStats.
	const uint32_t cubeVerts = 36 * m_foliage_instance_count;
	return { 3, 4 + 4 + cubeVerts, 0 };
}
