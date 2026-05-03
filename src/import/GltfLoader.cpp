#include "GltfLoader.h"

// cgltf is single-header; the implementation goes here. CGLTF_IMPLEMENTATION
// must be defined in exactly one TU; this file is that TU.
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// stb_image is already in dep/ and used elsewhere — define implementation
// in exactly one place. We don't define it here because shader uploads /
// texture loaders elsewhere already do. (If a future build ends up with no
// other definer, switch this to `#define STB_IMAGE_IMPLEMENTATION` here.)
#include "stb_image.h"

#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace gltf_import {
namespace {

// ---- Helpers ----

const char* CgltfResultName(cgltf_result r) {
    switch (r) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data_too_short";
        case cgltf_result_unknown_format: return "unknown_format";
        case cgltf_result_invalid_json: return "invalid_json";
        case cgltf_result_invalid_gltf: return "invalid_gltf";
        case cgltf_result_invalid_options: return "invalid_options";
        case cgltf_result_file_not_found: return "file_not_found";
        case cgltf_result_io_error: return "io_error";
        case cgltf_result_out_of_memory: return "out_of_memory";
        case cgltf_result_legacy_gltf: return "legacy_gltf";
        default: return "unknown";
    }
}

// Resolve a cgltf pointer to its index inside the parent array. cgltf parents
// every child via raw pointer to the array's contiguous storage, so pointer
// arithmetic gives a stable index without a search. Returns -1 for null.
int IndexOf(const cgltf_node* node, const cgltf_data* data) {
    if (!node || !data->nodes) return -1;
    return static_cast<int>(node - data->nodes);
}
int IndexOf(const cgltf_skin* skin, const cgltf_data* data) {
    if (!skin || !data->skins) return -1;
    return static_cast<int>(skin - data->skins);
}
int IndexOf(const cgltf_material* mat, const cgltf_data* data) {
    if (!mat || !data->materials) return -1;
    return static_cast<int>(mat - data->materials);
}
int IndexOf(const cgltf_texture* tex, const cgltf_data* data) {
    if (!tex || !data->textures) return -1;
    return static_cast<int>(tex - data->textures);
}

// Read TRS (translation, rotation, scale) from a cgltf node, falling back to
// the matrix decomposition path when has_matrix is set. cgltf already exposes
// the parsed TRS via has_translation / has_rotation / has_scale flags; matrix-
// only nodes are rare in production assets but we handle them defensively.
void ReadNodeTRS(const cgltf_node& src, glm::vec3& outT, glm::quat& outR, glm::vec3& outS) {
    outT = glm::vec3(0.0f);
    outR = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    outS = glm::vec3(1.0f);

    if (src.has_matrix) {
        // Matrix-only path: decompose. cgltf stores column-major into [16]; glm
        // mat4 ctor takes column-major float* directly.
        glm::mat4 m = glm::make_mat4(src.matrix);
        // Trivial decomposition: rows of the rotation matrix == basis vectors,
        // their lengths == scale, rotation = normalized basis. Skips skew /
        // shear which a well-formed glTF doesn't carry.
        outT = glm::vec3(m[3]);
        outS.x = glm::length(glm::vec3(m[0]));
        outS.y = glm::length(glm::vec3(m[1]));
        outS.z = glm::length(glm::vec3(m[2]));
        glm::mat3 rot(
            glm::vec3(m[0]) / outS.x,
            glm::vec3(m[1]) / outS.y,
            glm::vec3(m[2]) / outS.z);
        outR = glm::quat_cast(rot);
        return;
    }
    if (src.has_translation) outT = glm::vec3(src.translation[0], src.translation[1], src.translation[2]);
    if (src.has_rotation)    outR = glm::quat(src.rotation[3], src.rotation[0], src.rotation[1], src.rotation[2]); // glm quat ctor: w, x, y, z
    if (src.has_scale)       outS = glm::vec3(src.scale[0], src.scale[1], src.scale[2]);
}

