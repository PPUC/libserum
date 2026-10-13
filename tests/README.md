# libserum tests

`unit_tests.cpp` is a single translation unit that `#include`s
`src/serum-decode.cpp` directly. That is deliberate: almost everything worth
pinning down here is a `static` function or a file-scope buffer, and widening
the public surface so a test could reach it would be the worse trade. It also
means there must stay exactly **one** such TU, or the implementation is compiled
twice and the link fails.

The tests build their frames in memory — a handful of pixels chosen to make one
rule observable. There are no dumps and no colorization files in the repository.
The one test that can use a real file takes its path from the environment:

    SERUM_TEST_CRZ=/path/to/rom.cRZ ./build/serum_unit_tests crz/

Frame buffers come from `AllocateFrameWorkBuffers()`, the same function both
load paths call, so a buffer added there is present here too.

The `n256/` tests are the exception to building `g_serumData` by hand. They
serialize a small cROM exactly as the editor writes one (`n256::Synth`), put it
in a temporary directory and go through `Serum_Load()` and `Serum_Colorize()`,
once from the cROM and once from the cROMc generated from it. That covers the
stream loader, the concentrate round trip, identification and the sprite
sidecars without a fixture file. The ROM frame handed to `Serum_Colorize()` is a
heap buffer of exactly one frame, so a read past it is something the sanitizer
build names.

## Running

    cmake --build <build dir> --target serum_unit_tests
    ctest --test-dir <build dir>

Or run the binary directly; it takes an optional substring to filter test names:

    ./build/serum_unit_tests shadow/

Configure with `-DENABLE_SANITIZERS=ON` to run the same tests under
AddressSanitizer and UndefinedBehaviorSanitizer. It needs the build type and
the platform as well, because the flag is only honoured for a Debug build on
macOS or Linux and `PLATFORM` defaults to `win`:

    cmake -S . -B build-san -DENABLE_SANITIZERS=ON \
          -DCMAKE_BUILD_TYPE=Debug -DPLATFORM=macos -DARCH=arm64

Get that wrong and the suite still builds, still passes, and instruments
nothing — which is how three out-of-bounds accesses in the fixtures survived a
whole series of "sanitizer clean" runs. `CMakeLists.txt` now warns when the
option is set but cannot take effect; if you do not see the warning and the
binary is not linked against `libclang_rt.asan`, the run means nothing. Do that at least once per
change to the render path: one of the bugs below was an out-of-bounds read whose
*output* was whatever happened to follow a table in memory, so it cannot be
pinned by an assertion on pixels — but ASan names the line.

## What each test is protecting

Every case exists because something once broke. The commit named beside it
explains the rule; if the test fails, read that commit message first.

