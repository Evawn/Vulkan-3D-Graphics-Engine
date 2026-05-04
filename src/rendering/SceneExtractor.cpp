#include "SceneExtractor.h"
#include "Scene.h"
#include "AssetRegistry.h"
#include "SkinnedMeshAsset.h"
#include "RenderScene.h"
#include "RenderItem.h"
#include "AnimationEvaluator.h"

#include <algorithm>

void SceneExtractor::Extract(const Scene& scene, const AssetRegistry& assets, RenderScene& out, float dt) {
	// Const-cast: the public Scene::GetRoot() returns const&, but the extractor
	// needs to mutate Component::currentTime for skinned-mesh playback. The
	// scene tree itself is not modified; only the playback clock on each
	// SkinnedMesh component advances. This is a deliberate seam — a future
	// refactor that splits "render data" from "logic data" would land the
	// playback clock somewhere else.
	auto& root = const_cast<SceneNode&>(scene.GetRoot());
	Visit(root, glm::mat4(1.0f), assets, out, dt);
}

void SceneExtractor::Visit(SceneNode& node, const glm::mat4& parentWorld,
                           const AssetRegistry& assets, RenderScene& out, float dt) {
	if (!node.visible) return;

	const glm::mat4 world = parentWorld * node.LocalTransform();
	node.cachedWorld = world;
	node.worldDirty  = false;

	for (auto& comp : node.components) {
		switch (comp.type) {
			case ComponentType::Mesh: {
				const auto* mesh = assets.GetMesh(comp.asset);
				if (!mesh || mesh->indices.empty()) break;
				if (mesh->vertexBuffer.id == UINT32_MAX) break;
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
			case ComponentType::SkinnedMesh: {
				EmitSkinnedMesh(comp, world, assets, out, dt);
				break;
			}
		}
	}

	for (auto& child : node.children) {
		Visit(*child, world, assets, out, dt);
	}
}

void SceneExtractor::EmitSkinnedMesh(Component& comp, const glm::mat4& world,
                                     const AssetRegistry& assets, RenderScene& out, float dt) {
	const auto* mesh = assets.GetSkinnedMesh(comp.asset);
	if (!mesh || mesh->primitives.empty()) return;

	// Validate the skin index — out-of-range falls back to skin 0 so the asset
	// still draws (the draw will be in rest pose if the chosen skin is
	// missing).
	int skinIdx = comp.skinIndex;
	if (skinIdx < 0 || static_cast<size_t>(skinIdx) >= mesh->skins.size()) {
		skinIdx = 0;
	}

	// Advance playback time. Wrap modulo clip duration so looping is the
	// default; the panel can pause via comp.paused or scrub by writing
	// currentTime directly.
	const auto* clip = assets.GetAnimationClip(comp.clipAsset);
	if (clip && !comp.paused && clip->duration > 0.0f) {
		comp.currentTime += dt * comp.playbackSpeed;
		// fmod, but always-positive — currentTime can drift slightly negative
		// during fast scrubbing.
		float t = std::fmod(comp.currentTime, clip->duration);
		if (t < 0.0f) t += clip->duration;
		comp.currentTime = t;
	}

	// Pose the skeleton on flat parallel TRS arrays — see
	// SkinnedMeshAsset::restTranslation/Rotation/Scale. memcpy from the
	// asset's cached rest pose into our scratch arrays, then let
	// EvaluateClipFlat overwrite the channels the clip touches. This avoids
	// deep-copying the Node[] (each Node carries std::string + child vector,
	// so deep-copying ~3k of them per frame thrashes the heap).
	m_scratchTrans = mesh->restTranslation;
	m_scratchRot   = mesh->restRotation;
	m_scratchScale = mesh->restScale;

	if (clip) {
		// Build a tiny proxy Animation on the stack — same shape as a
		// gltf_import::Animation but borrowing the channel vector. The copy
		// of `channels` is unavoidable today because EvaluateClipFlat takes
		// an Animation by const&; future cleanup: have it take a span.
		gltf_import::Animation proxy;
		proxy.duration = clip->duration;
		proxy.channels = clip->channels;
		proxy.name     = clip->name;
		gltf_import::EvaluateClipFlat(proxy, comp.currentTime,
			m_scratchTrans, m_scratchRot, m_scratchScale);
	}

	// Pass the asset's prebuilt active-node mask so the BFS prunes subtrees
	// whose world matrices we never read (skins we don't render). nullptr
	// when the mask is empty (e.g. asset without skins) — full walk fallback.
	const std::vector<bool>* maskPtr = mesh->activeNodeMask.empty() ? nullptr : &mesh->activeNodeMask;
	gltf_import::ComputeWorldMatricesFlat(mesh->nodes,
		m_scratchTrans, m_scratchRot, m_scratchScale, m_scratchWorlds, maskPtr);

	const gltf_import::Skin& skin = mesh->skins[skinIdx];

	// Mesh-node world: each primitive's owner node, sampled from the *posed*
	// skeleton. Different primitives may live under different nodes; for now
	// all primitives we emit share the same skin and we use the first
	// primitive's owner node to define the mesh frame. (Multi-skin assets
	// would emit per-primitive components; v1 imports register one component
	// per skin via the import technique.)
	int meshNodeIdx = mesh->primitives.front().ownerNodeIndex;
	const glm::mat4 meshNodeWorld =
		(meshNodeIdx >= 0 && static_cast<size_t>(meshNodeIdx) < m_scratchWorlds.size())
		? m_scratchWorlds[meshNodeIdx]
		: glm::mat4(1.0f);

	gltf_import::ComputeJointMatrices(skin, m_scratchWorlds, meshNodeWorld, m_scratchJoints);

	// Push joint matrices into the per-frame arena. Every primitive emitted
	// below references the same range.
	const uint32_t firstJoint = out.AddJointMatrices(
		m_scratchJoints.data(),
		static_cast<uint32_t>(m_scratchJoints.size()));
	const uint32_t jointCount = static_cast<uint32_t>(m_scratchJoints.size());

	for (const auto& prim : mesh->primitives) {
		// Skip primitives that don't bind to the active skin. v1 always emits
		// all primitives whose skinIndex matches the component (handles the
		// "both bark and foliage rigged to skin 0" case).
		if (prim.skinIndex != skinIdx) continue;
		if (prim.vertexBuffer.id == UINT32_MAX) continue;
		RenderItem item{};
		item.type            = RenderItemType::SkinnedMesh;
		item.vertexBuffer    = prim.vertexBuffer;
		item.indexBuffer     = prim.indexBuffer;
		item.indexCount      = static_cast<uint32_t>(prim.indices.size());
		item.firstIndex      = 0;
		item.vertexOffset    = 0;
		item.instanceCount   = 1;
		item.firstInstance   = 0;
		item.transform       = world;
		item.firstJoint      = firstJoint;
		item.jointCount      = jointCount;
		item.baseColorFactor = prim.baseColorFactor;
		// Material binding (set 1) + alpha state — borrowed pointers /
		// per-frame descriptor set lives inside the BindingTable. The asset
		// outlives the RenderItem (the item is per-frame ephemeral; the
		// asset persists across frames), so the raw pointer is safe within
		// the frame it's emitted in.
		item.materialBindings = prim.materialBindings.get();
		item.alphaCutoff      = prim.alphaCutoff;
		item.alphaMode        = static_cast<uint8_t>(prim.alphaMode);
		item.doubleSided      = prim.doubleSided;
		out.Add(item);
	}
}