InterpolationMode MapInterp(cgltf_interpolation_type t) {
    switch (t) {
        case cgltf_interpolation_type_step:        return InterpolationMode::Step;
        case cgltf_interpolation_type_cubic_spline: return InterpolationMode::CubicSpline;
        case cgltf_interpolation_type_linear:
        default:                                    return InterpolationMode::Linear;
    }
}

AnimationPath MapPath(cgltf_animation_path_type p, bool& outSupported) {
    outSupported = true;
    switch (p) {
        case cgltf_animation_path_type_translation: return AnimationPath::Translation;
        case cgltf_animation_path_type_rotation:    return AnimationPath::Rotation;
        case cgltf_animation_path_type_scale:       return AnimationPath::Scale;
        default:
            // Weights (morph targets) and "invalid" fall here. v1 ignores them.
            outSupported = false;
            return AnimationPath::Translation;
    }
}

Material::AlphaMode MapAlphaMode(cgltf_alpha_mode a) {
    switch (a) {
        case cgltf_alpha_mode_mask:  return Material::AlphaMode::Mask;
        case cgltf_alpha_mode_blend: return Material::AlphaMode::Blend;
        case cgltf_alpha_mode_opaque:
        default:                     return Material::AlphaMode::Opaque;
    }
}

// ---- Texture decoding ----
//
// cgltf gives us the embedded image bytes in three flavours:
//   1. data URI (base64 inside the JSON)
//   2. external URI (relative path next to the .glb)
//   3. buffer view (binary chunk, the .glb common case)
// cgltf_load_buffers + cgltf_buffer_view_data abstract (3); we read the bytes
// and hand them to stb_image for PNG / JPEG decode.

bool DecodeImage(const cgltf_image& img, const cgltf_data* data,
                 const std::string& gltfDir, Texture& outTex)
{
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = nullptr;

    if (img.buffer_view) {
        // Embedded in the .glb's binary chunk — the common case for .glb files.
        const cgltf_buffer_view* bv = img.buffer_view;
        const uint8_t* base = static_cast<const uint8_t*>(bv->buffer->data);
        const uint8_t* bytes = base + bv->offset;
        pixels = stbi_load_from_memory(bytes, static_cast<int>(bv->size),
                                       &w, &h, &comp, STBI_rgb_alpha);
    } else if (img.uri) {
        if (std::strncmp(img.uri, "data:", 5) == 0) {
            // Data URI: cgltf_load_buffers should have already parsed it into
            // a buffer; if we hit this path we'd need to base64-decode. For v1
            // we don't support data URIs in image slots — log and skip.
            spdlog::get("Render")->warn("GLTF image uses data: URI; v1 doesn't decode these");
            return false;
        }
        // External file. URI is percent-encoded per glTF spec; cgltf already
        // unescaped it during parse if cgltf_options.unescape_path was set
        // (default true), so we can use the uri pointer directly.
        std::filesystem::path full = std::filesystem::path(gltfDir) / img.uri;
        pixels = stbi_load(full.string().c_str(), &w, &h, &comp, STBI_rgb_alpha);
    }
    (void)data;

    if (!pixels || w <= 0 || h <= 0) return false;

    outTex.width  = static_cast<uint32_t>(w);
    outTex.height = static_cast<uint32_t>(h);
    outTex.rgba8.assign(pixels, pixels + (w * h * 4));
    stbi_image_free(pixels);
    if (img.name)  outTex.name = img.name;
    return true;
}

// ---- Vertex attribute readers ----
//
// cgltf exposes a "read floats / read uints" helper that walks an accessor and
// normalizes types into the requested width — exactly what we need here, where
// JOINTS_0 may be u8 or u16 and WEIGHTS_0 may be normalized u8/u16 or float.

void ReadFloats(const cgltf_accessor* acc, size_t i, float* out, size_t comp) {
    if (!acc) { for (size_t k = 0; k < comp; ++k) out[k] = 0.0f; return; }
    cgltf_accessor_read_float(acc, i, out, comp);
}
void ReadUints(const cgltf_accessor* acc, size_t i, cgltf_uint* out, size_t comp) {
    if (!acc) { for (size_t k = 0; k < comp; ++k) out[k] = 0; return; }
    cgltf_accessor_read_uint(acc, i, out, comp);
}

} // namespace

