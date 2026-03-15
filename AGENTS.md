# AGENTS.md

## Purpose
This document explains how `libserum` works end-to-end, with emphasis on runtime flow, scene handling, and cROMc persistence.

**Maintenance rule:** Any feature change, behavior change, data format change, or API/signature change in this repository **must** be reflected in this file in the same PR/commit.

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
   - If loaded from cROMc v6 and no CSV update in this run: use stored lookup via `InitFrameLookupRuntimeStateFromStoredData()`.
   - Otherwise: rebuild via `BuildFrameLookupVectors()`.
8. Build/normalize packing sidecars via `BuildPackingSidecarsAndNormalize()`.
   - The normalization step is idempotent and guarded; repeated calls in the
     same load/save cycle are no-ops once completed.

Important:
- `BuildFrameLookupVectors()` must run after final scene data is known for this load cycle.
- CSV parsing after loading can invalidate stored scene lookup data and requires rebuild.

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
5. Initialize `lastfound_scene` / `lastfound_normal` from first available IDs.

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

Background placeholder policy:
- `Colorize_Framev2` supports `suppressFrameBackgroundImage`.
- When true, frame-level background images are treated as placeholders and existing output pixel is kept in masked background areas.
- This is used when a background scene is active so the scene background can continue while foreground content changes.

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
- Sparse vectors in packed sparse layout.
- Normalized sentinel vectors plus sidecar flag vectors for transparency and
  dynamic-zone activity.

Backward compatibility:
- v5 files are loadable.
- v5 sparse vectors are deserialized with legacy sparse-vector layout and converted to packed representation after load.
- For v5 loads, scene lookup vectors are rebuilt at startup.
- For v6 loads, stored lookup vectors are reused unless scene data changed in this load cycle (for example CSV update), in which case lookup vectors are rebuilt.

v6 snapshot policy:
- Compatibility between unreleased v6 development snapshots is not required.
- Compatibility to released v5 remains required.

## Logging
- Central callback configured by `Serum_SetLogCallback`.
- `serum-decode.cpp` and `SceneGenerator.cpp` both use callback-based `Log(...)`.
- Missing-file logs from `find_case_insensitive_file(...)` use normalized path joining.

## Safety invariants
- `frameIsScene.size()` must equal `nframes` before identification.
- `sceneFramesBySignature` must correspond to current scene data and current loaded frame definitions.
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
