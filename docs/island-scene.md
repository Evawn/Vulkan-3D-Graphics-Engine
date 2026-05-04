# Island Scene & Promote-to-Scene — Implementation Plan

Status: design draft, ready for implementation (open questions resolved)
Owner: Evan
Last updated: 2026-05-03

## 1. Goal & scope

Close out **Milestone 6** of `animated-voxel-import.md` ("Overlay view + promote-
to-scene") and, in the same pass, fix the long-standing rough edges in the
Scene workspace that make the round-trip story embarrassing: the island shape,
the beach/ocean rendering, and the grass-grid placement controls.

Concretely, after this milestone the user should be able to:

1. Open a `.glb` of an animated tree in **Import & Bake**.
2. See the mesh and the baked voxels superimposed via a new **Overlay** preview
   mode with a single mesh ↔ voxels crossfade slider.
3. Click **Promote to scene** — the workspace switches to **Scene**, the
   `CombinedRenderer` becomes active, and the previously-procedural foliage
   slot is now driven by the user's tree bake. One tree drops into the center
   of the island, trunk a few voxels into the ground.
4. Drag a new **Density** slider to populate the island with trees that are
   spaced relative to the asset's actual footprint, never on the beach, only
   firmly inland.
5. Look at an island that is no longer a perfect circle, with color-graded
   terrain (grass, sand, dirt, stone, and shallow underwater silt all show
   per-voxel chunky color variation) and an ocean that's opaque blue offshore
   but reveals the underwater silt-bottom near shore.

Out of scope:
- Multi-asset scenes (more than one promoted asset coexisting). The promote
  call replaces whatever instanced voxel asset the CombinedRenderer is currently
  driving.
- Loading `.vxa` files directly into Scene workspace (still routes through
  Import).
- A real fluid / wave / foam ocean. v1 is a flat shaded water surface.

## 2. Architectural framing

The work splits into four loosely-coupled chunks. Each can land as its own PR
behind the others; the order below is also the recommended landing order
because each step makes the next one demoable.

```
A. Asset-creation workflow         (Import workspace)
   └── Overlay preview mode with mesh ↔ voxels crossfade slider

B. Promote-to-scene pipeline       (Editor + RenderingSystem + CombinedRenderer)
   └── Hand baked AssetID to CombinedRenderer; switch workspace + technique;
       drop one instance at island center.

C. Instancing UI / placement       (CombinedRenderer)
   └── Density slider (replaces "Grid Side"); asset-aware spacing; beach
       exclusion; slope guard.

D. Terrain look                    (PrimitiveFactory + trace shaders)
   └── Domain-warped island shape; per-voxel color gradients across grass
       /sand/dirt/stone/subaqueous bands; depth-blended water plane that
       reveals shallow underwater silt near shore.
```

The data path that ties A → B → C is: the `AssetID` of the procedural animated
volume the import baker already populates is the *same* `AssetID` the
CombinedRenderer points its foliage trace at. No new asset-bytes pipeline is
introduced.

---

## 3. Section A — Overlay preview (asset creation workflow)

The voxel-preview shader and the skinned-mesh shader already share the
`GltfImportFrameUbo`, render to the same color/depth attachments, and run
back-to-back inside `GltfImportTechnique`. Today the technique picks ONE of
{Mesh, Voxels} per frame and registers only that pipeline's pass. Overlay
mode renders BOTH and **crossfades** between them with a single slider.

### 3.1 Crossfade semantics (the slider)

A single overlay slider `t ∈ [0, 1]`:
- `t = 0` — Mesh fully visible, voxels invisible.
- `t = 1` — Voxels fully visible, mesh invisible.
- `t = 0.5` — Both ghosted at roughly equal weight.

Implementation derives two per-pass alphas from `t`:
- `voxelAlpha = t`
- `meshAlpha  = 1 - t`

Each pass writes its own color attenuated by its own alpha and uses standard
"alpha over" blending against whatever the previous pass wrote.

### 3.2 Render order & blending

Pass order each frame in Overlay mode:

1. **Sky pre-pass** — clears color to sky, no depth write. (Already in place.)
2. **Voxel pass** — DDA into the volume. Output `vec4(litColor, voxelAlpha)`.
   Standard "src*srcA + dst*(1-srcA)" blending. **Depth write ON** (the
   voxels' depth becomes the canonical hit depth for any future passes).
3. **Mesh pass** — skinned mesh. Output `vec4(litColor, meshAlpha)`. Standard
   "alpha over" blending. **Depth write OFF**, depth test OFF (so the mesh
   shows through voxels from behind too — without depth-test off the back
   half of a tree mesh would be culled by its own front-half voxels and the
   crossfade would feel one-sided).

This isn't mathematically symmetric (the second pass's "over" blend
double-attenuates the first pass's color), but the endpoints are exact
(`t=0` → pure mesh, `t=1` → pure voxels) and the midpoint blend reads as a
clear ghost of both, which is the design intent. Trying to make it
mathematically symmetric requires either a single fragment shader doing both
trace+mesh in one pass, or premultiplied additive with a sky-coverage mask —
not worth the complexity for a preview-only effect.

### 3.3 Pipeline state — three modes, two extra pipelines

| Mode    | Voxel pipeline                          | Mesh pipeline                             |
|---------|------------------------------------------|--------------------------------------------|
| Mesh    | (not registered)                        | Opaque, depth on, write on (existing)     |
| Voxels  | Opaque, depth on, write on (existing)   | (not registered)                          |
| Overlay | Alpha-over, depth on, write on (NEW)    | Alpha-over, depth off, write off (NEW)    |

Two new pipeline variants total. Same shaders, different pipeline state. The
"Overlay voxel" pipeline can use the existing voxel shader unchanged — it
already outputs an alpha channel; we just gate on `voxelAlpha < 1` whether
to enable blending.

### 3.4 Pushing the slider value into the shaders

Both shaders need to know their per-pass alpha. Cheapest path is a push-
constant field on each:
- `VoxelPreviewDrawPC` adds `float voxelAlpha`. Currently 96 B; bumping to
  112 B (with pad) keeps it well under the 128 B PC limit.
- `SkinnedMeshPC` adds `float meshAlpha`. Currently 96 B; same bump.

In Mesh-only / Voxels-only modes the technique writes 1.0 into the
respective field — the shaders treat alpha=1 identically to today.

### 3.5 BakerPanel changes

Add a third button to the View Mode row: `[Mesh] [Voxels] [Overlay]`. In
Overlay mode reveal a single slider `Overlay Blend` labeled at the ends:
`Mesh ──────●────── Voxels`. Default 0.5. Wires to
`GltfImportTechnique::SetOverlayBlend(float)`.

### 3.6 Edge case: shadow ray inside the volume bounds

The voxel pass with shadows enabled (just landed) is correct under Overlay —
voxel shadows are computed against the volume's own occupancy; the mesh
overlay on top is decorative and isn't part of the shadow graph. No
interaction needed.

---

## 4. Section B — Promote-to-scene pipeline

### 4.1 Wire model

```
BakerPanel  "Promote to scene"
    │
    ▼
GltfImportTechnique::PromoteBakeToScene()
    │  (1) snapshot { AssetID, size, frameCount, voxelWorldSize }
    │  (2) RenderingSystem::SetPromotedFoliageAsset(snapshot)
    │  (3) Editor::SetWorkspace(Workspace::Scene)
    │  (4) RenderingSystem::RequestSwitchTechniqueByName("Combined Renderer")
    ▼
RenderingSystem
    │  applies snapshot → CombinedRenderer::AdoptPromotedFoliageAsset(snapshot)
    ▼
CombinedRenderer
    │  m_foliage_asset      ← snapshot.assetId
    │  m_foliage_size       ← snapshot.size
    │  m_foliage_frame_count← snapshot.frameCount
    │  m_use_baked_foliage  ← true
    │  m_foliage_grid_dim   ← 1                 (single tree at center)
    │  m_pending_grid_rebuild = true
    │  m_pending_shadow_topology_rebuild = true (size changed)
    │  → triggers ReloadTechnique → graph rebuild
```

### 4.2 What "promotion" actually does to CombinedRenderer

Today CombinedRenderer's foliage path is **fully procedural**:
- Owns `m_foliage_asset` (an `AssetID` for a `CreateProceduralAnimatedVoxelVolume`).
- Runs `instanced_voxel_generate.comp` every frame to write the volume image.
- Has hardcoded `m_foliage_size = (16, 32, 16)`, `m_foliage_frame_count = 8`.

After promotion, the slot is **driven by the import baker's volume**:
- `m_foliage_asset` points at the AssetID the baker registered (it's the same
  shape: a procedural animated volume whose image is updated CPU→GPU by the
  baker, not by a per-frame compute pass).
- The foliage-generate compute pass is **skipped** — `m_use_baked_foliage`
  gates it out of `RegisterPasses`.
- `m_foliage_size` / `m_foliage_frame_count` are taken from the asset.

The foliage instance trace pipeline, the shadow brickmap, and the bitmask
buffer all already key off `m_foliage_size` and `m_foliage_frame_count`, so
no shader changes are needed — only a graph rebuild because the volume image's
size changed.

### 4.3 AssetID ownership semantics — aliased

Promotion **aliases** the AssetID: both Import and Scene point at the same
underlying procedural animated voxel volume. If the user re-loads a different
GLB in Import while the Scene is using the promoted asset, the volume gets
resized in place and the Scene's trees mutate live. We accept that coupling
for v1 because it's the cheapest path and matches the "simplest/portable"
goal — no clone, no new registry method, no ownership tracking.

If/when the workflow grows to need independence, the upgrade path is a
single new `AssetRegistry::CloneVoxelVolume(srcId)` returning a fresh
`AssetID` with copied bytes. Promote would call clone instead of pass-through.
That's a one-line swap inside `PromoteBakeToScene` — designed to be deferred.

### 4.4 Workspace + technique switch

`Editor::SetWorkspace(Workspace::Scene)` already exists, and the Scene workspace
config has empty `lockedTechniqueName` (the Scene workspace doesn't pin a
technique). Promote needs to *also* request CombinedRenderer specifically, so
the technique picker lands on the right one even if the user had been on
BrickmapPalette before entering Import.

`RenderingSystem::RequestSwitchTechniqueByName("Combined Renderer")` exists;
just needs to be called after `SetWorkspace`.

### 4.5 Grid-size = 1 placement

When `m_foliage_grid_dim == 1`:
- Skip the grid loop entirely.
- Place exactly one instance at the island's geometric center.
- Look up the topmost solid voxel column under that XY (already have
  `FindTopSolidZ`).
- Sink the instance by `kPromotedSinkVoxels = 3` so the tree's trunk
  intersects the ground (no floating).
- Skip if that center column is below sea level + beach margin (rare for a
  reasonable island shape, but possible with extreme falloff settings — log
  a warning and place at the highest point on the island instead).

### 4.6 BakerPanel after promote

The BakerPanel state (loaded GLB, current bake, voxel-size slider position) is
**preserved** after promote — the bake is still in memory, and the user can
return to the Import workspace at any time to retune and re-promote. A
re-promote simply re-aliases (the AssetID is stable across Resize calls).

---

## 5. Section C — Instancing UI control upgrades

### 5.1 Replace "Grid Side" with "Density"

Today: `m_foliage_grid_dim` is the per-axis instance count (64 → 4096 trees).
This ignores asset size — placing a 16-voxel grass blade at pitch 16 is
appropriate, placing a 192-voxel tree at pitch 16 is overlap soup.

New model:
```
m_density        ∈ [0.05, 4.0]   slider, default 1.0
asset_footprint  = max(asset.size.x, asset.size.y)   in world voxels
asset_pitch      = ceil(asset_footprint / m_density)
m_foliage_grid_dim = max(1, island_size_voxels / asset_pitch)
```

So: `density = 1.0` → instances are spaced exactly one footprint apart (just
touching). `density = 0.5` → twice the spacing. `density = 4.0` → instances
overlap (good for grass, bad for trees, but the user controls it).

The inspector exposes:
- `Density` (Float, 0.05..4.0)
- `Asset Footprint (voxels)` (read-only display, derived from asset.size)
- `Computed Grid Side` (read-only display, derived)
- `Computed Pitch (voxels)` (read-only display)

The "Grid Side" int slider goes away. Internally `m_foliage_grid_dim` is still
the loop bound but it's *computed*, not user-set.

### 5.2 Beach / coast exclusion

Today: placement skips columns where `topZ < seaLevelZ + 2`. That's enough to
prevent underwater placement but the result is grass blades on the beach
band, which looks wrong (especially after we make the beach actually visible
in section D).

New criterion — column passes only if all of:
1. `topZ ≥ (seaLevel + beachWidth) * maxHeight + kInlandMarginVoxels` —
   "firmly above the beach". `kInlandMarginVoxels = 4` is the margin.
2. **Slope guard**: surface gradient magnitude < kMaxSlope. Sample the
   neighboring four columns' topZ; reject if any neighbor's topZ differs by
   more than `kMaxSlopeVoxels = 6` from this one. Keeps the placement on
   plateau-ish areas, rejects cliff-edge placements where a tree would
   visually float over the slope.
3. Surface material is `MAT_GRASS` (not `MAT_SAND`, not `MAT_DIRT`). Cheap
   re-confirmation that we're on inland terrain.

Together (1) and (2) mean trees never pop out of the beach band and never
hang off cliff edges. Both thresholds are inspector-tunable.

### 5.3 Single-instance promotion

Special-case `m_foliage_grid_dim == 1` per §4.5 above — place at island
center, height-fitted, with sink-into-ground. This is what Promote produces;
the user can drag Density up afterward to populate.

---

## 6. Section D — Terrain & water look

### 6.1 More interesting island shape

Today: pure radial smoothstep around the grid center → near-perfect circle.

Replace the radial mask with a **domain-warped** distance:
```
warp_x = noise2D(x * kWarpFreq, y * kWarpFreq, seed_a) * kWarpAmp
warp_y = noise2D(x * kWarpFreq, y * kWarpFreq, seed_b) * kWarpAmp
warped_dist = length(vec2(dx + warp_x, dy + warp_y)) / halfMin
mask = 1 - smoothstep(islandRadius, islandRadius + islandFalloff, warped_dist)
```

`kWarpFreq` ≈ 1/200 (cycles/voxel) and `kWarpAmp` ≈ 0.25 (× halfMin) gives an
irregular but bounded coast — bays, peninsulas, no detached blobs. Plateau
characteristic is preserved because the mask still feeds the *same* baseline
height; we're just deforming the level set, not the height response.

Two new `IslandTerrainConfig` fields:
- `domainWarpFreq` (default 0.005)
- `domainWarpAmp`  (default 0.25)

Both inspector-exposed under "Island Terrain".

### 6.2 Color-variation upgrade across the whole terrain

The current palette has one entry per material (grass, sand, dirt, stone) →
flat-shaded blobs that don't show off the engine's voxel-color expressiveness.
This section replaces single-entry materials with **gradient bands** —
multiple palette entries per material, selected per-voxel from a deterministic
parameter (height, depth-from-surface, distance-from-water) plus a small
per-voxel noise nudge so each band breaks up at the voxel scale.

The palette stays at one shared 256-entry RGBA8 LUT (no shader changes); we
just allocate more of those slots to gradient stops.

#### 6.2.1 Color story top-to-bottom

Reading island cross-section from sea floor to peak:

```
PEAK   ─────────────────  ALPINE GRAY     ┐
        DRY GRASS / MOSS                  │
        STANDARD GRASS                    │ Grass band
        LUSH GRASS                        ┘
   ─── BEACH-INLAND GRAD (1-voxel ripple) ─
        DRY SAND                          ┐
        BEACH SAND                        │ Beach band
        WET SAND                          ┘
   ───  SEA LEVEL  ────────────────────────
        SHORE SILT (visible thru water)   ┐
        UNDERWATER SAND / ALGAE           │ Subaqueous band
        DEEP SILT (rarely visible)        ┘
SEA FLOOR ────────────────────────────────
```

Plus the always-present subsurface materials seen in cliff faces / cuts:

```
DIRT (3 stops: warm topsoil, mid-brown loam, cool subsoil)
STONE (3 stops: noise-driven gray with subtle blue-gray and warm-tan lobes)
```

That's roughly **15 new palette entries** total (was 4: grass, sand, dirt,
stone). All deterministic from existing terrain coords — no new state.

#### 6.2.2 How the per-voxel selector works

For each surface voxel inside `BakeIslandTerrainBrickmap`, instead of picking
`MAT_GRASS` / `MAT_SAND` / etc. directly, evaluate:

```cpp
// Where in the band are we (0 at the bottom of the band, 1 at the top)?
float t = SaturateBandPosition(z, bandLow, bandHigh);

// Per-voxel jitter — value-noise sampled at (x, y, z * jitterZ) to break up
// stripes. Amplitude small enough that bands are still recognizable.
float jitter = (ValueNoise3D(x, y, z, seedColor) - 0.5f) * 0.15f;

uint8_t mat = SelectGradientStop(stops, clamp(t + jitter, 0.0f, 1.0f));
```

`SelectGradientStop` picks one of N palette indices (e.g. 4 grass stops) by
quantizing `t` into N buckets. The result: each gradient band looks like a
chunky, voxel-quantized color smear — same engine aesthetic as the existing
grass/sand foliage.

Three "bands" per axis:
- **Grass band** parameter = normalized elevation above beach top (0 = just
  above beach → lush; 1 = at peak → alpine/dry).
- **Sand band** parameter = distance-above-sea (0 = wet/dark, 1 = dry/light).
- **Subaqueous band** parameter = depth below sea (0 = just under shore →
  silt / algae green; 1 = deep → dark blue-gray).
- **Dirt** and **stone** parameters = depth below local surface, with a wider
  jitter so cliff cuts read as natural.

#### 6.2.3 Underwater terrain — actually voxelize the shallow band

Today, columns whose surface is below `seaY` are written as fully empty. The
ocean plane ends up rendered against pure sky. Two changes:

- **Voxelize the shallow underwater band** (depth ≤ `kUnderwaterMaxDepth ≈ 8`
  voxels below sea level) using the subaqueous gradient. Below that depth,
  keep skipping (saves brick budget; deep water is opaque enough to hide it
  anyway).
- The ocean plane (next subsection) becomes **depth-blended** — translucent
  over shallow underwater terrain, opaque over deep water. The shore visibly
  reads as "you can see the silt-bottom under the shallow water", which
  satisfies the "what would it look like under the water" intent.

#### 6.2.4 Visible beach — wider band by default

Beyond the gradient itself, bump default `beachWidth` from `0.04` to `0.08`
of `maxHeight` so the beach is visually present. The new wet-sand stops at
the water's edge make the transition read naturally instead of as a hard
texture boundary.

### 6.3 Visible ocean — depth-blended water plane

Render an **ocean plane** in the `combined_terrain_trace.frag` miss path
(and any "ray exits the brickmap" path, not just the primary miss). The plane
is at `z = seaY` in world space; intersect against it and:

```glsl
// Ray-plane against z=seaY
float tWater = (seaY - rayOrigin.z) / direction.z;
if (tWater > 0.0 && tWater < tTerrainHit) {
    vec3 waterHit = rayOrigin + direction * tWater;

    // Translucency: shallow → see-through, deep → opaque. tTerrainHit is the
    // distance the ray travels from the water plane to the underwater hit
    // (or +∞ if it leaves the brickmap into deep water).
    float waterDepthAlongRay = (tTerrainHit - tWater) * abs(direction.z);
    float opacity = smoothstep(0.0, kShallowDepth, waterDepthAlongRay);

    // Color: deep blue away, shallow tinted by what's under it. fresnel
    // brightens the grazing-angle horizon line so the water plane reads as
    // a surface, not a flat sticker.
    vec3 waterColor = mix(shallowColor, deepColor, opacity);
    float fres = SchlickFresnel(view, planeNormal, kF0Water);
    waterColor = mix(waterColor, frame.skyColor, fres * 0.5);

    // Blinn-Phong sun specular — sells the surface plane.
    float spec = pow(max(0, dot(reflect(view, planeNormal), -frame.sunDirection)),
                     kWaterShininess);
    waterColor += spec * frame.sunColor * frame.sunIntensity;

    // Composite onto whatever the previous pass wrote (terrain or sky).
    outColor.rgb = mix(outColor.rgb, waterColor, opacity);
    gl_FragDepth = waterPlaneClipZ;     // tree foliage in front of shore
                                        // correctly occludes water
}
```

Key points:
- `kShallowDepth ≈ 6` voxels — past that the water reads opaque. This is the
  knob that makes "underwater silt visible near shore" work.
- Fresnel and specular are kept simple (Schlick + Blinn-Phong, no PBR). The
  spec highlight uses the existing `sunDirection` / `sunIntensity` so all
  scene lighting toggles propagate.
- **No reflections, no refraction, no waves**. Confirmed out of scope. If we
  want subtle motion later, displacing the plane Z by a sin/cos of `time`
  is a one-line addition.
- A "Water Enabled" inspector bool defaults to true. Turning it off restores
  the see-through behavior — useful for debugging the underwater band.

### 6.4 Inspector rearrangement

The `CombinedRenderer` "Island Terrain" header gets:

```
Island Terrain
├── Size
├── Max Height
├── Noise Scale
├── Octaves / Lacunarity / Gain
├── Island Radius / Falloff
├── Domain Warp Freq      (new)
├── Domain Warp Amp       (new)
├── Sea Level
├── Beach Width
├── Color Jitter          (new — amplitude of per-voxel band jitter)
├── Underwater Depth      (new — voxels below seaY to voxelize)
├── Water Enabled         (new, bool)
├── Water Shallow Depth   (new — ray-distance for water opacity ramp)
├── Seed
└── Bake Island
```

---

## 7. Roadmap

Suggested PR-by-PR ordering. Each PR leaves the engine in a working,
demoable state.

### PR 1 — Overlay preview (Section A)
- `PreviewMode::Overlay` enum value (already declared in the design doc, add
  to the enum)
- Second skinned-mesh pipeline variant with alpha blending + depth-write off
- `SkinnedMeshPC.overlayAlpha` field
- BakerPanel: third view-mode button + alpha slider
- skinned_mesh.frag emits alpha
- **Demo**: load oak, hit Overlay, drag the alpha slider — voxel oak with
  ghost-mesh oak superimposed.

### PR 2 — Promote-to-scene plumbing (Section B, no terrain changes yet)
- `GltfImportTechnique::PromoteBakeToScene()`
- `RenderingSystem::SetPromotedFoliageAsset(...)`
- `CombinedRenderer::AdoptPromotedFoliageAsset(...)` + `m_use_baked_foliage`
  gate on the foliage compute pass
- Single-instance placement when `grid_dim == 1`
- BakerPanel "Promote to scene" button wired up
- Editor::SetWorkspace + RequestSwitchTechniqueByName chained
- **Demo**: bake an oak, click Promote — workspace flips to Scene, one tree
  appears at island center on the existing circular island.

### PR 3 — Asset-aware density + placement guards (Section C)
- Replace "Grid Side" with "Density" param
- Asset-footprint-driven pitch
- Beach exclusion + slope guard + grass-only material check
- **Demo**: drag Density up to 1.5 — trees populate the inland plateau
  spaced relative to their footprint, none on the beach, none on the
  cliffsides.

### PR 4 — Terrain & water look (Section D)
This is the largest of the four PRs and could optionally split into 4a
(shape + gradient palette) and 4b (water plane + underwater band) if the
diff feels too big.

- `IslandTerrainConfig.domainWarpFreq` / `domainWarpAmp` + smoothstep on
  warped distance for the new island shape
- ~15 new palette entries laid out per §6.2.1: grass (4 stops), sand (3),
  dirt (3), stone (3), subaqueous (3), alpine (1)
- Per-voxel band selector in `BakeIslandTerrainBrickmap` — `t` parameter +
  `ValueNoise3D` jitter + `SelectGradientStop`
- Voxelize shallow underwater band (≤ `kUnderwaterMaxDepth` below seaY)
  with subaqueous gradient; deeper still skipped
- Ocean plane in `combined_terrain_trace.frag` with depth-along-ray
  opacity ramp + Fresnel + Blinn-Phong
- Inspector additions per §6.4
- **Demo**: bake a new island — bays and peninsulas, visible color-graded
  beach, water that's opaque blue offshore but reveals silt-bottom near
  shore, and trees still placing correctly under the new shape.

---

## 8. Decisions on file

These were all open as of the first draft and are now settled. Captured here
so the implementation PRs don't re-litigate.

1. **Overlay** — single crossfade slider, not a mesh-only alpha. `t=0` is
   pure mesh, `t=1` is pure voxels, `t=0.5` is both ghosted. Implementation
   per §3.1–3.4.
2. **Asset ID ownership on promote** — alias for v1. Future
   `AssetRegistry::CloneVoxelVolume` is a deferred upgrade if the workflow
   ever needs independence. (§4.3)
3. **Density slider at promote** — force `grid_dim = 1` on promote so the
   user gets exactly one tree at island center. Density slider stays live;
   any movement after promote repopulates with the new density. (§4.5,
   §5.3)
4. **Beach color** — no single-entry wet-sand band; instead a broader
   color-gradient upgrade across grass, sand, dirt, stone, and a new
   subaqueous band. Per-voxel jitter inside each gradient gives the chunky
   voxel-color look the engine should be showing off. (§6.2)
5. **Underwater terrain** — voxelize the shallow band (≤ ~8 voxels below
   sea level) with a subaqueous gradient, render the water plane with
   depth-along-ray opacity, so silt-bottom reads through near shore. (§6.2.3,
   §6.3)
6. **Water surface ceiling** — flat plane with Fresnel + Blinn-Phong sun
   specular and depth-blended opacity. No reflections, no refraction, no
   waves. (§6.3)
7. **Island shape** — domain-warped single connected blob. Bays and
   peninsulas, no archipelagos / multi-lobes. (§6.1)
8. **Grass coexistence** — out of scope for this milestone. The foliage slot
   is exclusive: promoting a tree replaces the existing procedural grass
   entirely. Multi-asset instancing is a separate future project that will
   warrant its own design doc.

## 9. Future work (parking lot)

Non-goals for this milestone, captured to keep them out of scope and remind
us what's deferred:

- **Multi-asset instancing** — grass + trees + rocks coexisting on the same
  terrain. Requires a per-instance asset-ID field, fanned-out trace
  pipelines or one über-shader, and a UI surface for managing the asset
  list. The biggest deferred item; the rest of this plan keeps the door
  open without paying for it.
- **AssetRegistry::CloneVoxelVolume** — bytes-copy upgrade that converts the
  alias-on-promote to a transfer-on-promote. Drop-in replacement at the
  Promote call site. (§4.3)
- **Animated water** — sin/cos Z displacement of the water plane, possibly
  layered with a low-frequency normal map for ripples. One-line core change
  to §6.3, plus a noise sampler.
- **Reflections / refraction** — anything that requires a second trace pass.
  The water plane in §6.3 is intentionally a single-bounce surface so this
  expansion has a clean home.
- **Promoted-asset persistence** — round-trip a promoted scene to disk so
  the user can close & reopen with the tree still planted. Requires a Scene
  serialization story we don't have yet.
