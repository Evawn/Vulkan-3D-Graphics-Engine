#pragma once

#include "MeshIR.h"

#include <vector>

// ---- AnimationEvaluator ----
//
// Pure CPU sampling: clip + time -> per-node TRS -> per-skin joint matrices.
// No GPU resources, no allocations on the hot path (caller-owned scratch
// vectors). Stateless beyond what the caller hands in — the same evaluator
// can be re-used across components/threads.
//
// Lifecycle (per frame, per skinned mesh):
//
//   1. EvaluateClip(clip, time, nodes, scratchTRS)
//        Walks each channel, binary-searches keyframes, lerps/slerps into
//        nodes' local TRS. Channels not in the clip leave the node's TRS
//        unchanged from its rest pose.
//
//   2. ComputeWorldMatrices(nodes, outWorlds)
//        Top-down traversal. Each node's world = parent->world * local.
//
//   3. ComputeJointMatrices(skin, worlds, meshNodeWorld, outJoints)
//        For each joint i: jointMat[i] = inverse(meshNodeWorld) * worlds[joint[i]]
//                                       * inverseBindMatrix[i]
//        That mesh-node inverse keeps the skin in the mesh node's local frame
//        regardless of where the mesh node sits in the hierarchy.
//
// glTF-spec arithmetic — see the "Skinning" section of the glTF 2.0 spec.

namespace gltf_import {

// Sample one clip at `time` (clamped to [0, duration]) and write each
// channel's evaluated value into the matching node's TRS. Nodes whose TRS
// the clip doesn't touch are left at the value the IR loaded with (rest
// pose). Caller passes a *copy* of the IR's nodes — the evaluator never
// mutates the IR.
void EvaluateClip(const Animation& clip,
                  float             time,
                  std::vector<Node>& nodesInOut);

// Walk every node, computing world transforms from local TRS via DFS. Roots
// (parent < 0) start from the identity matrix. `outWorlds` is sized to
// nodes.size() and indexed by node index.
void ComputeWorldMatrices(const std::vector<Node>& nodes,
                          std::vector<glm::mat4>&  outWorlds);

// ---- Flat-TRS variants ----
//
// These take parallel TRS arrays instead of mutating-back-into-Node. Saves a
// per-frame deep copy of the node tree (Node carries std::string + child
// vector — copying 3000 of those is O(thousands-of-heap-allocs) and dominates
// the SceneExtractor's CPU cost for dense rigs). Layout: each array indexed
// by node index, same length as the IR's nodes vector.

void EvaluateClipFlat(const Animation&         clip,
                      float                    time,
                      std::vector<glm::vec3>&  trs_translation,
                      std::vector<glm::quat>&  trs_rotation,
                      std::vector<glm::vec3>&  trs_scale);

// Reads structural fields (parent, children) from `nodes`, but reads TRS
// from the parallel flat arrays. `nodes` is treated as read-only.
//
// activeMask (optional): when non-null, the BFS prunes non-masked subtrees.
// Only nodes whose mask bit is true get a world matrix computed; the rest
// are left at identity in outWorlds. Use this to skip world-matrix work for
// nodes that don't affect any rendered vertex (e.g. for the AnimatedOak,
// only ~939 of 3080 nodes drive skin 0). nullptr = walk every node.
void ComputeWorldMatricesFlat(const std::vector<Node>&        nodes,
                              const std::vector<glm::vec3>&   trs_translation,
                              const std::vector<glm::quat>&   trs_rotation,
                              const std::vector<glm::vec3>&   trs_scale,
                              std::vector<glm::mat4>&         outWorlds,
                              const std::vector<bool>*        activeMask = nullptr);

// Build the joint matrix array consumed by the vertex shader. Indexed by
// joint slot in the skin (the index referenced by JOINTS_0). Resizes
// outJoints to skin.joints.size().
void ComputeJointMatrices(const Skin&                    skin,
                          const std::vector<glm::mat4>&  worlds,
                          const glm::mat4&               meshNodeWorld,
                          std::vector<glm::mat4>&        outJoints);

} // namespace gltf_import
