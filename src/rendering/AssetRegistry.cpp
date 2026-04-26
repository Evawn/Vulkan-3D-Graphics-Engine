#include "AssetRegistry.h"
#include "RenderGraph.h"
#include "CommandBuffer.h"
#include "Buffer.h"

#include <spdlog/spdlog.h>
#include <cstring>

// ---- Mesh API ----

AssetID AssetRegistry::RegisterMesh(std::string name,
                                    std::vector<VWrap::Vertex> vertices,
                                    std::vector<uint32_t>      indices,
                                    glm::vec3 aabbMin,
                                    glm::vec3 aabbMax,
                                    std::string sourcePath)
{
	MeshAsset m{};
	m.name        = std::move(name);
	m.sourcePath  = std::move(sourcePath);
	m.vertices    = std::move(vertices);
	m.indices     = std::move(indices);
	m.aabbMin     = aabbMin;
	m.aabbMax     = aabbMax;
	m.needsUpload = !m.vertices.empty();
	m_meshes.push_back(std::move(m));
	// If we're inside a graph build, declare the resource immediately so the
	// caller (typically a technique inside RegisterPasses) can use the handle
	// straight away.
	if (m_currentGraph) DeclareMesh(m_meshes.back(), *m_currentGraph);
	return AssetID{ static_cast<uint32_t>(m_meshes.size() - 1), AssetID::Type::Mesh };
}

bool AssetRegistry::ReplaceMesh(AssetID id,
                                std::vector<VWrap::Vertex> vertices,
                                std::vector<uint32_t>      indices,
                                glm::vec3 aabbMin,
                                glm::vec3 aabbMax,
                                std::string sourcePath)
{
	auto* m = GetMesh(id);
	if (!m) return false;
	const size_t oldVCount = m->vertices.size();
	const size_t oldICount = m->indices.size();

	m->vertices    = std::move(vertices);
	m->indices     = std::move(indices);
	m->aabbMin     = aabbMin;
	m->aabbMax     = aabbMax;
	m->needsUpload = !m->vertices.empty();
	if (!sourcePath.empty()) m->sourcePath = std::move(sourcePath);

	const bool sizeChanged = (m->vertices.size() != oldVCount) || (m->indices.size() != oldICount);
	return sizeChanged;
}

const MeshAsset* AssetRegistry::GetMesh(AssetID id) const {
	if (id.type != AssetID::Type::Mesh || id.id >= m_meshes.size()) return nullptr;
	return &m_meshes[id.id];
}

MeshAsset* AssetRegistry::GetMesh(AssetID id) {
	if (id.type != AssetID::Type::Mesh || id.id >= m_meshes.size()) return nullptr;
	return &m_meshes[id.id];
}

// ---- Voxel volume API ----

AssetID AssetRegistry::RegisterVoxelVolume(std::string name, VoxModel model, std::string sourcePath) {
	VoxelVolumeAsset v{};
	v.name         = std::move(name);
	v.sourcePath   = std::move(sourcePath);
	v.isProcedural = false;
	v.size         = model.volumeSize;
	v.format       = VK_FORMAT_R8_UINT;
	v.data         = std::move(model.volume);
	v.palette      = model.palette;
	v.needsUpload  = !v.data.empty();
	m_volumes.push_back(std::move(v));
	if (m_currentGraph) DeclareVolume(m_volumes.back(), *m_currentGraph);
	return AssetID{ static_cast<uint32_t>(m_volumes.size() - 1), AssetID::Type::VoxelVolume };
}

bool AssetRegistry::ReplaceVoxelVolume(AssetID id, VoxModel model, std::string sourcePath) {
	auto* v = GetVoxelVolume(id);
	if (!v) return false;
	const glm::uvec3 oldSize = v->size;

	v->isProcedural = false;
	v->size         = model.volumeSize;
	v->format       = VK_FORMAT_R8_UINT;
	v->data         = std::move(model.volume);
	v->palette      = model.palette;
	v->needsUpload  = !v->data.empty();
	if (!sourcePath.empty()) v->sourcePath = std::move(sourcePath);

	return v->size != oldSize;
}

AssetID AssetRegistry::CreateProceduralVoxelVolume(std::string name, glm::uvec3 size,
                                                   VkFormat format, VkImageUsageFlags extraUsage)
{
	(void)extraUsage;  // reserved — current ImageDesc auto-derives storage usage from compute writers
	VoxelVolumeAsset v{};
	v.name         = std::move(name);
	v.isProcedural = true;
	v.size         = size;
	v.format       = format;
	v.needsUpload  = false;
	m_volumes.push_back(std::move(v));
	if (m_currentGraph) DeclareVolume(m_volumes.back(), *m_currentGraph);
	return AssetID{ static_cast<uint32_t>(m_volumes.size() - 1), AssetID::Type::VoxelVolume };
}

bool AssetRegistry::ResizeProceduralVoxelVolume(AssetID id, glm::uvec3 newSize) {
	auto* v = GetVoxelVolume(id);
	if (!v || !v->isProcedural) return false;
	if (v->size == newSize) return false;
	v->size = newSize;
	return true;
}

