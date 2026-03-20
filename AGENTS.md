# AGENTS.md

## Purpose
This document explains how `libserum` works end-to-end, with emphasis on runtime flow, scene handling, and cROMc persistence.

**Maintenance rule:** Any feature change, behavior change, data format change, or API/signature change in this repository **must** be reflected in this file in the same PR/commit.

**Platform-independence rule:** `libserum` is intended to behave the same on
all supported platforms. Runtime behavior, persisted `cROMc` semantics, and
derived lookup data must not depend on whether the archive was generated on
Windows, macOS, or Linux. If equal source data is loaded/generated, the
resulting `v6` `cROMc` content and runtime behavior are expected to be
platform-independent.

## High-level architecture
Core files:
- `src/serum-decode.cpp`: Main runtime engine (load, identify, colorize, rotate, scene orchestration).
- `src/SerumData.h/.cpp`: In-memory model + cROMc serialization/deserialization.
- `src/SceneGenerator.h/.cpp`: PUP scene CSV parsing + runtime scene frame generation.
- `src/sparse-vector.h`: Sparse/compressed storage for frame and asset blocks.
- `src/serum.h`: Public constants/flags/structs.
- `src/serum-decode.h`: Public C API declarations.
- `src/serum-version.h`: Library and concentrate format versions.

Main global runtime state (in `serum-decode.cpp`):
- Loaded data model: `g_serumData`.
- Current output: `mySerum` (`Serum_Frame_Struc`).
- Scene playback state: `sceneFrameCount`, `sceneCurrentFrame`, duration/flags/repeat, etc.
- Identification state: `lastfound`, `lastfound_normal`, `lastfound_scene`, CRC tracking.
- Scene lookup acceleration:
  - `g_serumData.frameIsScene`: frame ID -> scene/non-scene marker.
  - `g_serumData.sceneFramesBySignature`: `(mask,shape,hash)` -> matching scene frame IDs.
  - `g_serumData.sceneFrameIdByTriplet`: `(sceneId,group,frameIndex)` -> frame ID.

## SparseVector storage and compression
`SparseVector` now supports both legacy map payloads and packed sparse blobs.

Packed sparse payload format (used for v6 save):
- `packedIds`
- `packedOffsets`
- `packedSizes`
- `packedBlob`

Behavior:
- Packed payloads are deduplicated by payload content at pack time.
- Optional adaptive value packing exists for `uint8_t` payloads:
  - per-payload mode is derived from actual max value and encoded in payload header
  - 1-bit mode for values in `0..1`
  - 2-bit mode for values in `0..3`
  - 4-bit mode for values in `0..15`
  - fallback to raw 8-bit payload otherwise
- Value packing preserves exact values for packed modes (no nonzero->1 normalization).
- Packed vectors can still be modified at runtime (`set()`); mutable map storage is restored lazily when needed.
- Runtime lookup uses dense index fast-path when IDs are dense.
- Compressed sparse vectors keep a tiny bounded decoded cache (6 entries, LRU
  replacement) in addition to the last/second hot-entry cache, reducing decode
  churn for alternating IDs without large RAM growth.

Vector policy currently used in `SerumData`:
- `dyna4cols_v2` and `dyna4cols_v2_extra` are LZ4-compressed sparse vectors.
- `backgroundmask` and `backgroundmask_extra` use adaptive value packing + LZ4 compression.
- Sentinel-based vectors are normalized and packed with boolean sidecars:
  - `spriteoriginal` + `spriteoriginal_opaque`
  - `spritemask_extra` + `spritemask_extra_opaque`
  - `spritedescriptionso` + `spritedescriptionso_opaque`
  - `dynamasks` + `dynamasks_active`
  - `dynamasks_extra` + `dynamasks_extra_active`
  - `dynaspritemasks` + `dynaspritemasks_active`
  - `dynaspritemasks_extra` + `dynaspritemasks_extra_active`
  - During normalization, dynamic value vectors may preserve an explicit
    all-zero payload only when the paired active sidecar still marks active
    pixels; this is required because dynamic layer `0` is a valid value and is
    not equivalent to "no payload".
  - `BuildPackingSidecarsAndNormalize()` also snapshots each generated sidecar
    payload into `m_packingSidecarsStorage` as a transient two-dimensional byte
    store (`std::vector<std::vector<uint8_t>>`).
