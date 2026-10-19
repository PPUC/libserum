# AGENTS.md

## Purpose
This document explains how `libserum` works end-to-end, with emphasis on runtime flow, scene handling, and cROMc persistence.

**Maintenance rule:** Any feature change, behavior change, data format change, or API/signature change in this repository **must** be reflected in this file in the same PR/commit.
**Formatting rule:** Code formatting in this repository is defined by
`.clang-format`. Any code changes must be formatted accordingly before they are
considered done.

**Local notes rule:** If `AGENTS.local.md` exists in the repository root, read
it before working. It is untracked on purpose and carries detail about this
checkout that must not be committed, so its absence is normal and nothing in
this file may depend on its contents.

**Real-machine rule:** `is_real_machine()` and everything it gates is **off
limits**. Do not change it, do not simplify or unify the branches behind it, and
do not report any of it as dead, obsolete, unreachable, inconsistent or
erroneous — including in a code review or a cleanup pass.

It returns false on Windows, macOS and Android, and on Linux it is true only
when `/proc/device-tree/model` names a Raspberry Pi. So on a development machine
and in a virtual-pinball setup every branch behind it is the false one, and the
true one is never compiled out, never executed, and **not testable here**. Code
that is only reachable on hardware you do not have looks exactly like dead code,
and several of these branches look arbitrary or incomplete out of context; they
are neither.

Everything it gates is reachable with `grep -rn is_real_machine src/`, and
`AGENTS.local.md` lists the sites if it is present. Treat every branch it guards
as load-bearing, and keep new behaviour consistent with the pattern rather than
working around it.

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

Internal helper interface (`src/serum-accel.h`):
- The decoder calls a small set of internal helpers through `src/serum-accel.h`.
  `src/serum-accel-ref.cpp` is the implementation that ships and defines the
  behaviour.
- The CMake variable `SERUM_ACCEL_DIR` can substitute other sources for it at
  build time. This is used for instrumentation and measurement wrappers and for
  trying out alternative implementations during development. Any substitute
  must produce bit-identical output.
- Do not remove or inline these helpers because some look trivial in the
  reference implementation: a function that always returns the same answer is
  a deliberate extension point. Do not change a signature without raising
  `SERUM_ACCEL_SEAM_VERSION`.

Main global runtime state (in `serum-decode.cpp`):
- Loaded data model: `g_serumData`.
- Current output: `mySerum` (`Serum_Frame_Struc`).
- Scene playback state: `sceneFrameCount`, `sceneCurrentFrame`, duration/flags/repeat, etc.
- Identification state: `lastfound`, `lastfound_normal`, `lastfound_scene`, CRC tracking.
- Public C API entrypoints are serialized through a recursive runtime mutex
  because this state is process-global and scene rendering may re-enter
  colorization internally. `Serum_Rotate()` must not run concurrently with
  `Serum_Colorize()` while shared output buffers, flags, widths, rotation
  tables, or scene state are being updated.
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
- Packed sparse vectors track packed-index readiness separately from the
  optional dense index. Sparse high-ID vectors may intentionally use only the
  hash index, and runtime reads must not rebuild that hash index on every
  access just because no dense index was allocated.
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
  - The pre-normalization source vectors for `dynamasks*` and
    `dynaspritemasks*` must retain legacy `255` no-data sentinel semantics so
    an all-layer-`0` active payload is not dropped before sidecar generation.
  - `BuildPackingSidecarsAndNormalize()` also snapshots each generated sidecar
    payload into `m_packingSidecarsStorage` as a transient two-dimensional byte
    store (`std::vector<std::vector<uint8_t>>`).
- Precomputed frame-level dynamic fast flags are persisted:
  - `frameHasDynamic`
  - `frameHasDynamicExtra`
- Additional prepared-load metadata is persisted for direct `v6` startup:
  - `hasAnyExtraFrame`
  - `publicTriggerCount`
  - `Serum_LoadConcentratePrepared(...)` uses these to avoid whole-frame scans
    for extra-frame availability and public-trigger counting on direct `v6`
    loads.
- `Colorize_Framev1/v2` uses these flags to bypass dynamic-mask branches
  entirely for frames without active dynamic pixels.
- Runtime frame rendering must not rely on `activeframes` alone: frames with
  background-only or dynamic-only content are still renderable and must pass
  the render gate even when static base pixels are absent.
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

Requested-output mode:
- `FLAG_REQUEST_FALLBACK` extends single-plane extra-resolution requests.
- If the caller requests only the extra-resolution output plane
  (`FLAG_REQUEST_64P_FRAMES` for 32p source content, or
  `FLAG_REQUEST_32P_FRAMES` for 64p source content), libserum enters an
  extra-preferred runtime mode.
- In that mode without `FLAG_REQUEST_FALLBACK`, runtime renders only the extra
  plane and skips original-plane frame/rotation preparation entirely.
- In that mode with `FLAG_REQUEST_FALLBACK`, runtime first tries the extra
  plane for each matched frame/scene frame; if that frame has no complete extra
  payload (frame/background/sprite coverage), runtime renders only the original
  plane as fallback for that call.
- Authoring/update load flows may still widen internal load flags so both
  planes remain available for `.cROMc` regeneration, but runtime output
  behavior must continue to follow the caller's original request flags.
- An `is256x64` project has one plane, the `256x64` one, and none of the modes
  above applies to it as written: a `64p` request gets that plane, a
  `32p`-only request gets nothing, and a `32p`-only request with
  `FLAG_REQUEST_FALLBACK` gets the `256x64` plane. See "Output contract" under
  "The ROM frame, and `is256x64` projects".

1. Reset all runtime state via `Serum_free()`.
2. On real-machine runtime (`is_real_machine()==true`):
   - do not scan or apply `*.pup.csv`
   - ignore `skip-cromc.txt`
   - accept only `*.cROMc`
   - do not fall back to `*.cROM` or `*.cRZ`
3. On non-real-machine/runtime-update flows:
   - look for optional `*.pup.csv`
   - prefer loading `*.cROMc` unless `skip-cromc.txt` exists.
   - If `*.cROMc` starts with `CROM` magic, load via `SerumData::LoadFromFile`.
   - Otherwise, try encrypted in-memory load (`vault::read` + `SerumData::LoadFromBuffer`).
4. If cROMc load fails or is absent on non-real-machine/runtime-update flows,
   load `*.cROM`/`*.cRZ`.
   - Raw source loads are authoring/update inputs and may be re-saved as
     `*.cROMc` when runtime generation is not disabled through
     `Serum_SetGenerateCRomC(false)`.
   - When such a raw source load produces a new `*.cROMc`, libserum reloads
     that generated `*.cROMc` in the same load cycle so first-run behavior
     matches the next boot.
5. If CSV exists and format is v2, parse scenes via `SceneGenerator::parseCSV`.
   - If CSV parsing updates scene data for a loaded `*.cROMc` and runtime
     generation is enabled, libserum rewrites the `*.cROMc` and reloads it in
     the same load cycle so the current process immediately uses the persisted
     scene-aware data.
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

## Loading from memory
Entry points: `Serum_LoadFromMemory(crom, size, flags)` and
`Serum_LoadFromMemoryWithScenes(crom, size, csvPath, flags)`. Both run the
internal `LoadFromMemory()`; the first passes no `csvPath`.

