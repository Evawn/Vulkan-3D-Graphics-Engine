# Editor UI Overhaul Plan

A technical companion to [VISION.md](VISION.md), [FEATURE.md](FEATURE.md), and
[OPTIMIZATION.md](OPTIMIZATION.md). The previous three documents describe
*what* the engine renders and *why* it scales the way it does. This one is
about the surface the operator sees while running it: panels, layout, theme,
profiling readouts, render-graph introspection, console.

The current editor is functional but plateaued. Six panels (Viewport,
Hierarchy, Inspector, Metrics, Render Graph, Output) are already wired
through a docking layout in
[GUIRenderer.cpp](../src/editor/GUIRenderer.cpp) with a custom VSCode-dark +
matrix-green theme in [UIStyle.cpp](../src/editor/UIStyle.cpp). What's
missing isn't *new wiring* — it's the visualization quality and the
information density that turn the editor from "looks like the default
ImGui demo" into a tool you reach for instead of running with a profiler
attached.

This document scopes the overhaul before any code changes, in the same
shape as OPTIMIZATION.md: tier-by-tier so each tier is independently
shippable.

---

## 1. The framing

### 1.1 What's broken (concretely)

  - **The Inspector is a junk drawer.** Camera, technique selector,
    lighting, sky, post-process, selected-node properties, and screenshot
    button all live in one panel
    ([InspectorPanel.h:16-58](../src/editor/panels/InspectorPanel.h#L16-L58)).
    Eight collapsing headers stacked vertically; nothing groups
    semantically; finding any one knob takes scrolling.
  - **"Metrics" is three panels in a trench coat.** Frame perf
    ([MetricsPanel.cpp:31-58](../src/editor/panels/MetricsPanel.cpp#L31-L58)),
    draw-call stats
    ([MetricsPanel.cpp:61-78](../src/editor/panels/MetricsPanel.cpp#L61-L78)),
    and VMA memory
    ([MetricsPanel.cpp:81-122](../src/editor/panels/MetricsPanel.cpp#L81-L122))
    are unrelated concerns sharing one window. Each is shallow because
    the others are competing for vertical space.
  - **The render graph view is a flat table.** Pass introspection in
    [RenderGraphPanel.cpp:46-115](../src/editor/panels/RenderGraphPanel.cpp#L46-L115)
    is a one-row-per-pass table. The graph *is a DAG* — VISION.md §5.1
    calls it out as a Kahn's-algorithm topological sort over a real
    edge set — and we render none of the edges. Pass dependencies, queue
    affinity, async-compute fan-out, and barrier handoff are all
    invisible.
  - **The Output panel is level-flat.** Five level toggles
    ([OutputPanel.h:11-15](../src/editor/panels/OutputPanel.h#L11-L15))
    and a Clear button. No error count, no logger-name filter, no
    search, no source-line jump, no severity sticky-banner when an
    error is fresh, no auto-pause on error. `LogEntry` already carries
    `logger_name` ([ImGuiLogSink.h:8-12](../src/editor/ImGuiLogSink.h#L8-L12))
    but the panel ignores it.
  - **Per-pass timing is collected but barely used.**
    `GPUProfiler::PerformanceMetrics::passTimesMs`
    ([GPUProfiler.h:108-112](../src/utils/GPUProfiler.h#L108-L112)) is
    a `std::vector<float>` of *the latest sample only*. We have no
    history per pass, no min/max/avg, no stacked timeline, no warning
    when a pass spikes above its budget.
  - **VMA visualization is heap totals only.** A `vmaGetHeapBudgets` +
    `ProgressBar` with usage/budget per heap. We don't surface
    per-allocation breakdowns, bytes-by-resource, peak residency, or
    fragmentation — VMA's `vmaCalculateStatistics` returns all of it
    and we already call it
    ([MetricsPanel.cpp:99-101](../src/editor/panels/MetricsPanel.cpp#L99-L101)).
  - **Scrolling is too hot.** ImGui defaults `io.MouseWheel` to ~5 lines
    per notch at body-font size; in dense panels (Output, Inspector)
    that's 50+ pixels per click and content blows past the visible
    region. Nothing in [GUIRenderer.cpp](../src/editor/GUIRenderer.cpp)
    rescales it.
  - **There is no menu bar, no status bar, no toolbar.** Global actions
    (screenshot, viewport-only mode, layout reset, technique cycle) are
    buried inside the Inspector. Frame counters, FPS, error badges have
    nowhere to live persistently.

### 1.2 What "good" means

The bar is set by professional DCC and engine editors — not "looks like
Unreal" but "looks like a thing engineers built for engineers, where
every pixel earns its place." Specifically:

  - **A glanceable surface.** Frame time, FPS, error count, current
    technique, mode (viewport-only / dev) are visible without opening
    a panel. Status bar plus menu bar.
  - **Specialist panels.** Each panel is good at one thing and
    visualizes its data densely. Performance is not Memory, Render
    Graph is not Resources, Console is not Inspector.
  - **The render graph is a graph.** Nodes-and-edges canvas, not a
    table. Topological levels stack horizontally, queue streams on
    different rows, barriers shown as edge decorations. Click a node
    → drill into pass detail.
  - **Memory is comprehensible.** Stacked-area history of bytes resident
    per heap, treemap-or-list breakdown by allocation, per-resource
    bytes attributable to render-graph images / buffers / techniques.
  - **The console knows about errors.** Error count badge in status bar
    and in the panel header; first-error sticky banner; per-source
    filtering (the `App` / `Rendering` / future `RenderGraph` /
    `Substrate` loggers); search; severity-sorted view; jump-to-source
    where applicable.

### 1.3 Non-goals

  - **Theming choice.** The matrix-green-on-VSCode-dark identity stays.
    This document tightens its application, not replaces it.
  - **A new UI framework.** ImGui + DockBuilder is fine. The overhaul
    works within Dear ImGui (plus `imgui_internal` and a tiny
    node-graph helper); no port to ImNodes-as-third-party-dep, no port
    to Slint/QT.
  - **Authoring UI.** The plant tool ([VISION.md §4.1](VISION.md))
    is its own surface and lives in a future doc. This overhaul is
    about the **runtime / dev** editor.

---

## 2. Visual identity

### 2.1 Keep

  - **Palette.** `kBg`/`kBgLight`/`kBgLighter`/`kBgActive` +
    `kAccent`/`kAccentDim` from
    [UIStyle.h:18-27](../src/editor/UIStyle.h#L18-L27). Already
    cohesive.
  - **Inter font at 11/13/14.**
    [UIStyle.cpp:18-44](../src/editor/UIStyle.cpp#L18-L44). Inter
    renders well at editor sizes; the three-tier system gives
    body/header/detail without a fourth.
  - **Compact spacing.** `WindowPadding(8,6)`,
    `FramePadding(6,4)` — VSCode-density, not Unity-density.

### 2.2 Add

  - **A monospace font for numerics.** A second font (JetBrains Mono or
    IBM Plex Mono) loaded into the atlas at the same three sizes,
    surfaced as `UIStyle::FontMono*()`. Used for FPS readouts,
    timestamps, byte counts, hex hashes — anything where digit
    alignment improves scannability. Without this, the timing tables
    in §4.1 and §4.3 will jitter.
  - **A semantic color set beyond level colors.** Currently the only
    semantic colors are spdlog-level colors
    ([UIStyle.cpp:51-61](../src/editor/UIStyle.cpp#L51-L61)). The
    overhaul adds:
      - `kBudgetGood` / `kBudgetWarn` / `kBudgetOver` (greens →
        amber → red) for budget bars (frame time, heap usage).
      - `kQueueGraphics` / `kQueueCompute` for render-graph nodes —
        existing `Compute = green, Graphics = blue`
        ([RenderGraphPanel.cpp:108-110](../src/editor/panels/RenderGraphPanel.cpp#L108-L110))
        formalized as `UIStyle::kQueue*` constants.
      - `kResourceImage` / `kResourceBuffer` / `kResourceImported` for
        graph-canvas resource ports.
  - **Iconography.** A small icon font (or single packed PNG sprite-
    sheet sampled by ImGui) for the menu bar, status bar, and toolbar.
    Codicons (VSCode's MIT-licensed icon set) is the natural fit and
    matches the visual ancestry. Without iconography the menu/status
    bars look like a dumb terminal.
  - **A consistent header treatment.** The panels currently use
    `CollapsingHeader` with default ImGui styling. Replace with a
    shared `UIStyle::SectionHeader(label)` helper that renders a
    full-width amber separator with bold label, and drop the
    collapsing affordance for sections that never collapse in
    practice. (Collapsing stays where it earns its keep — long pass
    detail, multi-light setups.)

### 2.3 Drop

  - **`InvisibleButton`-as-splitter.** Already not used; ImGui's
    DockBuilder splitters are correct. Just confirming we don't
    backslide into hand-rolled splitters.

---

## 3. Layout

The current dockspace layout
([GUIRenderer.cpp:47-95](../src/editor/GUIRenderer.cpp#L47-L95))
is a 2-column root with viewport top-left / output bottom-left / metrics
+ render-graph tabs top-right / inspector bottom-right. The overhaul
keeps the *idea* (viewport-dominant, panels around it) and rebalances
which panels live where.

### 3.1 The new shell

```
+----------------------------------------------------------------+
|  Menu Bar  [File] [View] [Render] [Tools] [Help]    [⛶][⤓][▶] |   ← top bar
+----------------------------------------------------------------+
|                                              |  Hierarchy      |
|                                              |  ───            |
|                                              |  scene tree     |
|                                              +-----------------+
|              Viewport                        |  Inspector      |
|              (no tab bar)                    |  ───            |
|                                              |  selected node  |
|                                              |  / scene props  |
|                                              |                 |
+--------------------------------+-------------+-----------------+
|  Console     [errors: 0] [⏵]   |  Performance / Memory /       |
|  ──────                        |  Render Graph (tabs)          |
|  log lines, filterable,        |  ──────                       |
|  searchable                    |  active panel content         |
+----------------------------------------------------------------+
|  status: 144 fps · 6.9 ms · gfx · 1.2 GB · 0E 2W · master @abc1 |   ← bottom bar
+----------------------------------------------------------------+
```

Differences from today:

  - **Top bar (menu + toolbar).** Not currently present. Provided by
    `ImGui::BeginMainMenuBar` (full-viewport, not docked). Hosts:
      - File: New scene, Open .vox, Save layout, Reset layout, Quit.
      - View: Viewport-only toggle, OS fullscreen, individual panel
        show/hide (replaces today's hidden viewport-only-mode-only
        toggle), DPI scale override.
      - Render: Cycle technique (was in Inspector), reload shaders
        (was in Inspector), capture screenshot (was in Inspector).
      - Tools: Open command palette (§5.2).
      - Help: Repo link, key reference.
      - Right-side icon group: viewport-only, OS fullscreen, play/pause
        (for animated content), capture.
  - **Hierarchy moves to top-right; Inspector below it.** Today
    Hierarchy lives in the right-bottom of the right column above
    Inspector
    ([GUIRenderer.cpp:88-92](../src/editor/GUIRenderer.cpp#L88-L92)) —
    we keep that. The change is the right column gets the same
    proportional split as today (40/60 ratio per
    [GUIRenderer.cpp:77](../src/editor/GUIRenderer.cpp#L77)) but now
    Hierarchy sits *above* a slimmed Inspector (§4.5).
  - **Bottom-right becomes the analytics tab group.** Performance,
    Memory, Render Graph, Resources tabs share the bottom-right slot.
    Today, Metrics/RenderGraph cohabit a tab group in top-right.
    Moving them to bottom-right gives them more horizontal pixels
    (graph canvas wants width) and reserves the top-right for
    Hierarchy + Inspector (the editing surface).
  - **Console replaces Output in bottom-left.** Same dock slot, much
    deeper panel (§4.4). Header carries error/warn count badges that
    mirror the status bar.
  - **Status bar.** Not currently present. Provided by an ImGui
    `BeginViewportSideBar(ImGuiDir_Down)` (or a manually-positioned
    bottom child window). Hosts at-a-glance numerics in monospace
    font: frame ms, FPS, GPU stream queue (gfx vs async), live VRAM
    usage rounded to GB, error/warn counts, current git
    branch+sha, current technique. Click a status segment → opens the
    panel that owns it.

### 3.2 Layout persistence

Today `UIState` ([UIState.h](../src/editor/UIState.h)) carries panel
sizes in pixels and `viewport_only` and `layout_dirty`. The overhaul
extends it to include **named layout presets**: "Performance" (fullscreen
viewport + status bar only), "Develop" (the default shell above), and
"Profile" (viewport hidden, render-graph + performance + memory
maximized). The `View > Layout` submenu cycles them. `imgui.ini` already
serializes window positions; the preset just bumps `layout_dirty` after
calling a preset-specific `BuildLayout()`.

This earns its weight because the same physical workstation has
different jobs — driving the demo at full-screen, debugging a frame
spike, hunting a memory leak — and rebuilding the layout by hand each
time is friction.

### 3.3 What this is NOT

  - **Not a tool palette / properties / sidebar / dockable-anywhere
    setup à la Blender.** ImGui's DockBuilder supports it but the cost
    is layout fragility. We commit to a *small number of named
    layouts* and let the user resize within them.
  - **Not a multi-window setup.** ImGui viewports support tear-out
    windows; we don't enable it. The bar is "looks pro on one
    monitor"; multi-monitor is a later concern.

---

## 4. Panel-by-panel

### 4.1 Performance panel (split out from Metrics)

**File:** new
[`src/editor/panels/PerformancePanel.h/.cpp`](../src/editor/panels/).
Inherits today's frame/GPU history arrays.

The panel's job is **answering "where did the frame go?"** in two
levels of zoom: (a) the rolling whole-frame history, (b) the per-pass
breakdown for the current frame.

Sections:

  - **Frame Budget Strip.** A wide horizontal bar at the top, segmented
    into colored regions sized by per-pass `passTimesMs[]`. Hovering
    a segment highlights the corresponding row in the pass table. The
    16ms / 33ms budget lines drawn as vertical guides; segments past
    16ms tint amber, past 33ms tint red. Replaces the bare
    `PlotLines("##frametime")` from
    [MetricsPanel.cpp:49-57](../src/editor/panels/MetricsPanel.cpp#L49-L57).
  - **Frame Time History.** The 120-frame `m_frame_times` ring buffer,
    rendered as a stacked area chart (one band per pass, colored to
    match the budget strip). Today we have one flat `PlotLines`; the
    stacked version requires retaining per-pass history (§4.1.1).
    Min/avg/max/p99 numerics in the top-right corner.
  - **Pass Timing Table.** Sortable table of every pass with columns:
    name, queue (gfx / async), avg ms (last 60 frames), max ms, %
    of frame, sparkline (per-pass mini history). Click row → highlights
    in budget strip and selects pass in Render Graph panel. This is
    the workhorse table that
    [RenderGraphPanel.cpp:57-115](../src/editor/panels/RenderGraphPanel.cpp#L57-L115)
    is *trying* to be but currently lacks history.
  - **Recompute Counters.** The compute-pass-only counter that VISION.md
    §5.1 calls out — "is the volume regenerate compute pass running
    every frame?". Each compute pass declares whether it ran or was
    skipped this frame; counter shows runs/sec.

**4.1.1 Required GPUProfiler change.** Today
[`GPUProfiler::PerformanceMetrics`](../src/utils/GPUProfiler.h#L108-L112)
returns the latest `passTimesMs[]`. The Performance panel needs **one
ring buffer per pass** keyed by pass name (not index — pass index can
shuffle on graph rebuild). Two options:

  - **Owned by GPUProfiler.** Cleanest; profiler holds an
    `unordered_map<string, RingBuffer<float, 120>>` and
    `GetMetrics()` populates it. Cost: name lookup per pass per frame.
  - **Owned by PerformancePanel.** Profiler stays vector-of-floats;
    the panel does the per-frame copy into its own per-pass history.
    Cost: panel needs the snapshot's pass-name list (already has via
    `m_snapshot`).

V1 is option B (panel-side). Profiler stays a one-shot reader; history
is a UI concern.

### 4.2 Memory panel (split out from Metrics)

**File:** new
[`src/editor/panels/MemoryPanel.h/.cpp`](../src/editor/panels/).

The panel's job is **answering "where did the bytes go?"** at two
scales: (a) heap-level resident, (b) per-resource attribution.

Sections:

  - **Heap Strip.** Today's per-heap `ProgressBar`
    ([MetricsPanel.cpp:105-115](../src/editor/panels/MetricsPanel.cpp#L105-L115))
    is fine as a starting point; the upgrade is (1) coloring by
    `kBudgetGood/Warn/Over` thresholds (e.g. >80% → amber, >95% →
    red), (2) showing budget *headroom* in monospace next to the bar,
    (3) tracking the high-water mark and rendering it as a tick on
    the bar.
  - **Resident Bytes History.** A stacked-area chart over the 120-frame
    history, one band per heap (or per attribution group; see below).
    Same shape as Performance's frame-time chart; the value is
    spotting leaks (monotonic growth) and spikes (allocation churn)
    visually instead of by reading the live counter.
  - **Render-Graph Resources Bytes.** Walk
    `m_snapshot->images` and `m_snapshot->buffers`; for each, compute
    bytes from `ImageDesc` (width × height × depth × samples ×
    bytes-per-pixel) and `BufferDesc::size`; render as a sortable
    table with columns: name, size, source (transient / imported),
    lifetime (transient / persistent), pass writers. This is the
    single biggest information unlock that
    [RenderGraphTypes.h:261-266](../src/rendering/RenderGraphTypes.h#L261-L266)
    enables — every graph resource's footprint, attributable to its
    declaring pass — and the current panel doesn't render it.
  - **Top Allocations.** VMA's `vmaCalculateStatistics` already returns
    `total.statistics.allocationCount` and per-heap stats. We extend
    by walking `vmaGetAllocationInfo` over a tracked allocation list
    (the engine's wrappers all create through VMA; intercepting at
    `VWrap::Buffer::Create` / `VWrap::Image::Create` to stash the
    allocation handle + a `name` is one-day work). Top-N table:
    name, size, type (image / buffer), heap, age.
  - **Memory Map (stretch).** A horizontal bar per heap, colored
    rectangles per top-level allocation, scaled to bytes. Treemap
    layout. ImGui's draw list handles this; no extra dep. This is
    the visualization that maps "comprehendable memory" cleanest in
    the user's words.

**4.2.1 Allocation tracking.** VMA already names allocations if we set
`pUserData` (or use `vmaSetAllocationName`). We don't today. The
overhaul extends `VWrap::Buffer::Create` / `VWrap::Image::Create` to
accept an optional `const char* debugName`, plumbed through to
`vmaSetAllocationName`. Without names every allocation is anonymous in
the table.

### 4.3 Render Graph panel (overhaul)

**File:** existing
[`RenderGraphPanel.h/.cpp`](../src/editor/panels/RenderGraphPanel.h),
mostly rewritten.

The panel's job is **showing the DAG, not just the pass list.** Two
modes, switchable by a tab/segmented control at the top:

  - **Graph Mode (default).** Node-link canvas, top-down or
    left-to-right. Topological layers (Kahn levels — VISION.md §5.1)
    stack along the major axis; queue streams stack along the minor
    axis (graphics row, async-compute row). Each node is a
    rounded-rect ImGui draw-list block with: pass name, queue color
    band on the left edge, GPU time in monospace. Edges are drawn
    from each pass's writer-image-handles to the next consumer's
    reader-handles, colored by resource type (image vs. buffer) and
    annotated with the originating handle's name. Cross-stream
    handoffs — the semaphore edges between gfx and async-compute
    described in
    [RenderGraphTypes.h:192-198](../src/rendering/RenderGraphTypes.h#L192-L198) —
    drawn as dashed lines. This visualization needs no third-party
    library; ImGui's `ImDrawList::AddBezierCubic` and `AddRectFilled`
    cover it.
  - **Table Mode.** Today's flat table at
    [RenderGraphPanel.cpp:57-115](../src/editor/panels/RenderGraphPanel.cpp#L57-L115),
    kept for accessibility and quick scanning. Sort by record order,
    by GPU time, by queue.

The selected-pass detail (today's
[RenderGraphPanel.cpp:119-201](../src/editor/panels/RenderGraphPanel.cpp#L119-L201))
becomes a side pane that tracks selection in either mode. Inputs,
outputs, attachments, barriers, accepted item types — all useful, all
kept. Add: per-pass GPU-time sparkline (drawn from §4.1.1's history),
per-pass enabled-this-frame indicator, and a "jump to pass source" link
that opens the pass's declaring file (the technique's `Build()` call
site) — IDE integration is `system()` + `code -g file:line`, low-tech
fine.

**4.3.1 Required graph-introspection extensions.** The current
`GraphSnapshot`
([RenderGraphTypes.h:261-266](../src/rendering/RenderGraphTypes.h#L261-L266))
exposes nearly everything we need. The two missing pieces:

  - **Topological level per pass.** Today implicit (record order is
    one valid topo order, but we want the *levels*, i.e. "depth in
    the DAG", to lay nodes horizontally). DAGBuilder already knows
    this — exposing it on `PassInfo` as `uint16_t topoLevel` is one
    line.
  - **Queue affinity per pass.** Already in
    [RenderGraphTypes.h:77-86](../src/rendering/RenderGraphTypes.h#L77-L86)
    (`QueueAffinity`) but `PassInfo` doesn't carry it through to the
    snapshot. Adding `QueueAffinity affinity` to `PassInfo` is also
    one line.

Both changes are additive; no caller breaks.

### 4.4 Console panel (replaces Output)

**File:** rename
[`OutputPanel.h/.cpp`](../src/editor/panels/OutputPanel.h) to
`ConsolePanel.h/.cpp` (the editor's terminology should match what
operators expect; the existing name was fine but is being upgraded).

Sections in the panel header (single row of compact controls,
left-to-right):

  - **Search box.** `InputTextWithHint` for substring/regex match. The
    existing `m_entries` deque is small enough (2000 caps; 5000
    after lifting) to filter linearly per frame.
  - **Level filters.** Today's T/D/I/W/E toggles
    ([OutputPanel.cpp:38-47](../src/editor/panels/OutputPanel.cpp#L38-L47))
    kept; restyled as segmented buttons not separate small buttons.
  - **Source filter.** Multi-select dropdown driven by the unique set
    of `entry.logger_name` values seen so far. `ImGuiLogSink` already
    captures `logger_name`
    ([ImGuiLogSink.h:8-12](../src/editor/ImGuiLogSink.h#L8-L12)); the
    panel just needs to track distinct names and present checkboxes.
    Defaults: all sources on.
  - **Counts.** Persistent `errors: N` / `warnings: M` badges in the
    panel title bar (and in the status bar; §3.1). Counters are
    cumulative since Clear; clicking a count filters to that level.
    The user's specific ask. Implementation: `ImGuiLogSink` exposes
    counters and the panel reads them.
  - **Auto-scroll.** Today's checkbox kept.
  - **Pause-on-error.** New toggle; when enabled, the first error after
    Pause-on-error was set causes the panel to stop auto-scrolling and
    show a sticky banner with the error message until dismissed.
  - **Clear.**

Body changes:

  - **Group consecutive identical messages** as `[xN] message` — a
    very common spam pattern is "validation layer warning each frame";
    the dedup makes the rest of the log readable. Detection: if
    `entry.message == previous.message && entry.level == previous.level`,
    increment a count on the previous instead of pushing a new entry
    (track within the panel; doesn't disturb the sink).
  - **Render the source name.** Today
    [OutputPanel.cpp:66](../src/editor/panels/OutputPanel.cpp#L66)
    formats `[loggername] message` as one-string and colors by level
    only. New: `[loggername]` rendered in `kTextDim`, message in
    level color, separated. Time prefix in monospace if a `kShowTime`
    toggle is on.
  - **Click-to-jump (where messages carry source-line info).** Spdlog
    `log_msg` carries `source_loc` if compiled with the macro
    helpers. We don't use them today; lifting this is a separate
    initiative — flag it so the Console is *ready* for it without
    requiring it.

**4.4.1 Required ImGuiLogSink extensions.**

  - Add running counters: `std::atomic<size_t> m_warn_count{0},
    m_err_count{0}`, incremented in `sink_it_` and exposed via
    `GetCounts()` getters. Reset on `Clear()`.
  - Add `MAX_ENTRIES` lift from 2000 → 5000. Today's cap is fine
    short-term but we anticipate a chattier Render Graph compile log
    (per-pass barrier reasoning, per-resource bytes attribution
    debug) and want headroom.

### 4.5 Inspector panel (slim)

**File:** existing
[`InspectorPanel.h/.cpp`](../src/editor/panels/InspectorPanel.h),
restructured.

Today's inspector is one panel × eight collapsing headers (Camera,
Technique, Lighting, Sky, Post-Process, Selected Node, App Controls,
Screenshot) — eight unrelated concerns in vertical sequence. The new
shape is **tabs by audience**:

  - **Selected.** Selected-node properties (driven by Hierarchy
    selection, today's `SetSelectedNode` plumbing
    [InspectorPanel.h:52](../src/editor/panels/InspectorPanel.h#L52)
    is kept). Empty-state when nothing selected: "Select a node in
    Hierarchy."
  - **Scene.** Lighting + Sky + Post-Process. These are scene-level
    properties; they cohabit one tab.
  - **Camera.** Camera position, target, FOV, sensitivity, speed.
  - **Settings.** Active technique selector, reload-shaders button,
    DPI override, screenshot, app-level toggles. These are
    "configuration of the editor + renderer", not properties of
    anything in the scene.

Tabs are `ImGui::BeginTabBar` at the top of the panel, content per
tab uses today's `CollapsingHeader`s within where it earns nesting
(e.g. lighting has sub-sections per light).

The "Settings > Active technique" control gets a second life: the
top-bar's `Render > Cycle Technique` (§3.1) is the same action with a
hotkey.

### 4.6 Hierarchy panel (light touch)

Today's `HierarchyPanel` is reasonable — a collapsible tree of
`SceneNode`s with selection routing to Inspector. The overhaul touches:

  - **Search box** at the top filtering by node name.
  - **Type icons** (mesh / instanced cloud / brickmap / light / camera)
    sourced from the icon font. Without these the tree is identical
    rows of text.
  - **Right-click context menu** on a node: Rename, Delete, Duplicate,
    Focus camera. None of these exist today; "Focus camera" alone is
    high-value and easy.

### 4.7 Viewport panel (HUD overlays)

The viewport is mostly fine — it renders the scene texture. The
addition is **unobtrusive HUD overlays**:

  - **Top-left:** small frame-time / FPS readout in monospace, dimmed.
    Same value the status bar shows, but inside the viewport so
    full-screen mode doesn't lose it. Toggle in View menu.
  - **Top-right:** crosshair / camera-axis gizmo. Tiny RGB axis
    indicator showing world axes from camera POV — standard DCC
    affordance, ImGui draw-list trivial, makes orientation obvious
    without clicking around.
  - **Bottom-left:** current technique name. So full-screen demo
    captures show what's running.

All overlays are click-through (`ImGuiWindowFlags_NoMouseInputs`).

---

## 5. Cross-cutting

### 5.1 Scrolling sensitivity

The user-reported issue. Diagnosis: ImGui's default `io.MouseWheel` is
multiplied by `style.MouseWheelScrollRatio` (5.0) and applied as
`5 × FontSize` pixels per notch — at our 13px body font that's 65px
per notch, and on macOS Magic-Mouse-style smooth-scroll input the
events arrive at high frequency. Compounding into runaway scrolling.

Fix in [GUIRenderer.cpp](../src/editor/GUIRenderer.cpp) initialization:

  - Set `io.ConfigInputTrickleEventQueue = true` (already the default
    in modern ImGui; explicit for clarity).
  - At the top of each frame, scale `io.MouseWheel *= 0.4f` (or drive
    by a `UIState::scroll_sensitivity` setting exposed in
    Settings tab). Keep `MouseWheelH` unscaled.

This is a one-line change and the most directly addressable user
complaint.

### 5.2 Hotkeys & command palette

  - **Hotkeys.** Centralized hotkey table read at editor init. F1 →
    toggle viewport-only, F2 → cycle technique, F5 → reload shaders,
    F11 → OS fullscreen, F12 → screenshot, Ctrl-Shift-P → command
    palette, Ctrl-` → focus console, Esc → unfocus camera. Today
    these are partly done in app code; consolidate into a `Hotkeys.h`
    that both the editor and the menu bar reference (so menu items
    show their hotkey on the right, like a proper editor).
  - **Command palette.** Ctrl-Shift-P opens a fuzzy-searchable
    overlay listing every menu action by display name. Implementation:
    a single `ImGui::Begin` modal centered on viewport, an input
    box, and a substring-filtered list of registered commands.
    Commands are registered via a `CommandRegistry::Register(name,
    fn)` API that the menu bar code populates.

The command palette is the second-biggest information unlock after
the DAG view: *every* action in the editor becomes keyboard-reachable
without memorizing where it lives in the menu. Same energy as VSCode's,
modest implementation.

### 5.3 Status bar

Already covered structurally in §3.1. Implementation note: ImGui's
`BeginViewportSideBar` accepts a height and a direction; it gives a
docked strip outside the dockspace. Each segment is an ImGui Selectable
with a fixed-width child; clicking opens the relevant panel. Updated
once per frame from the same data sources panels read.

### 5.4 DPI handling

Today [`Editor::OnDpiChanged`](../src/editor/Editor.cpp#L146-L151)
reloads fonts and re-applies style; this is correct. The overhaul
preserves this and adds: DPI override slider in Settings tab (some
users want the editor at 1.25× the OS DPI; nothing currently allows
that without changing OS settings).

---

## 6. Implementation phasing

Tier-by-tier so each tier is shippable and the editor is *better* at
every checkpoint, not strictly *complete*.

### Tier 1 — Frame 1 wins (one-to-three day items)

These are direct, mostly-mechanical improvements that the user feels
immediately.

  1. **Scroll sensitivity fix.** §5.1, one line in
     [GUIRenderer.cpp](../src/editor/GUIRenderer.cpp). 5 minutes.
  2. **Split Metrics into Performance and Memory.** §4.1, §4.2 — file
     creation only; no new metrics yet, just isolate the existing
     code into two panels and fix the dock layout. 1 day.
  3. **Console error/warn counters.** §4.4 — `ImGuiLogSink` counters
     + panel header badges. 0.5 day.
  4. **Console source filter, search, dedup.** §4.4 — all panel-side
     logic over existing `LogEntry` data. 1 day.
  5. **Inspector tabs.** §4.5 — restructure today's collapsing
     headers into 4 tabs. 0.5 day.
  6. **Top menu bar + status bar.** §3.1, §5.3 — scaffolding only,
     one entry per menu, status bar reads existing perf counters.
     1 day.
  7. **Monospace font + numerics restyle.** §2.2 — second font into
     atlas, surface accessor, retrofit the perf and memory numerics.
     0.5 day.
  8. **Hotkeys consolidation.** §5.2 hotkey portion only. 0.5 day.

**Tier 1 total: ~5–6 days.** At the end the editor looks and feels
*professional* even though the new visualizations aren't in yet.

### Tier 2 — Visualizations (two-to-five day items)

These are the visual upgrades that justify the overhaul.

  9. **Render Graph DAG canvas.** §4.3 — node-link rendering using
     ImGui draw-list. Topological-level extension to `PassInfo`
     (§4.3.1). 3–4 days.
  10. **Per-pass GPU time history.** §4.1.1 — panel-side ring buffers
      keyed by pass name. 1 day.
  11. **Frame budget strip.** §4.1 — segmented bar with per-pass
      colors and budget guides. 1 day.
  12. **Stacked frame-time area chart.** §4.1 — stacked draw over
      per-pass history. 1 day.
  13. **Render-graph resource bytes table.** §4.2 — walk
      `GraphSnapshot::images/buffers`, derive bytes, render. 1 day.
  14. **VMA allocation naming + Top-N table.** §4.2.1 — plumb
      `debugName` through `VWrap::Buffer/Image::Create`; panel
      reads via `vmaGetAllocationInfo`. 2–3 days.
  15. **Command palette.** §5.2. 1 day.
  16. **Layout presets.** §3.2. 0.5 day.

**Tier 2 total: ~10–12 days.** At the end the editor is doing
*everything in the user's ask*: comprehensible memory, comprehensible
pass timing, an actual DAG view, organized error tracking.

### Tier 3 — Polish (later, opportunistic)

  17. **Memory treemap.** §4.2 stretch. 2 days.
  18. **Hierarchy icons + context menu.** §4.6. 2 days.
  19. **Viewport HUD overlays.** §4.7. 1 day.
  20. **Spdlog source-loc → click-to-jump in Console.** §4.4 final
      bullet. 2 days; requires audit of every log call site.

Tier 3 items individually small; bundle as you'd like.

---

## 7. Open questions

  - **Icon font choice.** Codicons fits the visual ancestry but is
    a subset; ForkAwesome or Material Symbols are richer. Not
    pinned; punt to Tier 1 step 6 when the bar is rendered.
  - **DAG canvas: build vs. import a node-graph lib.** The plan says
    build — `ImDrawList` is enough for a passive (read-only,
    no-rearrange) DAG view. If we ever want interactive
    rewire / drag-to-disable, ImNodes is the obvious dependency. Not
    needed for v1; flagged so we don't half-build something we'd
    have to throw away.
  - **Per-resource bytes attribution for transient images.** Easy
    when each transient image is one VMA allocation. The graph today
    doesn't *necessarily* allocate one VMA block per image; if
    aliasing is later added (transient images sharing memory across
    non-overlapping lifetimes), the bytes-by-resource view becomes
    "bytes by lifetime range" and the table layout needs revisiting.
  - **Status-bar git info.** `git rev-parse --short HEAD` at app
    start is fine for branch+sha; refreshing on commit is overkill.
    Pinning the value at startup is the easy answer.
  - **Console performance with deep history.** 5000 entries × full
    re-filter every frame is fine at 1k log lines/sec; if log volume
    grows the panel needs an indexed cache (filtered indices, dirty
    flag, recompute on filter change only). Defer until measured.

---

## 8. What this is NOT

  - **Not a port.** Stays Dear ImGui + DockBuilder. No QT, no Slint,
    no web view.
  - **Not a designer's redesign.** This is an engineer's restructure
    of an engineer's tool. The aesthetic stays VSCode-density; the
    bar is "looks like a thing built by someone who shipped" not
    "looks like a Figma mock."
  - **Not the editor's last overhaul.** When the plant tool
    ([VISION.md §4.1](VISION.md)) lands it gets its own surface and
    its own layout preset. This document is about getting the **dev
    editor** to first-class so the authoring editor has a baseline
    to extend, not replace.

---

## 9. Reading order

  1. [VISION.md](VISION.md) §5.1 (the render graph) and §3 (the
     substrate) — the architecture this UI surfaces.
  2. [OPTIMIZATION.md](OPTIMIZATION.md) §3.4 (GPU timestamps) —
     the data plumbing the Performance panel reads.
  3. This document — what the UI becomes.
  4. The code, starting with
     [Editor.cpp](../src/editor/Editor.cpp),
     [GUIRenderer.cpp](../src/editor/GUIRenderer.cpp),
     [UIStyle.cpp](../src/editor/UIStyle.cpp), then the panel
     subdirectory.
