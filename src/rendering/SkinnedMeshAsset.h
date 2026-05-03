#pragma once

#include "RenderGraphTypes.h"
#include "MeshIR.h"

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

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

    // ---- Per-primitive draw unit ----
    struct Primitive {
        std::vector<gltf_import::SkinnedVertex> vertices;
        std::vector<uint32_t>                   indices;

        // Material color tint applied per fragment. Texture sampling lands in
        // a later milestone — for v1 we paint the primitive in its base color
        // factor (which is exactly what cgltf hands us when there's no
        // baseColorTexture).
        glm::vec4 baseColorFactor = glm::vec4(1.0f);

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