It takes the bytes of a `cROM` file in a buffer the caller owns. It does not
go through `Serum_Load()`. The two share three helpers, which hold code moved
out of `Serum_Load()` unchanged:
- `BeginColorizationLoad()`: the profile and output-pointer resets after
  `Serum_free()`.
- `RequestBothPlanesByDefault()`: both planes when the caller requested none.
- `FinishColorizationLoad()`: steps 7 to 9 of the load flow and the scaling
  setup.

Between them `Serum_LoadFromMemory()` widens the load flags to both planes
unless `FLAG_REQUEST_FORCE` is set and reads the buffer with
`Serum_LoadFilev1Stream()`, as `Serum_Load()` does for a raw file. The result
equals `Serum_Load()` of the same bytes as a `.cROM` file with
`Serum_SetGenerateCRomC(false)`.

What differs from `Serum_Load()`:
- No file is looked up or written: no `cROMc` (neither loaded nor generated),
  no `*.pup.csv`, no `scaling.txt`, no `skip-cromc.txt`. `scalingAlgorithm` /
  `shadowOffsetMode` keep their defaults. Scenes are empty, unless
  `Serum_LoadFromMemoryWithScenes()` names a `pup.csv`: for a v2 colorization
  it is parsed after the cROM is read and before `FinishColorizationLoad()`
  (whose `sceneDataUpdatedFromCsv` it sets), and the scene depth is set from the
  colour count, in the order `Serum_Load()` uses for a `pup.csv` beside a raw
  cROM. `BuildFrameLookupVectors()` therefore sees the scenes, and the result
  equals `Serum_Load()` of the cROM with that `pup.csv` beside it and
  `Serum_SetGenerateCRomC(false)`. A `NULL` or empty path, or a file that does
  not parse, loads without scenes. This is an authoring-time path (an editor
  holding its project in memory); it has no real-machine branch.
- Only a raw `cROM` is accepted, not a `cROMc` or a `cRZ`.
- It has no platform branch of its own: the request flags are used as passed,
  and nothing of `Serum_Load()`'s per-platform load policy is applied. The
  shared helpers and the renderer behave as they do after `Serum_Load()`.
- The buffer is read through `MemoryCRomReader` with `throwOnShortData` set.
  Short data throws instead of reaching `exit(1)` in
  `SparseVector::readFromCRomReader()`, and `CountFitsSource()` refuses a v2
  header whose element counts exceed the bytes that follow. Both apply to this
  entry point only; file and `cRZ` loads read as before.
- On failure it calls `Serum_free()`, returns `NULL` and stores the reason for
  `Serum_GetLastErrorMessage()`. Nothing is loaded afterwards.
- `NULL` or empty input releases the loaded colorization and fails.

The buffer is not kept after the call. Dispose with `Serum_Dispose()`.

## Frame identification
Main function: `Identify_Frame(uint8_t* frame, bool sceneFrameRequested)`.

The incoming frame is the ROM frame, `NativeFramePixels()` bytes of it: for an
`is256x64` project that is `256x64`, not the `128x32` its header gives as
`fwidth x fheight` (see "The ROM frame, and `is256x64` projects").

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
- Scene identification walks the scene frames in frame order relative to
  `lastfound_scene` and hashes the input once per `(mask,shape)` pair: a frame
  whose pair was already hashed without a match is passed over. The first hash
  found in `sceneFramesBySignature` ends the search, so this gives the same
  result as hashing every scene frame. The `sceneFrameIdByTriplet` build at
  load time identifies every scene marker frame this way, so its cost grows
  with the number of distinct pairs rather than the number of scene frames.
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

Save-time invariant:
- `Serum_SaveConcentrate()` must set `SceneGenerator` depth from
  `g_serumData.nocolors` before `BuildFrameLookupVectors()`. Otherwise large
  v2 scene sets authored for 4-level scene markers can serialize empty derived
  scene lookup data even though the source `.pup.csv` parses successfully.

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

Preferred-extra runtime mode:
- When load flags request only the extra output plane, `Colorize_Framev2`
  treats extra resolution as the preferred render target.
- If `FLAG_REQUEST_FALLBACK` is not set, original-resolution output is not
  colorized for matched frames in that mode.
- If `FLAG_REQUEST_FALLBACK` is set, original-resolution output is colorized
  only on calls where `CheckExtraFrameAvailable(frameId)` fails; otherwise the
  original plane stays untouched for that frame.
- Rotation setup follows the same policy: only planes actually rendered on the
  current call receive fresh rotation tables/timers; inactive planes are reset
  so `Serum_Rotate()` does not keep rotating stale original-resolution output
  while extra-resolution output is active.

Dynamic-shadow hot path:
- `CheckDynaShadow(...)` receives pre-fetched per-frame shadow vectors
  (`dynashadowsdir*`, `dynashadowscol*`) from `Colorize_Framev2` instead of
  loading sparse vectors per pixel.
- Neighbor probing is done by iterating a compact offset table
  (8-connected neighbors) rather than repeated hand-written branch blocks.

