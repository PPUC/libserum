// Unit tests for libserum's v2 render path.
//
// See README.md in this directory: this is the one TU that includes the
// implementation, so the static internals are reachable.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "serum-decode.cpp"

// ---------------------------------------------------------------------------
// A very small harness. No dependency, no discovery magic: a test is a function
// in the table at the bottom of the file.
// ---------------------------------------------------------------------------

static int g_checks = 0;
static int g_failures = 0;
static const char* g_currentTest = "";

static void Fail(const char* file, int line, const char* fmt, ...) {
  ++g_failures;
  fprintf(stderr, "  FAIL %s\n    %s:%d\n    ", g_currentTest, file, line);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fputc('\n', stderr);
}

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) Fail(__FILE__, __LINE__, "expected: %s", #cond); \
  } while (0)

#define CHECK_EQ(actual, expected)                                       \
  do {                                                                   \
    ++g_checks;                                                          \
    const long long a_ = (long long)(actual);                            \
    const long long e_ = (long long)(expected);                          \
    if (a_ != e_)                                                        \
      Fail(__FILE__, __LINE__, "%s == %s: got %lld, want %lld", #actual, \
           #expected, a_, e_);                                           \
  } while (0)

// ---------------------------------------------------------------------------
// Synthetic frame setup.
//
// AllocateFrameWorkBuffers() is the same function both load paths call, so a
// buffer added there is automatically present here too.
// ---------------------------------------------------------------------------

static const uint32_t kW = 32;  // narrow frames keep the expectations readable
static const uint32_t kH = 16;

static std::vector<uint8_t> g_rom;

static void SetUpFrame(uint32_t w = kW, uint32_t h = kH) {
  Serum_free();
  g_serumData.SerumVersion = SERUM_V2;
  g_serumData.fwidth = w;
  g_serumData.fheight = h;
  g_serumData.fwidth_extra = w * 2;
  g_serumData.fheight_extra = h * 2;
  g_serumData.nframes = 4;
  g_serumData.nocolors = 16;
  CHECK(AllocateFrameWorkBuffers());

  mySerum.width32 = w;
  mySerum.width64 = w * 2;
  allocatedPlaneWidth64 = w * 2;
  mySerum.frame32 = (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
  mySerum.frame64 = (uint16_t*)calloc((size_t)w * h * 4, sizeof(uint16_t));
  mySerum.rotationsinframe32 =
      (uint16_t*)calloc((size_t)w * h * 2, sizeof(uint16_t));
  mySerum.rotationsinframe64 =
      (uint16_t*)calloc((size_t)w * h * 8, sizeof(uint16_t));

  g_rom.assign((size_t)w * h, 0);
  romFrameForUpscale = g_rom.data();
  runtimeScalingAlgorithm = SERUM_SCALING_SCALE2X_PRESERVE;
  shadowOffsetModeRuntime = SERUM_SHADOW_OFFSET_NATIVE;
  upscaleExtraFromOriginal = true;
  originalPlaneRequestedByCaller = true;
  memset(hdDynaLayerMap, 0, (size_t)w * h * 4);
  memset(sdDynaLayerMap, 0, (size_t)w * h);
  memset(scaledLayerCoverage, 1, (size_t)w * h);  // the layer owns everything
  scaledLayerHasCoverage = true;
  separatorMaskValid = false;
  extraPlaneIsDerived = false;
  mySerum.flags = 0;
}

static void TearDownFrame(void) {
  romFrameForUpscale = NULL;
  Serum_free();
}

// Paint one source pixel: ROM shade and output colour together, since every
// rule in the upscale reads both.
static void Px(uint32_t x, uint32_t y, uint8_t shade, uint16_t colour) {
  g_rom[(size_t)y * g_serumData.fwidth + x] = shade;
  mySerum.frame32[(size_t)y * g_serumData.fwidth + x] = colour;
}

static uint16_t Hd(uint32_t x, uint32_t y) {
  return mySerum.frame64[(size_t)y * (g_serumData.fwidth * 2) + x];
}

// How many of a source pixel's four destination pixels kept its own colour.
static int QuadrantsKept(uint32_t sx, uint32_t sy) {
  const uint16_t want = mySerum.frame32[(size_t)sy * g_serumData.fwidth + sx];
  int n = 0;
  for (uint32_t dy = 0; dy < 2; ++dy)
    for (uint32_t dx = 0; dx < 2; ++dx)
      if (Hd(sx * 2 + dx, sy * 2 + dy) == want) ++n;
  return n;
}

// ---------------------------------------------------------------------------
// Scale2xPreserve: which corners may be rounded away
// ---------------------------------------------------------------------------

// a424478. This is spagb_100's "SUPER JACKPOT" line: five-pixel text whose top
// arm sits one row under a solid band of artwork. For the quadrant Scale2x
// wants to chip, the three source pixels behind it are the rest of the arm and
// two pixels of that band -- all lit, so a corner test that asks only whether
// they are lit calls this a solid corner and lets the arm erode. It is lit, but
// it is not the glyph: the shades differ, which is what the test compares.
static void Test_ThinArmUnderLitBandIsKept(void) {
  SetUpFrame();
  const uint16_t kArt = 0x0400, kText = 0x7fff;
  const uint32_t band = 5, arm = 6, ax = 6;
  for (uint32_t x = 0; x < kW; ++x) Px(x, band, 3, kArt);
  // A one-pixel-tall arm of four pixels, nothing above it but the band and
  // nothing below it: every pixel of it is a corner.
  for (uint32_t i = 0; i < 4; ++i) Px(ax + i, arm, 15, kText);
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);

  for (uint32_t i = 0; i < 4; ++i) {
    ++g_checks;
    if (QuadrantsKept(ax + i, arm) != 4)
      Fail(__FILE__, __LINE__, "arm pixel %u kept %d of 4 quadrants", i,
           QuadrantsKept(ax + i, arm));
  }
  TearDownFrame();
}

// The band itself is ordinary artwork and keeps being scaled as artwork: the
// rule above must not turn into blanket protection for everything.
static void Test_LitBandStillScalesAsArtwork(void) {
  SetUpFrame();
  const uint16_t kArt = 0x0400;
  for (uint32_t x = 4; x < 12; ++x)
    for (uint32_t y = 4; y < 8; ++y) Px(x, y, 3, kArt);
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  CHECK_EQ(QuadrantsKept(4, 4), 3);  // the block's corner rounds
  CHECK_EQ(QuadrantsKept(11, 7), 3);
  CHECK_EQ(QuadrantsKept(7, 5), 4);  // its interior does not
  TearDownFrame();
}

// 84330ef. A glyph two pixels thick has its outer corner rounded, the way
// reference Scale2x would and the way the font's own chamfer already looks.
static void Test_SolidCornerIsRounded(void) {
  SetUpFrame();
  const uint16_t kGlyph = 0xffff;
  for (uint32_t y = 4; y < 12; ++y)
    for (uint32_t x = 4; x < 12; ++x) Px(x, y, 15, kGlyph);
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);

  // The block's four outer corners each lose exactly the quadrant pointing
  // away from it, and nothing else.
  CHECK_EQ(QuadrantsKept(4, 4), 3);
  CHECK_EQ(QuadrantsKept(11, 4), 3);
  CHECK_EQ(QuadrantsKept(4, 11), 3);
  CHECK_EQ(QuadrantsKept(11, 11), 3);
  CHECK_EQ(Hd(8, 8), 0);             // top-left corner's outer quadrant
  CHECK_EQ(QuadrantsKept(7, 7), 4);  // an interior pixel is untouched
  TearDownFrame();
}

