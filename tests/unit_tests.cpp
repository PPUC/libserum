// Unit tests for libserum's v2 render path.
//
// See README.md in this directory: this is the one TU that includes the
// implementation, so the static internals are reachable.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
  runtimeScalingAlgorithm = SERUM_SCALING_SCALE2X;
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

// Build a score line and report whether the filter recognizes its separators:
// `tall` rows of digits in `shades` gradient bands, commas `cw` by `ch`
// hanging below the baseline in the colour that band gives them.
static uint32_t SeparatorsFoundFor(uint32_t tall, int shades, uint32_t cw,
                                   uint32_t ch) {
  const uint32_t W = g_serumData.fwidth, H = g_serumData.fheight;
  memset(mySerum.frame32, 0, (size_t)W * H * sizeof(uint16_t));
  memset(sdDynaLayerMap, 0, (size_t)W * H);
  std::fill(g_rom.begin(), g_rom.end(), 0);
  const uint32_t top = 4, bottom = top + tall - 1;
  uint32_t x = 4;
  int digits = 0;
  while (x + 6 < W - 4) {
    for (uint32_t dy = 0; dy < tall; ++dy) {
      const int band = shades > 1 ? (int)(dy * shades / tall) : 0;
      const uint16_t colour = (uint16_t)(0xffe0 - band * 0x0420);
      for (uint32_t dx = 0; dx < 5; ++dx) {
        if (dx > 0 && dx < 4 && dy > 0 && dy < tall - 1) continue;
        mySerum.frame32[(top + dy) * W + x + dx] = colour;
        g_rom[(top + dy) * W + x + dx] = 15;  // one shade, gradient in colour
        sdDynaLayerMap[(top + dy) * W + x + dx] = 1;
      }
    }
    x += 6;
    if (++digits % 3 == 0 && x + cw + 6 < W - 4) {
      const uint16_t colour =
          (uint16_t)(0xffe0 - (shades > 1 ? shades - 1 : 0) * 0x0420);
      for (uint32_t cy = 0; cy < ch; ++cy)
        for (uint32_t dx = 0; dx < cw; ++dx) {
          const uint32_t yy = bottom - (ch - 1) + cy + 1;
          if (yy >= H) continue;
          mySerum.frame32[yy * W + x + dx] = colour;
          g_rom[yy * W + x + dx] = 15;
          sdDynaLayerMap[yy * W + x + dx] = 1;
        }
      x += cw + 1;
    }
  }
  separatorMaskValid = false;
  return DetectSeparators(W, H);
}

// What the filter will and will not accept as a thousands separator.
//
// Detection runs on the ROM's shades, so a score font drawn as a colour
// gradient is still one glyph here and the allowance follows its height: three
// rows of descender on a six-row font, four on an eight-row one, five on a
// twelve-row one. Judged on the colorized plane instead, each colour of that
// gradient is a horizontal band a few rows tall, the allowance is computed from
// the band, and every font gets the same three rows however large it is.
//
// The WIDTH is still a fixed two columns and does not follow the font. No
// colorization to hand draws a wider separator, so widening it would be a
// change made blind; this states the bound so that changing it is deliberate.
static void Test_SeparatorEnvelopeFollowsTheFont(void) {
  SetUpFrame(128, 32);
  struct Case {
    uint32_t tall, cw, ch;
    bool want;
  };
  const Case cases[] = {
      // Always recognized, at every size.
      {6, 1, 2, true},
      {16, 1, 2, true},
      {6, 2, 3, true},
      {16, 2, 3, true},
      // A descender of four rows needs a font tall enough to justify it.
      {6, 2, 4, false},
      {8, 2, 4, true},
      {16, 2, 4, true},
      // Five needs taller still.
      {8, 2, 5, false},
      {12, 2, 5, true},
      // Three columns is beyond the fixed width bound at every size.
      {6, 3, 3, false},
      {16, 3, 3, false},
  };
  for (const Case& c : cases) {
    const bool got = SeparatorsFoundFor(c.tall, 3, c.cw, c.ch) != 0;
    ++g_checks;
    if (got != c.want)
      Fail(__FILE__, __LINE__, "%u-row font, %ux%u separator: %s, expected %s",
           c.tall, c.cw, c.ch, got ? "recognized" : "not recognized",
           c.want ? "recognized" : "not recognized");
  }
  TearDownFrame();
}

// The same separator drawn twice on one line is treated the same way, whatever
// digits happen to sit beside it.
//
// spagb_100 frame 38 shows "36,269,900" in a gradient font, and its two commas
// are the same three pixels. Judged on the colorized plane they fell into
// different column groups of one colour -- nine glyph bottoms under the first,
// four under the second -- and only the first was recognized. Which of them
// survived came down to the value on the display.
static void Test_IdenticalSeparatorsAreTreatedAlike(void) {
  SetUpFrame(128, 32);
  // Sixteen digits fit across the frame, so five separators -- and every one
  // of them has to be found, not whichever ones the digits beside them happen
  // to favour.
  CHECK_EQ(SeparatorsFoundFor(8, 3, 1, 2), 5);
  // The same line without the gradient, which always worked, to show the two
  // now agree.
  CHECK_EQ(SeparatorsFoundFor(8, 1, 1, 2), 5);
  TearDownFrame();
}

