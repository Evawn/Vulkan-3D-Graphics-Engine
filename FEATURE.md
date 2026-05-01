# Feature Vision

A long-term mood board for what this engine is becoming. Not a spec — a direction.

## The scene

A grassy island in an open ocean. Trees, flowers, blades of grass swaying. Light catches on the wet edges of leaves. The water moves. Everything is voxels.

The whole thing should feel hand-made and alive, like a diorama you can walk around inside of, but rendered efficiently enough that the world is dense — millions of plants, not dozens.

## The three rendering pillars

1. **Static geometry** — the terrain itself. Currently brickmap-palette voxels. Eventually fed by procedural heightmaps or sculpted scene representations rather than `.vox` files.
2. **Animated instanced geometry** — grass, leaves, flowers. Small flat 3D voxel textures with per-frame animation, instanced thousands to millions of times. Wind, sway, growth.
3. **Dynamic surface** — the ocean. A displaced, voxelized surface that is neither static nor pre-baked. Its own pass.

These three render in sequence into a shared lighting and post-processing pipeline. They look like they belong to the same world.

## The plant tool

A first-party tool for authoring plants: shape them, animate them, export them as flat 3D voxel textures with per-frame data. Procedural generation of novel species — variations, hybrids, biomes. Plants are content the engine can grow, not assets it has to import.

## Aesthetic notes

- Stylized, not realistic. Soft palette, painterly light.
- Voxels are the medium, not the brand. The viewer should feel the world before they notice the cubes.
- Density matters. A field of a thousand grass blades reads as a field, not a lawn of identical mesh instances.
- Motion matters. A still frame should feel like it was paused.

## What this is NOT (yet)

- Not a game engine. No gameplay, no physics, no networking.
- Not a general-purpose voxel renderer. Bespoke, focused on this scene.
- Not realtime-edited. Authoring tools may be offline and engine-adjacent.

## What "done" looks like

Standing on the island, wind blowing across the grass, ocean lapping at the shore, sun catching the petals of a flower the engine procedurally grew this morning. Sixty frames a second.
