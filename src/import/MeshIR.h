#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ---- MeshIR ----
//
// Intermediate representation produced by GltfLoader. Carries everything the
// engine cares about from a .glb in a renderer-agnostic shape: nodes, meshes,
// skins, animations, materials, decoded textures.
//
// MeshIR is deliberately separate from MeshAsset / SkinnedMeshAsset (which live
// in AssetRegistry) — the IR keeps source-file fidelity (full node hierarchy,
// every animation, etc.) while the registry types only carry what the runtime
// needs to render. Conversion happens in one place (the import pipeline) so
// adding a new asset shape later doesn't ripple through every loader.
//
// All transforms / units are passed through unchanged from the .glb. glTF is
// right-handed Y-up by spec; the renderer's camera matrix handles the convention
// match.

namespace gltf_import {

// ---- Vertex ----
//
// Skinned-mesh vertex layout. Mirrors the engine's runtime SkinnedVertex
// (see SkinnedMeshAsset.h) so a primitive's IR vertex array can be moved into
// the asset without re-walking. JOINTS_0 is widened to u32 here for storage
// uniformity even though the .glb may have stored it as u8 / u16 — cgltf
// normalizes for us when we read.

struct SkinnedVertex {
    glm::vec3   position;
    glm::vec3   normal;
    glm::vec2   uv;
    glm::uvec4  joints;     // u32 per slot — renderer narrows on upload if it wants u16
    glm::vec4   weights;    // sum-to-1 normalized
};

// ---- Material ----

struct Material {
    std::string name;
    glm::vec4   baseColorFactor = glm::vec4(1.0f);
    int         baseColorTextureIndex = -1;     // index into MeshIR::textures (-1 if none)
    bool        doubleSided = false;
    enum class AlphaMode : uint8_t { Opaque, Mask, Blend } alphaMode = AlphaMode::Opaque;
    float       alphaCutoff = 0.5f;             // honored when alphaMode == Mask
};

// ---- Texture ----
//
// Decoded RGBA8 pixel data + dimensions. Embedded PNGs from the .glb are
// decoded by stb_image during load; URI-referenced textures are resolved
// against the .glb's directory and decoded the same way.

struct Texture {
    std::string          name;
    uint32_t             width = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> rgba8;        // size = width * height * 4
};

// ---- Primitive ----
//
// One draw unit. A single .glb mesh can have multiple primitives (different
// material assignments) — we flatten across glTF meshes so the IR is one flat
// list. Each primitive remembers which IR node owned the parent mesh, which
// skin (if any) drives it, and which material it uses.

struct Primitive {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t>      indices;
    int                         materialIndex = -1;   // index into MeshIR::materials (-1 if none)
    int                         skinIndex     = -1;   // index into MeshIR::skins (-1 = static)
    int                         ownerNodeIndex = -1;  // which IR node carried this mesh

    // World-space AABB at rest pose, computed during load. Used by the bake
    // pipeline to size the voxel grid; not consumed by the runtime today.
    glm::vec3 aabbMin = glm::vec3(0.0f);
    glm::vec3 aabbMax = glm::vec3(0.0f);
};

// ---- Skin ----
//
// glTF skin: a list of joint nodes plus inverse-bind matrices that move
// vertices from mesh-local space into joint-local space at rest. Joints are
// indexed by position in this array — the vertex's JOINTS_0 attribute names
// indices into joints[], NOT into MeshIR::nodes directly.

struct Skin {
    std::string             name;
    std::vector<int>        joints;              // indices into MeshIR::nodes
    std::vector<glm::mat4>  inverseBindMatrices; // one per joint
    int                     skeletonRootNode = -1; // optional, -1 if absent
};

// ---- Node ----
//
// Scene-graph node. Stores local TRS (passed through from the .glb), parent /
// children indices, and references to the mesh / skin attached to this node
// (-1 if absent). The IR does not flatten — the full hierarchy is preserved so
// the AnimationEvaluator can walk parent → child to compute world transforms.

struct Node {
    std::string         name;
    int                 parent   = -1;          // -1 for roots
    std::vector<int>    children;
    glm::vec3           translation = glm::vec3(0.0f);
    glm::quat           rotation    = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3           scale       = glm::vec3(1.0f);
    int                 meshIndex  = -1;         // -1 = no mesh attached
    int                 skinIndex  = -1;         // -1 = static
};

// ---- Animation ----
//
// One animation clip with one or more channels. A channel targets a single
// (node, path) pair — Translation, Rotation, or Scale — and supplies a sampler
// that holds the keyframe times (input) and values (output) plus an
// interpolation mode. Duration is the largest input time across all channels.

enum class AnimationPath : uint8_t { Translation, Rotation, Scale };
enum class InterpolationMode : uint8_t { Linear, Step, CubicSpline };

struct AnimationChannel {
    int                  targetNode = -1;        // index into MeshIR::nodes
    AnimationPath        path = AnimationPath::Translation;
    InterpolationMode    interpolation = InterpolationMode::Linear;
    std::vector<float>   times;                  // sorted, in seconds
    // Flat values: vec3 packed as 3 floats per keyframe for Translation/Scale,
    // vec4 quaternion packed as 4 floats per keyframe for Rotation. Caller
    // selects width via the path.
    std::vector<float>   values;
};

struct Animation {
    std::string                    name;
    float                          duration = 0.0f;
    std::vector<AnimationChannel>  channels;
};

// ---- MeshIR ----

struct MeshIR {
    std::vector<Node>       nodes;
    std::vector<int>        rootNodes;
    std::vector<Primitive>  primitives;     // flattened across all glTF meshes
    std::vector<Material>   materials;
    std::vector<Texture>    textures;
    std::vector<Skin>       skins;
    std::vector<Animation>  animations;

    // Source path the IR was loaded from. Empty when constructed in tests.
    std::string             sourcePath;

    // Quick stats for logging / UI display.
    size_t TotalVertices() const {
        size_t n = 0;
        for (const auto& p : primitives) n += p.vertices.size();
        return n;
    }
    size_t TotalTriangles() const {
        size_t n = 0;
        for (const auto& p : primitives) n += p.indices.size() / 3;
        return n;
    }
};

} // namespace gltf_import