// Two lines of scores in one dynamic zone, with a border down the side.
//
// The column heights are measured as the longest UNBROKEN run of rows, not as
// how many rows the colour covers in that column. A second line puts a second
// run in the same columns, so a total would be two glyphs tall and the
// allowance built on it would grow with every line added -- at three lines it
// would exceed the display and nothing could ever be cut out. The longest run
// is one glyph however many lines there are.
static void Test_TwoLinesOfScoresKeepTheirSeparators(void) {
  SetUpFrame(64, 32);
  const uint16_t kText = 0xffe0;
  const auto lit = [&](uint32_t x, uint32_t y) {
    Px(x, y, 15, kText);
    sdDynaLayerMap[(size_t)y * 64 + x] = 1;
  };
  // Two lines of four digits with a comma, one above the other, a blank row
  // between them, in the same zone.
  for (int line = 0; line < 2; ++line) {
    const uint32_t top = 4 + (uint32_t)line * 12, bottom = top + 7;
    uint32_t x = 4;
    for (int d = 0; d < 4; ++d, x += 3)
      for (uint32_t y = top; y <= bottom; ++y) {
        lit(x, y);
        lit(x + 1, y);
      }
    lit(x + 1, bottom - 1);
    lit(x + 1, bottom);
    lit(x, bottom + 1);
    x += 3;
    for (int d = 0; d < 4; ++d, x += 3)
      for (uint32_t y = top; y <= bottom; ++y) {
        lit(x, y);
        lit(x + 1, y);
      }
  }
  separatorMaskValid = false;
  const uint32_t both = DetectSeparators(64, 32);
  CHECK_EQ(both, 2);  // one separator on each line

  // A border down the side in the same shade and the same zone, placed just
  // inside the spacing that still reads as one run -- otherwise it would not
  // join the text and the test would prove nothing.
  uint32_t lastLit = 0;
  for (uint32_t x = 0; x < 64; ++x)
    for (uint32_t y = 0; y < 32; ++y)
      if (sdDynaLayerMap[(size_t)y * 64 + x]) lastLit = x;
  const uint32_t borderX = lastLit + 2;
  CHECK(borderX < 64);
  for (uint32_t y = 0; y < 32; ++y) lit(borderX, y);
  separatorMaskValid = false;
  CHECK_EQ(DetectSeparators(64, 32), both);
  TearDownFrame();
}

// The left-hand zone of a Stern SAM ROM: up to four player scores stacked in
// one dynamic zone, the active player's drawn larger than the rest, the value
// of the current shot under it, and the divider down the side of the zone.
//
// Everything here shares one column group, so every one of these has to keep
// its separator: several lines, more than one type size among them, and a
// full-height rule alongside.
static void Test_SternStyleScoreColumnKeepsEverySeparator(void) {
  SetUpFrame(64, 32);
  const uint16_t kText = 0xffe0;
  const auto lit = [&](uint32_t x, uint32_t y) {
    Px(x, y, 15, kText);
    sdDynaLayerMap[(size_t)y * 64 + x] = 1;
  };
  // One line of "N,NNN": four digits with a separator after the first.
  const auto line = [&](uint32_t top, uint32_t height, uint32_t stroke) {
    const uint32_t bottom = top + height - 1;
    uint32_t x = 3;
    for (uint32_t dx = 0; dx < stroke; ++dx)
      for (uint32_t y = top; y <= bottom; ++y) lit(x + dx, y);
    x += stroke + 1;
    lit(x, bottom - 1);
    lit(x, bottom);
    lit(x - 1, bottom + 1);  // the separator's tail
    x += 2;
    for (int d = 0; d < 3; ++d, x += stroke + 1)
      for (uint32_t dx = 0; dx < stroke; ++dx)
        for (uint32_t y = top; y <= bottom; ++y) lit(x + dx, y);
  };
  // Spaced as a display spaces them: a separator hangs one row below its line,
  // and the next line starts below that.
  line(1, 5, 2);   // player 1
  line(8, 5, 2);   // player 2
  line(15, 7, 3);  // the active player, drawn larger
  line(25, 5, 2);  // the value of the current shot

  separatorMaskValid = false;
  const uint32_t found = DetectSeparators(64, 32);
  CHECK_EQ(found, 4);  // one per line, the larger one included

  // ...and the divider down the side changes none of it.
  uint32_t lastLit = 0;
  for (uint32_t x = 0; x < 64; ++x)
    for (uint32_t y = 0; y < 32; ++y)
      if (sdDynaLayerMap[(size_t)y * 64 + x]) lastLit = x;
  const uint32_t dividerX = lastLit + 2;
  CHECK(dividerX < 64);
  for (uint32_t y = 0; y < 32; ++y) lit(dividerX, y);
  separatorMaskValid = false;
  CHECK_EQ(DetectSeparators(64, 32), found);
  TearDownFrame();
}

// A separator is scaled as a shape of its own, not stamped flat.
//
// A comma is a body with its tail one row down and one column across, and the
// diagonal between those is exactly what the scaler is for. Line doubling it --
// which is how it was kept from bridging to the digit beside it -- left the two
// halves meeting at a corner while every glyph around them was smoothed.
// Scaling a picture holding nothing but the separators keeps both properties.
// Lays out a score line: four digits, a comma, four more. `gap` is how many
// empty columns separate the comma from the digit before it.
static uint32_t g_commaBodyX = 0, g_commaTailX = 0, g_commaBottom = 0;
static void BuildScoreWithComma(uint32_t gap) {
  const uint16_t kText = 0xffe0;
  const uint32_t top = 8, bottom = 17;
  const auto lit = [&](uint32_t x, uint32_t y) {
    Px(x, y, 15, kText);
    sdDynaLayerMap[(size_t)y * g_serumData.fwidth + x] = 1;
  };
  uint32_t x = 2;
  for (int d = 0; d < 4; ++d, x += 3)
    for (uint32_t y = top; y <= bottom; ++y) {
      lit(x, y);
      lit(x + 1, y);
    }
  // The comma: a body of two pixels on the bottom line, its tail one row below
  // and one column across.
  const uint32_t tailX = x - 1 + gap, bodyX = tailX + 1;
  lit(bodyX, bottom - 1);
  lit(bodyX, bottom);
  lit(tailX, bottom + 1);
  x = bodyX + 2;
  for (int d = 0; d < 4; ++d, x += 3)
    for (uint32_t y = top; y <= bottom; ++y) {
      lit(x, y);
      lit(x + 1, y);
    }
  g_commaBodyX = bodyX;
  g_commaTailX = tailX;
  g_commaBottom = bottom;
  separatorMaskValid = false;
}

static void Test_SeparatorIsScaledAsItsOwnShape(void) {
  SetUpFrame(64, 32);
  BuildScoreWithComma(1);
  const uint32_t W = 64, bottom = g_commaBottom;
  CHECK(DetectSeparators(64, 32) > 0);
  // The whole comma, both halves, or there is nothing to round.
  CHECK_EQ(separatorMask[(size_t)(bottom - 1) * W + g_commaBodyX], 1);
  CHECK_EQ(separatorMask[(size_t)bottom * W + g_commaBodyX], 1);
  CHECK_EQ(separatorMask[(size_t)(bottom + 1) * W + g_commaTailX], 1);

  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);

  // These columns hold the comma and nothing else, so everything counted here
  // is the separator. Line doubling its three source pixels gives twelve
  // extra-plane pixels; the corner between body and tail is filled from both
  // sides, so there are fourteen.
  unsigned painted = 0;
  for (uint32_t y = (bottom - 1) * 2; y <= (bottom + 1) * 2 + 1; ++y)
    for (uint32_t x2 = g_commaTailX * 2; x2 <= g_commaBodyX * 2 + 1; ++x2)
      if (mySerum.frame64[(size_t)y * 128 + x2] == 0xffe0) ++painted;
  CHECK_EQ(painted, 14);
  TearDownFrame();
}

