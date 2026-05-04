#pragma once

#include "RenderGraphTypes.h"
#include "MeshIR.h"
#include "Image.h"
#include "ImageView.h"

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class BindingTable;

// ---- SkinnedMeshAsset ----
//
// Engine-side counterpart of gltf_import::Primitive + Skin + Node — what the
// AssetRegistry stores once an imported .glb has been promoted from the IR.
// Distinct from MeshAsset (which is OBJ-shaped, no skinning) so the two
// flavours can keep their own vertex layouts and lifecycle.
//
// One SkinnedMeshAsset = one source .glb = N primitives + a skeleton snapshot.
// Each primitive may bind to a different skin (.glb supports multi-skin
// assets like the AnimatedOak's three trees), but the v1 SceneExtractor
// drives all primitives under one Component::SkinnedMesh.
//
// Vertex layout is fixed: position + normal + uv + joints4 (u32) + weights4.
// Re-uses gltf_import::SkinnedVertex so importer-side code can move vectors
// in without translating layouts.

struct SkinnedMeshAsset {
    std::string name;
    std::string sourcePath;

    // ---- GpuTexture ----
    //
    // Decoded texel data uploaded to a GPU image, with its accompanying
    // image view. One per `gltf_import::Texture` in the source IR plus a
    // 1×1 white fallback at the END of the vector (so primitives with no
    // texture point at a known index without special-casing the shader).
    //
    // Format is `VK_FORMAT_R8G8B8A8_SRGB` — sampling automatically
    // linearizes per the Vulkan spec, matching the glTF base-color
    // convention. Single mip level for v1 (mipmap generation is future
    // work).
    //
    // Lifetime: created at registry-time (UploadSkinnedMesh's texture
    // pass), persists across graph rebuilds (textures are static GPU
    // resources, unlike the per-primitive vertex/index buffers which live
    // inside the graph and need to be re-declared on each rebuild).
    struct GpuTexture {
        std::shared_ptr<VWrap::Image>     image;
        std::shared_ptr<VWrap::ImageView> view;
        uint32_t width  = 0;
        uint32_t height = 0;
    };
    std::vector<GpuTexture> textures;
    int whiteFallbackIndex = -1;          // index into textures[]; -1 until upload runs

    // ---- Per-primitive draw unit ----
    struct Primitive {
        std::vector<gltf_import::SkinnedVertex> vertices;
        std::vector<uint32_t>                   indices;

        // RAW glTF base-color factor (M6 runtime-texture path: no longer
        // pre-multiplied by the texture's averaged tint — the shader does
        // `sample × factor` per pixel). For untextured primitives the
        // shader still does `sample × factor`, but `sample` is the 1×1
        // white fallback, so factor passes through as the flat color.
        glm::vec4 baseColorFactor = glm::vec4(1.0f);

        // Index into the parent asset's textures[]. -1 → use whiteFallbackIndex
        // (or, equivalently, the runtime can substitute the fallback at bind
        // time). The bake worker still consults `m_meshIR->textures[]`
        // directly; this field is for the runtime mesh path only.
        int baseColorTextureIndex = -1;

        // glTF alpha-mode metadata. Mask + Blend honor `alphaCutoff` (the
        // shader does `if (sample.a < cutoff) discard`); Opaque skips the
        // test entirely. doubleSided selects the no-cull pipeline at draw
        // time — almost universally true for foliage cards.
        gltf_import::Material::AlphaMode alphaMode = gltf_import::Material::AlphaMode::Opaque;
        float alphaCutoff   = 0.5f;
        bool  doubleSided   = false;

        // Per-primitive descriptor table for set 1 (the material set: a
        // combined image+sampler binding for the base-color texture).
        // Built at registry-upload time, stable across graph rebuilds, and
        // borrowed by the SceneExtractor when emitting RenderItems. The
        // pointer flow is:
        //   asset.primitives[i].materialBindings  ←  registry uploads textures + builds this
        //                          ↓
        //   RenderItem::materialBindings         ←  SceneExtractor stamps the raw pointer
        //                          ↓
        //   technique draw         binds GetSet(frameIdx) to slot 1
        std::shared_ptr<BindingTable> materialBindings;