const VoxelVolumeAsset* AssetRegistry::GetVoxelVolume(AssetID id) const {
	if (id.type != AssetID::Type::VoxelVolume || id.id >= m_volumes.size()) return nullptr;
	return &m_volumes[id.id];
}

VoxelVolumeAsset* AssetRegistry::GetVoxelVolume(AssetID id) {
	if (id.type != AssetID::Type::VoxelVolume || id.id >= m_volumes.size()) return nullptr;
	return &m_volumes[id.id];
}

// ---- Lifecycle ----

void AssetRegistry::DeclareGraphResources(RenderGraph& graph) {
	// Declare all currently-known assets, then stamp the graph as "current" so
	// any new assets created mid-RegisterPasses (e.g. a technique calling
	// CreateProceduralVoxelVolume from within RegisterPasses) get declared on
	// the spot. UploadPending clears the pointer at the end of the build.
	for (auto& m : m_meshes) DeclareMesh(m, graph);
	for (auto& v : m_volumes) DeclareVolume(v, graph);
	m_currentGraph = &graph;
}

void AssetRegistry::DeclareMesh(MeshAsset& m, RenderGraph& graph) {
	if (m.vertices.empty()) {
		// Unloaded — leave handles invalid; the next ReplaceMesh + rebuild fills in.
		m.vertexBuffer = {};
		m.indexBuffer  = {};
		return;
	}

	BufferDesc vb{};
	vb.size     = sizeof(VWrap::Vertex) * m.vertices.size();
	vb.usage    = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	vb.lifetime = Lifetime::Persistent;
	m.vertexBuffer = graph.CreateBuffer(m.name + "_vertices", vb);

	BufferDesc ib{};
	ib.size     = sizeof(uint32_t) * m.indices.size();
	ib.usage    = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	ib.lifetime = Lifetime::Persistent;
	m.indexBuffer  = graph.CreateBuffer(m.name + "_indices", ib);
}

void AssetRegistry::DeclareVolume(VoxelVolumeAsset& v, RenderGraph& graph) {
	if (v.size.x == 0 || v.size.y == 0 || v.size.z == 0) {
		v.volumeImage = {};
		return;
	}

	ImageDesc d{};
	d.width      = v.size.x;
	d.height     = v.size.y;
	d.depth      = v.size.z;
	d.format     = v.format;
	d.samples    = VK_SAMPLE_COUNT_1_BIT;
	d.imageType  = VK_IMAGE_TYPE_3D;
	// File-loaded volumes need TRANSFER_DST for the host upload. Procedural
	// volumes don't strictly need it — the graph derives STORAGE from a
	// compute writer — but we set it uniformly so a procedural volume can also
	// be host-clobbered (e.g. by a future "save asset to disk" capture path).
	d.extraUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	d.lifetime   = Lifetime::Persistent;
	v.volumeImage = graph.CreateImage(v.name + "_volume", d);
}

void AssetRegistry::UploadPending(RenderGraph& graph, std::shared_ptr<VWrap::CommandPool> pool) {
	for (auto& m : m_meshes) UploadMesh(m, graph, pool);
	for (auto& v : m_volumes) UploadVolume(v, graph, pool);
	m_currentGraph = nullptr;
}

void AssetRegistry::UploadMesh(MeshAsset& m, RenderGraph& graph, std::shared_ptr<VWrap::CommandPool> pool) {
	if (!m.needsUpload || m.vertices.empty()) return;
	const VkDeviceSize vbSize = sizeof(VWrap::Vertex) * m.vertices.size();
	const VkDeviceSize ibSize = sizeof(uint32_t)     * m.indices.size();
	graph.UploadBufferData(m.vertexBuffer, m.vertices.data(), vbSize, pool);
	graph.UploadBufferData(m.indexBuffer,  m.indices.data(),  ibSize, pool);
	m.needsUpload = false;
}

void AssetRegistry::UploadVolume(VoxelVolumeAsset& v, RenderGraph& graph,
                                 std::shared_ptr<VWrap::CommandPool> pool) {
	if (!v.needsUpload || v.isProcedural || v.data.empty()) return;
	if (v.volumeImage.id == UINT32_MAX) return;

	// Mirror BrickmapPaletteRenderer::UploadVolumeData — staging buffer →
	// CmdCopyBufferToImage → leave the image in GENERAL so subsequent compute
	// passes can read it without re-transitioning. The graph's first-use
	// barrier still issues a layout transition if a different layout is needed.
	const VkDeviceSize size = static_cast<VkDeviceSize>(v.size.x) * v.size.y * v.size.z;
	auto allocator = graph.GetImage(v.volumeImage)->GetAllocator();
	auto staging   = VWrap::Buffer::CreateStaging(allocator, size);
	void* mapped   = staging->Map();
	std::memcpy(mapped, v.data.data(), size);
	staging->Unmap();

	auto image = graph.GetImage(v.volumeImage);
	auto cmd = VWrap::CommandBuffer::Create(pool);
	cmd->BeginSingle();
	cmd->CmdTransitionImageLayout(image, v.format,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	cmd->CmdCopyBufferToImage(staging, image, v.size.x, v.size.y, v.size.z);
	cmd->CmdTransitionImageLayout(image, v.format,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	cmd->EndAndSubmit();

	v.needsUpload = false;
}