Sprite matching prefilter:
- `Check_Spritesv2` works on the ROM frame at its own resolution (see "The ROM
  frame, and `is256x64` projects").
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
- With background-scene flag `32`, dynamic-zone pixels whose selected dynamic
  palette color is black (`0`) are treated as background-scene holes when a
  frame background mask covers that pixel. These blacked-out dynamic pixels do
  not generate dynamic shadows, and the rule does not replace an already-written
  dynamic shadow pixel.
- The same flag makes the dynamic colour decide for unlit pixels too: a
  ROM-black pixel inside an active dynamic zone whose set maps shade `0` to a
  non-black colour keeps that colour instead of taking the background under the
  mask. Without the flag the background test runs first and such a pixel always
  shows the background, which is the historical precedence existing
  colorizations rely on.
  Such a pixel is content like a lit one: it casts its layer's dynamic shadow
  and a shadow does not cover it. That is what makes inverted text work, where
  the glyph is the unlit pixel and the lit surround is mapped to black.

Mixed-resolution background scenes:
- `applySceneBackground` caches the already-rendered scene background together
  with its source dimensions.
- If a background scene and the current foreground are rendered on different
  planes because fallback selected the original-resolution foreground, the
  cached scene background is sampled into the target plane instead of being
  reused by raw flat-buffer index.
- This preserves correct background-scene composition for `64p` scene /
  `32p` fallback foreground and the inverse `32p` scene / `64p` fallback
  foreground case.

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

## The ROM frame, and `is256x64` projects

The frame a ROM delivers and the colour planes of a colorization are separate
things. Everything that touches the ROM frame — the buffer passed to
`Serum_Colorize()`, its hash, sprite detection, the shade looked up for a
pixel, the copy in `lastFrame` — goes through the *native frame* accessors in
`serum-decode.cpp`, never through `fwidth`/`fheight` directly:

- `NativeFrameWidth()`, `NativeFrameHeight()`, `NativeFramePixels()`
- `ExtraPlaneIsNative()` — the extra plane has the ROM frame's geometry, and is
  the only plane rendered

There are three layouts:

| | ROM frame | original plane | extra plane | planes libserum renders |
|---|---|---|---|---|
| 32-row ROM | `fwidth x 32` | the ROM frame's size | twice it (`64p`) | both |
| 64-row ROM (e.g. `192x64`) | `fwidth x 64` | the ROM frame's size | half of it (`32p`) | both |
| `is256x64` | `256x64` | stored at `128x32`, **not used** | the ROM frame's size, `256x64` | the `256x64` one only |

The half-size plane of the 64-row layout exists for legacy layouts and is
rendered as it always was.

`is256x64` is a header flag (cROM header revision 20, `sizeheader >= 20 * 4`).
Such a file is accepted only with `fwidth x fheight == 128x32` and an extra
plane that is `256x64` (or absent); `ValidateNativeGeometry()` refuses anything
else at load, from a cROM and from a cROMc alike. The flag belongs to the
colorization and is cleared by `SerumData::Clear()`: an older header and a v1
file never set it.

**Rule: sprite matching and all rendering happen on the ROM frame at its own
resolution. libserum does not downscale, so a ROM whose frame is `256x64` has
no `128x32` output.**

### What an `is256x64` project means at runtime

- The caller passes `256 * 64` bytes to `Serum_Colorize()`.
- **Identification** hashes all `256 * 64` bytes; comparison masks are `256x64`.
- **There is one output plane, `256x64`, in `frame64`.** It is the stored extra
  plane, rendered 1:1 with the ROM frame: plane pixel `(x, y)` reads the ROM
  shade at `(x, y)`. That shade decides "ROM lit nothing here" for a background
  mask, indexes a dynamic colour set, and is what the monochrome fallback
  paints. It is rendered by the historical all-or-nothing path the 64-row
  layout uses, from `cframes_v2_extra`, `dynamasks_extra`,
  `dyna4cols_v2_extra`, `backgroundframes_v2_extra`, `backgroundmask_extra`,
  `dynashadowsdir_extra`/`dynashadowscol_extra` and `colorrotations_v2_extra`.
- **Nothing is scaled, in either direction.** `upscaleExtraFromOriginal` (layer
  mode) is never set, and `isoriginalrequested` / `isoriginalfallbackrequested`
  are never set either, so the original-plane passes of `Colorize_Framev2()`
  and `Colorize_Spritev2()` do not run. `scaling.txt` and the scaling algorithm
  have no effect.
- **The stored `128x32` data is loaded and ignored**: `cframes_v2`,
  `dynamasks`, `dyna4cols_v2`, `backgroundframes_v2`, `backgroundmask`,
  `dynashadowsdir`/`dynashadowscol`, `colorrotations_v2`, `spritecolored`,
  `dynaspritemasks`, `dynasprite4cols`. The Win32 editor writes them, a cROM
  and a cROMc carry them, and no runtime path reads them.
- **Sprite detection** (`Check_Spritesv2`) runs on the `256x64` frame.
  `framespriteBB`, `spritedetareas`, `spritedetdwordpos`, the pixels of
  `spriteoriginal`, and the reported `frx/fry/spx/spy/wid/hei` are all in
  ROM-frame pixels. The exact-dword index and the shape-mode copy of the frame
  (`frameshape`) are sized by the ROM frame.
- **Sprite drawing** (`Colorize_Spritev2`), for a sprite found at `(frx, fry)`
  showing its region `(spx, spy, wid, hei)`: plane pixel `(frx + i, fry + j)`
  takes sprite pixel `(spx + i, spy + j)` for `i < wid`, `j < hei`. Opacity is
  `spritemask_extra`, colour is `spritecolored_extra`, the dynamic mask is
  `dynaspritemasks_extra` with `dynasprite4cols_extra`, indexed by the ROM
  shade at that same pixel. These arrays line up with `spriteoriginal` pixel
  for pixel.
- **The monochrome palette trigger** (`MONOCHROME_PALETTE_TRIGGER_ID`) takes its
  palette from the frame's `dyna4cols_v2_extra` set 0, not `dyna4cols_v2`. On a
  frame without `isextraframe` there is none and the capture fails, as it does
  elsewhere when the set is missing.
- **`lastFrame`**, the copy of the last identified ROM frame that scene handling
  re-renders, is a whole `256x64` frame.
- **A black frame** does not end a monochrome stream; "black" is judged over the
  whole ROM frame.
- **Scene markers** are `128x32` frames from `SceneGenerator`, in every layout.
  They are identified like a ROM frame, so for these projects they are hashed as
  the `4096` marker bytes followed by `12288` zeros — that is what the runtime
  `sceneFrame` buffer holds, and `BuildFrameLookupVectors()` hashes its
  generated markers the same way. Nothing draws a marker as a picture; a scene
  frame's content is whatever the colorization stores for the frame it
  identifies, i.e. that frame's `256x64` extra-plane data. Scenes, background
  scenes and their composition therefore all happen on the one plane.

### Output contract

`NativePlaneRequested()` decides whether the plane is produced, and
`ConfigureRequestedOutputMode()`, `Allocate32OutputPlane()` and
`Allocate64OutputPlane()` follow it. It follows the precedent of a 64-row
project asked for `32p` only, mirrored: the plane that would need scaling is
not available, `FLAG_REQUEST_FALLBACK` means "the other plane will do".

| `Serum_Load()` flags | plane | after `Serum_Load()` | after `Serum_Colorize()` on a renderable frame |
|---|---|---|---|
| `64P` | `256x64` | `frame64` allocated, `width64 == 256`; `frame32 == NULL`, `width32 == 0` | `FLAG_RETURNED_64P_FRAME_OK`, `width64 == 256`; no `FLAG_RETURNED_32P_FRAME_OK`, `width32 == 0` |
| `32P \| 64P` (also: neither, which requests both) | `256x64` | same | same |
| `32P \| FALLBACK` | `256x64` | same | same |
| `64P \| FALLBACK`, `32P \| 64P \| FALLBACK` | `256x64` | same | same; there is nothing to fall back to, the flag changes nothing |
| `32P` | none | load succeeds; `frame32 == NULL`, `frame64 == NULL`, `width32 == width64 == 0` | neither `FRAME_OK` flag, both widths `0` |

- `frame32`, `rotations32`, `rotationsinframe32` and `modifiedelements32` are
  never allocated for these projects, and `FLAG_RETURNED_32P_FRAME_OK` /
  `FLAG_RETURNED_V2_ROTATED32` are never reported.
- `mySerum.flags` after `Serum_Load()` is, as for every project, the request
  flags plus `FLAG_RETURNED_EXTRA_AVAILABLE` when the plane is requested and at
  least one frame has extra content.
- **A `32P`-only request** still loads the colorization and still identifies
  frames: an identified frame is a "new frame" return (`0`: no plane, so no
  rotation timer) with `frameID` set, and **triggers are reported**; monochrome
  mode is tracked. There is only no picture. Because the finished output is
  then always the same (nothing), `FinishedFrameIsAlreadyOnDisplay()` reports
  the next identified frame that fires no trigger as `IDENTIFY_SAME_FRAME`,
  like any other frame that changes nothing on the display.
- **An unknown frame** (monochrome mode, `Serum_SetMaximumUnknownFramesToSkip()`,
  `Serum_SetIgnoreUnknownFramesTimeout()`) is painted in grey or in the
  monochrome palette on the `256x64` plane, pixel for pixel, with
  `FLAG_RETURNED_64P_FRAME_OK` and `width64 == 256` only. Without the plane
  (`32P`-only) the call still returns `0` with `frameID == 0xfffffffd`, and
  both `FRAME_OK` flags and widths are clear.
- **Extra-plane data is kept at load** whenever the plane is requested,
  including for `32P | FALLBACK`: the raw v2 loader and the cROMc
  deserializer (`SerumData::serialize()`) both test for it before dropping the
  `isextra*` indices.
- On a real machine `FLAG_REQUEST_64P_FRAMES` is forced on by the existing load
  policy, so the plane is always produced there.

### Frames without extra-plane data

A frame gets its `256x64` plane only if `CheckExtraFrameAvailable()` holds — the
frame has `isextraframe`, its background (if any) has `isextrabackground`, and
**every** sprite listed for the frame has `isextrasprite`. That is the existing
all-or-nothing rule of the extra plane in the non-layer path.

Where it does not hold there is nothing to fall back to: libserum does not
upscale the unused `128x32` data, with or without `FLAG_REQUEST_FALLBACK`. The
frame is treated the way a 64-row project treats a frame without a `32p`
version under a `32p`-only request: it is identified, `frameID` and the trigger
are reported and the return value is the usual one, but neither `FRAME_OK` flag
is set and both widths are `0` for that call. `frame64` is not written and keeps
the previous picture; both rotation planes are reset. An editor must therefore
keep all three flags set throughout an `is256x64` project.

A file with the flag and no extra plane at all loads and identifies frames but
never produces a picture.

### The same statements for the other two layouts

A rendered plane holds its content — frame, masks, sprite picture — at its own
resolution, and reaches the ROM frame through its scale relative to it.

| | plane the size of the ROM frame | plane half its size | plane twice its size |
|---|---|---|---|
| which plane | original (32-row, 64-row); extra (`is256x64`) | extra (64-row) | extra (32-row) |
| ROM shade for plane pixel `(x, y)` | `(x, y)` | `(2x, 2y)` | `SampleUpscaled2x` at `(x, y)` — but see below |
| sprite lands at | `(frx, fry)`, `wid x hei` | `(frx/2, fry/2)`, `wid/2 x hei/2` | `(2frx, 2fry)`, `2wid x 2hei` |
| sprite pixel read | `(spx + i, spy + j)` | `(spx/2 + i, spy/2 + j)` | `(2spx + i, 2spy + j)` |
| dynamic sprite shade | ROM `(frx + i, fry + j)` | ROM `(frx + 2i, fry + 2j)` | `SampleUpscaled2x` |

Sprite detection, and with it `framespriteBB`, the detection areas and dwords
and `spriteoriginal`, is in ROM-frame pixels in every layout; for the first two
that is the original plane, which is why the distinction never showed.

The last column is the historical path. With a `64p` request a 32-row ROM is
rendered in layer mode instead (next section): dynamic content and sprites
without HD art are rendered on the original plane and upscaled once, and the
extra dynamic masks are ignored.

The original plane's sprite arrays (`spritecolored`, `dynaspritemasks`,
`dynasprite4cols`) and the extra plane's (`spritecolored_extra`,
`spritemask_extra`, `dynaspritemasks_extra`, `dynasprite4cols_extra`) always
belong to their plane. Opacity is `spriteoriginal` for the original plane and
`spritemask_extra` for the extra plane.

