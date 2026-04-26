#pragma once

#include "RenderGraphTypes.h"
#include "VoxLoader.h"
#include "Utils.h"          // VWrap::Vertex
#include "CommandPool.h"

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class RenderGraph;

// ---- AssetID ----
//
// Stable, content-agnostic handle to an asset stored in the registry. The id
// indexes into a per-type vector inside the registry; the type tag selects
// which vector. Default-constructed = invalid (the registry returns one of
// these on lookup failure).

struct AssetID {
	enum class Type : uint8_t { Invalid, Mesh, VoxelVolume };
	uint32_t id   = UINT32_MAX;
	Type     type = Type::Invalid;

	bool valid() const { return id != UINT32_MAX && type != Type::Invalid; }
	bool operator==(const AssetID& o) const { return id == o.id && type == o.type; }
};

// ---- MeshAsset ----
//
// CPU-side mesh data + the graph-managed buffers it uploads into. Owned by
// AssetRegistry; not copied. Techniques reference by AssetID and look up the
// live BufferHandles on each graph rebuild via the registry.

struct MeshAsset {
	std::string name;
	std::string sourcePath;            // empty if procedural / programmatic

	std::vector<VWrap::Vertex> vertices;
	std::vector<uint32_t>      indices;
	glm::vec3 aabbMin = glm::vec3(0.0f);
	glm::vec3 aabbMax = glm::vec3(0.0f);

	// Re-allocated by AssetRegistry::DeclareGraphResources on every graph
	// rebuild. Stale outside the current build.
	BufferHandle vertexBuffer;
	BufferHandle indexBuffer;

	// True after data is replaced or first registered, false after upload runs.
	bool needsUpload = true;
};

// ---- VoxelVolumeAsset ----
//
// 3D R8_UINT volume + palette. Two flavours: file-loaded (data populated from
// .vox; needsUpload=true) and procedural (data empty; needsUpload=false; an
// engine compute pass writes the volume each frame). Both flavours allocate
// the same kind of graph image — what differs is whether the registry copies
// host data into it.

struct VoxelVolumeAsset {
	std::string name;
	std::string sourcePath;

	bool        isProcedural = false;
	glm::uvec3  size = glm::uvec3(0);
	VkFormat    format = VK_FORMAT_R8_UINT;

	// Number of animation frames packed into the volume image. Frames are
	// stored as Z-slabs of a single 3D image: voxel `(x, y, z)` of frame `f`
	// lives at `(x, y, z + f * size.z)`. The image's full Z-extent is therefore
	// `size.z * frameCount`. `frameCount == 1` is a static volume — the v1 path
	// for .vox files and the existing brickmap-palette / animated-geometry
	// procedural volumes. The InstancedVoxelTechnique reads frameCount per
	// per-instance offset into time → frame index in the shader.
	uint32_t    frameCount = 1;

	// File-backed only — empty for procedural.
	std::vector<uint8_t>           data;
	std::array<uint8_t, 256 * 4>   palette{};

	// Re-allocated on every graph rebuild.
	ImageHandle volumeImage;

	// True for file-backed assets after Replace / first Register; cleared by
	// UploadPending. Procedural assets stay false (compute pass writes them).
	bool needsUpload = false;
};

// ---- AssetRegistry ----
//
// Engine-owned, scene-scope storage for mesh + voxel-volume assets. The point
// is to centralize "the data" so techniques are pure consumers (referencing
// assets by AssetID) and so the future scene graph + foliage workflow have one
// place to read/publish assets.
//
// Lifecycle (driven by RenderingSystem during graph rebuild):
//
//   1. Registry::DeclareGraphResources(graph)   ← before technique RegisterPasses
//   2. technique->RegisterPasses(...)            ← can call registry.GetMesh / GetVoxelVolume
//      and use the (now-allocated) handles in BindingTables / RenderItem fields
//   3. graph.Compile()                           ← VkBuffer / VkImage allocated
//   4. Registry::UploadPending(graph, pool)      ← copies host data into devices resources
//   5. technique->OnPostCompile(graph)           ← any technique-specific seeding
//
// Persistence: all registry-owned graph resources are Lifetime::Persistent so
// they survive viewport-size changes. They're only re-allocated when the graph
// is fully rebuilt (technique switch, geometry size change, etc.).

class AssetRegistry {
public:
	// ---- Mesh API ----
	// Register a freshly-parsed mesh. Returns a stable AssetID. The next graph
	// rebuild will allocate persistent buffers; the upload runs after Compile.
	AssetID RegisterMesh(std::string name,
	                     std::vector<VWrap::Vertex> vertices,
	                     std::vector<uint32_t>      indices,
	                     glm::vec3 aabbMin = glm::vec3(0.0f),
	                     glm::vec3 aabbMax = glm::vec3(0.0f),
	                     std::string sourcePath = "");

