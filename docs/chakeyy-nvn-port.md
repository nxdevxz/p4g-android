# Chakeyy NVN optimization test build for P4G

Source: [Chakeyy/p5r-android](https://github.com/Chakeyy/p5r-android), branch
`perf/universal-nvn-opt`, commit `5f17826cab38eaeca12162abc4337ca0831a3a65`.
The fork's `main` does not contain that optimization commit.

This is a selective adaptation of the CPU-side command, buffer and descriptor reuse changes.
It is not a merge of the complete P5R branch, and no FPS gain has been measured on a P4G device.

## Included changes

- Store command payloads in one reusable arena instead of allocating a vector for every payload.
  P4G additionally aligns each payload and checks the 32-bit offset bounds.
- Recycle up to 64 command lists, retaining capacity and waiting until no recording/replay owner
  still uses a list. Reset cached dispatch counts, called handles and texture epochs on reuse.
- Use hash lookups for recorded command handles, plus a two-entry per-submit replay cache.
- Use stack storage for the bounded vertex attribute/stream recording arrays.
- Cache four recently used program layouts.
- Use dynamic storage-buffer descriptors on Android when the Vulkan device supports the layout's
  descriptor count. A changed upload offset can reuse an existing descriptor set. Allocate the
  matching descriptor-pool capacity and supply offsets in Vulkan set/binding order, including when
  a bind starts at set 1 or skips empty sets. `NX_DYNAMIC_SSBO=0` restores ordinary storage descriptors;
  other platforms retain ordinary storage descriptors unless explicitly enabled with `=1`.
- Retain up to 256 immutable driver-constant blocks per submit and cache the last graphics-stage
  block and its upload. New submits, changed storage bindings and upload-ring resets invalidate
  the corresponding cached data. Compute constants keep the existing path.

P4G's origin, stencil, frame pacing, local shader generation, prewarming, upload subrange checks,
compute write-backs and texture synchronization are preserved. In particular, the source fork's
recording-time stencil-value suppression is not used: it does not distinguish front/back faces.
The more invasive cross-submit descriptor pools, sampled buffer hashes, cached texture readiness,
barrier changes, CPU affinity and P5R-specific rendering/game changes are outside this port.
The existing cross-submit buffer-cache default is unchanged; this build does not assume that
unchanged addresses imply unchanged buffer contents.

## Automated verification

On a Linux host with a C++20 compiler and Vulkan headers:

```sh
python3 tests/test_nvn_optimizations.py --vulkan-include /path/to/Vulkan-Headers/include --sanitize
```

The tests exercise dynamic-offset ordering/partial binds, aligned command payloads after arena
growth, bounds checks, in-flight ownership and reuse of actual command lists, and actual
constant-cache invalidation on binding changes, new submits and upload-ring resets.
Vulkan calls are not executed by these tests. The Android APK build compiles and links the full runtime.
The existing P4G Beta workflow also runs these regressions after downloading Vulkan headers.

## Build and compare on the phone

The test branch is `perf/chakeyy-nvn-p4g`. Pushing this branch starts the existing **P4G Beta**
workflow and produces its usual signed APK artifact if all build steps and repository signing
secrets are available. It can also be run manually from Actions with this branch selected.

Compare that APK with the current main build using the same phone, driver, save, resolution and
FPS cap. Use a scene where the baseline is below the cap; a locked 30 FPS scene cannot demonstrate
additional throughput. Warm the same route once for shader compilation, then repeat it and record
FPS, stutters and phone temperature. Check menus/dialogues, battles, scene transitions and a
cutscene for rendering regressions. A CPU overhead reduction may have little effect in a GPU-bound
scene. Keep the prior APK and back up saves before switching builds.

The package and signing setup remain those of P4G. Whether an APK can install over an older build
depends on using the same signing key; this port does not change the game's version or save format.

## Second test: direct descriptor keys

The second build adapts Chakeyy's direct descriptor-key construction. Each draw builds compact
keys directly for its four descriptor sets. Vulkan write structures and buffer/image-info arrays
are materialized only for cache misses. Identical consecutive sets use an exact comparison before
hashing; the set layout and submit generation are included in the validity checks.
Texture readiness checks, buffer uploads and compute synchronization still run as before.
`NX_SET_CACHE=0` retains the original uncached descriptor-write path.

A regression test covers buffer/image descriptor reconstruction, identical draws, changed views,
layout changes, reuse of an older set, submit resets, empty sets and pointer stability beyond the
old fixed vector reservation. This reduces repeated CPU work; device FPS impact remains unmeasured.