- Precomputed frame-level dynamic fast flags are persisted:
  - `frameHasDynamic`
  - `frameHasDynamicExtra`
- `Colorize_Framev1/v2` uses these flags to bypass dynamic-mask branches
  entirely for frames without active dynamic pixels.
- Color rotations use a precomputed lookup index:
  `colorRotationLookupByFrameAndColor[(frameId,isExtra,color)] -> (rotation,position)`
  restored from v6 cROMc when present.
  - v5 / authoring-time rebuild flows may rebuild the lookup before re-save.
  - v6 persistence stores this derived lookup in canonical sorted-entry form
    rather than direct `unordered_map` archive order, so `cROMc` output stays
    platform-independent and deterministic for identical source data.
  - `ColorInRotation` uses lookup-only runtime path (no linear scan fallback).
- Critical monochrome trigger frames use a precomputed lookup:
  `criticalTriggerFramesBySignature[(mask,shape,hash)] -> frameId(s)`
  restored from v6 cROMc when present and rebuilt during frame-lookup
  preprocessing otherwise.
- Sprite runtime sidecars are precomputed and used by `Check_Spritesv2`:
  - frame candidate list with sprite slot indices (`spriteCandidateOffsets`,
    `spriteCandidateIds`, `spriteCandidateSlots`)
  - frame-level shaped-sprite marker (`frameHasShapeSprite`)
  - per-sprite dimensions and shape flags (`spriteWidth`, `spriteHeight`,
    `spriteUsesShape`)
  - flattened detection metadata (`spriteDetectOffsets`, `spriteDetectMeta`)
  - per-sprite opaque row-segment runs (`spriteOpaqueRowSegmentStart`,
    `spriteOpaqueRowSegmentCount`, `spriteOpaqueSegments`)
  - `BuildSpriteRuntimeSidecars()` also snapshots the generated runtime sidecar
    vectors into `m_packingSidecarsStorage` as raw byte copies.
- Runtime uses sidecar flags instead of `255` sentinels for transparency / dynamic-zone activity.
- Runtime does not include sentinel-based fallback in sprite/dynamic helpers;
  missing/incorrect sidecars are treated as a conversion/load bug and are not
  masked by `255` compatibility logic.
- Dynamic-zone value vectors (`dynamasks*`, `dynaspritemasks*`) use adaptive
  value packing + compression, with sidecar active flags for sentinel-free
  semantics.
- `compmasks` and `backgroundmask*` are already boolean-mask domain (`mask==0`
  include / `>0` exclude) and therefore do not need separate transparency
  sidecar vectors.

## Load flow
Entry point: `Serum_Load(altcolorpath, romname, flags)`.

1. Reset all runtime state via `Serum_free()`.
2. Look for optional `*.pup.csv`.
3. Prefer loading `*.cROMc` unless `skip-cromc.txt` exists.
   - If `*.cROMc` starts with `CROM` magic, load via `SerumData::LoadFromFile`.
   - Otherwise, try encrypted in-memory load (`vault::read` + `SerumData::LoadFromBuffer`).
4. If cROMc load fails or is absent, load `*.cROM`/`*.cRZ`.
5. If CSV exists and format is v2, parse scenes via `SceneGenerator::parseCSV`.
6. Set scene depth from color count when scenes are active.
7. Build or restore frame lookup acceleration:
   - If loaded from cROMc v6 and no CSV update in this run: use stored lookup
     via `InitFrameLookupRuntimeStateFromStoredData()`.
   - Otherwise: rebuild via `BuildFrameLookupVectors()`.
   - Stored `v6` lookup data is expected to be valid across supported
     platforms; do not introduce platform-specific load branching for direct
     `v6` runtime loads.
