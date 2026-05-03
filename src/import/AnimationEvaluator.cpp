#include "AnimationEvaluator.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cassert>

namespace gltf_import {
namespace {

// Binary-search the keyframe interval that brackets `t`. Returns:
//   leftIdx, rightIdx — the two keyframe indices to interpolate between
//   alpha — [0,1] fraction within (times[left], times[right])
// For times outside the [first, last] range, clamps to the endpoint with
// alpha=0 (so EvaluateValue degenerates into "use the boundary keyframe").
struct Bracket { size_t left; size_t right; float alpha; };

Bracket BracketKeyframe(const std::vector<float>& times, float t) {
    Bracket b{0, 0, 0.0f};
    if (times.empty()) return b;
    if (t <= times.front()) { b.left = 0; b.right = 0; b.alpha = 0.0f; return b; }
    if (t >= times.back())  { b.left = times.size() - 1; b.right = b.left; b.alpha = 0.0f; return b; }

    // std::upper_bound returns the first time strictly greater than t. The
    // bracket is then [it-1, it].
    auto it = std::upper_bound(times.begin(), times.end(), t);
    size_t right = static_cast<size_t>(std::distance(times.begin(), it));
    size_t left  = right - 1;
    float t0 = times[left], t1 = times[right];
    float dt = t1 - t0;
    b.alpha = (dt > 1e-9f) ? (t - t0) / dt : 0.0f;
    b.left = left; b.right = right;
    return b;
}

// Read a vec3 keyframe at index k. For Linear/Step paths each keyframe is one
// vec3; for CubicSpline each keyframe is *three* vec3s (in-tangent, value,
// out-tangent) — middle is the value at exact knot times.
glm::vec3 ReadVec3(const std::vector<float>& v, size_t k, InterpolationMode mode) {
    const size_t stride = (mode == InterpolationMode::CubicSpline) ? 9 : 3;
    const size_t base   = k * stride + ((mode == InterpolationMode::CubicSpline) ? 3 : 0);
    return glm::vec3(v[base + 0], v[base + 1], v[base + 2]);
}
// Same for quaternions (vec4 instead of vec3). glTF stores as (x, y, z, w);
// glm::quat ctor takes (w, x, y, z).
glm::quat ReadQuat(const std::vector<float>& v, size_t k, InterpolationMode mode) {
    const size_t stride = (mode == InterpolationMode::CubicSpline) ? 12 : 4;
    const size_t base   = k * stride + ((mode == InterpolationMode::CubicSpline) ? 4 : 0);
    return glm::quat(v[base + 3], v[base + 0], v[base + 1], v[base + 2]);
}

void ApplyChannel(const AnimationChannel& ch, std::vector<Node>& nodes, float t) {
    if (ch.targetNode < 0 || static_cast<size_t>(ch.targetNode) >= nodes.size()) return;
    if (ch.times.empty()) return;
    Bracket b = BracketKeyframe(ch.times, t);
    Node& n = nodes[ch.targetNode];

    switch (ch.path) {
        case AnimationPath::Translation: {
            glm::vec3 a = ReadVec3(ch.values, b.left,  ch.interpolation);
            glm::vec3 cval = (b.right == b.left) ? a : ReadVec3(ch.values, b.right, ch.interpolation);
            switch (ch.interpolation) {
                case InterpolationMode::Step: n.translation = a; break;
                case InterpolationMode::Linear: n.translation = glm::mix(a, cval, b.alpha); break;
                case InterpolationMode::CubicSpline: {
                    // Hermite spline: p(t) = (2t^3-3t^2+1)P0 + (t^3-2t^2+t)dt*M0 + (-2t^3+3t^2)P1 + (t^3-t^2)dt*M1
                    // M0 = out-tangent at left keyframe, M1 = in-tangent at right keyframe.
                    const float ta = b.alpha;
                    const float dt = ch.times[b.right] - ch.times[b.left];
                    glm::vec3 outTan(ch.values[b.left  * 9 + 6], ch.values[b.left  * 9 + 7], ch.values[b.left  * 9 + 8]);
                    glm::vec3 inTan (ch.values[b.right * 9 + 0], ch.values[b.right * 9 + 1], ch.values[b.right * 9 + 2]);
                    float t2 = ta * ta;
                    float t3 = t2 * ta;
                    n.translation = (2*t3 - 3*t2 + 1) * a
                                  + (t3 - 2*t2 + ta) * dt * outTan
                                  + (-2*t3 + 3*t2) * cval
                                  + (t3 - t2) * dt * inTan;
                    break;
                }
            }
            break;
        }
        case AnimationPath::Scale: {
            glm::vec3 a = ReadVec3(ch.values, b.left, ch.interpolation);
            glm::vec3 cval = (b.right == b.left) ? a : ReadVec3(ch.values, b.right, ch.interpolation);
            switch (ch.interpolation) {
                case InterpolationMode::Step: n.scale = a; break;
                case InterpolationMode::Linear: n.scale = glm::mix(a, cval, b.alpha); break;
                case InterpolationMode::CubicSpline: {
                    const float ta = b.alpha;
                    const float dt = ch.times[b.right] - ch.times[b.left];
                    glm::vec3 outTan(ch.values[b.left  * 9 + 6], ch.values[b.left  * 9 + 7], ch.values[b.left  * 9 + 8]);
                    glm::vec3 inTan (ch.values[b.right * 9 + 0], ch.values[b.right * 9 + 1], ch.values[b.right * 9 + 2]);
                    float t2 = ta * ta;
                    float t3 = t2 * ta;
                    n.scale = (2*t3 - 3*t2 + 1) * a
                            + (t3 - 2*t2 + ta) * dt * outTan
                            + (-2*t3 + 3*t2) * cval
                            + (t3 - t2) * dt * inTan;
                    break;
                }
            }
            break;
        }
        case AnimationPath::Rotation: {
            glm::quat a = ReadQuat(ch.values, b.left, ch.interpolation);
            glm::quat cval = (b.right == b.left) ? a : ReadQuat(ch.values, b.right, ch.interpolation);
            switch (ch.interpolation) {
                case InterpolationMode::Step: n.rotation = a; break;
                case InterpolationMode::Linear: n.rotation = glm::normalize(glm::slerp(a, cval, b.alpha)); break;
                case InterpolationMode::CubicSpline: {
                    // Per glTF spec, rotation cubic-spline is interpolated as a
                    // vec4 then re-normalized. Simpler than implementing a
                    // proper quaternion spline.
                    const float ta = b.alpha;
                    const float dt = ch.times[b.right] - ch.times[b.left];
                    glm::vec4 outTan(ch.values[b.left  * 12 + 4], ch.values[b.left  * 12 + 5], ch.values[b.left  * 12 + 6], ch.values[b.left  * 12 + 7]);
                    glm::vec4 inTan (ch.values[b.right * 12 + 0], ch.values[b.right * 12 + 1], ch.values[b.right * 12 + 2], ch.values[b.right * 12 + 3]);
                    glm::vec4 av(a.x, a.y, a.z, a.w);
                    glm::vec4 cv(cval.x, cval.y, cval.z, cval.w);
                    float t2 = ta * ta;
                    float t3 = t2 * ta;
                    glm::vec4 q = (2*t3 - 3*t2 + 1) * av
                                + (t3 - 2*t2 + ta) * dt * outTan
                                + (-2*t3 + 3*t2) * cv
                                + (t3 - t2) * dt * inTan;
                    glm::quat r(q.w, q.x, q.y, q.z);
                    n.rotation = glm::normalize(r);
                    break;
                }
            }
            break;
        }
    }
}

glm::mat4 LocalMat(const Node& n) {
    glm::mat4 t = glm::translate(glm::mat4(1.0f), n.translation);
    glm::mat4 r = glm::mat4_cast(n.rotation);
    glm::mat4 s = glm::scale(glm::mat4(1.0f), n.scale);
    return t * r * s;
}

} // namespace

void EvaluateClip(const Animation& clip, float time, std::vector<Node>& nodesInOut) {
    const float t = std::clamp(time, 0.0f, clip.duration);
    for (const auto& ch : clip.channels) {
        ApplyChannel(ch, nodesInOut, t);
    }
}

namespace {

// Parallel-array channel application. Mirrors ApplyChannel above but writes
// into flat TRS arrays instead of into a Node[]. Callers in the per-frame
// SceneExtractor use this so they don't have to deep-copy a vector<Node>
// (which carries per-element std::string + std::vector<int> child arrays).
void ApplyChannelFlat(const AnimationChannel& ch,
                      std::vector<glm::vec3>& trs_t,
                      std::vector<glm::quat>& trs_r,
                      std::vector<glm::vec3>& trs_s,
                      float t)
{
    if (ch.targetNode < 0 || static_cast<size_t>(ch.targetNode) >= trs_t.size()) return;
    if (ch.times.empty()) return;
    Bracket b = BracketKeyframe(ch.times, t);
    const size_t idx = static_cast<size_t>(ch.targetNode);

    auto evalVec3 = [&](glm::vec3& outVal) {
        glm::vec3 a    = ReadVec3(ch.values, b.left, ch.interpolation);
        glm::vec3 cval = (b.right == b.left) ? a : ReadVec3(ch.values, b.right, ch.interpolation);
        switch (ch.interpolation) {
            case InterpolationMode::Step:   outVal = a; break;
            case InterpolationMode::Linear: outVal = glm::mix(a, cval, b.alpha); break;
            case InterpolationMode::CubicSpline: {
                const float ta = b.alpha;
                const float dt = ch.times[b.right] - ch.times[b.left];
                glm::vec3 outTan(ch.values[b.left  * 9 + 6], ch.values[b.left  * 9 + 7], ch.values[b.left  * 9 + 8]);
                glm::vec3 inTan (ch.values[b.right * 9 + 0], ch.values[b.right * 9 + 1], ch.values[b.right * 9 + 2]);
                float t2 = ta * ta;
                float t3 = t2 * ta;
                outVal = (2*t3 - 3*t2 + 1) * a
                       + (t3 - 2*t2 + ta) * dt * outTan
                       + (-2*t3 + 3*t2) * cval
                       + (t3 - t2) * dt * inTan;
                break;
            }
        }
    };

    switch (ch.path) {
        case AnimationPath::Translation: evalVec3(trs_t[idx]); break;
        case AnimationPath::Scale:       evalVec3(trs_s[idx]); break;
        case AnimationPath::Rotation: {
            glm::quat a = ReadQuat(ch.values, b.left, ch.interpolation);
            glm::quat cval = (b.right == b.left) ? a : ReadQuat(ch.values, b.right, ch.interpolation);
            switch (ch.interpolation) {
                case InterpolationMode::Step:   trs_r[idx] = a; break;
                case InterpolationMode::Linear: trs_r[idx] = glm::normalize(glm::slerp(a, cval, b.alpha)); break;
                case InterpolationMode::CubicSpline: {
                    const float ta = b.alpha;
                    const float dt = ch.times[b.right] - ch.times[b.left];
                    glm::vec4 outTan(ch.values[b.left  * 12 + 4], ch.values[b.left  * 12 + 5], ch.values[b.left  * 12 + 6], ch.values[b.left  * 12 + 7]);
                    glm::vec4 inTan (ch.values[b.right * 12 + 0], ch.values[b.right * 12 + 1], ch.values[b.right * 12 + 2], ch.values[b.right * 12 + 3]);
                    glm::vec4 av(a.x, a.y, a.z, a.w);
                    glm::vec4 cv(cval.x, cval.y, cval.z, cval.w);
                    float t2 = ta * ta;
                    float t3 = t2 * ta;
                    glm::vec4 q = (2*t3 - 3*t2 + 1) * av
                                + (t3 - 2*t2 + ta) * dt * outTan
                                + (-2*t3 + 3*t2) * cv
                                + (t3 - t2) * dt * inTan;
                    glm::quat r(q.w, q.x, q.y, q.z);
                    trs_r[idx] = glm::normalize(r);
                    break;
                }
            }
            break;
        }
    }
}

inline glm::mat4 LocalMatFlat(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 mt = glm::translate(glm::mat4(1.0f), t);
    glm::mat4 mr = glm::mat4_cast(r);
    glm::mat4 ms = glm::scale(glm::mat4(1.0f), s);
    return mt * mr * ms;
}

} // namespace

void EvaluateClipFlat(const Animation& clip, float time,
                      std::vector<glm::vec3>& trs_t,
                      std::vector<glm::quat>& trs_r,
                      std::vector<glm::vec3>& trs_s)
{
    const float t = std::clamp(time, 0.0f, clip.duration);
    for (const auto& ch : clip.channels) {
        ApplyChannelFlat(ch, trs_t, trs_r, trs_s, t);
    }
}

void ComputeWorldMatricesFlat(const std::vector<Node>& nodes,
                              const std::vector<glm::vec3>& trs_t,
                              const std::vector<glm::quat>& trs_r,
                              const std::vector<glm::vec3>& trs_s,
                              std::vector<glm::mat4>& outWorlds,
                              const std::vector<bool>* activeMask)
{
    outWorlds.assign(nodes.size(), glm::mat4(1.0f));
    std::vector<int> stack;
    stack.reserve(nodes.size());
    auto inMask = [&](int idx) -> bool {
        return !activeMask || (idx >= 0 && static_cast<size_t>(idx) < activeMask->size() && (*activeMask)[idx]);
    };

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent >= 0) continue;
        if (!inMask(static_cast<int>(i))) continue;
        outWorlds[i] = LocalMatFlat(trs_t[i], trs_r[i], trs_s[i]);
        stack.push_back(static_cast<int>(i));
    }
    while (!stack.empty()) {
        const int parent = stack.back();
        stack.pop_back();
        const glm::mat4& parentWorld = outWorlds[parent];
        for (int childIdx : nodes[parent].children) {
            if (childIdx < 0 || static_cast<size_t>(childIdx) >= nodes.size()) continue;
            if (!inMask(childIdx)) continue;
            outWorlds[childIdx] = parentWorld * LocalMatFlat(trs_t[childIdx], trs_r[childIdx], trs_s[childIdx]);
            stack.push_back(childIdx);
        }
    }
}