	// Replace the data backing an existing mesh asset (e.g. user picks a new
	// OBJ). Returns true if the new geometry size differs from the previous
	// one — caller should request a graph rebuild so persistent buffers
	// re-allocate at the new size. Returns false for same-size in-place
	// replacement; in that case the next UploadPending push the new data.
	bool ReplaceMesh(AssetID id,
	                 std::vector<VWrap::Vertex> vertices,
	                 std::vector<uint32_t>      indices,
	                 glm::vec3 aabbMin = glm::vec3(0.0f),
	                 glm::vec3 aabbMax = glm::vec3(0.0f),
	                 std::string sourcePath = "");

	const MeshAsset* GetMesh(AssetID id) const;
	MeshAsset*       GetMesh(AssetID id);

	// ---- Voxel volume API ----
	// File-loaded volume (data + palette already parsed by VoxLoader).
	AssetID RegisterVoxelVolume(std::string name, VoxModel model, std::string sourcePath = "");
	// Replace existing volume's data. Returns true if size changed (graph rebuild needed).
	bool    ReplaceVoxelVolume(AssetID id, VoxModel model, std::string sourcePath = "");

	// Procedural volume — registry allocates the graph image; no host upload.
	// A compute pass writes into it. Use this for animated-geometry's volume,
	// for foliage editor outputs, etc.
	AssetID CreateProceduralVoxelVolume(std::string name, glm::uvec3 size,
	                                    VkFormat format = VK_FORMAT_R8_UINT,
	                                    VkImageUsageFlags extraUsage = 0);

	// Procedural animated volume — `frameCount` Z-slabs of `size`. Image is
	// allocated as a 3D image of dimensions (size.x, size.y, size.z * frameCount).
	// Shaders address frame `f` by sampling at z' = z + f * size.z. Used by the
	// foliage workflow (compute writes each frame into its slab) and by the
	// InstancedVoxelTechnique (per-instance shader picks a frame).
	AssetID CreateProceduralAnimatedVoxelVolume(std::string name, glm::uvec3 size,
	                                            uint32_t frameCount,
	                                            VkFormat format = VK_FORMAT_R8_UINT,
	                                            VkImageUsageFlags extraUsage = 0);

	// File-backed animated volume — `frameCount * size.x * size.y * size.z`
	// bytes of host data, frames packed sequentially. Reuses the .vox palette.
	// Useful for hand-authored animated assets exported from MagicaVoxel before
	// the foliage editor lands.
	AssetID RegisterAnimatedVoxelAsset(std::string name, glm::uvec3 size,
	                                   uint32_t frameCount,
	                                   std::vector<uint8_t> framesData,
	                                   std::array<uint8_t, 256 * 4> palette,
	                                   std::string sourcePath = "");
	// Resize an existing procedural volume. Returns true if size actually
	// changed (caller should request a graph rebuild). Format stays as-is.
	bool    ResizeProceduralVoxelVolume(AssetID id, glm::uvec3 newSize);

	const VoxelVolumeAsset* GetVoxelVolume(AssetID id) const;
	VoxelVolumeAsset*       GetVoxelVolume(AssetID id);

	// ---- Lifecycle hooks (called by RenderingSystem) ----
	// Re-declare every owned asset's persistent graph resources. Must run
	// before techniques' RegisterPasses so handles are valid when techniques
	// look them up. Also stamps the graph as "current" so any new asset
	// created *during* RegisterPasses is declared immediately (otherwise
	// late-arriving assets would miss the declaration window).
	void DeclareGraphResources(RenderGraph& graph);

	// Push pending host-side data into the freshly-allocated device resources.
	// Must run after graph.Compile() and before any pass uses the data.
	// Procedural assets are skipped (their data is produced by compute passes).
	// Also clears the "current graph" pointer set by DeclareGraphResources.
	void UploadPending(RenderGraph& graph, std::shared_ptr<VWrap::CommandPool> pool);

	// ---- Introspection ----
	size_t MeshCount()        const { return m_meshes.size(); }
	size_t VoxelVolumeCount() const { return m_volumes.size(); }

private:
	std::vector<MeshAsset>        m_meshes;
	std::vector<VoxelVolumeAsset> m_volumes;

	// Set by DeclareGraphResources, cleared by UploadPending. While non-null
	// any new asset registered via the public Register*/Create* APIs has its
	// graph resource declared on the spot — letting techniques create assets
	// inside RegisterPasses and immediately use the resulting handle.
	RenderGraph* m_currentGraph = nullptr;

	void DeclareMesh(MeshAsset& m, RenderGraph& graph);
	void DeclareVolume(VoxelVolumeAsset& v, RenderGraph& graph);
	void UploadMesh(MeshAsset& m, RenderGraph& graph, std::shared_ptr<VWrap::CommandPool> pool);
	void UploadVolume(VoxelVolumeAsset& v, RenderGraph& graph, std::shared_ptr<VWrap::CommandPool> pool);
};