8. Build/normalize packing sidecars via `BuildPackingSidecarsAndNormalize()`.
   - This normalization/repair path is for source-data build flows and `v5`
     compatibility handling.
   - Direct `v6` cROMc runtime load is expected to consume already-normalized
     runtime-ready data instead of mutating or repairing it on device.
   - Direct `v5` cROMc load must rerun this step after deserialization because
     the persisted v5 format does not contain the normalized opacity/dynamic
     sidecars used by current runtime code.
9. Build or restore sprite runtime sidecars via `BuildSpriteRuntimeSidecars()`.
   - For direct `v6` cROMc loads, runtime sidecars are expected to be restored
     from file as final runtime data.
   - Rebuild-on-load behavior belongs to `v5` compatibility handling and
     authoring-time rebuild flows, not to the final-device direct `v6` path.
Important:
- `BuildFrameLookupVectors()` must run after final scene data is known for this load cycle.
- CSV parsing after loading can invalidate stored scene lookup data and requires rebuild.
- Design policy:
  - `v5` backward-compatibility logic must remain scoped to `v5` loads.
  - `pup.csv`-driven rebuild/update logic is an authoring-time path and is not
    the target for memory-sensitive final-device runtime behavior.
  - Direct `v6` runtime load is expected to trust the stored runtime-ready data;
    do not add safety nets, compatibility shims, or repair logic for unreleased
    `v6` snapshot-to-snapshot compatibility on that path.
  - If a direct `v6` runtime load still needs mutation/repair to work, that is
    a generation/save contract bug and should be fixed at `cROMc` creation time
    rather than masked in the final-device load path.
  - The final-device direct `v6` load path must not run
    `BuildPackingSidecarsAndNormalize()` or rebuild missing sprite runtime
    sidecars on load.

## Frame identification
Main function: `Identify_Frame(uint8_t* frame, bool sceneFrameRequested)`.

Identification compares incoming original DMD frame against loaded frame definitions using:
- `compmaskID` (mask)
- `shapecompmode` (shape mode)
- `hashcodes` (precomputed CRC32 domain value)

Behavior:
- Matching starts from the stream-specific last found ID and wraps.
- Stream split is enforced:
  - normal search skips scene frames
  - scene search skips normal frames
  using `g_serumData.frameIsScene`.
- Scene requests use signature lookup in `sceneFramesBySignature` for the current `(mask,shape,hash)`.
- Normal-frame identification uses a persisted signature lookup in
  `normalFramesBySignature` to narrow candidates before applying the existing
  wrap-around / same-frame selection rules.
- Normal-frame identification does not include a fallback full-frame scan once
  `normalFramesBySignature` is available; missing/incorrect lookup data is a
  build/load contract bug rather than a runtime fallback case.
- Runtime normal identification iterates unique `(mask,shape)` buckets in
  frame-order relative to `lastfound_normal`, so frame-order semantics are
  preserved while each bucket hash is computed only once per input frame.
- Scene rendering can bypass generic scene identification when a direct triplet
  entry exists in `sceneFrameIdByTriplet`.
- During scene playback, direct-triplet mode uses libserum-owned timing
  (`sceneDurationPerFrame` plus a runtime next-frame timestamp) together with
  `SceneGenerator::updateAndGetCurrentGroup(...)`, and bypasses both
  `SceneGenerator::generateFrame(...)` and `Identify_Frame()` by supplying the
  precomputed `sceneFrameIdByTriplet` frame ID directly to the internal
  colorizer.
- Legacy same-frame behavior (`IDENTIFY_SAME_FRAME`) is preserved with full-frame CRC check.

Return values:
- `IDENTIFY_NO_FRAME` when no match.
- `IDENTIFY_SAME_FRAME` when same frame detected with same full CRC.
- matched frame ID otherwise.

## Scene lookup vector build
Function: `BuildFrameLookupVectors()`.

Goal: classify loaded frame IDs into scene/non-scene and build scene signature index.