| test | commit |
|---|---|
| `preserve/thin_arm_under_lit_band` | `a424478` — a corner is judged by the glyph's shade, not by whether anything behind it is lit |
| `preserve/lit_band_scales_as_artwork` | the same rule must not become blanket protection |
| `preserve/solid_corner_rounds` | `84330ef` — a glyph two pixels thick gets its corner rounded |
| `preserve/corner_needs_both_orthogonals` | `c2e1a9b` (reverted) — testing only the diagonal chips small curved letters |
| `preserve/rounds_on_black_background` | `cabe025` — libframeutil must be asked for plain Scale2x, or it decides first |
| `upscale/every_pixel_written` | `5b5b0f7` — nothing else writes this plane, so a skipped pixel keeps the previous frame |
| `upscale/line_doubling_replicates` | line doubling is exactly a 2x2 replication |
| `upscale/no_yield_to_hd_nothing` | the layer yields a rounded pixel to HD content, but not to the black an author leaves under a dynamic zone |
| `upscale/sprite_region_not_left_stale` | a sprite's own pass paints its whole region, so the frame's earlier composite of the picture before the sprite drew cannot survive at its rounded edges |
| `shadow/every_dyna_layer` | `fb5c3a1` — the layer index is bounded by the table it indexes, not by the v1 constant |
| `shadow/claim_marker_is_not_a_layer` | `e9f8c07` — a painted shadow is not content and casts nothing |
| `shadow/offset_modes` | native is one extra-plane pixel, proportional is two |
| `shadow/never_covers_lit_content` | a shadow goes behind the glyph, and the first one to claim a pixel keeps it |
| `shadow/deferred_leaves_plane_alone` | `8dd46b4` — the upscale must read a shadow-free picture |
| `shadow/replay_yields_to_lit_content` | the replay keyed on `sdDynaLayerMap`, not on the reused `isdynapix` |
| `rotation/no_rotation_writes_both_halves` | `effef51` — the offset is written too, or the output differs between runs |
| `hash/frame_dword_slot_uses_high_bits` | Fibonacci hashing carries its entropy upwards |
| `separator/not_fused_with_digit` | a thousands separator is taken out of the picture before scaling, so it cannot bridge to the digit |
| `separator/stern_score_column` | the left zone of a Stern SAM ROM — four player scores stacked in one zone, the active one drawn larger, the shot value under it, a divider alongside — every line keeps its separator |
| `separator/two_lines_keep_separators` | two lines of scores in one zone keep theirs, with a border alongside — the column height is the longest unbroken run, so it does not grow with the number of lines |
| `separator/full_height_rule_ignored` | a column far taller than the glyphs around it is cut out of the text run, so a divider beside a score cannot change whether its separators are found |
| `separator/scaled_as_its_own_shape` | it is put back by scaling it by itself, so its own diagonal rounds instead of being stamped flat |
| `separator/touching_a_digit_is_not_grown` | one drawn hard against a digit keeps its square tail rather than growing into the digit |
| `separator/still_casts_a_shadow` | it carries its dyna layer, so the shadow pass still gives it a shadow, following the rounded shape |
| `separator/identical_treated_alike` | the same separator drawn twice on one line is treated the same way, whatever digits sit beside it |
| `separator/survives_a_row_wipe` | a score wiped away one row at a time keeps its commas while the picture around them is untouched, and stops keeping them when the wipe arrives |

Not everything here is a past bug. The rest covers behaviour that had no test at
all and would fail quietly — a misparsed `scaling.txt` falls back to a default
nobody chose, a rotation that ticks at the wrong rate looks like a slow machine,
and a packing bug corrupts every colorization at once.

