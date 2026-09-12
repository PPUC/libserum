# libserum tests

`unit_tests.cpp` is a single translation unit that `#include`s
`src/serum-decode.cpp` directly. That is deliberate: almost everything worth
pinning down here is a `static` function or a file-scope buffer, and widening
the public surface so a test could reach it would be the worse trade. It also
means there must stay exactly **one** such TU, or the implementation is compiled
twice and the link fails.

The tests build their frames in memory — a handful of pixels chosen to make one
rule observable. There are no dumps and no colorization files in the repository.
Frame buffers come from `AllocateFrameWorkBuffers()`, the same function both
load paths call, so a buffer added there is present here too.

## Running

    cmake --build <build dir> --target serum_unit_tests
    ctest --test-dir <build dir>

Or run the binary directly; it takes an optional substring to filter test names:

    ./build/serum_unit_tests shadow/

Configure with `-DENABLE_SANITIZERS=ON` to run the same tests under
AddressSanitizer and UndefinedBehaviorSanitizer. Do that at least once per
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

Not everything here is a past bug. The rest covers behaviour that had no test at
all and would fail quietly — a misparsed `scaling.txt` falls back to a default
nobody chose, a rotation that ticks at the wrong rate looks like a slow machine,
and a packing bug corrupts every colorization at once.

| test | what it pins |
|---|---|
| `separator/envelope_follows_the_font` | how tall a separator may be follows the height of the text it belongs to; the width bound is still fixed, and the test says so |
| `colorize/unchanged_picture_keeps_extra_plane` | an unchanged `128x32` picture keeps the `256x64` plane already derived from it, and a changed one derives again; the state does not outlive the colorization |
| `colorize/hd_sprite_art_blocks_reuse` | HD sprite artwork lands in the plane after the composite, so the plane can no longer be kept |
| `colorize/hd_static_frame_is_not_reused` | frames that composite HD statics each draw their own artwork, whichever frame the plane was last derived from |
| `colorize/static_content` | a frame's own colours come through untouched, whatever the ROM shade was |
| `colorize/background_mask` | the background image shows where its mask is set and the ROM lit nothing, and only there |
| `colorize/dynamic_zone_colours` | inside a dynamic zone the colour comes from that zone's set, indexed by the ROM shade |
| `colorize/dynamic_zone_active_mask` | a zone only applies where its active mask says so; elsewhere the static colour stands |
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

## Not covered yet

In rough order of what is worth doing next:

- **`Identify_Frame()`** — which colorization frame a ROM frame is. Attempted and
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
- **Sprite detection** (`Check_Spritesv2`) and `Colorize_Spritev2()`.
- **A cROMc save/load round trip**, which needs no fixture file at all: build
  `g_serumData`, save, reload, compare.
- **Two of the `ExtraPlaneNoLongerDerived()` sites**: the monochrome fallback
  and scene-finish blanking. Both write the extra plane outside the upscale, so
  a later frame with a matching `frame32` must not keep it — the same rule the
  sprite case pins, on paths that need the fixtures above. The sprite site is
  covered; these two are reasoned, not tested.

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