        // Which skin in the parent asset drives this primitive (-1 = static).
        // Today the SceneExtractor uses Component::skinIndex for the whole
        // asset; this field is kept for future per-primitive multi-skin.
        int skinIndex = -1;

        // The IR node this primitive's mesh hung off — needed when computing
        // the skin's mesh-local frame in the AnimationEvaluator.
        int ownerNodeIndex = -1;

        // Re-allocated by AssetRegistry::DeclareGraphResources on every graph
        // rebuild. Stale outside the current build.
        BufferHandle vertexBuffer;
        BufferHandle indexBuffer;
    };

    std::vector<Primitive> primitives;

    // Skeleton snapshot — full IR node hierarchy + skin metadata. The
    // AnimationEvaluator walks a *copy* of `nodes` each frame (TRS overrides),
    // so this stays the canonical rest pose. Held purely as CPU data; no GPU
    // resources.
    std::vector<gltf_import::Node> nodes;
    std::vector<gltf_import::Skin> skins;

    // Flat rest-pose TRS arrays, parallel to `nodes`. Built once at
    // registration time (typically by the import pipeline). The
    // SceneExtractor memcpys these into scratch arrays each frame, applies
    // the active clip's overrides, then computes world matrices — avoiding a
    // deep-copy of `nodes` (each Node carries a std::string + child
    // vector, so deep-copying ~3k of them per frame is heap-allocation
    // pathological).
    std::vector<glm::vec3> restTranslation;
    std::vector<glm::quat> restRotation;
    std::vector<glm::vec3> restScale;

    // "Active node" mask for the v1 single-skin renderer. True for nodes
    // that are joints of `skins[0]` OR transitive ancestors of such joints
    // — i.e. nodes whose world transforms feed into the joint matrix array
    // the GPU consumes. ComputeWorldMatricesFlat takes this mask and prunes
    // unrelated subtrees from the BFS, which on the AnimatedOak's tri-tree
    // rig drops world-matrix work from 3080 → ~939 nodes (~3.3× cheaper).
    // Built at registration time; stays empty for assets without skins.
    std::vector<bool>      activeNodeMask;

    // Union of primitive AABBs at rest pose. Informational; the bake pipeline
    // will compute a clip-wide AABB by sampling the animation.
    glm::vec3 aabbMin = glm::vec3(0.0f);
    glm::vec3 aabbMax = glm::vec3(0.0f);

    // True after first register; cleared by UploadPending.
    bool needsUpload = true;

    // Texture upload is independent of vertex/index upload because textures
    // are technique-side resources (created outside the render graph,
    // persisted across rebuilds), while vertex/index buffers live INSIDE
    // the graph and need re-uploading on every rebuild. Textures only
    // re-upload when the IR's texture array has actually changed (i.e. on
    // LoadGlb / ReplaceSkinnedMesh).
    bool needsTextureUpload = true;

    // The IR's source textures, copied here at registry time. The registry
    // walks this on UploadPending to populate the `textures[]` GpuImages.
    // Held as raw decoded RGBA8 + dims; one entry per IR texture, plus
    // the 1×1 white fallback the registry appends.
    struct PendingTexture {
        std::vector<uint8_t> rgba8;
        uint32_t             width  = 0;
        uint32_t             height = 0;
    };
    std::vector<PendingTexture> pendingTextures;
};

// ---- AnimationClipAsset ----
//
// Pure-CPU asset: keyframe channels driving named nodes' TRS over `duration`.
// No GPU resources because the SceneExtractor evaluates clips on the CPU and
// uploads only the resulting joint matrices. Multiple clips can target the
// same SkinnedMeshAsset — the Component picks which clip plays.

struct AnimationClipAsset {
    std::string                                name;
    std::string                                sourcePath;
    float                                      duration = 0.0f;
    std::vector<gltf_import::AnimationChannel> channels;
};
