# Build Versioning

How to choose, switch, and configure CMake build types for this engine.
Reference doc — cross-link from `README.md` if you ever onboard another contributor.

Last updated: 2026-05-02

## TL;DR

```bash
# One-time setup — two parallel build dirs
cmake -DCMAKE_BUILD_TYPE=Debug   -S . -B build-debug
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build-release

# Day-to-day
cmake --build build-debug   && ./build-debug/src/VulkanEngine     # for development
cmake --build build-release && ./build-release/src/VulkanEngine   # for "is it actually fast?"
```

## Build types — what each one does

CMake's single-config generators (Make, Ninja — what this project uses) bake
the optimization + symbol flags into the `.o` files at compile time. The build
type is chosen by `-DCMAKE_BUILD_TYPE=<Type>` at configure time and persists
in that build directory's `CMakeCache.txt`.

| Type             | Compiler flags          | NDEBUG | Asserts | Validation layers† | Use for |
| ---------------- | ----------------------- | ------ | ------- | ------------------ | ------- |
| `Debug`          | `-g -O0`                | no     | on      | **on**             | Iterating, debugger, validation hunts |
| `Release`        | `-O3 -DNDEBUG`          | yes    | off     | off                | Demoing, perf measurement |
| `RelWithDebInfo` | `-O2 -g -DNDEBUG`       | yes    | off     | off                | Profiling, "the app feels slow" |
| `MinSizeRel`     | `-Os -DNDEBUG`          | yes    | off     | off                | Shipping a small binary (rarely needed) |
| *(unset)*        | **none**                | no     | on      | on                 | Almost never what you want |

† This project's `Application.h` reads `NDEBUG` to set `ENABLE_VALIDATION_LAYERS`.
That means **only `Debug` and the unset case turn validation layers on** —
`RelWithDebInfo` looks like a debug build for symbols but still runs without
the validation overhead unless you override `NDEBUG` manually.

### Why "unset" is a trap

Without an explicit `-DCMAKE_BUILD_TYPE`, CMake's single-config generators emit
**no optimization flags at all** — not the same as `Debug`, just an empty
optimization configuration. Code runs at `-O0`-ish speeds without `-g` debug
symbols, which is the worst of both worlds.

This is why the very first `cmake .` run of this project produces a binary
that's slower than even Debug. If you ever notice "this is mysteriously
slower than I remembered," check `grep CMAKE_BUILD_TYPE build/CMakeCache.txt`
first — `CMAKE_BUILD_TYPE:STRING=` (empty value) is the culprit.

## Three workflow patterns

### Pattern 1 — Toggle the existing build dir

Simplest. Changes the meaning of `build/` between runs.

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build
./build/src/VulkanEngine

cmake -DCMAKE_BUILD_TYPE=Debug -S . -B build
cmake --build build
./build/src/VulkanEngine
```

**Downside:** every switch invalidates all `.o` files (different optimization
flags = different output), triggering a full rebuild. Annoying when you're
iterating and want to check "would this run fast in release?"

### Pattern 2 — Two parallel build dirs (recommended)

```bash
cmake -DCMAKE_BUILD_TYPE=Debug   -S . -B build-debug
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build-release

