#include "SceneExtractor.h"
#include "Scene.h"
#include "AssetRegistry.h"
#include "RenderScene.h"
#include "RenderItem.h"

void SceneExtractor::Extract(const Scene& scene, const AssetRegistry& assets, RenderScene& out) const {
	Visit(scene.GetRoot(), glm::mat4(1.0f), assets, out);
}

void SceneExtractor::Visit(const SceneNode& node, const glm::mat4& parentWorld,
                           const AssetRegistry& assets, RenderScene& out) const {
	if (!node.visible) return;

	const glm::mat4 world = parentWorld * node.LocalTransform();
	node.cachedWorld = world;
	node.worldDirty  = false;

	for (const auto& comp : node.components) {
		switch (comp.type) {
			case ComponentType::Mesh: {
				const auto* mesh = assets.GetMesh(comp.asset);
				if (!mesh || mesh->indices.empty()) break;
				if (mesh->vertexBuffer.id == UINT32_MAX) break;  // not yet declared
				RenderItem item{};
				item.type          = RenderItemType::Mesh;
				item.vertexBuffer  = mesh->vertexBuffer;
				item.indexBuffer   = mesh->indexBuffer;
				item.indexCount    = static_cast<uint32_t>(mesh->indices.size());
				item.firstIndex    = 0;
				item.vertexOffset  = 0;
				item.instanceCount = 1;
				item.firstInstance = 0;
				item.transform     = world;
				item.aabbMin       = mesh->aabbMin;
				item.aabbMax       = mesh->aabbMax;
				out.Add(item);
				break;
			}
			case ComponentType::VoxelVolume: {
				const auto* vol = assets.GetVoxelVolume(comp.asset);
				if (!vol) break;
				if (vol->volumeImage.id == UINT32_MAX) break;
				RenderItem item{};
				item.type          = RenderItemType::BrickmapVolume;
				item.voxelAsset    = vol->volumeImage;
				item.frameCount    = comp.frameCount;
				item.instanceCount = 1;
				item.firstInstance = 0;
				item.transform     = world;
				item.aabbMin       = glm::vec3(0.0f);
				item.aabbMax       = glm::vec3(static_cast<float>(vol->size.x),
				                               static_cast<float>(vol->size.y),
				                               static_cast<float>(vol->size.z));
				out.Add(item);
				break;
			}
			case ComponentType::InstanceCloud: {
				const auto* vol = assets.GetVoxelVolume(comp.asset);
				if (!vol || vol->volumeImage.id == UINT32_MAX) break;
				if (comp.instanceBuffer.id == UINT32_MAX) break;
				if (comp.instanceCount == 0) break;
				RenderItem item{};
				item.type           = RenderItemType::InstancedVoxelMesh;
				item.voxelAsset     = vol->volumeImage;
				item.frameCount     = vol->frameCount;
				item.instanceBuffer = comp.instanceBuffer;
				item.instanceCount  = comp.instanceCount;
				item.firstInstance  = 0;
				item.transform      = world;
				item.aabbMin        = comp.instanceAabbMin;
				item.aabbMax        = comp.instanceAabbMax;
				out.Add(item);
				break;
			}
		}
	}

	for (const auto& child : node.children) {
		Visit(*child, world, assets, out);
	}
}
