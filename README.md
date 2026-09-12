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

## Upscaling

If you ask `libserum` for `256x64` frames and the ROM is `128x32`, you get
`256x64` frames. `libserum` performs the upscale itself, using the algorithm the
colorization author selected, rather than leaving it to the host. That is the
only way a colorization looks the same in every player, so hosts should no
longer scale Serum output themselves.

A frame is rendered as two layers:

- HD-authored content — static colorization, background images and background
  scenes — is rendered natively at `256x64` and stays sharp.
- Everything driven by the `128x32` ROM frame — dynamic zones, sprites without
  an HD version, and colour rotations — is rendered at original resolution and
  upscaled **once**, then composited on top.

Dynamic shadows are generated afterwards, directly on the upscaled result, so a
shadow always follows the shape of the glyph it belongs to.

Two algorithms are available:

- **Scale2x** (default) — a convex corner is rounded only where the glyph is
  solid behind it. Reference Scale2x rounds every corner by taking a neighbour,
  and where that neighbour is empty one of the pixel's four output pixels goes
  with it. On a large digit that single chip is the wanted rounding, and it
  matches the chamfer the font already draws at the top. On DMD text five pixels
  tall, where a stroke is one pixel wide and a glyph is almost nothing but
  corners, the same chip reads as a hole — `S`, `R` and `C` stop being readable.

  The two cases are told apart by the three source pixels *behind* the corner:
  they carry the glyph's own shade exactly when it is at least two pixels thick
  there. A large glyph rounds; a one-pixel stroke never can, so it is kept
  whole. Testing only the diagonal is not enough — a diagonal stroke has a lit
  diagonal neighbour by definition, and small curved letters were chipped
  anyway — and testing only whether the pixels behind are *lit* fails wherever
  a ROM draws its text over artwork rather than over black.

  The test is made on the ROM frame, not on the output colour: after
  colorization an unlit pixel is only recognizable when the palette happens to
  paint it black, so text on a coloured background would lose the protection
  entirely.
- **line doubling** — each source pixel becomes a `2x2` block

Reference Scale2x used to be offered beside this one. It is not any more: a
colorization cannot ask for one algorithm on its text frames and another on its
artwork — most ROMs alternate between the two constantly — and rounding by what
is behind the corner is right for both, which is what reference Scale2x is not.