### Known gaps in the format for `is256x64`

- Whether `spritemask_extra` may differ from `spriteoriginal`'s opacity here is
  not defined. libserum draws the plane through `spritemask_extra`, as it does
  for every extra plane; detection uses `spriteoriginal`.
- `dynasprite4cols_extra` and `dynaspritemasks_extra` are loaded with
  `isextraframe` as their parent, indexed by sprite id. That is existing
  behaviour in every layout — a sprite's extra dynamic data is dropped when the
  *frame* with the same index has no extra content — but here those arrays are
  the only ones the sprite is drawn from.
- Scene markers have no `256x64` form (above).
- The header's `fwidth x fheight` still reads `128x32` and the file still
  carries a full set of original-plane arrays that nothing renders.

### cROMc

`is256x64` has been part of the serialized header since v5, and the comparison
masks carry their own element size, so the flag and the `256x64` masks survive
a save/load without any change to the format. The unused original-plane arrays
are serialized like any others. Nothing else that is persisted depends on the
ROM frame's size: the sprite runtime sidecars are derived from the sprite
arrays alone, and the identification lookups are keyed on stored hashes. The
scene lookups (`frameIsScene`, `sceneFramesBySignature`,
`sceneFrameIdByTriplet`) are derived by hashing generated markers and so do
depend on it — they are built at save time with the padded marker described
above. A cROMc written from an `is256x64` project **with scenes** by a libserum
older than this rule holds scene lookups hashed past the end of a `128x32`
buffer, i.e. garbage; it has to be regenerated from the cROM/cRZ. Nothing in
the file says which libserum wrote it, and the concentrate version was not
raised for this.

## Resolution scaling and the two-layer renderer

When the caller requests `64p` output and the content is `128x32`, libserum
produces the `64p` frame itself instead of handing back a `32p` frame for the
host to scale. This is what keeps a colorization looking the same in every
player.

### Layers

Rendering splits by *provenance*, not by plane:

- **HD-authored content** — `cframes_v2_extra`, `backgroundframes_v2_extra`,
  `backgroundmask_extra`, HD background scenes, and HD sprite art
  (`spritecolored_extra`) — rendered natively at `64p`.
- **ROM-driven content** — dynamic zones, sprites with no HD version, and the
  dynamic parts of sprites that do have HD art — rendered at original resolution
  and upscaled **once**, then composited on top.

`HasHdStaticContent()` gates the first layer. It deliberately does **not**
require every referenced sprite to have an HD version, unlike the historical
`CheckExtraFrameAvailable()`: sprites are their own layer now, so one without HD
art no longer forces the whole frame to discard its authored HD background.

**HD dynamic masks are ignored outright.** `dynamasks_extra` and friends carry no
spatial information the SD masks do not, and the authoring tool line-doubles
them, so honouring them produced blocky dynamic zones next to sharp statics. In
layer mode `frameHasDynamicExtra` is forced false, which also makes the extra
plane render the correct *underlay* beneath dynamic content.

### The coverage mask

`scaledLayerCoverage` (one byte per original-resolution pixel) records what the
scaled layer owns. Dynamic zones always; statics only when there are no HD
statics; a sprite's dynamic pixels always, its other pixels only when it has no
HD art. Pixels suppressed by `FLAG_SCENE_REPLACE_DYNAMIC_BLACK` are left unowned
so the HD background shows through.

The mask decides what the layer **contributes**. It does not decide how the
upscaler **rounds** — see the first invariant below, which is why the SD statics
are always rendered even when HD statics will cover them.

### Three invariants that are easy to break

**Select on `frame32`, never on an ownership-tagged key.** The scaled layer has
to round its edges exactly as a whole-frame upscale of the same picture would,
because that is what every other player produces. Feeding ownership into the
comparison breaks that along every layer boundary: black *outside* the layer and
black *inside* it stop comparing equal, so Scale2x's `b == h` guard — the thing
that preserves a glyph pixel sitting on the boundary — no longer fires and the
pixel is rounded away.

