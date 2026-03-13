# AGENTS.md

## Purpose
This document summarizes `libserum` runtime flow, cROMc persistence, and compatibility rules.

Maintenance rule: behavior, format, or API changes must update this file in the same change.

## Core files
- `src/serum-decode.cpp`: runtime load / identify / colorize pipeline.
- `src/SerumData.h/.cpp`: model + cROMc serialization.
- `src/SceneGenerator.h/.cpp`: PUP CSV scene parsing/generation.
- `src/sparse-vector.h`: sparse storage and payload serialization.

## Load flow
Entry: `Serum_Load(altcolorpath, romname, flags)`.

1. Reset runtime state (`Serum_free`).
2. Detect optional `*.pup.csv`.
3. Prefer `*.cROMc` unless `skip-cromc.txt` exists.
4. Fallback to `*.cROM` / `*.cRZ`.
5. Parse CSV scenes when present.
6. Build/refresh frame lookup vectors after final scene data is known.

## Identification and colorization strategy
- Keep master strategy/semantics for frame matching and colorization.
- Normal matching excludes scene frames.
- Scene and normal trackers remain independent.
- No behavior change here is part of this memory-optimization step.

## cROMc format
Current concentrate version: **6**.

### v6 sparse-vector payload layout
- Sparse vectors are serialized in packed form:
  - `packedIds`
  - `packedOffsets`
  - `packedSizes`
  - `packedBlob`
- Packed payloads are deduplicated by content during packing.
- Optional binary bit-packing is supported for boolean-like `uint8_t` vectors.
- Runtime can still modify vectors via `set()`; packed storage is restored to mutable map form only when needed.

### v5 compatibility
- v5 cROMc files remain loadable.
- v5 sparse-vector legacy layout is deserialized via a load-time legacy flag and converted to packed runtime representation.
- Backward compatibility is required only for v5.
- Compatibility between unreleased v6 development snapshots is not required.

## Compression policy notes
- `dyna4cols_v2` and `dyna4cols_v2_extra` are LZ4-compressed sparse vectors.
- `backgroundmask` and `backgroundmask_extra` use binary bit-packing plus LZ4.
- Sprite vectors with known sensitivity (`spritedescriptionso`, `spritedescriptionsc`, `spriteoriginal`, `spritemask_extra`) remain uncompressed/unbitpacked.

## Validation checklist
1. `cmake --build build -j4`
2. Load tests:
- v5 cROMc
- v6 cROMc
- cROM/cRZ with and without CSV
3. Verify no functional change in master matching/colorization behavior.
