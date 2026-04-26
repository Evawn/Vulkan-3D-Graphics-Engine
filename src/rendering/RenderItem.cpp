#include "RenderItem.h"
#include "RenderGraph.h"
#include <cassert>

void DrawMeshItem(const PassContext& ctx, const RenderItem& item, const RenderGraph& graph) {
	if (item.indexCount == 0) return;
	auto vk_cmd = ctx.cmd->Get();

	VkBuffer vbufs[] = { graph.GetVkBuffer(item.vertexBuffer) };
	VkDeviceSize offsets[] = { 0 };
	vkCmdBindVertexBuffers(vk_cmd, 0, 1, vbufs, offsets);
	vkCmdBindIndexBuffer(vk_cmd, graph.GetVkBuffer(item.indexBuffer), 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(vk_cmd,
		item.indexCount, item.instanceCount,
		item.firstIndex, item.vertexOffset, item.firstInstance);
}

void DrawFullscreenItem(const PassContext& ctx, const RenderItem& item) {
	// FSQ contract: the pipeline uses post_fullscreen.vert.spv, which generates
	// a 4-vertex strip from gl_VertexIndex. No vertex/index buffers needed.
	ctx.cmd->CmdDraw(4, item.instanceCount, 0, item.firstInstance);
}

void DrawInstancedVoxelMesh(const PassContext& ctx, const RenderItem& item, const RenderGraph& graph) {
	// Reserved — the instanced animated voxel renderer (foliage) hasn't been
	// built yet. The intended shape: a unit cube (provided as item.vertexBuffer/
	// item.indexBuffer) is rasterized once per instance with item.transform
	// scaled to (aabbMax - aabbMin), and the fragment shader DDA-traverses
	// inward into voxelAsset using the per-instance SSBO entry pointed at by
	// firstInstance + gl_InstanceIndex.
	(void)ctx; (void)item; (void)graph;
	assert(false && "DrawInstancedVoxelMesh: not implemented yet — reserved for foliage renderer");
}