// ---- Public API ----

std::optional<MeshIR> LoadGlb(const std::string& path, const LoadOptions& options) {
    auto logger = spdlog::get("Render");

    cgltf_options copts{};
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse_file(&copts, path.c_str(), &data);
    if (r != cgltf_result_success) {
        logger->warn("GLB parse failed: {} ({})", path, CgltfResultName(r));
        return std::nullopt;
    }
    // Load buffer payloads (the BIN chunk plus any external .bin files). After
    // this call, cgltf_buffer::data is populated and accessor reads work.
    r = cgltf_load_buffers(&copts, data, path.c_str());
    if (r != cgltf_result_success) {
        logger->warn("GLB load_buffers failed: {} ({})", path, CgltfResultName(r));
        cgltf_free(data);
        return std::nullopt;
    }
    // Sanity check (catches malformed asset that parsed but doesn't validate).
    r = cgltf_validate(data);
    if (r != cgltf_result_success) {
        logger->warn("GLB validate warned: {} ({}); continuing", path, CgltfResultName(r));
    }

    MeshIR ir;
    ir.sourcePath = path;

    // ---- Nodes ----
    ir.nodes.resize(data->nodes_count);
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& src = data->nodes[i];
        Node& n = ir.nodes[i];
        n.name = src.name ? src.name : "";
        n.parent = IndexOf(src.parent, data);
        n.children.reserve(src.children_count);
        for (size_t c = 0; c < src.children_count; ++c) {
            n.children.push_back(IndexOf(src.children[c], data));
        }
        ReadNodeTRS(src, n.translation, n.rotation, n.scale);
        n.meshIndex  = -1;          // resolved during primitive walk below
        n.skinIndex  = IndexOf(src.skin, data);
    }
    // Roots = nodes with no parent.
    for (size_t i = 0; i < ir.nodes.size(); ++i) {
        if (ir.nodes[i].parent < 0) ir.rootNodes.push_back(static_cast<int>(i));
    }

    // ---- Materials ----
    ir.materials.resize(data->materials_count);
    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& src = data->materials[i];
        Material& m = ir.materials[i];
        m.name = src.name ? src.name : "";
        m.doubleSided = src.double_sided != 0;
        m.alphaMode = MapAlphaMode(src.alpha_mode);
        m.alphaCutoff = src.alpha_cutoff;

        // We only handle metallic-roughness here. Specular-glossiness is rare in
        // modern exports and not in v1 scope.
        if (src.has_pbr_metallic_roughness) {
            const auto& pbr = src.pbr_metallic_roughness;
            m.baseColorFactor = glm::vec4(pbr.base_color_factor[0], pbr.base_color_factor[1],
                                          pbr.base_color_factor[2], pbr.base_color_factor[3]);
            m.baseColorTextureIndex = IndexOf(pbr.base_color_texture.texture, data);
        }
    }

    // ---- Textures (decode embedded images) ----
    // glTF textures = (image, sampler) pair; we collapse to one Texture per
    // texture index since the runtime needs the decoded pixels regardless of
    // sampler. Sampler params are reserved for a future pass.
    ir.textures.reserve(data->textures_count);
    for (size_t i = 0; i < data->textures_count; ++i) {
        const cgltf_texture& src = data->textures[i];
        Texture tex;
        if (src.image && DecodeImage(*src.image,
                                     data,
                                     std::filesystem::path(path).parent_path().string(),
                                     tex)) {
            ir.textures.push_back(std::move(tex));
        } else {
            // Push a 1x1 white placeholder so material indices remain stable
            // — referenced-but-undecodable textures would otherwise shift the
            // index map of every later texture.
            Texture white;
            white.width = 1; white.height = 1;
            white.rgba8 = { 255, 255, 255, 255 };
            ir.textures.push_back(std::move(white));
        }
    }

    // ---- Skins ----
    ir.skins.resize(data->skins_count);
    for (size_t i = 0; i < data->skins_count; ++i) {
        const cgltf_skin& src = data->skins[i];
        Skin& s = ir.skins[i];
        s.name = src.name ? src.name : "";
        s.skeletonRootNode = IndexOf(src.skeleton, data);
        s.joints.reserve(src.joints_count);
        for (size_t j = 0; j < src.joints_count; ++j) {
            s.joints.push_back(IndexOf(src.joints[j], data));
        }
        // Inverse bind matrices: optional in glTF (absent = identity per joint).
        s.inverseBindMatrices.assign(src.joints_count, glm::mat4(1.0f));
        if (src.inverse_bind_matrices && src.inverse_bind_matrices->count >= src.joints_count) {
            for (size_t j = 0; j < src.joints_count; ++j) {
                float vals[16];
                cgltf_accessor_read_float(src.inverse_bind_matrices, j, vals, 16);
                s.inverseBindMatrices[j] = glm::make_mat4(vals);
            }
        }
    }

    // ---- Primitives ----
    // Walk every (mesh, primitive) pair, owned by some node in the hierarchy.
    // Capture which IR node owns each so the bake step can hand the right
    // world transform per primitive.
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& nodeSrc = data->nodes[ni];
        if (!nodeSrc.mesh) continue;
        ir.nodes[ni].meshIndex = static_cast<int>(ir.primitives.size());  // first primitive index attached to this node
        const cgltf_mesh* mesh = nodeSrc.mesh;
        const int skinIdx = IndexOf(nodeSrc.skin, data);
        for (size_t pi = 0; pi < mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) {
                // Triangle primitives only for v1 — strips / fans / lines / points are out of scope.
                continue;
            }
            // Find the standard attributes by semantic.
            const cgltf_accessor* posA = nullptr;
            const cgltf_accessor* nrmA = nullptr;
            const cgltf_accessor* uvA  = nullptr;
            const cgltf_accessor* jntA = nullptr;
            const cgltf_accessor* wgtA = nullptr;
            for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute& a = prim.attributes[ai];
                if (a.type == cgltf_attribute_type_position && a.index == 0) posA = a.data;
                else if (a.type == cgltf_attribute_type_normal && a.index == 0) nrmA = a.data;
                else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uvA = a.data;
                else if (a.type == cgltf_attribute_type_joints && a.index == 0) jntA = a.data;
                else if (a.type == cgltf_attribute_type_weights && a.index == 0) wgtA = a.data;
            }
            if (!posA) continue; // a primitive without position is malformed
            const size_t vCount = posA->count;

            Primitive out;
            out.materialIndex = IndexOf(prim.material, data);
            out.skinIndex     = skinIdx;
            out.ownerNodeIndex = static_cast<int>(ni);
            out.vertices.resize(vCount);

            for (size_t vi = 0; vi < vCount; ++vi) {
                SkinnedVertex& v = out.vertices[vi];
                float pos[3]; ReadFloats(posA, vi, pos, 3);
                v.position = glm::vec3(pos[0], pos[1], pos[2]);
                if (nrmA) {
                    float nrm[3]; ReadFloats(nrmA, vi, nrm, 3);
                    v.normal = glm::vec3(nrm[0], nrm[1], nrm[2]);
                } else {
                    v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                }
                if (uvA) {
                    float uv[2]; ReadFloats(uvA, vi, uv, 2);
                    v.uv = glm::vec2(uv[0], uv[1]);
                } else {
                    v.uv = glm::vec2(0.0f);
                }
                if (jntA) {
                    cgltf_uint j[4]; ReadUints(jntA, vi, j, 4);
                    v.joints = glm::uvec4(j[0], j[1], j[2], j[3]);
                } else if (options.fillMissingSkinningWithIdentity) {
                    v.joints = glm::uvec4(0, 0, 0, 0);
                } else {
                    v.joints = glm::uvec4(0);
                }
                if (wgtA) {
                    float w[4]; ReadFloats(wgtA, vi, w, 4);
                    v.weights = glm::vec4(w[0], w[1], w[2], w[3]);
                    // Renormalize: glTF spec allows 1e-3 tolerance on the sum,
                    // but for skinning we want exact-1 to avoid scale drift.
                    float sum = w[0] + w[1] + w[2] + w[3];
                    if (sum > 1e-6f) v.weights /= sum;
                } else if (options.fillMissingSkinningWithIdentity) {
                    v.weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                } else {
                    v.weights = glm::vec4(0.0f);
                }
            }

            // Indices — glTF allows missing index buffer (draw arrays). Materialize
            // a 0..N sequence in that case so the runtime uniformly draws indexed.
            if (prim.indices) {
                out.indices.resize(prim.indices->count);
                for (size_t ii = 0; ii < prim.indices->count; ++ii) {
                    out.indices[ii] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, ii));
                }
            } else {
                out.indices.resize(vCount);
                for (size_t ii = 0; ii < vCount; ++ii) out.indices[ii] = static_cast<uint32_t>(ii);
            }

            // AABB — precomputed by glTF on the accessor when min/max are present.
            if (posA->has_min && posA->has_max) {
                out.aabbMin = glm::vec3(posA->min[0], posA->min[1], posA->min[2]);
                out.aabbMax = glm::vec3(posA->max[0], posA->max[1], posA->max[2]);
            } else {
                out.aabbMin = out.vertices.empty() ? glm::vec3(0.0f) : out.vertices[0].position;
                out.aabbMax = out.aabbMin;
                for (const auto& v : out.vertices) {
                    out.aabbMin = glm::min(out.aabbMin, v.position);
                    out.aabbMax = glm::max(out.aabbMax, v.position);
                }
            }

            ir.primitives.push_back(std::move(out));
        }
    }

    // ---- Animations ----
    ir.animations.resize(data->animations_count);
    for (size_t i = 0; i < data->animations_count; ++i) {
        const cgltf_animation& src = data->animations[i];
        Animation& a = ir.animations[i];
        a.name = src.name ? src.name : "";
        float maxTime = 0.0f;

        a.channels.reserve(src.channels_count);
        for (size_t ci = 0; ci < src.channels_count; ++ci) {
            const cgltf_animation_channel& ch = src.channels[ci];
            if (!ch.sampler) continue;
            bool pathSupported = false;
            AnimationPath path = MapPath(ch.target_path, pathSupported);
            if (!pathSupported) continue;

            AnimationChannel out;
            out.targetNode    = IndexOf(ch.target_node, data);
            out.path          = path;
            out.interpolation = MapInterp(ch.sampler->interpolation);

            // Input (times) — always SCALAR float.
            const cgltf_accessor* inA = ch.sampler->input;
            const cgltf_accessor* outA = ch.sampler->output;
            if (!inA || !outA) continue;
            out.times.resize(inA->count);
            for (size_t k = 0; k < inA->count; ++k) {
                cgltf_accessor_read_float(inA, k, &out.times[k], 1);
                maxTime = std::max(maxTime, out.times[k]);
            }
            // Output values — width depends on path (vec3 vs vec4), but for
            // CubicSpline interpolation each keyframe carries 3× values
            // (in-tangent, value, out-tangent). We store them flat; the
            // evaluator splits them.
            const size_t comp = (path == AnimationPath::Rotation) ? 4 : 3;
            const size_t totalFloats = outA->count * comp;
            out.values.resize(totalFloats);
            for (size_t k = 0; k < outA->count; ++k) {
                float buf[4];
                cgltf_accessor_read_float(outA, k, buf, comp);
                for (size_t f = 0; f < comp; ++f) out.values[k * comp + f] = buf[f];
            }
            a.channels.push_back(std::move(out));
        }
        a.duration = maxTime;
    }

    cgltf_free(data);
    return ir;
}

} // namespace gltf_import