How it works:
1. Initialize `frameIsScene` with all zeros.
2. If scene generator is active (v2 scene mode), pre-generate all scene frames:
   - iterate all scenes from `sceneGenerator->getSceneData()`
   - iterate all groups (`frameGroups`, default 1)
   - iterate all `frameIndex` values
   - generate with `generateFrame(..., disableTimer=true)`
3. Build scene signatures in identification domain:
   - collect unique `(mask,shape)` combinations from loaded frames
   - for every generated scene frame and every unique `(mask,shape)`, compute CRC via `calc_crc32`
4. For each loaded frame ID, if `(mask,shape,hashcodes[id])` signature is in scene signature set:
   - mark `frameIsScene[id] = 1`
   - add to `sceneFramesBySignature[signature]`.
5. Build `normalFramesBySignature` for all non-scene frames:
   - `(mask,shape,hash) -> matching normal frame IDs`
   - precompute flat normal identification buckets:
     - `normalIdentifyBuckets`
     - `frameToNormalBucket`
    - runtime normal identification uses this as a candidate source while still
      preserving wrap-around ordering relative to `lastfound_normal`.
    - runtime does not fall back to a linear normal-frame scan if this lookup is
      missing or inconsistent.
   - runtime walks precomputed `(mask,shape)` buckets in frame order and resolves
     matching frame IDs from the lookup, avoiding repeated per-frame hash
     computation inside the same bucket and avoiding per-frame bucket-dedup
     container overhead.
6. For v6 (`concentrateFileVersion >= 6`), precompute direct scene frame IDs:
   - generate each `(sceneId,group,frameIndex)` scene marker frame
   - identify it once
   - persist mapping in `sceneFrameIdByTriplet`.
   - Runtime playback then combines this triplet lookup with trigger-provided
     `sceneDurationPerFrame`; it does not regenerate marker frames on each
     scene tick.
7. During the same preprocessing pass, build critical monochrome-trigger
   signatures for non-scene frames:
   - include only frames with trigger IDs:
     - `MONOCHROME_TRIGGER_ID`
     - `MONOCHROME_PALETTE_TRIGGER_ID`
   - persist mapping in `criticalTriggerFramesBySignature`.
8. Initialize `lastfound_scene` / `lastfound_normal` from first available IDs.

Log line:
- `Loaded X frames and Y rotation scene frames`

## Colorization flow (v2)
Entry point: `Serum_ColorizeWithMetadatav2(frame, sceneFrameRequested=false)`.

Main phases:
1. Identify frame ID via `Identify_Frame`.
2. Trigger / monochrome handling.
3. Scene trigger handling.
4. Render base frame via `Colorize_Framev2(...)`.
5. Optional background-scene overlay via second `Colorize_Framev2(..., applySceneBackground=true, ...)`.
6. Optional sprite overlays.
7. Configure color rotations and return next timer.

Dynamic-shadow hot path:
- `CheckDynaShadow(...)` receives pre-fetched per-frame shadow vectors
  (`dynashadowsdir*`, `dynashadowscol*`) from `Colorize_Framev2` instead of
  loading sparse vectors per pixel.
- Neighbor probing is done by iterating a compact offset table
  (8-connected neighbors) rather than repeated hand-written branch blocks.

Sprite matching prefilter:
- `Check_Spritesv2` builds an exact per-frame 32-bit dword index and skips
  detection-area scans for detection words that are not present.
- This replaces the Bloom prefilter path (no false positives from hash
  collisions).
- Shape-mode sprites use a separate exact dword index built from the binary
  `frameshape` domain, so shape detection words are not filtered against raw
  grayscale frame dwords.
- Detection-area verification uses precomputed opaque row-segment runs to avoid
  per-pixel checks on transparent sprite zones.

Background placeholder policy:
- `Colorize_Framev2` supports `suppressFrameBackgroundImage`.
- When true, frame-level background images are treated as placeholders and existing output pixel is kept in masked background areas.
- This is used when a background scene is active so the scene background can continue while foreground content changes.