That was a real bug, and a subtle one: every glyph whose top row coincided with
the top of a dynamic zone lost its top row, on both the top and bottom edges of
the zone. It is also why the SD static render must always run. Skipping it when
HD statics cover it looks like free work to save, but it leaves `frame32`
incomplete, which is what forced the tagged key in the first place.

**Let the selected source decide painting, not the centre pixel.** If the
selection lands on an unowned pixel the destination is left to the HD layer —
that *is* the boundary being rounded. Falling back to the centre instead makes
every boundary pixel take the centre value, which for a thin feature such as a
one-pixel shadow is byte-identical to line doubling regardless of the algorithm
selected.

The converse is equally load-bearing: a destination pixel whose **own** source
is unowned must not be painted at all, even when the selection reaches back
inside the layer. Testing only the selected source let a dynamic zone with a
bright edge paint over the HD content beside it — Iron Man's settings screen,
where the highlighted border of the active tab bled into the letter next to it.
Both tests are needed, and they answer different questions: the selected source
decides whether the boundary rounds, the destination's own source decides
whether the layer may paint there at all. An editor upscaling the whole frame
cannot hit this, which is why the same frame looks right there.

One exception, and it is about what "HD content" means. An author who puts a
dynamic zone over artwork leaves the HD frame **black** across the zone's
footprint — the zone is what covers that area, so there is nothing to draw
there. Yielding to that black puts a speck along every edge the scaler rounds
outwards, which is what `bdk_294` frame 689 showed around the sprite drawing
its `30`. So where the selection lands outside the layer and the HD frame holds
nothing at that destination, the layer keeps its own pixel instead of yielding.
The rounding is lost at that one pixel — a square edge against the artwork —
and nothing changes where the HD frame does hold content. Measured: 77 specks
gone across the 20 reported frames, and not one pixel different on `afm_113b`
(94 frames) or `im_185ve`.

A bounded pass -- a sprite compositing its own region -- does not yield at all,
and the reason is what stands underneath. The whole frame is composited before
any sprite draws, so inside a sprite's rectangle the plane holds this layer's
own earlier output for the picture *before* the sprite existed: on `avr_200`
the ROM's white digits, under a sprite that replaces them with pink artwork.
That is not HD content to defer to, it is a stale intermediate, and yielding to
it left white specks around the rounded corners of every sprite glyph. So where
`srcBounds` is set the layer keeps its own pixel instead. Twenty-two specks per
frame on the reported frames, none now, and not a pixel different on
`afm_113b`, `bdk_294` or `im_185ve`.

**Handle `kUpscaleSourceOutside`.** Outside the frame is black, not a copy of
the edge pixel — `FrameUtil` clamped there until `db067bc`, which made content
touching row 0 see its own colour "above" it, a false edge that trips the
rounding branch. Scores drawn on row 0 lost the tops of `8`, `S`, `0`, `9` and
`3`, and only there, because the same glyphs were intact at the bottom of the
frame.

Since outside is now a real value the selection can land on it, and
`SelectUpscaled2xSourceIndex()` then returns `kUpscaleSourceOutside` instead of
an index. The composite indexes `frame32`, `sdDynaLayerMap` and
`rotationsinframe32` with that result, so it **must** test for the sentinel
first; using it as an index reads far out of bounds. The layer simply paints
nothing there.

### Keeping an unchanged plane

A frame with no HD content anywhere is the whole-frame upscale of `frame32`, so
when `frame32` has not changed the plane already holds the answer.
`lastUpscaledSdCrc` holds the CRC of the finished `frame32` — taken at the end
of the call, after every dynamic, zone, shadow and rotation entry is in it — and
a match skips both `UpscaleOriginalPlaneIntoExtra()` and
`GenerateExtraPlaneShadows()`, leaving `frame64` untouched. The flags and
`width64` are still advertised, because the caller must not be told the plane is
absent just because it was not rewritten.

This is not only work saved; it is the fix for a class of visible glitch. ROMs
send frames that differ in pixels the colorization does not use, so a stream of
"flickering" ROM frames can colorize to one static `frame32`. The upscale reads
the ROM frame too — `CornerIsSolid()` judges corners by ROM shade — so those
invisible differences moved `frame64` while `frame32` stood still. Measured on
`afm_113b` and `im_185ve`: of the single-pixel ROM changes that leave the
frame ID and `frame32` byte-identical, 360, 320 and 96 respectively used to move
the plane; now none do.

Two things make it correct rather than merely cheap:

- **Compare the finished picture, not the frame ID.** The same static picture is
  reached through different frame IDs all the time, and a rule keyed on the ID
  would miss exactly the streams this exists for.
- **Only a plane that is a pure function of that picture may be kept.**
  `lastUpscaledSdValid` follows `extraPlaneIsDerived`, and the condition also
  requires `!isextra` for the current frame. These are not the same test: the
  first rules out a plane that *was* composited over HD statics, the second
  rules out a frame that *is about to* composite them. `Colorize_Spritev2()`
  clears the flag before it draws HD sprite art for the same reason — that art
  lands after the composite, and sprite detection reads the ROM frame, which is
  precisely what differs here. `CompositeSpriteScaledLayer()` clears it too: a
  sprite without HD art reaches the plane after the frame's composite just the
  same, and `lastUpscaledSdCrc` was taken before any sprite drew, so keeping
  the plane left the sprite standing where it had been after it moved or went
  away. Both guards are load-bearing and neither can be
  mutation-tested while the other stands; `tests/README.md` says how that trap
  reads.
- **Every other writer of the plane ends the record too.** The monochrome /
  unknown-frame path paints `frame64` itself, in two ways: on a colorization
  with no authored extra plane it upscales the grey `frame32`, and on one that
  has an authored `64p` plane it writes the grey picture straight into it.
  Both call `ExtraPlaneNoLongerDerived()`. The second did not at first, and the
  frame that ends a monochrome stretch is very often the picture shown before
  it, reached from a ROM frame that differs only under the comparison mask --
  same `frame32`, same CRC -- so the grey `64p` plane was kept beside a
  colorized `32p` one until the picture changed. On `spagb_100` that was the
  first identified frame after a monochrome frame, 99 times in 85516 dump
  frames; nothing else moved.

`Serum_free()` clears both, so one colorization can never inherit another's
plane, and the CRC table is a compile-time constant — a zeroed table
hashes every buffer alike, which would not weaken the test but invert it into
"always the same" and freeze the plane on the first frame.

### Reporting a frame that is already on the display

`lastReportedOutputCrc` and `lastReportedRotationCrc` hold what the caller was
last told about. At the end of `Serum_ColorizeWithMetadatav2Internal()`, once
the frame is finished, `FinishedFrameIsAlreadyOnDisplay()` decides whether
announcing it would tell the caller anything, and when it would not the call
returns `IDENTIFY_SAME_FRAME` with `mySerum.frameID` left at
`IDENTIFY_NO_FRAME` — the same shape as the identifier's own same-frame answer,
so a host needs to understand nothing new.

The comparison has to be after the render, not after identification: two frame
ids routinely colorize to the same picture, which is the entire point of a
colorization, and only the finished frame says so.

What must never be suppressed, and is checked before the CRCs:

- **A scene**, in any form — `sceneFrameRequested`, `rotationIsScene`, a scene
  running (`sceneFrameCount`), or an end hold pending. A scene advances because
  the caller keeps being driven; tell it nothing and the scene stalls.