| test | what it pins |
|---|---|
| `separator/envelope_follows_the_font` | how tall a separator may be follows the height of the text it belongs to; the width bound is still fixed, and the test says so |
| `colorize/unchanged_picture_keeps_extra_plane` | an unchanged `128x32` picture keeps the `256x64` plane already derived from it, and a changed one derives again; the state does not outlive the colorization |
| `colorize/hd_sprite_art_blocks_reuse` | HD sprite artwork lands in the plane after the composite, so the plane can no longer be kept |
| `colorize/monochrome_frame_blocks_reuse` | a monochrome frame written into an authored `64`-row plane ends the kept plane, so the colorized picture that follows it -- the same `128x32` picture as before the monochrome frame -- is derived again instead of standing beside a grey plane; from a cROM and from its cROMc, for `64P` and `32P \| 64P` |
| `colorize/hd_static_frame_is_not_reused` | frames that composite HD statics each draw their own artwork, whichever frame the plane was last derived from |
| `colorize/finished_frame_already_on_display` | a finished frame that matches the picture the caller already has is not announced again — unless it rotates differently, or a scene or trigger makes it news whatever it looks like |
| `colorize/static_content` | a frame's own colours come through untouched, whatever the ROM shade was |
| `colorize/background_mask` | the background image shows where its mask is set and the ROM lit nothing, and only there |
| `colorize/dynamic_zone_colours` | inside a dynamic zone the colour comes from that zone's set, indexed by the ROM shade |
| `colorize/dynamic_zone_active_mask` | a zone only applies where its active mask says so; elsewhere the static colour stands |
| `crz/write_then_read_round_trips` | a cRZ written from a cROM file reads back byte for byte under the entry name it was given, through the public call and through the loader's own extraction |
| `crz/plain_deflated_zip` | a small cROM yields a plain deflated zip: no zip64, no data descriptor, no UTF-8 flag on an ASCII name — what older readers expect |
| `crz/failures_are_clean` | a failed read hands back nothing to free and says why; a failed write neither destroys an archive already there nor leaves half of one |
| `crz/real_archive` | checks nothing unless `SERUM_TEST_CRZ` names a real cRZ, which must then read and survive being written again |
| `memory/matches_file_load` | `Serum_LoadFromMemory()` gives the same loaded state and the same output as `Serum_Load()` of the same bytes as a `.cROM` file, for a `128x32` and an `is256x64` project and seven request flag combinations |
| `memory/writes_no_file` | a memory load writes nothing, with cROMc generation on |
| `memory/failures_are_clean` | `NULL`, empty, truncated at any block, impossible element counts, a refused geometry and bytes that are no cROM all return `NULL` with an error message and nothing loaded; a valid load afterwards works |
| `sidecar/spellings` | every spelling `scaling.txt` advertises, bare and under `scaling:`/`algorithm:` |
| `sidecar/shadow_offset` | `native`/`proportional`/`1`/`2`, and both keys in one file |
| `sidecar/tolerance` | comments, blank lines, padding, case; an unknown key or value leaves the stored choice alone rather than replacing it with a default |
| `sparse/round_trip` | an element never set reads back as the no-data signature, so the render path can index it unconditionally |
| `sparse/drops_empty_payloads` | an all-no-data payload is not stored — the point of the structure |
| `sparse/parent_gating` | a child vector stores nothing where its parent has nothing (`dynamasks_extra` hangs off `isextraframe` this way) |
| `sparse/value_packing_is_exact` | packed values survive exactly, at every width; a normalization to 0/1 would flatten every dynamic mask |
| `sparse/wide_values_round_trip` | `uint16_t` vectors — the colorized frames — come back bit for bit |
| `rotation/advances_tagged_pixels_only` | a pixel tagged with a slot advances, a pixel tagged `0xffff` never does |
| `rotation/wraps_at_length` | the shift wraps at the rotation's length instead of running off its colour list |
| `rotation/empty_slot_not_due_next` | a slot with no colours or no delay is not a rotation and is not reported as due |

### `is256x64` projects

A cROM whose ROM frame is `256x64` while its header describes a `128x32`
original plane and a `256x64` extra plane. libserum renders the `256x64` plane
only and has no `128x32` output for these; the synthetic project gives the
stored `128x32` data colours of its own, so any of it reaching a plane fails a
pixel comparison. `AGENTS.md` ("The ROM frame, and `is256x64` projects") has the
rule each test pins.