// A separator touching the digit beside it is exactly what the filter exists
// for, and there the shape cannot be grown: following the connection would
// swallow the digit and line double it. Only the descending columns are taken,
// as before, so the comma keeps its square tail and the digit is untouched.
static void Test_SeparatorTouchingADigitIsNotGrown(void) {
  SetUpFrame(64, 32);
  BuildScoreWithComma(0);  // the tail now sits diagonally against the digit
  const uint32_t W = 64, bottom = g_commaBottom;
  CHECK(DetectSeparators(64, 32) > 0);
  CHECK_EQ(separatorMask[(size_t)(bottom + 1) * W + g_commaTailX], 1);
  CHECK_EQ(separatorMask[(size_t)bottom * W + g_commaBodyX], 0);
  CHECK_EQ(separatorMask[(size_t)(bottom - 1) * W + g_commaBodyX], 0);
  // And nothing of the digit before it.
  for (uint32_t y = 8; y <= bottom; ++y)
    CHECK_EQ(separatorMask[(size_t)y * W + g_commaTailX - 1], 0);
  TearDownFrame();
}

// ...and it still carries its dyna layer, so the shadow pass gives it a shadow
// like any other dynamic pixel -- following the shape that was drawn, corner
// pixels included, rather than a square one.
static void Test_SeparatorStillCastsAShadow(void) {
  SetUpFrame(64, 32);
  BuildScoreWithComma(1);
  CHECK(DetectSeparators(64, 32) > 0);
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  // Every extra-plane pixel the tail painted names the layer it belongs to,
  // which is what GenerateExtraPlaneShadows() works from.
  unsigned withLayer = 0;
  for (uint32_t y = (g_commaBottom + 1) * 2; y <= (g_commaBottom + 1) * 2 + 1;
       ++y)
    for (uint32_t x2 = g_commaTailX * 2; x2 < g_commaTailX * 2 + 2; ++x2)
      if (hdDynaLayerMap[(size_t)y * 128 + x2] == 1) ++withLayer;
  CHECK_EQ(withLayer, 4);
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// Colorize_Framev2: the render itself
// ---------------------------------------------------------------------------

// A colorization with one frame and one background, sized to the synthetic
// frame. Everything is empty until a test fills it in.
static const uint32_t kFrameId = 1;
// The render is gated on the original plane being 32 rows, so these use a full
// height rather than the shorter frame the scaling tests are built on.
static const uint32_t kCH = 32;

static void SetUpColorization(void) {
  SetUpFrame(kW, kCH);
  const size_t px = (size_t)kW * kCH;
  g_serumData.nframes = 4;
  g_serumData.nocolors = 16;
  g_serumData.nbackgrounds = 1;
  g_serumData.frameHasDynamic.assign(g_serumData.nframes, 0);
  g_serumData.frameHasDynamicExtra.assign(g_serumData.nframes, 0);

  const std::vector<uint16_t> noColours(px, 0);
  g_serumData.cframes_v2.set(kFrameId, noColours.data(), px);
  const std::vector<uint16_t> bg(px, 0);
  g_serumData.backgroundframes_v2.set(0, bg.data(), px);
  const std::vector<uint8_t> noMask(px, 0);
  g_serumData.backgroundmask.set(kFrameId, noMask.data(), px);
  // 0xffff means "no background for this frame".
  const uint16_t noBackground[1] = {0xffff};
  g_serumData.backgroundIDs.set(kFrameId, noBackground, 1);
  // isextraframe is index-based and its no-data value is 0, which is already
  // "this frame has no extra-resolution content" -- leaving it unset says that.

  isoriginalrequested = true;
  isextrarequested = false;
  isoriginalfallbackrequested = false;
  upscaleExtraFromOriginal = false;  // plain 32p render, no layer mode
  mySerum.width32 = kW;
}

// Give the frame a dynamic zone: `mask` selects which pixels belong to colour
// set `couche`, and that set maps each ROM shade to a colour.
static void GiveFrameADynamicZone(const std::vector<uint8_t>& active,
                                  uint8_t couche,
                                  const std::vector<uint16_t>& shadeToColour) {
  const size_t px = (size_t)kW * kCH;
  g_serumData.frameHasDynamic[kFrameId] = 1;
  const std::vector<uint8_t> couches(px, couche);
  g_serumData.dynamasks.set(kFrameId, couches.data(), px);
  g_serumData.dynamasks_active.set(kFrameId, active.data(), px);
  std::vector<uint16_t> sets(
      (size_t)MAX_DYNA_SETS_PER_FRAME_V2 * g_serumData.nocolors, 0);
  for (uint32_t i = 0; i < g_serumData.nocolors && i < shadeToColour.size();
       ++i)
    sets[(size_t)couche * g_serumData.nocolors + i] = shadeToColour[i];
  g_serumData.dyna4cols_v2.set(kFrameId, sets.data(), sets.size());
}

// Static content comes through untouched: every pixel of the 32p output is the
// colour the colorization stored for it, whatever the ROM shade was.
static void Test_StaticContentIsTakenFromTheFrame(void) {
  SetUpColorization();
  const size_t px = (size_t)kW * kCH;
  std::vector<uint16_t> colours(px);
  for (size_t i = 0; i < px; ++i) colours[i] = (uint16_t)(0x1000 + (i % 501));
  g_serumData.cframes_v2.set(kFrameId, colours.data(), px);

  std::vector<uint8_t> rom(px);
  for (size_t i = 0; i < px; ++i) rom[i] = (uint8_t)(i % 16);
  Colorize_Framev2(rom.data(), kFrameId);

  for (size_t i = 0; i < px; ++i) CHECK_EQ(mySerum.frame32[i], colours[i]);
  TearDownFrame();
}

// Where the background mask is set and the ROM lit nothing, the background
// image shows through instead of the frame's own colour.
static void Test_BackgroundShowsThroughItsMask(void) {
  SetUpColorization();
  const size_t px = (size_t)kW * kCH;
  const std::vector<uint16_t> colours(px, 0x0111);
  g_serumData.cframes_v2.set(kFrameId, colours.data(), px);
  const std::vector<uint16_t> bg(px, 0x0222);
  g_serumData.backgroundframes_v2.set(0, bg.data(), px);
  std::vector<uint8_t> mask(px, 0);
  for (uint32_t x = 0; x < kW; ++x) mask[(size_t)4 * kW + x] = 1;  // one row
  g_serumData.backgroundmask.set(kFrameId, mask.data(), px);
  const uint16_t background[1] = {0};
  g_serumData.backgroundIDs.set(kFrameId, background, 1);

  std::vector<uint8_t> rom(px, 0);
  rom[(size_t)4 * kW + 3] = 5;  // one lit pixel on the masked row
  Colorize_Framev2(rom.data(), kFrameId);

  for (uint32_t x = 0; x < kW; ++x) {
    const size_t i = (size_t)4 * kW + x;
    // Lit pixels keep the frame's colour; the rest show the background.
    CHECK_EQ(mySerum.frame32[i], x == 3 ? 0x0111 : 0x0222);
  }
  // A row outside the mask is the frame's colour throughout.
  for (uint32_t x = 0; x < kW; ++x)
    CHECK_EQ(mySerum.frame32[(size_t)5 * kW + x], 0x0111);
  TearDownFrame();
}

// Inside a dynamic zone the colour comes from the zone's own set, indexed by
// the ROM shade -- which is what makes the same artwork recolour as the game
// changes the set, and what the whole dynamic-content path exists for.
static void Test_DynamicZoneColoursComeFromItsSet(void) {
  SetUpColorization();
  const size_t px = (size_t)kW * kCH;
  const std::vector<uint16_t> statics(px, 0x0111);
  g_serumData.cframes_v2.set(kFrameId, statics.data(), px);

  std::vector<uint8_t> active(px, 0);
  for (uint32_t x = 0; x < 8; ++x) active[(size_t)6 * kW + x] = 1;
  std::vector<uint16_t> shadeToColour(16, 0);
  for (int i = 0; i < 16; ++i) shadeToColour[i] = (uint16_t)(0x2000 + i);
  GiveFrameADynamicZone(active, /*couche=*/3, shadeToColour);

  std::vector<uint8_t> rom(px, 0);
  for (uint32_t x = 0; x < 8; ++x) rom[(size_t)6 * kW + x] = (uint8_t)(x + 1);
  Colorize_Framev2(rom.data(), kFrameId);

  for (uint32_t x = 0; x < 8; ++x)
    CHECK_EQ(mySerum.frame32[(size_t)6 * kW + x], 0x2000 + x + 1);
  // Outside the zone the static colour stands.
  CHECK_EQ(mySerum.frame32[(size_t)6 * kW + 9], 0x0111);
  CHECK_EQ(mySerum.frame32[(size_t)7 * kW + 0], 0x0111);
  TearDownFrame();
}

// A dynamic zone is only dynamic where its active mask says so; elsewhere the
// frame's static colour stands even though the zone's colour set covers it.
static void Test_DynamicZoneOnlyAppliesWhereActive(void) {
  SetUpColorization();
  const size_t px = (size_t)kW * kCH;
  const std::vector<uint16_t> statics(px, 0x0111);
  g_serumData.cframes_v2.set(kFrameId, statics.data(), px);

  std::vector<uint8_t> active(px, 0);
  active[(size_t)6 * kW + 2] = 1;
  std::vector<uint16_t> shadeToColour(16, 0x2222);
  GiveFrameADynamicZone(active, /*couche=*/0, shadeToColour);

  std::vector<uint8_t> rom(px, 4);  // every pixel lit, same shade
  Colorize_Framev2(rom.data(), kFrameId);

  CHECK_EQ(mySerum.frame32[(size_t)6 * kW + 2], 0x2222);
  CHECK_EQ(mySerum.frame32[(size_t)6 * kW + 1], 0x0111);
  CHECK_EQ(mySerum.frame32[(size_t)6 * kW + 3], 0x0111);
  TearDownFrame();
}

// A full-height rule beside the text must not change what the text does.
//
// The column group's row band is the rows it covers without a break, so a
// column running the height of the display stretches that band over the whole
// frame, drags the bottom line down to the last row, and the text sharing the
// group stops being recognized as text. im_185ve frames 506 and 507 show the
// same score beside the same divider -- dynamic in one, static in the other --
// and the thousands separators were filtered in one frame and not the other.
static void Test_AFullHeightRuleDoesNotDisturbTheText(void) {
  SetUpFrame(64, 32);
  BuildScoreWithComma(1);
  separatorMaskValid = false;
  const uint32_t withoutRule = DetectSeparators(64, 32);
  CHECK(withoutRule > 0);

  // Put a divider two columns to the right of the number, running top to
  // bottom, in the same shade.
  const uint32_t ruleX = g_commaBodyX + 14;
  for (uint32_t y = 0; y < 32; ++y) {
    Px(ruleX, y, 15, 0xffe0);
    sdDynaLayerMap[(size_t)y * 64 + ruleX] = 1;
  }
  separatorMaskValid = false;
  CHECK_EQ(DetectSeparators(64, 32), withoutRule);
  TearDownFrame();
}

// A stream of slightly different ROM frames that colorize to one picture must
// not make the extra plane move.
//
// The upscale does not read only the 32p picture: the preserving corner test
// consults the ROM frame, so the same picture upscales differently as the ROM
// frame underneath it changes. Here the colours come from the frame's own
// static content, so both ROM frames produce byte-identical 32p output while
// the shades behind one corner differ -- enough to change what the corner test
// decides, and enough to make the extra plane flicker on a static picture.
// Fills an index-based vector the way a load does; set() refuses those.
template <typename T>
static void FillIndexed(SparseVector<T>& v, const std::vector<T>& bytes,
                        size_t elementSize, uint32_t numElements) {
  struct MemReader {
    const uint8_t* p;
    size_t left;
    bool readExact(void* dst, size_t n) {
      if (n > left) return false;
      memcpy(dst, p, n);
      p += n;
      left -= n;
      return true;
    }
  } reader{(const uint8_t*)bytes.data(), bytes.size() * sizeof(T)};
  v.readFromCRomReader(elementSize, numElements, reader);
}

static void Test_UnchangedPictureKeepsItsExtraPlane(void) {
  SetUpColorization();
  upscaleExtraFromOriginal = true;
  isextrarequested = false;
  const size_t px = (size_t)kW * kCH;

  // A solid block, coloured from the frame rather than from the ROM.
  std::vector<uint16_t> colours(px, 0);
  for (uint32_t y = 8; y < 16; ++y)
    for (uint32_t x = 8; x < 16; ++x) colours[(size_t)y * kW + x] = 0xffff;
  g_serumData.cframes_v2.set(kFrameId, colours.data(), px);

  std::vector<uint8_t> romA(px, 0), romB(px, 0);
  for (uint32_t y = 8; y < 16; ++y)
    for (uint32_t x = 8; x < 16; ++x) {
      romA[(size_t)y * kW + x] = 15;
      romB[(size_t)y * kW + x] = 15;
    }
  // One shade behind a corner differs, which is what the corner test reads.
  romB[(size_t)9 * kW + 9] = 7;

  romFrameForUpscale = romA.data();
  extraPlaneReuseCount = 0;
  Colorize_Framev2(romA.data(), kFrameId);
  CHECK_EQ(extraPlaneReuseCount, 0);  // nothing to reuse yet
  CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
  std::vector<uint16_t> after(mySerum.frame64, mySerum.frame64 + px * 4);
  const std::vector<uint16_t> sd(mySerum.frame32, mySerum.frame32 + px);

  romFrameForUpscale = romB.data();
  Colorize_Framev2(romB.data(), kFrameId);
  CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
  CHECK_EQ(mySerum.width64, kW * 2);
  // The 32p picture is the same...
  for (size_t i = 0; i < px; ++i) CHECK_EQ(mySerum.frame32[i], sd[i]);
  // ...so the extra plane is the one already derived from it.
  CHECK_EQ(extraPlaneReuseCount, 1);
  unsigned moved = 0;
  for (size_t i = 0; i < px * 4; ++i)
    if (mySerum.frame64[i] != after[i]) ++moved;
  CHECK_EQ(moved, 0);

  // A picture that does change is derived again.
  for (uint32_t y = 8; y < 16; ++y) colours[(size_t)y * kW + 20] = 0xffff;
  g_serumData.cframes_v2.set(kFrameId, colours.data(), px);
  Colorize_Framev2(romB.data(), kFrameId);
  CHECK_EQ(extraPlaneReuseCount, 1);  // derived again, not reused
  unsigned changed = 0;
  for (size_t i = 0; i < px * 4; ++i)
    if (mySerum.frame64[i] != after[i]) ++changed;
  CHECK(changed > 0);

  // The plane belongs to this colorization. A load frees it, so the next one
  // must not be told there is a plane to keep -- its own first frame would
  // compare against a CRC taken from someone else's picture.
  TearDownFrame();
  CHECK(!lastUpscaledSdValid);
}

// A sprite's HD art is drawn into the extra plane after the composite, so the
// plane stops being a pure function of the SD picture and cannot be kept.
static void Test_HdSpriteArtBlocksReuse(void) {
  SetUpColorization();
  upscaleExtraFromOriginal = true;
  isextrarequested = false;
  const size_t px = (size_t)kW * kCH;

  std::vector<uint16_t> colours(px, 0);
  for (uint32_t y = 8; y < 16; ++y)
    for (uint32_t x = 8; x < 16; ++x) colours[(size_t)y * kW + x] = 0xffff;
  g_serumData.cframes_v2.set(kFrameId, colours.data(), px);

  std::vector<uint8_t> rom(px, 0);
  for (uint32_t y = 8; y < 16; ++y)
    for (uint32_t x = 8; x < 16; ++x) rom[(size_t)y * kW + x] = 15;
  romFrameForUpscale = rom.data();

  // One sprite, HD art all through, no dynamic pixels.
  const size_t spritePixels = (size_t)MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT;
  g_serumData.nsprites = 1;
  FillIndexed<uint8_t>(g_serumData.isextrasprite, {1}, 1, 1);
  const std::vector<uint8_t> opaque(spritePixels, 1);
  g_serumData.spriteoriginal_opaque.set(0, opaque.data(), spritePixels);
  const std::vector<uint16_t> art(spritePixels, 0x1234);
  g_serumData.spritecolored.set(0, art.data(), spritePixels);
  g_serumData.spritemask_extra_opaque.set(0, opaque.data(), spritePixels,
                                          &g_serumData.isextrasprite);
  g_serumData.spritecolored_extra.set(0, art.data(), spritePixels,
                                      &g_serumData.isextrasprite);

  extraPlaneReuseCount = 0;
  Colorize_Framev2(rom.data(), kFrameId);
  CHECK_EQ(extraPlaneReuseCount, 0);
  // The same picture again, with no sprite: kept, as the rule says.
  Colorize_Framev2(rom.data(), kFrameId);
  CHECK_EQ(extraPlaneReuseCount, 1);

  // Now a frame that draws HD sprite art over the plane.
  Colorize_Framev2(rom.data(), kFrameId);
  CHECK_EQ(extraPlaneReuseCount, 2);
  Colorize_Spritev2(rom.data(), 0, /*frx=*/2, /*fry=*/2, /*spx=*/0, /*spy=*/0,
                    /*wid=*/4, /*hei=*/4, kFrameId);
  CHECK_EQ(mySerum.frame64[2 * 2 * (kW * 2) + 2 * 2], 0x1234);

  // The next frame has the same SD picture, but the plane now holds that art,
  // so it must be derived again rather than kept.
  Colorize_Framev2(rom.data(), kFrameId);
  CHECK_EQ(extraPlaneReuseCount, 2);
  CHECK(mySerum.frame64[2 * 2 * (kW * 2) + 2 * 2] != 0x1234);
  TearDownFrame();
}

// The same SD picture can come from different frame IDs, and each ID draws its
// own HD artwork. So which frame the plane was derived from decides nothing:
// what matters is whether this frame composites HD content over it.
static void Test_HdStaticFrameIsNotReused(void) {
  SetUpColorization();
  upscaleExtraFromOriginal = true;
  // A colorization with HD statics is asked for its extra plane; the scaled
  // layer composites under it.
  isextrarequested = true;
  const size_t px = (size_t)kW * kCH;
  const uint32_t kPlain = kFrameId, kHdA = kFrameId + 1, kHdB = kFrameId + 2;

  // All three frames colorize to the same SD picture.
  std::vector<uint16_t> colours(px, 0);
  for (uint32_t y = 8; y < 16; ++y)
    for (uint32_t x = 8; x < 16; ++x) colours[(size_t)y * kW + x] = 0xffff;
  const std::vector<uint8_t> noMask(px, 0);
  const uint16_t noBackground[1] = {0xffff};
  for (uint32_t id : {kPlain, kHdA, kHdB}) {
    g_serumData.cframes_v2.set(id, colours.data(), px);
    g_serumData.backgroundmask.set(id, noMask.data(), px);
    g_serumData.backgroundIDs.set(id, noBackground, 1);
  }

  // Two of them carry HD artwork, and it differs; the third has none.
  std::vector<uint8_t> hasExtra(g_serumData.nframes, 0);
  hasExtra[kHdA] = 1;
  hasExtra[kHdB] = 1;
  FillIndexed<uint8_t>(g_serumData.isextraframe, hasExtra, 1,
                       g_serumData.nframes);
  const std::vector<uint16_t> hdA(px * 4, 0x0aaa), hdB(px * 4, 0x0bbb);
  g_serumData.cframes_v2_extra.set(kHdA, hdA.data(), px * 4,
                                   &g_serumData.isextraframe);
  g_serumData.cframes_v2_extra.set(kHdB, hdB.data(), px * 4,
                                   &g_serumData.isextraframe);

  std::vector<uint8_t> rom(px, 0);
  for (uint32_t y = 8; y < 16; ++y)
    for (uint32_t x = 8; x < 16; ++x) rom[(size_t)y * kW + x] = 15;
  romFrameForUpscale = rom.data();

  // A pixel outside the block, so it shows HD artwork rather than scaled
  // content.
  const size_t probe = (size_t)2 * (kW * 2) + 2;
  extraPlaneReuseCount = 0;

  Colorize_Framev2(rom.data(), kHdA);
  CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
  CHECK_EQ(mySerum.frame64[probe], 0x0aaa);
  const std::vector<uint16_t> sd(mySerum.frame32, mySerum.frame32 + px);

  // Another ID, the same SD picture, different artwork.
  Colorize_Framev2(rom.data(), kHdB);
  for (size_t i = 0; i < px; ++i) CHECK_EQ(mySerum.frame32[i], sd[i]);
  CHECK_EQ(mySerum.frame64[probe], 0x0bbb);

  // A frame with no HD content: its plane is the upscale of the SD picture
  // alone, so the artwork composited a moment ago must be gone.
  Colorize_Framev2(rom.data(), kPlain);
  CHECK(mySerum.frame64[probe] != 0x0bbb);
  CHECK(mySerum.frame64[probe] != 0x0aaa);

  // And back: after a plane that *was* derived, a frame with HD artwork still
  // composites it rather than keeping what the upscale left.
  Colorize_Framev2(rom.data(), kHdA);
  CHECK_EQ(mySerum.frame64[probe], 0x0aaa);

  // None of these frames may have kept a plane.
  CHECK_EQ(extraPlaneReuseCount, 0);
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// scaling.txt, the per-colorization override
// ---------------------------------------------------------------------------

// Writes a scaling.txt and hands back the directory holding it.
static std::string WriteSidecar(const char* body) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "libserum_tests" / "altcolor";
  std::filesystem::create_directories(dir);
  std::ofstream out(dir / "scaling.txt", std::ios::trunc);
  out << body;
  out.close();
  return dir.string();
}

static void ExpectSidecar(const char* body, int algorithm, int shadowOffset) {
  const ScalingSidecar got = read_scaling_sidecar(WriteSidecar(body));
  ++g_checks;
  if (algorithm < 0) {
    if (got.algorithm)
      Fail(__FILE__, __LINE__, "%s: algorithm set to %d, want untouched", body,
           (int)*got.algorithm);
  } else if (!got.algorithm || (int)*got.algorithm != algorithm) {
    Fail(__FILE__, __LINE__, "%s: algorithm %s, want %d", body,
         got.algorithm ? std::to_string((int)*got.algorithm).c_str() : "unset",
         algorithm);
  }
  ++g_checks;
  if (shadowOffset < 0) {
    if (got.shadowOffsetMode)
      Fail(__FILE__, __LINE__, "%s: shadow-offset set to %d, want untouched",
           body, (int)*got.shadowOffsetMode);
  } else if (!got.shadowOffsetMode ||
             (int)*got.shadowOffsetMode != shadowOffset) {
    Fail(__FILE__, __LINE__, "%s: shadow-offset %s, want %d", body,
         got.shadowOffsetMode
             ? std::to_string((int)*got.shadowOffsetMode).c_str()
             : "unset",
         shadowOffset);
  }
}

// The file is how an author overrides the stored choice, so every spelling the
// parser advertises has to keep working, and anything it does not recognize
// has to leave the stored choice alone rather than fall back to a default.
static void Test_ScalingSidecarSpellings(void) {
  ExpectSidecar("scale2x\n", SERUM_SCALING_SCALE2X, -1);
  ExpectSidecar("line-doubling\n", SERUM_SCALING_LINE_DOUBLING, -1);
  ExpectSidecar("linedoubling\n", SERUM_SCALING_LINE_DOUBLING, -1);
  ExpectSidecar("linedouble\n", SERUM_SCALING_LINE_DOUBLING, -1);
  ExpectSidecar("scaling: scale2x\n", SERUM_SCALING_SCALE2X, -1);
  ExpectSidecar("algorithm: line-doubling\n", SERUM_SCALING_LINE_DOUBLING, -1);
}

// The value libserum reports is the one a host passes to libframeutil to scale
// the same way, so the two have to keep the same numbering -- and libserum's
// Scale2x is libframeutil's Scale2xPreserve, not its reference Scale2x.
static void Test_AlgorithmMatchesLibframeutil(void) {
  SetUpFrame();
  CHECK_EQ(SERUM_SCALING_SCALE2X,
           (int)FrameUtil::ScalingAlgorithm::Scale2xPreserve);
  CHECK_EQ(SERUM_SCALING_LINE_DOUBLING,
           (int)FrameUtil::ScalingAlgorithm::LineDoubling);
  runtimeScalingAlgorithm = SERUM_SCALING_LINE_DOUBLING;
  CHECK_EQ((int)RuntimeScalingAlgorithm(),
           (int)FrameUtil::ScalingAlgorithm::LineDoubling);
  runtimeScalingAlgorithm = SERUM_SCALING_SCALE2X;
  CHECK_EQ((int)RuntimeScalingAlgorithm(),
           (int)FrameUtil::ScalingAlgorithm::Scale2xPreserve);
  // Anything else is out of range and renders as the default rather than as
  // whatever libframeutil has at that value.
  runtimeScalingAlgorithm = 0;
  CHECK_EQ((int)RuntimeScalingAlgorithm(),
           (int)FrameUtil::ScalingAlgorithm::Scale2xPreserve);
  TearDownFrame();
}

static void Test_ScalingSidecarShadowOffset(void) {
  ExpectSidecar("shadow-offset: native\n", -1, SERUM_SHADOW_OFFSET_NATIVE);
  ExpectSidecar("shadow-offset: proportional\n", -1,
                SERUM_SHADOW_OFFSET_PROPORTIONAL);
  ExpectSidecar("shadowoffset: 1\n", -1, SERUM_SHADOW_OFFSET_NATIVE);
  ExpectSidecar("shadow-offset: 2\n", -1, SERUM_SHADOW_OFFSET_PROPORTIONAL);
  ExpectSidecar("scale2x\nshadow-offset: proportional\n", SERUM_SCALING_SCALE2X,
                SERUM_SHADOW_OFFSET_PROPORTIONAL);
}

// Comments, blank lines, padding and case are all tolerated; an unknown key,
// an unknown value and a missing file all leave the stored choice standing.
static void Test_ScalingSidecarTolerance(void) {
  ExpectSidecar("# a comment\n\n  SCALE2X  \n", SERUM_SCALING_SCALE2X, -1);
  ExpectSidecar("scale2x # trailing comment\n", SERUM_SCALING_SCALE2X, -1);
  ExpectSidecar("\tShadow-Offset :\tPROPORTIONAL \n", -1,
                SERUM_SHADOW_OFFSET_PROPORTIONAL);
  ExpectSidecar("hq2x\n", -1, -1);
  // The names the preserving variant of Scale2x used to have. There is one
  // Scale2x now, so these are unknown like any other unknown value: the stored
  // choice stands rather than being replaced.
  ExpectSidecar("scale2x-preserve\n", -1, -1);
  ExpectSidecar("preserve\n", -1, -1);
  ExpectSidecar("scaling: hq2x\n", -1, -1);
  ExpectSidecar("shadow-offset: sideways\n", -1, -1);
  ExpectSidecar("colours: many\n", -1, -1);
  ExpectSidecar("", -1, -1);

  const ScalingSidecar none = read_scaling_sidecar(
      (std::filesystem::temp_directory_path() / "libserum_tests_absent")
          .string());
  CHECK(!none.algorithm);
  CHECK(!none.shadowOffsetMode);
  CHECK(!none.any());
}

// ---------------------------------------------------------------------------
// SparseVector, the storage every vector in a colorization sits on
// ---------------------------------------------------------------------------

// What goes in comes out, for an element that was set and for one that never
// was -- the latter reads back as the no-data signature rather than as absent,
// which is what lets the render path index it unconditionally.
static void Test_SparseVectorRoundTrip(void) {
  SparseVector<uint16_t> v(0);
  const uint16_t a[4] = {1, 2, 3, 4};
  const uint16_t b[4] = {9, 8, 7, 6};
  v.set(0, a, 4);
  v.set(2, b, 4);
  CHECK(v.hasData(0));
  CHECK(v.hasData(2));
  CHECK(!v.hasData(1));
  for (int i = 0; i < 4; ++i) {
    CHECK_EQ(v[0][i], a[i]);
    CHECK_EQ(v[2][i], b[i]);
    CHECK_EQ(v[1][i], 0);
  }
}

// An element whose payload is entirely the no-data signature is not stored at
// all: that is the whole point of the structure, and a colorization is mostly
// such elements.
static void Test_SparseVectorDropsEmptyPayloads(void) {
  SparseVector<uint8_t> v(0);
  const uint8_t empty[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t filled[8] = {0, 0, 1, 0, 0, 0, 0, 0};
  v.set(5, empty, 8);
  v.set(6, filled, 8);
  CHECK(!v.hasData(5));
  CHECK(v.hasData(6));
  CHECK_EQ(v[6][2], 1);
}

// A vector parented to another only stores an element where the parent has
// one. dynamasks_extra hangs off isextraframe this way, so a frame with no
// extra content costs nothing.
static void Test_SparseVectorParentGating(void) {
  SparseVector<uint8_t> parent(0);
  const uint8_t on[1] = {1};
  parent.set(3, on, 1);

  SparseVector<uint8_t> child(0);
  const uint8_t payload[4] = {7, 7, 7, 7};
  child.set(3, payload, 4, &parent);
  child.set(4, payload, 4, &parent);  // parent has nothing for 4
  CHECK(child.hasData(3));
  CHECK(!child.hasData(4));
  CHECK_EQ(child[3][0], 7);
}

// Values are packed to the fewest bits that can hold them, and must survive it
// exactly. dynamasks is constructed this way -- no-data 255, compressed, value
// packed -- and a normalization to 0/1 here would flatten every dynamic mask
// into "is there a zone", losing which of the four colour sets each pixel uses.
static void Test_SparseVectorValuePackingIsExact(void) {
  SparseVector<uint8_t> v(255, false, true, true, 0, 1);
  uint8_t payload[16];
  for (int i = 0; i < 16; ++i) payload[i] = (uint8_t)i;  // needs four bits
  v.set(1, payload, 16);
  for (int i = 0; i < 16; ++i) CHECK_EQ(v[1][i], (uint8_t)i);

  // Two bits' worth, the width a dynamic mask actually uses.
  uint8_t couches[16];
  for (int i = 0; i < 16; ++i) couches[i] = (uint8_t)(i % 4);
  v.set(2, couches, 16);
  for (int i = 0; i < 16; ++i) CHECK_EQ(v[2][i], (uint8_t)(i % 4));

  // And a payload too wide to pack still round-trips, unpacked.
  uint8_t wide[16];
  for (int i = 0; i < 16; ++i) wide[i] = (uint8_t)(i * 16);
  v.set(3, wide, 16);
  for (int i = 0; i < 16; ++i) CHECK_EQ(v[3][i], (uint8_t)(i * 16));
}

// uint16_t vectors -- the colorized frames themselves -- are never value
// packed, and must come back bit for bit.
static void Test_SparseVectorWideValuesRoundTrip(void) {
  SparseVector<uint16_t> w(0);
  uint16_t wide[8] = {0, 1, 255, 256, 4095, 4096, 65534, 65535};
  w.set(1, wide, 8);
  for (int i = 0; i < 8; ++i) CHECK_EQ(w[1][i], wide[i]);
}

// ---------------------------------------------------------------------------
// Colour rotations
// ---------------------------------------------------------------------------

// One rotation in slot 0: four colours, 10 ms apart. A pixel tagged with the
// slot advances; a pixel tagged 0xffff never does.
static void Test_RotationAdvancesTaggedPixelsOnly(void) {
  SetUpFrame();
  mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
  mySerum.rotations32 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations64 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations32[0] = 4;   // four colours
  mySerum.rotations32[1] = 10;  // every 10 ms
  for (int i = 0; i < 4; ++i)
    mySerum.rotations32[2 + i] = (uint16_t)(0x100 + i);

  const size_t px = (size_t)kW * 32;
  for (size_t i = 0; i < px; ++i) {
    mySerum.rotationsinframe32[i * 2] = 0xffff;
    mySerum.rotationsinframe32[i * 2 + 1] = 0xffff;
  }
  mySerum.rotationsinframe32[0] = 0;  // pixel 0 is in slot 0, at offset 0
  mySerum.rotationsinframe32[1] = 0;
  mySerum.frame32[0] = 0x100;
  mySerum.frame32[1] = 0xDEAD;  // untagged, must not move

  colorshiftinittime32[0] = 0;  // long overdue
  colorshifts32[0] = 0;
  Serum_ApplyRotationsv2();

  CHECK_EQ(mySerum.frame32[0], 0x101);
  CHECK_EQ(mySerum.frame32[1], 0xDEAD);
  CHECK_EQ(colorshifts32[0], 1);
  TearDownFrame();
}

// The shift wraps at the rotation's length rather than running off the end of
// the colour list.
static void Test_RotationWrapsAtItsLength(void) {
  SetUpFrame();
  mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
  mySerum.rotations32 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations64 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations32[0] = 3;
  mySerum.rotations32[1] = 1;
  for (int i = 0; i < 3; ++i)
    mySerum.rotations32[2 + i] = (uint16_t)(0x200 + i);
  const size_t px = (size_t)kW * 32;
  for (size_t i = 0; i < px; ++i) mySerum.rotationsinframe32[i * 2] = 0xffff;
  mySerum.rotationsinframe32[0] = 0;
  mySerum.rotationsinframe32[1] = 0;

  colorshifts32[0] = 2;
  for (int step = 0; step < 4; ++step) {
    colorshiftinittime32[0] = 0;
    Serum_ApplyRotationsv2();
    CHECK(colorshifts32[0] < 3);
    CHECK(mySerum.frame32[0] >= 0x200 && mySerum.frame32[0] <= 0x202);
  }
  TearDownFrame();
}

// A slot with no colours, or no delay, is not a rotation and must not be
// reported as one due next.
static void Test_EmptyRotationSlotIsNotDueNext(void) {
  SetUpFrame();
  mySerum.rotations32 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations64 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  CHECK_EQ(Calc_Next_Rotationv2(1000), 0);

  mySerum.rotations32[0] = 4;
  mySerum.rotations32[1] = 0;  // no delay: still not a rotation
  CHECK_EQ(Calc_Next_Rotationv2(1000), 0);

  mySerum.rotations32[1] = 25;
  colorrotnexttime32[0] = 1200;
  CHECK_EQ(Calc_Next_Rotationv2(1000), 200);
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
    {"separator/envelope_follows_the_font",
     Test_SeparatorEnvelopeFollowsTheFont},
    {"separator/identical_treated_alike",
     Test_IdenticalSeparatorsAreTreatedAlike},
    {"separator/stern_score_column",
     Test_SternStyleScoreColumnKeepsEverySeparator},
    {"separator/two_lines_keep_separators",
     Test_TwoLinesOfScoresKeepTheirSeparators},
    {"separator/full_height_rule_ignored",
     Test_AFullHeightRuleDoesNotDisturbTheText},
    {"separator/scaled_as_its_own_shape", Test_SeparatorIsScaledAsItsOwnShape},
    {"separator/touching_a_digit_is_not_grown",
     Test_SeparatorTouchingADigitIsNotGrown},
    {"separator/still_casts_a_shadow", Test_SeparatorStillCastsAShadow},
    {"colorize/static_content", Test_StaticContentIsTakenFromTheFrame},
    {"colorize/unchanged_picture_keeps_extra_plane",
     Test_UnchangedPictureKeepsItsExtraPlane},
    {"colorize/hd_sprite_art_blocks_reuse", Test_HdSpriteArtBlocksReuse},
    {"colorize/hd_static_frame_is_not_reused", Test_HdStaticFrameIsNotReused},
    {"colorize/background_mask", Test_BackgroundShowsThroughItsMask},
    {"colorize/dynamic_zone_colours", Test_DynamicZoneColoursComeFromItsSet},
    {"colorize/dynamic_zone_active_mask",
     Test_DynamicZoneOnlyAppliesWhereActive},
    {"sidecar/spellings", Test_ScalingSidecarSpellings},
    {"sidecar/matches_libframeutil", Test_AlgorithmMatchesLibframeutil},
    {"sidecar/shadow_offset", Test_ScalingSidecarShadowOffset},
    {"sidecar/tolerance", Test_ScalingSidecarTolerance},
    {"sparse/round_trip", Test_SparseVectorRoundTrip},
    {"sparse/drops_empty_payloads", Test_SparseVectorDropsEmptyPayloads},
    {"sparse/parent_gating", Test_SparseVectorParentGating},
    {"sparse/value_packing_is_exact", Test_SparseVectorValuePackingIsExact},
    {"sparse/wide_values_round_trip", Test_SparseVectorWideValuesRoundTrip},
    {"rotation/advances_tagged_pixels_only",
     Test_RotationAdvancesTaggedPixelsOnly},
    {"rotation/wraps_at_length", Test_RotationWrapsAtItsLength},
    {"rotation/empty_slot_not_due_next", Test_EmptyRotationSlotIsNotDueNext},
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
