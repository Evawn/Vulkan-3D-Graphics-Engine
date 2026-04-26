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
	// Procedural unit cube via gl_VertexIndex — no vertex/index buffers. The
	// vertex shader reads its position from a hardcoded 36-vertex cube table
	// (two triangles per face, 6 faces) and scales by the per-instance AABB
	// from instance SSBO. The fragment shader DDAs into voxelAsset using the
	// per-instance frame offset. firstInstance offsets into the instance SSBO
	// so multiple clouds can share one buffer.
	(void)graph;
	ctx.cmd->CmdDraw(36, item.instanceCount, 0, item.firstInstance);
}