- **A trigger**, tested after the existing suppression block leaves
  `mySerum.triggerID` at `0xffffffff`. A trigger is an event in itself, not a
  property of the picture.
- **Different rotations behind the same picture.** The rotation tables are part
  of the record for exactly this: colours that repaint identically now but are
  about to start moving have to be announced, or they never will.

`ReportedOutputNoLongerOnDisplay()` ends the record wherever something other
than the renderer puts a picture on the display — scene-finish blanking and the
monochrome/unknown-frame path. Without it the caller would be told "nothing
changed" while the display holds the black a finished scene left behind.

### Rotations

Rotation entries are carried through the upscale using the same source selection
as the colour, so `Serum_Rotate()` animates the composited frame directly with
no re-composite per tick.

The geometry is therefore frozen at colorize time: a rotation that changed a
colour enough to alter Scale2x's edge decisions would not be reflected. This is
deliberate. It requires a rotating colour to become *equal* to one of its
neighbours, which colorizations avoid — a rotation that momentarily matched its
surroundings would read as the background rotating.

**Constraint on new algorithms:** every algorithm in
`FrameUtil::ScalingAlgorithm` must be *selection-based*, so
`SelectUpscaled2xSourceIndex()` can express it. An interpolating scaler (hq2x,
xBR, bilinear) blends colours and produces pixels no rotation entry covers.

Still outstanding: on a frame with HD statics, scaled-layer pixels resolve their
rotation index against `colorrotations_v2` while the plane ships
`colorrotations_v2_extra`. The two are independently authored and can differ in
colour list, length *and* tick rate, so a rotating non-HD sprite on such a frame
animates against the wrong table. The fix is to keep both tables and tag each
pixel (`0x0100 | index` for the SD table), which needs a second set of slot
timers and `Calc_Next_Rotationv2()` scanning 8 slots rather than 4.

### Dynamic shadows

Shadows are generated **on the extra plane, from the upscaled glyph**
(`GenerateExtraPlaneShadows()`), not scaled up with the layer. Generating them at
original resolution gets the geometry wrong twice: the shadow outline is rounded
independently of the glyph's, and a one-pixel shadow becomes two pixels, which on
a tight glyph such as `8` closes the gap between its loops.

The composite carries each lit pixel's dyna layer into `hdDynaLayerMap`, which is
what lets the pass know which per-layer direction bitmask and colour apply.

The original-resolution shadows must not be in the picture the upscale reads, or
they would be drawn twice at two thicknesses. Marking them unowned was the wrong
way to achieve that: the composite then skipped their whole `2x2` destination
block, and under the default native offset the regenerated shadow filled only one
of those two pixels, so the other was **written by nothing at all** and kept the
previous frame. On `afm_113b` that was 169391 stale extra-plane pixels across 632
of 760 frames — speckles in colours from an unrelated screen, worst around large
digits, where they read as notches bitten out of the glyph.

So they are recorded instead (`sdShadowClaim` / `sdShadowColour`) and the pixel is
left to the static pass, which paints what the shadow sits on and owns it
normally. `ReplayOriginalPlaneShadows()` puts them into the `32p` plane after the
upscale — for every request, not only for a caller that asked for that plane,
because the sprites that draw next read it (see "Sprites in layer mode"). Keyed on
`sdDynaLayerMap`, not `isdynapix` — the extra-plane pass clears and refills
`isdynapix`, so by then it no longer describes the original plane.

The `32p` output is byte-identical either way. Its per-pixel rotation entries are
not: a shadow pixel used to keep the entry belonging to the colour it had covered,
so `Serum_Rotate()` would animate the shadow with someone else's rotation. The
replay stamps `0xffff`.

Outside layer mode `CheckDynaShadow()` paints the shadow in place, and it sets
the rotation entry of the pixel to `0xffff` with it. It did not before. The
pixel loop skips a pixel a shadow has claimed, so such a pixel kept the entry an
earlier frame had left there, or what the allocation held on the first frame
after a load, and a shadow painted over a pixel already rendered kept that
pixel's entry. `Serum_Rotate()` then animated the shadow: on `afm_113b` frames
16 to 77 (white high-score digits over a sky whose stars rotate through white)
the shadow under every digit ran through the stars' colours. This path is every
`32p`-only request for a 32-row ROM, and the native plane of the other two
layouts.

**Rule: neither dynamic-zone content nor a dynamic shadow takes part in a colour
rotation, even when its colour is one of the rotation's colours.** Both get the
entry `0xffff`. Background, static and sprite pixels are looked up by colour.