Critical-trigger fast rejection:
- While a non-interruptable scene (or its end-hold) is active, normal incoming
  frames are not always sent through full `Identify_Frame(...)`.
- `Serum_ColorizeWithMetadatav2Internal(...)` first checks a tiny precomputed
  subset containing only non-scene frames with trigger IDs:
  - `MONOCHROME_TRIGGER_ID`
  - `MONOCHROME_PALETTE_TRIGGER_ID`
- If no such critical trigger frame matches, the incoming frame is rejected
  immediately without full identification.
- This preserves important monochrome/service-menu transitions while avoiding
  most irrelevant input-frame work during non-interruptable scenes.
- If such a critical monochrome trigger frame does match, it is allowed to
  preempt the non-interruptable scene immediately; libserum stops the current
  scene/end-hold and processes the monochrome-trigger frame normally.

## Scene playback and options
Scene data comes from CSV (`SceneGenerator`).

Flags (from `serum.h`):
- `1`: black when scene finished
- `2`: show previous frame when scene finished
- `4`: run scene as background
- `8`: only dynamic content in foreground over background scene
- `16`: resume interrupted scene if retriggered within 8s

`startImmediately` behavior:
- `startImmediately` is honored only for foreground scenes.
- For background scenes (`FLAG_SCENE_AS_BACKGROUND`), `startImmediately` is forced to `false`.

## cROMc persistence
Current concentrate version: **6**.

Stored in v6:
- Full Serum model payload.
- Scene data (`SceneGenerator` scene vector).
- Scene lookup acceleration:
  - `frameIsScene`
  - `sceneFramesBySignature`
  - `normalFramesBySignature`
  - `normalIdentifyBuckets`
  - `frameToNormalBucket`
  - `sceneFrameIdByTriplet`
- Critical monochrome-trigger lookup:
  - `criticalTriggerFramesBySignature`
- Color-rotation lookup acceleration:
  - `colorRotationLookupByFrameAndColor`
- Derived lookup tables are serialized in canonical sorted-entry form instead of
  direct `unordered_map` archive order, so equal data yields equal `cROMc`
  bytes across platforms.
- `v6` `cROMc` archives are intended to be portable across supported
  platforms. A `cROMc` generated on one platform must load with the same
  semantics on another platform without archive-format forks or
  platform-specific compatibility branches.
- Sprite runtime sidecars:
  - `spriteCandidateOffsets`, `spriteCandidateIds`, `spriteCandidateSlots`
  - `frameHasShapeSprite`
  - `spriteWidth`, `spriteHeight`, `spriteUsesShape`
  - `spriteDetectOffsets`, `spriteDetectMeta`
  - `spriteOpaqueRowSegmentStart`, `spriteOpaqueRowSegmentCount`,
    `spriteOpaqueSegments`
- Scene data block uses guarded encoding (`SCD1` magic + bounded count) to
  prevent unbounded allocations on corrupted/misaligned input.
- Sparse vectors in packed sparse layout.
- Normalized sentinel vectors plus sidecar flag vectors for transparency and
  dynamic-zone activity.

Backward compatibility:
- v5 files are loadable.
- v5 sparse vectors are deserialized with legacy sparse-vector layout and converted to packed representation after load.
- For v5 loads, scene lookup vectors and other derived runtime sidecars may be rebuilt at startup.
- For direct `v6` loads, stored runtime sidecars are expected to be consumed as
  persisted runtime-ready data.
- For direct `v6` loads, stored scene/color-rotation lookup tables are reused
  directly; their persisted representation is platform-independent.
- Cross-platform differences in `v6` behavior are treated as bugs in canonical
  persistence or runtime reconstruction, not as an acceptable reason to add
  platform-tagged `cROMc` variants.
- A `pup.csv` update in the same authoring-time load cycle may invalidate persisted scene lookup data and requires rebuild before re-save.
- Direct scene-triplet preprocessing is only executed for v6.
- v6 scene-data deserialization validates block magic and count before
  allocation.