// The rule an earlier attempt got wrong: it asked only whether the pixel
// diagonally behind the corner was part of the glyph, which is true of any
// curve, and small letters were chipped anyway. Both orthogonal neighbours
// behind have to agree as well.
//
// A pure diagonal cannot show this -- Scale2x returns the centre unchanged the
// moment the vertical or horizontal neighbours match, so it never touches one.
// What does is a corner whose diagonal continues the glyph while the pixel
// beside it belongs to something else.
static void Test_CornerNeedsBothOrthogonalsBehind(void) {
  SetUpFrame();
  const uint16_t kArt = 0x0400, kText = 0x7fff;
  const uint32_t cx = 8, cy = 8;
  Px(cx, cy, 15, kText);          // the corner under test
  Px(cx, cy + 1, 15, kText);      // the glyph continues downwards
  Px(cx + 1, cy + 1, 15, kText);  // ...and diagonally behind the corner
  Px(cx + 1, cy, 3, kArt);        // but beside it is artwork, not glyph
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  CHECK_EQ(QuadrantsKept(cx, cy), 4);
  TearDownFrame();
}

// cabe025. The preserving decision belongs to libserum, which knows the ROM
// frame. If the selection is asked for with Scale2xPreserve, libframeutil
// answers "keep the centre" first and the corner test never runs.
static void Test_PreserveStillRoundsOnBlackBackground(void) {
  SetUpFrame();
  const uint16_t kGlyph = 0xffff;
  for (uint32_t y = 4; y < 12; ++y)
    for (uint32_t x = 4; x < 12; ++x) Px(x, y, 15, kGlyph);  // on colour 0
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  CHECK_EQ(Hd(8, 8), 0);
  CHECK_EQ(QuadrantsKept(4, 4), 3);
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// The composite writes every destination pixel
// ---------------------------------------------------------------------------

// 5b5b0f7. Where no HD content stands behind it, the composite is the only
// writer, so a pixel it skips keeps the previous frame. The frame border is
// where this bit: a selection can land outside, which stands for black.
static void Test_EveryDestinationPixelIsWritten(void) {
  SetUpFrame();
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x)
      Px(x, y, (uint8_t)((x + y) % 4), (uint16_t)(0x1000 + ((x * 7 + y) % 97)));
  const size_t px = (size_t)kW * kH * 4;
  for (size_t i = 0; i < px; ++i) mySerum.frame64[i] = 0xDEAD;
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  size_t unwritten = 0;
  for (size_t i = 0; i < px; ++i)
    if (mySerum.frame64[i] == 0xDEAD) ++unwritten;
  CHECK_EQ(unwritten, 0);
  TearDownFrame();
}