| test | what it pins |
|---|---|
| `n256/loads_and_forgets` | the flag leaves one plane, `256x64`; no `32p` buffer is allocated, the scaled layer and the original-plane passes stay off, and the flag is gone again after `Serum_free()` |
| `n256/rejects_other_geometry` | the flag with any other original plane is refused at load |
| `n256/identification` | the hash covers all `256x64` pixels and a comparison mask is `256x64` |
| `n256/static_content` | the plane shows the frame authored for it, and only `FLAG_RETURNED_64P_FRAME_OK` / `width64` are reported |
| `n256/dynamic_content` | the plane takes the ROM shade of its own pixel and the extra plane's colour set; `lastFrame` holds the whole frame |
| `n256/background_over_black` | "the ROM lit nothing here" is judged on the plane's own ROM pixel |
| `n256/sprite_detection` | a sprite at an odd position in row 37 is found there, by value and by shape, where a `128x32` reading has no such row; the bounding box is in ROM-frame pixels |
| `n256/sprite_drawing` | a sprite is drawn 1:1 where it was found, from the extra arrays, dynamic pixel included |
| `n256/rotation` | the plane rotates with the extra table and nothing is reported for a `32p` plane |
| `n256/request_both` | `32P \| 64P`: the `256x64` plane, no `32p` buffer, flag or width; the frame's trigger is reported |
| `n256/request_32p_only` | `32P` alone: the colorization loads from cROM, generated cROMc and a direct cROMc load; frames are identified and a trigger is reported, but there is no buffer and no plane, for a known frame and an unknown one, and plane-less frames after the first read as already on display |
| `n256/request_fallback` | `32P \| FALLBACK` gets the `256x64` plane, also from a direct cROMc load and from a forced cROM load, where nothing widens the request; with `64P` the flag changes nothing |
| `n256/unknown_frame_fallback` | the grey fallback is the ROM frame pixel for pixel on the `256x64` plane only, for every request that has a plane |
| `n256/monochrome_palette` | the monochrome palette trigger takes the extra plane's colour set, and none when the frame has no extra content |
| `n256/without_extra_data` | a frame, a background or a sprite without extra data leaves its frame with no plane for that call, with or without `FALLBACK`: identified, trigger reported, `frame64` untouched, and the next complete frame renders |
| `n256/black_frame_is_judged_whole` | a frame that is black in its first 4096 bytes and lit below is not a black frame |
| `n256/scene_lookup` | scene markers are hashed as the marker followed by zeros, at load and at save, without reading past the generated marker |
| `n256/flag_off_is_unchanged` | the same file without the flag is an ordinary `128x32` project in layer mode, with its `32p` plane rendered from its own data |

Each was checked the way the section below asks: one mutation at a time on a
copy of the tree, under the sanitizer build — 26 of them, covering the native
frame accessors, the request-mode decisions (`NativePlaneRequested()`, the
`is256x64` branch of `ConfigureRequestedOutputMode()`, the `32p` allocation),
both places that keep or drop extra-plane data at load, the palette source and
the no-upscale rule for a frame without extra data. Three are only caught by
the sanitizer — a `frameshape` buffer or a generated scene marker sized
`128x32` is an out-of-bounds read, not a wrong pixel, and a sprite doubled into
the `256x64` plane writes past it before any pixel is compared. And taking the
geometry check out does not fail an assertion either: the loader then reads a
file shorter than the header claims and exits.

One thing is reasoned rather than tested: the frame dword table is sized from
the ROM frame so that it can never fill. Undersizing it would hang the probe
loop instead of failing a check, and it takes more distinct four-pixel windows
than a four-shade test frame can hold.

### Sprites in layer mode

A `128x32` project asked for its `64`-row plane, which libserum upscales. The
sprite is drawn into the `128x32` plane and its region is composited into the
extra plane from there; `AGENTS.md` ("Sprites in layer mode") has the rules.

| test | what it pins |
|---|---|
| `sprite/drawn_for_64p_only_request` | a `64P`-only request shows the sprite at twice its position and size, dynamic pixel included, with and without HD art, in both algorithms — the same plane a `32P \| 64P` request gets — and still reports only the `64`-row plane |
| `sprite/frame_shadow_inside_sprite_region` | a frame's dynamic shadow under a transparent sprite pixel survives the sprite's composite for a `64P`-only request as it does for `32P \| 64P` |
| `sprite/scaled_sprite_blocks_reuse` | a sprite without HD art ends the kept extra plane, so it does not stay behind when the next frame has the same `128x32` picture without it |
| `sprite/layer_mode_end_to_end` | the same through `Serum_Load()` and `Serum_Colorize()`, from a cROM and from its cROMc: a sprite found at one place, then another, then gone, then back, on one frame ID, for `64P` and `32P \| 64P` |