v6 snapshot policy:
- Compatibility between unreleased v6 development snapshots is not required.
- Compatibility to released v5 remains required.
- Therefore:
  - if `v6` data needs new runtime-ready fields or stricter invariants, update
    the `v6` generation/load contract directly rather than adding fallback logic
    for older `v6` development snapshots.
  - do not introduce final-device runtime safety nets, repair paths, or
    compatibility shims merely to keep older `v6` development snapshots
    loading.

## Logging
- Central callback configured by `Serum_SetLogCallback`.
- `serum-decode.cpp` and `SceneGenerator.cpp` both use callback-based `Log(...)`.
- Missing-file logs from `find_case_insensitive_file(...)` use normalized path joining.
- Optional runtime debug tracing is env-gated and split by verbosity:
  - `SERUM_DEBUG_TRACE_INPUTS=1` enables high-level lifecycle logs (input,
    trigger, scene-info).
  - `SERUM_DEBUG_IDENTIFY_VERBOSE=1` enables per-candidate identification logs.
  - `SERUM_DEBUG_SPRITE_VERBOSE=1` enables sprite candidate/detection/rejection
    logs.
  - `SERUM_DEBUG_SCENE_VERBOSE=1` enables scene-path and scene-event logs.
  - `SERUM_DEBUG_INPUT_CRC`, `SERUM_DEBUG_FRAME_ID`, and
    `SERUM_DEBUG_STAGE_HASHES=1` remain available as output filters and
    expensive hash tracing controls.
- Optional runtime profiling:
  - If env `SERUM_PROFILE_DYNAMIC_HOTPATHS` is enabled (`1/true/on/yes`),
    periodic average timings are logged for the full end-to-end rendered-frame
    round trip (`frame`), `Colorize_Framev2`, and `Colorize_Spritev2`, along
    with average identification time (`Identify_Frame`) split into
    normal/scene calls plus the critical-trigger mini-matcher, input/result
    counters (`inputs`, `rendered`, `same`, `noFrame`), and current process
    RSS memory usage and process-local peak RSS seen so far.
  - If env `SERUM_PROFILE_DYNAMIC_HOTPATHS_WINDOWED=1`, the same counters are
    reset after each emitted 240-frame block so each `Perf dynamic avg` line
    reflects only the most recent window rather than a cumulative average.
  - The same profiler also logs a one-time startup summary before normal frame
    processing begins:
    `Perf startup peak: start=...MiB current=...MiB peak=...MiB stage=...`
    where `peak` is the highest sampled RSS observed during the load pipeline.
  - If env `SERUM_PROFILE_SPARSE_VECTORS=1`, sparse-vector access snapshots are
    logged at the same cadence (accesses, decode count, cache hits, direct hits)
    for key runtime vectors (`cframes_v2*`, `backgroundmask*`, `dynamasks*`,
    `dynaspritemasks*`).

## Safety invariants
- `frameIsScene.size()` must equal `nframes` before identification.
- `sceneFramesBySignature` must correspond to current scene data and current loaded frame definitions.
- `sceneFrameIdByTriplet` (when present) must correspond to current scene data.
- Any change to scene generation domain (`mask/shape/hash`), sparse-vector serialization layout, or cROMc schema requires updating this file.

## How to validate after changes
Minimum validation:
1. Build: `cmake --build build -j4`
2. Load scenarios:
   - cROM/cRZ without CSV
   - cROM/cRZ with CSV
   - cROMc v5 with CSV update
   - cROMc v6 without CSV update
3. Verify log line:
   - `Loaded <normal> frames and <scene> rotation scene frames`
4. Verify scene behaviors:
   - background scene
   - end-of-scene behavior flags
   - resume flag `16`
10. Build color-rotation lookup index via `BuildColorRotationLookup()` during
    v5 / authoring-time rebuild flows so persisted v6 data provides O(1)
    `ColorInRotation` checks without direct-load fallback.
