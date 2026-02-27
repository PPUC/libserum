# libserum

This is a cross-platform library for decoding Serum files, a colorization format for pinball ROMs.

Originally, libserum has been created by [Zed](https://github.com/zesinger).
Since he quit to maintain his original version, this (friendly) fork of the [original libserum](https://github.com/zesinger/libserum) is the official successor where bugfixing and the development of new features is happening.

Some of these new features are
- support of the cROMc format which leads to faster loading times and a way lower memory footprint
- better support for iOS and Android
- rotation scenes
- monochrome mode for not colorized frames like error messages, system diagnostics, settings menu, tools like motor adjustments, coin door open warnings or older or patched ROM versions

## monochrome triggers

libserum supports two dedicated trigger IDs:

- `65432` (`MONOCHROME_TRIGGER_ID`):
  - Enables monochrome fallback mode.
  - In v2, incoming ROM shades are rendered with fixed `greyscale_4` / `greyscale_16`.
  - In v1, the configured standard monochrome palette is used.

- `65431` (`MONOCHROME_PALETTE_TRIGGER_ID`):
  - Enables palette-based monochrome fallback mode (v2 only).
  - The monochrome palette is captured from dynamic color set `0` of the triggering frame.
  - Subsequent monochrome frames use this captured palette instead of `greyscale_4` / `greyscale_16`.

Both modes remain active while frames are unknown/not colorized and are reevaluated when a new frame is identified.

## rotation scenes

Format of a PUP scene line:
```
1: PUP scene ID
2: number of frames
3: duration of each frame
4: 0 - not interruptable, 1 - interruptable by frame match or PUP event
5: 0 - start immediately, replacing triggering frame, 1 - start after frame duration (see 3)
6: 0 - play once, 1 - loop, >= 2 - repeat x times
7: 0 - no frame groups, >= 2 - create x frame groups (you get x times the number of frames entered in 2 to play changing scenes)
8: 0 - play frame group in order, 1 - play random frame group
9: 0 - no autostart, >= 1 - start this scene after x seconds of inactivity (no new frames), only use once, could be combined with frame groups.
       if scene flag 0 is used for a non-interruptable scene, this value is used as end-hold duration (seconds) instead
10: scene flags. When no flags are provided and scene is finished, the last frame of the scene is shown until a new frame is matched.
   1 - black screen when scene finished
   2 - show last frame before scene started when scene finished
   4 - run scene as background
   8 - replace static content with background scene, only dynamic zones, sprites and shadows will be in the foreground
  16 - continue scene at previous frame when interrupted for less than 8s
```

Postions 4 - 10 are optional. If not provided, the default is 0.