The offset is a persisted per-colorization choice, `shadowOffsetMode`:
`SERUM_SHADOW_OFFSET_NATIVE` (1 extra-plane pixel — what the pre-layer renderer
produced, and what authors tuned against) or `_PROPORTIONAL` (2 pixels, matching
the `32p` output's relative thickness). Real colorizations disagree about which
looks right, hence the setting.

### Sprites in layer mode

A sprite reaches the `64p` plane through the `32p` one. `Colorize_Spritev2()`
runs after `Colorize_Framev2()` has composited the whole frame, and for each
sprite, in the order `Check_Spritesv2()` found them:

1. `scaledLayerCoverage` is reset, inside the sprite's rectangle, to what the
   frame owns there (`frameLayerCoverage`).
2. The sprite is drawn into `frame32` at `(frx, fry)`, `wid x hei`, from
   `spritecolored` / `dynaspritemasks` / `dynasprite4cols`, the dynamic colour
   indexed by the ROM shade of that pixel. Every opaque pixel is drawn. It is
   *marked* for the scaled layer when the sprite has no HD art, or when the
   pixel is dynamic in the `32p` mask.
3. `CompositeSpriteScaledLayer()` upscales the rectangle, grown by one source
   pixel, from `frame32` into `frame64` with the colorization's algorithm —
   the same selection as the whole-frame composite, on the whole `frame32`, so
   the sprite's edges round against whatever the frame shows beside it. It
   paints the destinations whose own source the mask owns and never yields
   (`srcBounds`, above). Nothing is painted when the sprite marked no pixel.
4. A sprite with HD art (`spritecolored_extra` and `spritemask_extra`) is then
   drawn natively at `(2frx, 2fry)`, `2wid x 2hei`, except where the pixel is
   dynamic in the `32p` mask; `dynaspritemasks_extra` is not consulted.

Three things follow, and each was a bug:

- **The `32p` plane is drawn into whether or not the caller is given it.** For
  a `64p`-only request the frame composite takes `FLAG_RETURNED_32P_FRAME_OK`
  back as soon as the plane has been upscaled, which is before any sprite
  draws. The sprite pass used to ask that flag whether there was a plane to
  draw into, so with a `64p`-only request a sprite without HD art never
  appeared and one with HD art lost its dynamic pixels, on every frame without
  HD statics (a frame with HD statics keeps the flag). In layer mode the plane
  is always rendered, so the mode is what the sprite pass asks.
- **The frame's dynamic shadows inside a sprite's rectangle come from the
  `32p` plane.** Step 3 repaints the rectangle, including the pixels
  `GenerateExtraPlaneShadows()` had written there, so what shows afterwards is
  the upscaled original-resolution shadow, two pixels thick, not the one
  generated on the extra plane. That requires the replay to have run, for
  every request; without it a `64p`-only request showed no shadow at all
  there.
- **The plane is no longer derived once a sprite has composited into it**, see
  "Keeping an unchanged plane".

With these the `64p` plane is the same picture for a `64p`-only request and a
`32p | 64p` one, sprites included.

### Selection and persistence

`SERUM_SCALING_*` and `SERUM_SHADOW_OFFSET_*` live in `serum.h`; the shared
implementation is `FrameUtil::ScalingAlgorithm` in libframeutil, fetched into
`third-party/include` by `platforms/<platform>/<arch>/external.sh`. Both values
are stored in the `cROMc` header from concentrate version 8 and are overridable
per colorization through `altcolor/<romname>/scaling.txt`
(`read_scaling_sidecar()`), which is **not** read on a real machine — the stored
values are authoritative there, matching how `pup.csv` and `skip-cromc.txt` are
treated.

`SERUM_SCALING_SCALE2X` (value `2`) is the default and one of only two
selectable algorithms, the other being `SERUM_SCALING_LINE_DOUBLING`. It is
libframeutil's `Scale2xPreserve`: a convex corner is rounded only where the
glyph is solid behind it. Reference
Scale2x always rounds, which eats five-pixel-tall DMD text; protecting every
lit pixel, as an earlier version did, leaves large digits square at the bottom
while the font's own chamfer still tapers the top.

The decision is `CornerIsSolid()` in `serum-decode.cpp`: for the quadrant
Scale2x wants to chip, the three source pixels behind it — both orthogonal
neighbours on the far side and the diagonal between them — must carry **the same
ROM shade as the pixel being chipped**, which happens exactly when the glyph is
two pixels thick there. Testing the diagonal alone is not sufficient: a diagonal
stroke always has a lit diagonal neighbour, so small curved glyphs were chipped
regardless.

Comparing the shade rather than mere lit-ness matters wherever a ROM draws text
over artwork. `spagb_100`'s `SUPER JACKPOT` line sits one row below a solid band
of shade 3, so every corner of that five-pixel text read as backed by solid
glyph, nothing was protected, and `S`, `R` and `C` eroded exactly as under
reference Scale2x. The band is lit, but it is not the glyph.

It is judged on the ROM frame (`romFrameForUpscale`), not on the output colour.
libframeutil can only recognize an unlit neighbour when the palette paints it
black, so a colorization with a coloured background — `afm_113b`'s
`BALL`/`CREDITS` caption on dark blue — silently lost the protection.

Because libserum applies the rule itself, it asks libframeutil to select with
plain **`Scale2x`** (`selectionAlgorithm` in `UpscaleOriginalPlaneIntoExtra()`).
libframeutil's own `Scale2xPreserve` decides first and returns the centre
whenever the neighbour it would have taken is colour zero, so `src == own` and
`CornerIsSolid()` never runs — which reinstated the blocky corners on every glyph
sitting on black: 325 of `spagb_100`'s 364 solid corners and all 174 of
`im_185ve`'s.

Reference Scale2x is not offered. The two values stay locked to libframeutil's
numbering by `static_assert`, so a host can pass what
`Serum_GetScalingAlgorithm()` returns straight through and scale the way
libserum did; libframeutil's value `0` is its reference Scale2x, which nothing
here selects. It remains there because a host scaling a finished frame has no
ROM frame to judge a corner against, so it is what that host has to use.

A stored value that is neither reads as `SERUM_SCALING_SCALE2X`, and
`scaling.txt` accepts only `scale2x` and `line-doubling`.
The selector must never be gated on extra-plane geometry: the whole-frame upscale
runs precisely when there is no extra plane, and gating there silently made every
`32p`-only colorization fall back to line doubling.

## Scene playback and options
Scene data comes from CSV (`SceneGenerator`).

Flags (from `serum.h`):
- Scene finish behavior is selected by the low finish-mode bits: `0`, `1`, or
  `2`. Other scene flags are orthogonal and may be combined with that finish
  mode.
- `0` (default): keep the last scene frame visible when the scene finishes
  until a new normal frame is identified
- `1`: black when scene finished
- `2`: show previous frame when scene finished
- `4`: run scene as background
- `8`: only dynamic content in foreground over background scene
- `16`: resume interrupted scene if retriggered within 8s
- `32`: with background scenes, replace dynamic-zone pixels whose selected
  dynamic color is black with the background scene when covered by the frame
  background mask. These blacked-out dynamic pixels do not generate dynamic
  shadows, and the rule does not replace an already-written dynamic shadow
  pixel.
  Unlit dynamic-zone pixels whose dynamic color for shade `0` is not black keep
  that color instead of showing the background scene.
  They cast dynamic shadows like lit dynamic content.

Finished-scene default behavior:
- Foreground scenes with flag `0` leave the last rendered scene frame visible
  until normal-frame identification resumes with a newly matched normal frame.
- If that next newly matched normal frame would immediately retrigger the same
  scene, libserum does not restart the scene and keeps the preserved last scene
  frame visible.
- Background scenes with flag `0` keep the last scene frame composited as
  background until a newly identified normal frame stops that background
  state.
- Same-trigger background-scene continuation does not clear that preserved
  background frame; it follows the historical seamless continuation path.

`startImmediately` behavior:
- `startImmediately` is honored for foreground scenes.
- Background scenes do not use foreground-style immediate takeover semantics.
- When a new background scene is armed, libserum immediately renders the first
  scene frame into background state and then continues by rendering the
  triggering normal frame in the foreground on the same call.
- Same-trigger background-scene continuation still uses the historical
  `IDENTIFY_SAME_FRAME` path so repeated trigger frames do not restart or
  recompose the scene between rotations.
- Foreground scenes still stop color rotations when they start immediately;
  background scenes do not.
- In preferred-extra runtime mode, scene rendering follows the same per-frame
  extra-first policy as normal frame colorization: if a scene frame has extra
  content, only the extra plane is prepared for that call; original-plane scene
  rendering is used only as `FLAG_REQUEST_FALLBACK` fallback when the extra
  scene frame is incomplete/unavailable.

## cROMc persistence
Current concentrate version: **6**.

`cROMc` stores the full Serum model and supports both Serum v1 and Serum v2
content. Real-machine policy may still restrict which source formats are
accepted at load time, but the persisted `cROMc` format itself is not v2-only.

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
- `SerumData::LoadFromFile()` must remain a streaming inflate path.
  - Do not switch file-based `cROMc` load to "read compressed payload + fully
    inflate to memory" just to reduce desktop startup time.
  - Low-memory devices depend on streaming archive load so the full inflated
    payload does not need to coexist in memory during direct file load.
  - `LoadFromBuffer()` may still use whole-buffer inflate because the caller
    has already materialized the archive in memory.
- Prepared-load metadata:
  - `hasAnyExtraFrame`
  - `publicTriggerCount`
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
- For direct `v6` loads, prepared-load metadata is reused directly; `v5`
  compatibility loads still compute it at runtime.
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

## cRZ archive API
A `cRZ` is a zip archive holding one cROM. The authoring tools write it next to
the `cROM`, and it is what gets passed around. Four public functions in
`serum-decode.h` let a host handle it without bringing its own zip code:

- `bool Serum_CRZ_Read(const char* crzPath, uint8_t** data, size_t* size)` —
  the cROM bytes, in a buffer owned by libserum.
- `void Serum_CRZ_Free(uint8_t* data)` — releases that buffer, so the caller
  does not need libserum's allocator. `NULL` is accepted.
- `bool Serum_CRZ_GetEntryName(const char* crzPath, char* name, size_t nameSize)`
  — the name of the entry `Serum_CRZ_Read()` returns. Too small a buffer is an
  error, not a truncation.
- `bool Serum_CRZ_WriteFromFile(const char* crzPath, const char* entryName,
  const char* cromPath)` — creates the archive from a cROM on disk.

Rules that hold for all of them:
- **One extraction.** `Serum_CRZ_Read()` and the loader both go through
  `ExtractCRZEntryToHeap()`; `ExtractCRZEntryToMemory()` is the loader's
  `std::vector` wrapper around it. Which entry counts as the cROM is decided in
  one place, `FindCRZEntry()`: the first entry that is a regular, supported,
  unencrypted file. The entry name is never interpreted and nothing is ever
  extracted to a path — the archive is untrusted input.
- **No runtime state.** They work on files only, neither need nor change a
  loaded colorization, and may run while another thread colorizes. They
  therefore do **not** hold the API mutex for their duration (deflating a
  450 MB cROM takes seconds); only the shared error buffer is written under it.
  They are also not gated on `is_real_machine()`: they do nothing unless a host
  calls them, and the load path's own real-machine policy is untouched.
- **Errors.** A failure returns `false` and stores a message for
  `Serum_GetLastErrorMessage()`; every call clears the message first.
- **Paths** are what miniz takes: UTF-8 on Windows (MSVC and MinGW-w64),
  native bytes elsewhere.

What `Serum_CRZ_WriteFromFile()` writes:
- one entry, deflated at miniz's default level, streamed from the source file
  through miniz's 64 KiB buffer, so the cROM is never held in memory;
- sizes and CRC in the local header, no trailing data descriptor
  (`MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE`), and no UTF-8 flag on an ASCII name —
  the layout the ColorizingDMD Win32 editor's LiteZip produces, minus its `UT`
  timestamp extra field;
- zip64 only when miniz needs it, i.e. for a source of 4 GiB or more;
- the source file's modification time as the entry time, so the archive bytes
  are not a pure function of the cROM bytes.
- The entry name has to be a plain file name; `/`, `\` and `:` anywhere in it,
  and `.`/`..`, are refused before anything is written. miniz itself only
  refuses a leading `/`, and an archive whose entry reads as a path is a hazard
  for whatever extracts it by name.
- An existing `crzPath` is replaced. The source is checked first, so a missing
  cROM does not cost the caller the archive already there; if writing fails
  after the archive was created, the incomplete file is removed. It does not
  write beside the target and rename — a caller that needs that does it.

## Logging
- Central callback configured by `Serum_SetLogCallback`.
- `serum-decode.cpp` and `SceneGenerator.cpp` both use callback-based `Log(...)`.
- Windows DLL API-boundary crash diagnostics:
  - On MSVC Windows builds, exported `libserum` API entrypoints catch both C++
    exceptions and Windows structured exceptions (`SEH`) at the DLL boundary.
  - Fatal API-boundary failures are recorded in a process-global message buffer
    exposed through `Serum_GetLastErrorMessage()`.
  - The same buffer carries the reason a `Serum_CRZ_*` call returned `false`
    or `Serum_LoadFromMemory()` / `Serum_LoadFromMemoryWithScenes()` returned
    `NULL`, on every platform.
  - Those fatal diagnostics are emitted through the configured log callback and
    also mirrored to `stderr` and `OutputDebugStringA`, so crash reasons remain
    visible even when the host does not install a log callback.
  - Structured-exception diagnostics include the Windows exception code, a
    short reason string, and the reported fault address; access violations also
    include whether the fault happened on read/write/execute and the target
    address when available.
- Successful load logging includes Serum runtime version and, for `cROMc`
  loads, the concentrate version.
- Under `SERUM_DEBUG_SCENE_VERBOSE=1`, `SceneGenerator::parseCSV(...)` logs a
  one-line parse summary with parsed scene count, skipped invalid-line count,
  and final active state. Use this first when a found `.pup.csv` still appears
  to yield `0` rotation scene frames.
- Missing-file logs from `find_case_insensitive_file(...)` use normalized path joining.
- Optional runtime debug tracing is env-gated and split by verbosity:
  - `SERUM_DEBUG_TRACE_INPUTS=1` enables high-level lifecycle logs (input,
    trigger, scene-info).
  - `SERUM_DEBUG_IDENTIFY_VERBOSE=1` enables per-candidate identification logs.
  - `SERUM_DEBUG_SPRITE_VERBOSE=1` enables sprite candidate/detection/rejection
    logs.
  - `SERUM_DEBUG_SCENE_VERBOSE=1` enables scene-path and scene-event logs.
    It also emits compact scene-lookup persistence summaries at `cROMc`
    save/load time (`pre-save`, `post-load-file`, `post-load-buffer`) with
    counts for `frameIsScene`, `sceneFramesBySignature`, and
    `sceneFrameIdByTriplet`, which is the primary debug surface for
    scene-related direct-`v6` load mismatches.
  - `SERUM_DEBUG_INPUT_CRC`, `SERUM_DEBUG_FRAME_ID`, and
    `SERUM_DEBUG_STAGE_HASHES=1` remain available as output filters and
    expensive hash tracing controls.
  - Debug-only identify/sprite/stage-hash lines must stay silent by default
    and may only appear when the corresponding env-gated debug mode is enabled.
- Optional runtime profiling:
  - If env `SERUM_PROFILE_LOAD_TIMES=1`, startup load timing is logged for the
    major load stages. This emits:
    - archive-level timing from `SerumData::LoadFromFile/LoadFromBuffer`
      (`Perf load archive: ... total=... deserialize=...`)
    - top-level startup timing from `Serum_Load(...)`
      (`Perf load total: ...`)
    - Use this first when comparing `v5` vs `v6` startup regressions, because
      it separates archive deserialize cost from post-load restore/rebuild
      stages.
  - If env `SERUM_PROFILE_DYNAMIC_HOTPATHS` is enabled (`1/true/on/yes`),
    periodic average timings are logged for the full end-to-end rendered-frame
    round trip (`frame`), `Colorize_Framev2`, `Colorize_Spritev2`, and
    `Check_Spritesv2`, along
    with average identification time (`Identify_Frame`) split into
    normal/scene calls plus the critical-trigger mini-matcher, input/result
    counters (`inputs`, `rendered`, `same`, `noFrame`), and current process
    RSS memory usage and process-local peak RSS seen so far.
  - `Perf dynamic avg` is emitted on fixed 240-input host windows, not on
    rendered-output count, so runs stay comparable even when different
    branches suppress or render different numbers of frames from the same dump.
    The window size is conveyed by `inputs=...`; no extra trailer is appended.
  - If env `SERUM_PROFILE_DYNAMIC_HOTPATHS_WINDOWED=1`, the same counters are
    reset after each emitted 240-input block so each `Perf dynamic avg` line
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
    - `SerumData::SaveToFile()` must ensure
      `colorRotationLookupByFrameAndColor` is populated before serializing
      `v6`; otherwise direct `v6` load will rebuild the lookup at startup and
      incur avoidable load-time cost.