void ComputeWorldMatrices(const std::vector<Node>& nodes, std::vector<glm::mat4>& outWorlds) {
    // Single-pass BFS from each root through its child array. Each node's
    // world matrix is computed exactly once: parent-world × local. O(N), as
    // opposed to the previous O(depth × N) sweep which dominated CPU time
    // for dense rigs (the AnimatedOak's 3080-node hierarchy was burning
    // ~16 ms/frame inside this function at debug-build speeds).
    outWorlds.assign(nodes.size(), glm::mat4(1.0f));

    // Reused scratch — capacity grows once and stays. The single int-per-node
    // allocation is dwarfed by the mat4 vector outWorlds anyway.
    std::vector<int> stack;
    stack.reserve(nodes.size());

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent >= 0) continue;  // not a root
        outWorlds[i] = LocalMat(nodes[i]);
        stack.push_back(static_cast<int>(i));
    }
    while (!stack.empty()) {
        const int parent = stack.back();
        stack.pop_back();
        const glm::mat4& parentWorld = outWorlds[parent];
        for (int childIdx : nodes[parent].children) {
            if (childIdx < 0 || static_cast<size_t>(childIdx) >= nodes.size()) continue;
            outWorlds[childIdx] = parentWorld * LocalMat(nodes[childIdx]);
            stack.push_back(childIdx);
        }
    }
}

void ComputeJointMatrices(const Skin& skin,
                          const std::vector<glm::mat4>& worlds,
                          const glm::mat4& meshNodeWorld,
                          std::vector<glm::mat4>& outJoints)
{
    outJoints.assign(skin.joints.size(), glm::mat4(1.0f));
    const glm::mat4 invMeshWorld = glm::inverse(meshNodeWorld);
    for (size_t i = 0; i < skin.joints.size(); ++i) {
        const int jointNode = skin.joints[i];
        if (jointNode < 0 || static_cast<size_t>(jointNode) >= worlds.size()) continue;
        outJoints[i] = invMeshWorld * worlds[jointNode] * skin.inverseBindMatrices[i];
    }
}

} // namespace gltf_import