The algorithms come from
[libframeutil](https://github.com/PPUC/libframeutil), shared with the rest of
the PPUC stack, where reference Scale2x remains — a host scaling a finished
frame has no ROM frame to judge a corner against, so it is what that host has to
use. Hosts can read libserum's selection back with `Serum_GetScalingAlgorithm()`
and pass it straight to libframeutil, so any further scaling they do matches.

### Thousands separators

Scale2x joins two pixels of the same colour that touch only diagonally, and in
a score that is exactly what fuses a comma to the digit beside it: the comma's
tail sits one row below the text and one column across, so the scaler runs a
diagonal between the two and they become one shape.

libserum takes those separators out of the picture before scaling and puts them
back afterwards by scaling them **by themselves**, so the digits have nothing
adjacent to bridge to while the separator's own diagonal — a comma is a body
with its tail one row down and one column across — still rounds like everything
around it. Line doubling them instead kept the bridge away but squared the comma
off, leaving its two halves meeting at a corner in the middle of smoothed text.

What is taken is the whole separator, grown from the columns that reach below
the bottom line to whatever they connect to. If that shape turns out to be too
large or too wide to be a separator — a descender joined to its letter, or a
comma drawn hard against the digit beside it — only the descending columns are
taken, which is the conservative answer: the comma keeps a square tail and the
letter is left alone. The search
runs one shade of the **ROM frame** at a time, not one colour of the finished
picture. That is not an optimization: a colorization that paints a background
behind its score — most of them — has no empty row anywhere for a whole-frame
search to find text with, so the search needs something to separate text from
what surrounds it. Within one shade the structure is unmistakable, and a
separator is then a narrow column carrying pixels below the text's bottom line.

The run of columns that spacing alone would join is then narrowed to the line of
text inside it. A separator sits within a number, three digits from its end, so
only that number can say where the bottom line is — and a group's row band is
the rows it covers without a break, so anything taller pulled into the group
pulls the bottom line with it. Columns more than twice the height of the glyphs
around them are therefore cut out, and the run is judged in the pieces that
remain: a rule, a border, a divider or a bargraph standing beside a score is not
part of it.

A column's height is the longest unbroken run of rows it covers, not how many
rows it covers in total. Two lines of scores in one dynamic zone put two runs in
the same columns, and a total would make every column twice a glyph tall — so
the allowance would grow with each line added, and at three lines it would
exceed the display and nothing could be cut out at all. The longest run is one
glyph however many lines share the zone.

On `im_185ve` the same `4,076,760` sits two columns from the same divider on two
frames — close enough for the spacing to join them — as dynamic content on one
and static on the other. Its commas were filtered on one frame and not the other
until the divider stopped counting as part of the number.

The ROM rather than the output because everything the search asks is a question
about shape, and the shape is in the ROM. A score font drawn as a colour
gradient is one glyph there; in the finished picture each of its colours is a
horizontal band a few rows tall, scattered across the digits, and the search
sees several unrelated scraps of text instead of one number. On `spagb_100` that
split the two commas of `36,269,900` — the same three pixels drawn twice — so
one was filtered and the other was not, and which of them survived changed with
the value on the display.
A column that carries the glyph above it is never a candidate, which is what
keeps a letter's own pixels out of the mask; marked pixels are line doubled, so
marking one would damage the letter around it.

There is no sidecar setting for this — it is always on. A handful of artwork
pixels per frame can match the same shape and get line doubled with the commas.
Measured at 0.3% of a frame, and not noticeable in practice.

### `scaling.txt`

Authors override the defaults with an optional sidecar next to the colorization
files:

```text
altcolor/<romname>/scaling.txt
```

Each non-empty line is either a bare algorithm name or a `key: value` setting.
`#` starts a comment:

```text
# the default; line-doubling is the alternative
scale2x
shadow-offset: proportional
```

| setting | values | default | meaning |
|---|---|---|---|
| *(bare word)* or `scaling:` | `scale2x`, `line-doubling` | `scale2x` | upscaling algorithm |
| `shadow-offset:` | `native`, `proportional` | `native` | how far dynamic shadows are offset on the upscaled plane |

`shadow-offset: native` offsets a shadow by one `256x64` pixel, which is what
`libserum` has always rendered into the high-resolution plane and therefore what
most colorizations were tuned against. `proportional` uses two pixels, keeping
the shadow's thickness proportional to the glyph and matching the `128x32`
output. Tight glyphs such as `8` can lose the gap between their loops under
`proportional`, while thicker fonts often look better with it — which is why it
is a per-colorization choice.

Dropping in or editing `scaling.txt` next to an existing `*.cROMc` regenerates
that `*.cROMc` on the next load, so the settings take effect immediately and are
then carried by the archive itself. On real-machine targets `scaling.txt` is not
read and the values stored in the `*.cROMc` are used.

Downscaling is not done by `libserum`. A `128x32` request against `64p`-only
content still returns the `64p` frame, and the host decides how to reduce it.

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
   the finish behavior is selected by the low bits: 0, 1 or 2
   0 - default: keep the last scene frame visible when the scene finishes
       until a new normal frame is identified
       if that next identified normal frame would immediately retrigger the
       same scene, the scene is not restarted and the preserved last scene
       frame remains visible
   1 - black screen when scene finished
   2 - show last frame before scene started when scene finished
   4 - run scene as background
       after a background scene finishes with flag 0, its last scene frame
       remains visible in the background until a newly identified normal frame
       stops that background state; same-trigger continuation does not clear it
   8 - replace static content with background scene, only dynamic zones,
       sprites and shadows stay in the foreground
  16 - continue scene at previous frame when interrupted for less than 8s
  32 - with flag 4, replace dynamic-zone pixels whose selected dynamic color is
       black with the background scene; these pixels do not generate dynamic
       shadows and do not replace already-written dynamic shadow pixels
```

Positions `4` to `10` are optional. If not provided, the default is `0`.

## Build

#### Windows (x64)

```shell
cmake -G "Visual Studio 18 2026" -DPLATFORM=win -DARCH=x64 -B build
cmake --build build --config Release
```

#### Windows (x86)

```shell
cmake -G "Visual Studio 18 2026" -A Win32 -DPLATFORM=win -DARCH=x86 -B build
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