// Line doubling has to be exactly that: each source pixel becomes its 2x2
// block, whatever else the render path is doing.
static void Test_LineDoublingReplicates(void) {
  SetUpFrame();
  runtimeScalingAlgorithm = SERUM_SCALING_LINE_DOUBLING;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x)
      Px(x, y, (uint8_t)((x * 3 + y) % 16), (uint16_t)(0x2000 + x * 31 + y));
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) CHECK_EQ(QuadrantsKept(x, y), 4);
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// Dynamic shadows on the extra plane
// ---------------------------------------------------------------------------

// Give one frame a shadow direction and colour on one layer, and lay a solid
// block of that layer's lit content into the extra-plane map.
static const uint32_t kShadowFrame = 1;
static const uint16_t kShadowColour = 0x1234;

static void ArmShadow(int layer, uint8_t directions) {
  std::vector<uint8_t> dir(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  std::vector<uint16_t> col(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  dir[layer] = directions;
  col[layer] = kShadowColour;
  g_serumData.dynashadowsdir.set(kShadowFrame, dir.data(), dir.size());
  g_serumData.dynashadowscol.set(kShadowFrame, col.data(), col.size());
}

static void PlaceLitBlock(int layer, uint32_t x0, uint32_t y0, uint32_t side) {
  const uint32_t w = g_serumData.fwidth * 2;
  memset(hdDynaLayerMap, 0, (size_t)w * g_serumData.fheight * 2);
  for (uint32_t y = y0; y < y0 + side; ++y)
    for (uint32_t x = x0; x < x0 + side; ++x)
      hdDynaLayerMap[(size_t)y * w + x] = (uint8_t)(layer + 1);
}

static unsigned CountShadowPixels(void) {
  unsigned n = 0;
  const size_t px = (size_t)g_serumData.fwidth * g_serumData.fheight * 4;
  for (size_t i = 0; i < px; ++i)
    if (mySerum.frame64[i] == kShadowColour) ++n;
  return n;
}

// fb5c3a1. The tables are read with MAX_DYNA_SETS_PER_FRAME_V2 entries.
// Bounding the layer index with MAX_DYNA_4COLS_PER_FRAME -- the old v1 limit,
// half the size -- silently dropped every colour set from the seventeenth on,
// so those captions were shadowed at original resolution and bare on the extra
// plane.
static void Test_EveryDynaLayerCastsItsShadow(void) {
  for (int layer = 0; layer < MAX_DYNA_SETS_PER_FRAME_V2; ++layer) {
    SetUpFrame();
    ArmShadow(layer, 1u << 5);  // straight down
    PlaceLitBlock(layer, 8, 8, 4);
    GenerateExtraPlaneShadows(kShadowFrame);
    if (CountShadowPixels() != 4)
      Fail(__FILE__, __LINE__, "layer %d cast %u shadow pixels, want 4", layer,
           CountShadowPixels());
    ++g_checks;
    TearDownFrame();
  }
}

// e9f8c07. The pass marks a pixel it has painted so a later shadow cannot
// overwrite it. That marker is not a layer, and decoding it as one indexed far
// past the end of a table of directions and colours -- so shadows cast further
// shadows, in colours no author chose, and not even the same ones twice.
static void Test_ShadowClaimMarkerIsNotALayer(void) {
  SetUpFrame();
  ArmShadow(0, 1u << 5);
  PlaceLitBlock(0, 8, 8, 4);
  GenerateExtraPlaneShadows(kShadowFrame);
  const unsigned first = CountShadowPixels();
  CHECK_EQ(first, 4);

  // Exactly the row below the block, and nothing beyond it: a shadow that was
  // decoded as a layer would cast one of its own, one row further down.
  const uint32_t w = g_serumData.fwidth * 2;
  for (uint32_t x = 8; x < 12; ++x) {
    CHECK_EQ(Hd(x, 12), kShadowColour);
    CHECK(Hd(x, 13) != kShadowColour);
    CHECK_EQ(hdDynaLayerMap[(size_t)13 * w + x], 0);
  }
  TearDownFrame();
}

// The offset is a deliberate per-colorization choice, and it has to reach the
// pass: native is one extra-plane pixel, proportional is two.
static void Test_ShadowOffsetModes(void) {
  SetUpFrame();
  ArmShadow(0, 1u << 5);
  PlaceLitBlock(0, 8, 8, 4);
  shadowOffsetModeRuntime = SERUM_SHADOW_OFFSET_NATIVE;
  GenerateExtraPlaneShadows(kShadowFrame);
  CHECK_EQ(CountShadowPixels(), 4);

  SetUpFrame();
  ArmShadow(0, 1u << 5);
  PlaceLitBlock(0, 8, 8, 4);
  shadowOffsetModeRuntime = SERUM_SHADOW_OFFSET_PROPORTIONAL;
  GenerateExtraPlaneShadows(kShadowFrame);
  CHECK_EQ(CountShadowPixels(), 8);
  TearDownFrame();
}

// A shadow belongs behind the glyph, never over it, and the first shadow to
// claim a pixel keeps it -- both as the original-resolution pass behaves.
static void Test_ShadowNeverCoversLitContent(void) {
  SetUpFrame();
  ArmShadow(0, 0xff);  // all eight directions
  PlaceLitBlock(0, 8, 8, 4);
  for (uint32_t y = 8; y < 12; ++y)
    for (uint32_t x = 8; x < 12; ++x)
      mySerum.frame64[(size_t)y * (kW * 2) + x] = 0xBEEF;
  GenerateExtraPlaneShadows(kShadowFrame);
  for (uint32_t y = 8; y < 12; ++y)
    for (uint32_t x = 8; x < 12; ++x) CHECK_EQ(Hd(x, y), 0xBEEF);
  // A ring one pixel wide around a 4x4 block.
  CHECK_EQ(CountShadowPixels(), 6 * 6 - 4 * 4);
  TearDownFrame();
}

// 8dd46b4. In layer mode the original-resolution shadow is recorded, not
// painted: the picture the upscale reads has to be shadow-free, or the pixel
// under the shadow is never rendered and nothing ever writes it.
static void Test_DeferredShadowLeavesThePlaneAlone(void) {
  SetUpFrame();
  std::vector<uint8_t> dir(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  std::vector<uint16_t> col(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  dir[0] = 1u << 5;
  col[0] = kShadowColour;
  std::vector<uint8_t> isdynapix((size_t)kW * kH, 0);
  mySerum.frame32[(size_t)6 * kW + 5] = 0xABCD;  // what the shadow will cover

  CheckDynaShadow(mySerum.frame32, dir.data(), col.data(), 0, isdynapix.data(),
                  5, 5, kW, kH, nullptr, nullptr, /*defer=*/true);

  const size_t below = (size_t)6 * kW + 5;
  CHECK_EQ(mySerum.frame32[below], 0xABCD);  // untouched, so it can be scaled
  CHECK_EQ(isdynapix[below], 0);             // and still rendered as background
  CHECK_EQ(sdShadowClaim[below], 1);
  CHECK_EQ(sdShadowColour[below], kShadowColour);

  // Replayed afterwards, for the 32p output only.
  sdDynaLayerMap[below] = 0;
  ReplayOriginalPlaneShadows();
  CHECK_EQ(mySerum.frame32[below], kShadowColour);
  CHECK_EQ(mySerum.rotationsinframe32[below * 2], 0xffff);
  TearDownFrame();
}

// The replay must not paint over content that turned out to be lit.
static void Test_ReplayedShadowYieldsToLitContent(void) {
  SetUpFrame();
  const size_t at = (size_t)6 * kW + 5;
  sdShadowClaim[at] = 1;
  sdShadowColour[at] = kShadowColour;
  sdShadowClaimAny = true;
  sdDynaLayerMap[at] = 3;  // lit dynamic content, layer 2
  mySerum.frame32[at] = 0xABCD;
  ReplayOriginalPlaneShadows();
  CHECK_EQ(mySerum.frame32[at], 0xABCD);
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// The per-pixel rotation plane
// ---------------------------------------------------------------------------

// effef51. A pixel that is in no rotation gets 0xffff in the slot AND in the
// offset. The composite copies the offset through with the colour whether the
// slot names a rotation or not, so leaving it behind reads uninitialised memory
// and makes the library's own output differ between runs.
static void Test_NoRotationWritesBothHalves(void) {
  SetUpFrame();
  uint16_t slot = 0x5a5a, offset = 0x5a5a;
  const bool found = ColorInRotation(0, 0x1234, &slot, &offset, false);
  CHECK(!found);
  CHECK_EQ(slot, 0xffff);
  CHECK_EQ(offset, 0xffff);
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// Frame identity hashing
// ---------------------------------------------------------------------------

// Fibonacci hashing puts the entropy in the HIGH bits. Taking the low ones sent
// nearly every frame to the same slot -- correct, but 22 us a frame instead of
// 4.8 on a real colorization.
static void Test_FrameDwordSlotUsesHighBits(void) {
  SetUpFrame();
  AllocateFrameDwordTable(kW, kH);
  // Values that differ only high up. Fibonacci hashing carries its entropy
  // upwards, so taking the low bits of the product sends every one of these to
  // the same slot -- still correct, and 22 us a frame instead of 4.8.
  std::vector<int> used(frameDwordMask + 1, 0);
  for (uint32_t i = 1; i <= 64; ++i) used[FrameDwordSlot(i << 24)] = 1;
  int distinct = 0;
  for (size_t i = 0; i < used.size(); ++i) distinct += used[i];
  CHECK(distinct > 32);
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// Thousands separators
// ---------------------------------------------------------------------------

// A comma is one narrow column of its own, and Scale2x will happily bridge it
// to the digit beside it. It is taken out of the picture before scaling and
// stamped back line-doubled, so it keeps exactly the shape the ROM drew.
static void Test_SeparatorIsNotFusedWithTheDigit(void) {
  SetUpFrame(48, 24);
  const uint16_t kDigit = 0xffe0;
  // Three groups of three digit columns, separated by a one-pixel comma that
  // sits below the text baseline.
  const uint32_t top = 6, bottom = 15;
  uint32_t x = 2;
  for (int group = 0; group < 3; ++group) {
    for (int d = 0; d < 3; ++d, x += 3) {
      for (uint32_t y = top; y <= bottom; ++y) {
        Px(x, y, 15, kDigit);
        Px(x + 1, y, 15, kDigit);
      }
    }
    if (group < 2) {
      Px(x, bottom, 15, kDigit);      // the comma: one column, two pixels,
      Px(x, bottom + 1, 15, kDigit);  // hanging below the baseline
      x += 2;
    }
  }
  for (size_t i = 0; i < (size_t)48 * 24; ++i)
    sdDynaLayerMap[i] = mySerum.frame32[i] ? 1 : 0;
  separatorMaskValid = false;

  const uint32_t found = DetectSeparators(48, 24);
  CHECK_EQ(found, 2);
  CHECK_EQ(separatorMask[(size_t)bottom * 48 + 11], 1);
  CHECK_EQ(separatorMask[(size_t)bottom * 48 + 22], 1);
  // A digit column is never a separator.
  CHECK_EQ(separatorMask[(size_t)bottom * 48 + 2], 0);
  CHECK_EQ(separatorMask[(size_t)top * 48 + 2], 0);
  TearDownFrame();
}

// ---------------------------------------------------------------------------

struct TestCase {
  const char* name;
  void (*fn)(void);
};

static const TestCase kTests[] = {
    {"preserve/thin_arm_under_lit_band", Test_ThinArmUnderLitBandIsKept},
    {"preserve/lit_band_scales_as_artwork", Test_LitBandStillScalesAsArtwork},
    {"preserve/solid_corner_rounds", Test_SolidCornerIsRounded},
    {"preserve/corner_needs_both_orthogonals",
     Test_CornerNeedsBothOrthogonalsBehind},
    {"preserve/rounds_on_black_background",
     Test_PreserveStillRoundsOnBlackBackground},
    {"upscale/every_pixel_written", Test_EveryDestinationPixelIsWritten},
    {"upscale/line_doubling_replicates", Test_LineDoublingReplicates},
    {"shadow/every_dyna_layer", Test_EveryDynaLayerCastsItsShadow},
    {"shadow/claim_marker_is_not_a_layer", Test_ShadowClaimMarkerIsNotALayer},
    {"shadow/offset_modes", Test_ShadowOffsetModes},
    {"shadow/never_covers_lit_content", Test_ShadowNeverCoversLitContent},
    {"shadow/deferred_leaves_plane_alone",
     Test_DeferredShadowLeavesThePlaneAlone},
    {"shadow/replay_yields_to_lit_content",
     Test_ReplayedShadowYieldsToLitContent},
    {"rotation/no_rotation_writes_both_halves",
     Test_NoRotationWritesBothHalves},
    {"hash/frame_dword_slot_uses_high_bits", Test_FrameDwordSlotUsesHighBits},
    {"separator/not_fused_with_digit", Test_SeparatorIsNotFusedWithTheDigit},
};

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : NULL;
  int ran = 0;
  for (const TestCase& t : kTests) {
    if (filter && !strstr(t.name, filter)) continue;
    g_currentTest = t.name;
    const int before = g_failures;
    t.fn();
    ++ran;
    printf("%-6s %s\n", g_failures == before ? "ok" : "FAILED", t.name);
  }
  printf("\n%d tests, %d checks, %d failures\n", ran, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