Each of the three rules was taken out by itself on a copy of the tree: asking
the advertised flag again fails the first, third and fourth test, replaying
shadows only for a `32p` caller fails the second, and leaving the plane
derived after a sprite fails the third and fourth.

## Not covered yet

In rough order of what is worth doing next:

- **`Identify_Frame()`** on a hand-built `g_serumData` — which colorization
  frame a ROM frame is. The `n256/` tests reach it through `Serum_Load()`
  instead, and an ordinary `128x32` project could be covered the same way.
  The in-memory attempt was
  backed out: the fixture needs more than the render does. Two things are
  already solved for whoever picks it up. `hashcodes` is index-based, so
  `set()` refuses it, but `readFromCRomReader()` takes any object with a
  `readExact()`, so an in-memory reader fills it the way the loader does. And
  `framechecked` is allocated by each of the three load paths rather than by
  `AllocateFrameWorkBuffers()`, because it is sized by the frame count rather
  than by the frame — a fixture has to allocate it itself. With both of those
  the search still answers `IDENTIFY_SAME_FRAME` for distinct frames and
  teardown crashes, so something else in the identification state is uninitialized.
- **Scene playback** and the finish/background flag semantics, several of which
  have their own bug history.
- **Sprite detection** (`Check_Spritesv2`) for the 32-row and 64-row layouts,
  beyond the one sprite `sprite/layer_mode_end_to_end` finds, and
  `Colorize_Spritev2()` outside layer mode: a 32-row project asked for `32P`
  only, and the 64-row layout. The `sprite/` tests cover layer mode and the
  `n256/` tests the `is256x64` layout.
- **A cROMc save/load round trip** that compares the model rather than the
  rendered output. `n256/` renders from both and expects the same pixels.
- **The wiring of `FinishedFrameIsAlreadyOnDisplay()` into
  `Serum_ColorizeWithMetadatav2Internal()`.** The decision itself is covered;
  reaching it from a colorize call needs a frame to be identified, so it is
  blocked on the same fixture as `Identify_Frame()` above. Until then the
  scene and trigger conditions are checked only as arguments to the predicate,
  not as the states that produce them.
- **Two of the `ExtraPlaneNoLongerDerived()` sites**: the monochrome fallback
  on a colorization *without* an authored extra plane, and scene-finish
  blanking. Both write the extra plane outside the upscale, so a later frame
  with a matching `frame32` must not keep it — the same rule the sprite case
  pins. The sprite site and the monochrome fallback into an authored extra
  plane are covered; these two are reasoned, not tested.

## What must not be covered

Anything behind `is_real_machine()`. It is false on every platform this suite
runs on, so those branches are unreachable here by construction — they are not
missing coverage, and a mutation of one of them would go uncaught for the same
reason. See the real-machine rule in `AGENTS.md`: do not test it, do not adjust
it to make it testable, and do not report it as dead.

## Keeping the suite honest

A test that cannot fail is worse than no test, so each one above was checked by
reverting the fix it guards and confirming it goes red. Nine of the ten
reversions are caught by an assertion; the tenth — decoding a shadow's own claim
marker as a dyna layer — reads out of bounds, and what it returns is whatever
follows the table, so it is caught by the sanitizer build instead.

Do the same for anything added here: break the code on purpose, watch the test
fail, then put it back.

Two traps are worth knowing before trusting a red-to-green result.

`make` compares timestamps to the second, so editing `src/serum-decode.cpp` and
rebuilding within the same second can leave the previous binary in place — and a
mutation that "passes" that way looks exactly like a mutation nothing catches.
Delete the object file rather than touching the source:

    rm -f <build dir>/CMakeFiles/serum_unit_tests.dir/tests/unit_tests.cpp.o

And a rule guarded twice cannot be mutation-tested one guard at a time: each
mutation on its own leaves the other guard standing, so both survive and look
untested. Either construct the case where only one of them is load-bearing, or
take the redundancy out.
