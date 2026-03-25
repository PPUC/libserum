# libserum

`libserum` is a cross-platform library for decoding Serum colorization files for
pinball ROMs.

Originally, `libserum` was created by
[Zed](https://github.com/zesinger/libserum). The last upstream/original release
line was `v2.3.1`. This repository is the actively maintained successor fork.

## Overview

`libserum` loads a colorization package from the `altcolor/<romname>/`
directory, identifies incoming ROM frames, colorizes them, applies color
rotations, handles sprites and dynamic zones, and optionally plays rotation scene
animations.

At runtime, the caller mainly interacts with:

- `Serum_Load(...)`
- `Serum_Colorize(...)`
- `Serum_Dispose()`

All runtime output is returned through `Serum_Frame_Struc`, defined in
`src/serum.h`.

## Serum Formats

`libserum` supports two Serum content generations:

- `Serum v1`
  - classic palette-based colorization output
- `Serum v2`
  - direct color frame output for 32-pixel and 64-pixel height DMD planes
  - optional rotation scenes

This fork also supports the concentrated archive format:

- `*.cROMc`
  - preprocessed runtime archive generated from `cROM` / `cRZ` source data
  - supports both `Serum v1` and `Serum v2` content
  - stores additional derived runtime-ready data so startup is faster and RAM
    use is lower than rebuilding everything from raw source data every time
  - Memory paeks can happen when loading the colorization

## Main Differences To Original libserum (`v2.3.1`)

Compared to the original upstream line ending at `v2.3.1`, this fork adds and
maintains:

- `cROMc` support
  - concentrated runtime archive format
  - automatic generation from raw `cROM` / `cRZ` source files
  - much better startup behavior on repeated loads
- better cross-platform support
  - Windows, macOS, Linux, iOS, tvOS, Android
- Serum v2 rotation scene support with persisted runtime lookup data
- monochrome fallback handling for non-colorized ROM frames
- extensive bug fixes and performance work in:
  - frame identification
  - sprite handling
  - dynamic zones
  - scene playback
  - direct `cROMc` runtime loading

The important architectural difference is:

- original `libserum v2.3.1` primarily worked with raw Serum source files
- this fork keeps raw source support, but treats `cROMc` as the preferred
  runtime format

## Loading Model

On desktop/authoring-style usage, `libserum` typically:

1. looks for `*.cROMc`
2. if not present or skipped, loads raw `*.cROM` / `*.cRZ`
3. for `Serum v2`, optionally applies `*.pup.csv` (only relevant for colorization authors)
4. can write an updated `*.cROMc`

On real-machine targets:

- only `*.cROMc` is supported
- `*.pup.csv` and `skip-cromc.txt` are ignored

## Monochrome Triggers

`libserum` supports two dedicated trigger IDs:

- `65432` (`MONOCHROME_TRIGGER_ID`)
  - enables monochrome fallback mode
  - in `v2`, incoming ROM shades are rendered with fixed `greyscale_4` /
    `greyscale_16`
  - in `v1`, the configured standard monochrome palette is used

- `65431` (`MONOCHROME_PALETTE_TRIGGER_ID`)
  - enables palette-based monochrome fallback mode (`v2` only)
  - the monochrome palette is captured from dynamic color set `0` of the
    triggering frame
  - subsequent monochrome frames use this captured palette instead of the fixed
    greyscale palette

Both modes remain active while frames are unknown/not colorized and are
reevaluated when a new frame is identified.

## Rotation Scenes

For `Serum v2`, scenes are authored in `*.pup.csv`.

Format of a PUP scene line:

```text
1: PUP scene ID
2: number of frames
3: duration of each frame
4: 0 - not interruptable, 1 - interruptable by frame match or PUP event
5: 0 - start immediately, replacing the triggering frame, 1 - start after frame duration
   background scenes do not replace the triggering frame; instead the first
   background scene frame is prepared immediately and the triggering normal
   frame still renders in the foreground
6: 0 - play once, 1 - loop, >= 2 - repeat x times
7: 0 - no frame groups, >= 2 - create x frame groups
8: 0 - play frame group in order, 1 - play random frame group
9: 0 - no autostart, >= 1 - start this scene after x seconds of inactivity
   if scene flag 0 is used for a non-interruptable scene, this value is used as
   end-hold duration in seconds instead
10: scene flags
   1 - black screen when scene finished
   2 - show last frame before scene started when scene finished
   4 - run scene as background
   8 - replace static content with background scene, only dynamic zones,
       sprites and shadows stay in the foreground
  16 - continue scene at previous frame when interrupted for less than 8s
```

Positions `4` to `10` are optional. If not provided, the default is `0`.

## Build

#### Windows (x64)

```shell
cmake -G "Visual Studio 17 2022" -DPLATFORM=win -DARCH=x64 -B build
cmake --build build --config Release
```

#### Windows (x86)

```shell
cmake -G "Visual Studio 17 2022" -A Win32 -DPLATFORM=win -DARCH=x86 -B build
cmake --build build --config Release
```

#### Windows MinGW / MSYS2 UCRT64 (x64)

Requires MSYS2 with UCRT64 environment. Install dependencies:

```shell
pacman -S --noconfirm \
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libwinpthread \
  mingw-w64-ucrt-x86_64-cmake
```

Build inside the MSYS2 UCRT64 shell:

```shell
MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -l -c "
  cd \"$(pwd)\" &&
  cmake -DCMAKE_BUILD_TYPE=Release -DPLATFORM=win-mingw -DARCH=x64 -B build &&
  cmake --build build -- -j\$(nproc)
"
```

#### Linux (x64)

```shell
cmake -DPLATFORM=linux -DARCH=x64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### Linux (aarch64)

```shell
cmake -DPLATFORM=linux -DARCH=aarch64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### macOS (arm64)

```shell
cmake -DPLATFORM=macos -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### macOS (x64)

```shell
cmake -DPLATFORM=macos -DARCH=x64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### iOS (arm64)

```shell
cmake -DPLATFORM=ios -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### iOS Simulator (arm64)

```shell
cmake -DPLATFORM=ios-simulator -DARCH=arm64 -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### tvOS (arm64)

```shell
cmake -DPLATFORM=tvos -DARCH=arm64 -DBUILD_SHARED=OFF -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

#### Android (arm64-v8a)

```shell
cmake -DPLATFORM=android -DARCH=arm64-v8a -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```