cmake --build build-debug    && ./build-debug/src/VulkanEngine
cmake --build build-release  && ./build-release/src/VulkanEngine
```

Each dir keeps its own object cache, so incremental rebuilds stay fast in
both. Flip between them by running a different binary — no reconfigure
needed. This is how serious C++ projects ship.

Add to `.gitignore`:
```
build-debug/
build-release/
```

### Pattern 3 — `RelWithDebInfo` for profiling

When you need fast code AND a debugger / Instruments / `samply` profile that
shows function names instead of `???`:

```bash
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build-relwd
cmake --build build-relwd
./build-relwd/src/VulkanEngine
```

`-O2 -g` produces near-Release performance with usable symbols. The win
becomes obvious if you ever attach `samply record ./build-relwd/src/VulkanEngine`
and want to see hot functions instead of inlined-into-oblivion stacks.

**Caveat:** Vulkan validation layers are off (NDEBUG is set). If you need
both perf-y code AND validation, configure with:
```bash
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-UNDEBUG" -S . -B build-relwd-validate
```
The `-UNDEBUG` undefines `NDEBUG` after the default flags, re-enabling both
asserts and the validation-layer guard in `Application.h`.

## Recommended setup

For day-to-day work on this engine:

1. **Keep `build-debug/` and `build-release/` in parallel** — Pattern 2.
2. **Default to debug while iterating** on architecture, scene state,
   shaders. Validation layers + asserts catch real bugs.
3. **Switch to release whenever you're measuring perf or showing the engine
   to someone**. Debug glm is so slow it makes the engine misleadingly look
   like it has CPU bottlenecks it doesn't.
4. **Spin up `build-relwd` only when you need to profile.** It's not a
   day-to-day workspace.

### Make "no flag" mean something sensible

Add this near the top of the top-level `CMakeLists.txt` so a naive
`cmake .` (yours or a future contributor's) picks `RelWithDebInfo` instead
of the trap-empty default:

```cmake
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Default build type when none is specified" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()
```

This only affects the *unset* case — explicit `-DCMAKE_BUILD_TYPE=Debug`
still wins. The `set_property` line is cosmetic but lets `cmake-gui` show a
dropdown.

## Common gotchas

### "It says Debug but it ran fast" — that wasn't your build type

The CMake configure output line `-- Build type: Debug` is from **spdlog's**
`FetchContent` configure announcing *its own* build type, not yours. To check
what your project actually compiled as:

```bash
grep CMAKE_BUILD_TYPE build/CMakeCache.txt
```

That cache entry is authoritative.

### "I changed the build type and it didn't switch"

You ran `cmake -DCMAKE_BUILD_TYPE=Release` in a different directory than
your binary. The build type is per-build-dir. Verify with the cache check
above, then `cmake --build <dir>` against the dir you actually configured.

### "Release runs faster but I lost validation"

Expected — `NDEBUG` short-circuits the validation-layer init in
`Application.h`. Three options:

- Stay on Debug while developing, switch to Release for perf measurement
- Use `RelWithDebInfo` with `-DCMAKE_CXX_FLAGS="-UNDEBUG"` to keep both
- Edit `Application.h` to decouple `ENABLE_VALIDATION_LAYERS` from
  `NDEBUG` (e.g. drive it from a CMake option `ENGINE_VALIDATION=ON`)

### Vsync makes Release perf measurements meaningless

Release on this engine caps at the display refresh rate (120 Hz on a
ProMotion display = 8.33 ms/frame). Frame time below that is invisible.

To measure the actual unconstrained perf, change the swapchain present mode
from `VK_PRESENT_MODE_FIFO_KHR` to `VK_PRESENT_MODE_MAILBOX_KHR` or
`VK_PRESENT_MODE_IMMEDIATE_KHR` in `Swapchain.cpp`. Don't ship that change —
just toggle it locally for the measurement.

### macOS-specific: ARM64 + AVX confusion

Apple Silicon doesn't have AVX/AVX2 — glm auto-detects NEON. Don't pass
`-mavx2` or similar. The default Release flags do the right thing.

### Stale CMake cache after editing CMakeLists.txt

If you add a new directory to `target_include_directories(...)` and the next
build complains it can't find headers, blow away the cache:

```bash
rm -rf build-debug/CMakeCache.txt build-debug/CMakeFiles
cmake -DCMAKE_BUILD_TYPE=Debug -S . -B build-debug
cmake --build build-debug
```

Or simpler: delete the whole build dir and reconfigure. CMake's incremental
configure misses some kinds of changes (especially around `FetchContent`
versions and dependency-graph edits).

## Quick reference

```bash
# Configure (per build dir, once)
cmake -DCMAKE_BUILD_TYPE=Debug          -S . -B build-debug
cmake -DCMAKE_BUILD_TYPE=Release        -S . -B build-release
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build-relwd

# Build (incremental)
cmake --build build-debug
cmake --build build-release -j         # -j = parallel; CMake auto-picks core count

# Check current type
grep CMAKE_BUILD_TYPE build/CMakeCache.txt

# Run
./build-debug/src/VulkanEngine
./build-release/src/VulkanEngine

# Clean rebuild (keep config, drop objects)
cmake --build build-debug --target clean
cmake --build build-debug

# Nuke and reconfigure (when CMakeLists.txt changes feel sticky)
rm -rf build-debug && cmake -DCMAKE_BUILD_TYPE=Debug -S . -B build-debug
```

## See also

- [animated-voxel-import.md](animated-voxel-import.md) — feature plan for the
  GLB import pipeline (skinned mesh + voxelizer)
- [`Application.h`](../src/Application.h) — where `NDEBUG` gates validation
  layers
- [`CMakeLists.txt`](../CMakeLists.txt) — top-level CMake (no default
  build type set today; see the snippet under "Make 'no flag' mean
  something sensible")
