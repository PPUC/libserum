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
#ifdef SERUM_ACCEL_REFERENCE_IN_TEST_TU
#include "serum-accel-ref.cpp"
#endif

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
  // Both branches of the loader guarantee these are long enough to index with
  // every dyna set, because the render path reads all of them whether the
  // colorization set any or not -- an element that was never set reads back as
  // the no-data buffer, and that buffer is only as long as it was reserved.
  // Without this the fixture is shorter than a real load and the shadow pass
  // reads past it; see the reserve() calls in Serum_LoadFilev2Stream().
  g_serumData.dynashadowsdir.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
  g_serumData.dynashadowscol.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
  g_serumData.dynashadowsdir_extra.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
  g_serumData.dynashadowscol_extra.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
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

// Where the scaled layer rounds a pixel outwards, the HD frame underneath is
// what shows -- and an author who puts a dynamic zone over artwork leaves that
// frame black across the zone's footprint, because the zone is what covers it.
// Yielding to that black puts a speck along every edge the scaler rounds.
//
// bdk_294 frame 689 draws its "30" in such a zone and the specks appeared
// around the sprite. Where the HD frame does hold content it still stands.
static void Test_LayerDoesNotYieldToHdNothing(void) {
  SetUpFrame();
  const uint32_t W = g_serumData.fwidth;
  // A zone in the middle of the frame, everything outside it unowned.
  memset(scaledLayerCoverage, 0, (size_t)W * kH);
  const uint32_t zx0 = 4, zx1 = 9, zy0 = 4, zy1 = 9;
  for (uint32_t y = zy0; y <= zy1; ++y)
    for (uint32_t x = zx0; x <= zx1; ++x) scaledLayerCoverage[y * W + x] = 1;
  // Content inside it, and a different colour all around: the scaler then has
  // a neighbour worth rounding to along the zone's edge.
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const bool inZone = x >= zx0 && x <= zx1 && y >= zy0 && y <= zy1;
      Px(x, y, inZone ? 15 : 3, inZone ? 0x7777 : 0x2222);
    }
  // A diagonal step on the zone's edge, which is what Scale2x rounds.
  Px(zx1, zy0, 3, 0x2222);

  // The HD pass has run and left the zone's footprint black, artwork outside.
  const auto renderHdStatics = [&]() {
    for (uint32_t y = 0; y < kH * 2; ++y)
      for (uint32_t x = 0; x < W * 2; ++x) {
        const bool inZone = (x / 2) >= zx0 && (x / 2) <= zx1 &&
                            (y / 2) >= zy0 && (y / 2) <= zy1;
        mySerum.frame64[(size_t)y * W * 2 + x] = inZone ? 0 : 0x3333;
      }
    mySerum.flags = FLAG_RETURNED_64P_FRAME_OK;
    extraPlaneIsDerived = false;  // HD content stands where the layer stops
  };

  renderHdStatics();
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  // No destination whose own source is inside the zone may be left black.
  unsigned specks = 0;
  for (uint32_t y = zy0 * 2; y <= zy1 * 2 + 1; ++y)
    for (uint32_t x = zx0 * 2; x <= zx1 * 2 + 1; ++x)
      if (mySerum.frame64[(size_t)y * W * 2 + x] == 0) ++specks;
  CHECK_EQ(specks, 0);

  // With artwork behind it instead of nothing, the layer still yields: that is
  // the boundary being rounded against what is actually drawn there.
  renderHdStatics();
  for (uint32_t y = 0; y < kH * 2; ++y)
    for (uint32_t x = 0; x < W * 2; ++x)
      if (mySerum.frame64[(size_t)y * W * 2 + x] == 0)
        mySerum.frame64[(size_t)y * W * 2 + x] = 0x5555;
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true);
  unsigned yielded = 0;
  for (uint32_t y = zy0 * 2; y <= zy1 * 2 + 1; ++y)
    for (uint32_t x = zx0 * 2; x <= zx1 * 2 + 1; ++x)
      if (mySerum.frame64[(size_t)y * W * 2 + x] == 0x5555) ++yielded;
  CHECK(yielded > 0);
  TearDownFrame();
}

// A sprite composites its own region after the whole frame has already been
// composited, and what the frame left there is the picture from BEFORE the
// sprite drew -- on avr_200 the ROM's own white digits, under a sprite that
// replaces them with pink artwork. Yielding to that at the sprite's rounded
// edges keeps stale pixels: white specks around the corners of the glyphs.
//
// So a bounded pass paints every destination whose own source it owns, even
// where the rounding would reach outside. A whole-frame pass still yields,
// because there what stands is authored HD content rather than its own
// earlier output -- that is `upscale/no_yield_to_hd_nothing`.
static void Test_SpriteRegionIsNotLeftStale(void) {
  SetUpFrame();
  const uint32_t W = g_serumData.fwidth;
  const uint32_t zx0 = 6, zx1 = 9, zy0 = 6, zy1 = 9;
  memset(scaledLayerCoverage, 0, (size_t)W * kH);
  for (uint32_t y = zy0; y <= zy1; ++y)
    for (uint32_t x = zx0; x <= zx1; ++x) scaledLayerCoverage[y * W + x] = 1;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const bool in = x >= zx0 && x <= zx1 && y >= zy0 && y <= zy1;
      Px(x, y, in ? 15 : 3, in ? 0x7777 : 0x2222);
    }
  // A diagonal step on the region's edge, so the scaler reaches outside it.
  Px(zx1, zy0, 3, 0x2222);

  // The plane already holds an earlier composite of the picture before the
  // sprite drew -- a colour that is neither black nor the sprite's.
  const uint16_t kStale = 0xffff;
  for (uint32_t y = 0; y < kH * 2; ++y)
    for (uint32_t x = 0; x < W * 2; ++x)
      mySerum.frame64[(size_t)y * W * 2 + x] = kStale;
  mySerum.flags = FLAG_RETURNED_64P_FRAME_OK;
  extraPlaneIsDerived = false;  // HD content stands where the layer stops

  const uint16_t bounds[4] = {(uint16_t)zx0, (uint16_t)zy0, (uint16_t)zx1,
                              (uint16_t)zy1};
  UpscaleOriginalPlaneIntoExtra(false, /*onlyCoveredPixels=*/true, bounds);

  unsigned stale = 0;
  for (uint32_t y = zy0 * 2; y <= zy1 * 2 + 1; ++y)
    for (uint32_t x = zx0 * 2; x <= zx1 * 2 + 1; ++x)
      if (mySerum.frame64[(size_t)y * W * 2 + x] == kStale) ++stale;
  CHECK_EQ(stale, 0);
  // Outside the region the pass must still leave the plane alone.
  CHECK_EQ(mySerum.frame64[(size_t)0 * W * 2 + 0], kStale);
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

  CheckDynaShadow(mySerum.frame32, mySerum.rotationsinframe32, dir.data(),
                  col.data(), 0, isdynapix.data(), 5, 5, kW, kH, nullptr,
                  nullptr, /*defer=*/true);

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
  SetUpFrame(kW, 32);
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
#ifdef SERUM_ACCEL_REFERENCE_IN_TEST_TU
  using namespace serum_accel;
  SetUpFrame();
  FrameDwordSet& set = frameDwordSets[0];
  CHECK(AllocateFrameDwordSet(set, kW, kH));
  // Values that differ only high up. Fibonacci hashing carries its entropy
  // upwards, so taking the low bits of the product sends every one of these to
  // the same slot -- still correct, and 22 us a frame instead of 4.8.
  std::vector<int> used(set.mask + 1, 0);
  for (uint32_t i = 1; i <= 64; ++i) used[FrameDwordSlot(set, i << 24)] = 1;
  int distinct = 0;
  for (size_t i = 0; i < used.size(); ++i) distinct += used[i];
  CHECK(distinct > 32);
  TearDownFrame();
#endif
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
// Several Stern games wipe a score away one row at a time from the top rather
// than blinking it. Two rows in, the fragment that is left is too short for the
// analysis to place a bottom line, and the commas stop being recognized -- so
// they fuse with the remains of the digits in the upscale, exactly when those
// remains are hardest to read. The commas themselves never moved, so the
// decision made while the number was whole is carried until the wipe reaches
// them.
//
// Drawn from the frame the effect was reported on: "1,113,134" on bdk_294,
// whose own digits are what make the fragment ambiguous. A score of solid
// blocks keeps enough of a bottom line to be recognized all the way down, so it
// cannot show the rule working.
static void Test_SeparatorSurvivesARowWipe(void) {
  SetUpFrame(128, 32);
  const uint32_t W = g_serumData.fwidth, H = g_serumData.fheight;
  static const char* kScore[] = {
      "###...###..#..###...###.###.###.", "..#.....#.##..#.......#.#.#.#.#.",
      ".#.....#...#..###....#..###.#.#.", ".#.....#...#....#....#....#.#.#.",
      ".#..#..#..###.###.#..#..###.###.", "...#.............#..............",
  };
  const uint32_t kTop = 1, kLeft = 8;
  const uint32_t kRows = (uint32_t)(sizeof(kScore) / sizeof(kScore[0]));
  const auto draw = [&]() {
    memset(mySerum.frame32, 0, (size_t)W * H * sizeof(uint16_t));
    memset(sdDynaLayerMap, 0, (size_t)W * H);
    std::fill(g_rom.begin(), g_rom.end(), 0);
    for (uint32_t r = 0; r < kRows; ++r)
      for (uint32_t c = 0; kScore[r][c]; ++c) {
        if (kScore[r][c] != '#') continue;
        const size_t i = (size_t)(kTop + r) * W + kLeft + c;
        mySerum.frame32[i] = 0xffe0;
        g_rom[i] = 15;
        sdDynaLayerMap[i] = 1;
      }
  };
  const auto detect = [&]() {
    separatorMaskValid = false;
    return DetectSeparators(W, H);
  };
  const auto wipeTo = [&](uint32_t lastRow) {
    for (uint32_t y = kTop; y <= lastRow; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        mySerum.frame32[(size_t)y * W + x] = 0;
        g_rom[(size_t)y * W + x] = 0;
        sdDynaLayerMap[(size_t)y * W + x] = 0;
      }
  };

  draw();
  const uint32_t whole = detect();
  CHECK_EQ(whole, 2);  // both commas of "1,113,134"

  // Eaten from the top, one row per frame. Both commas keep their decision.
  for (uint32_t y = kTop; y <= kTop + 2; ++y) {
    wipeTo(y);
    const uint32_t got = detect();
    ++g_checks;
    if (got != whole)
      Fail(__FILE__, __LINE__, "rows %u..%u wiped: %u separators, expected %u",
           kTop, y, got, whole);
  }

  // And the memory is what is doing that: the same fragment, judged on its own,
  // has already lost them.
  rememberedSeparatorCount = 0;
  CHECK(detect() < whole);

  // The memory ends where the comma does. Its window reaches one row above it,
  // so wiping that row drops it rather than holding a decision about a picture
  // that has changed.
  draw();
  CHECK_EQ(detect(), whole);
  wipeTo(kTop + 3);  // row 4, the margin over the comma on row 5
  CHECK(detect() < whole);
  TearDownFrame();
}

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

// Under a background mask an unlit pixel shows the background before the
// dynamic zone is ever asked -- unless FLAG_SCENE_REPLACE_DYNAMIC_BLACK is in
// force, where the zone's colour decides: a set that gives shade 0 a colour
// keeps it, and one that leaves shade 0 black still shows the background.
static void Test_UnlitDynamicColourBeatsTheBackgroundOnRequest(void) {
  SetUpColorization();
  const size_t px = (size_t)kW * kCH;
  const std::vector<uint16_t> bg(px, 0x0222);
  g_serumData.backgroundframes_v2.set(0, bg.data(), px);
  const std::vector<uint8_t> mask(px, 1);
  g_serumData.backgroundmask.set(kFrameId, mask.data(), px);
  const uint16_t background[1] = {0};
  g_serumData.backgroundIDs.set(kFrameId, background, 1);

  const size_t zone = (size_t)6 * kW + 2;
  std::vector<uint8_t> active(px, 0);
  active[zone] = 1;
  std::vector<uint8_t> rom(px, 0);  // nothing lit

  GiveFrameADynamicZone(active, /*couche=*/2, {0x0333});
  Colorize_Framev2(rom.data(), kFrameId);
  CHECK_EQ(mySerum.frame32[zone], 0x0222);
  Colorize_Framev2(rom.data(), kFrameId, false, false,
                   /*replaceDynamicBlackContent=*/true);
  CHECK_EQ(mySerum.frame32[zone], 0x0333);
  // Outside the zone the background is untouched by the flag.
  CHECK_EQ(mySerum.frame32[zone + 1], 0x0222);

  GiveFrameADynamicZone(active, /*couche=*/2, {0});
  Colorize_Framev2(rom.data(), kFrameId, false, false, true);
  CHECK_EQ(mySerum.frame32[zone], 0x0222);
  TearDownFrame();
}

// Inverted text under FLAG_SCENE_REPLACE_DYNAMIC_BLACK: the glyph is the unlit
// pixel and the lit surround is mapped to black, so it is the unlit pixel that
// has to cast the shadow -- onto the holes either side of it, whichever way
// the raster reaches them -- and no shadow may cover the glyph itself.
static void Test_UnlitDynamicColourCastsItsShadow(void) {
  SetUpColorization();
  const size_t px = (size_t)kW * kCH;
  const std::vector<uint16_t> bg(px, 0x0222);
  g_serumData.backgroundframes_v2.set(0, bg.data(), px);
  const std::vector<uint8_t> mask(px, 1);
  g_serumData.backgroundmask.set(kFrameId, mask.data(), px);
  const uint16_t background[1] = {0};
  g_serumData.backgroundIDs.set(kFrameId, background, 1);

  // hole, glyph, glyph, hole -- all inside the zone.
  const size_t first = (size_t)6 * kW + 2;
  std::vector<uint8_t> active(px, 0);
  std::vector<uint8_t> rom(px, 0);
  for (size_t i = first; i < first + 4; ++i) active[i] = 1;
  rom[first] = rom[first + 3] = 5;
  GiveFrameADynamicZone(active, /*couche=*/2, {0x0333});  // shade 5 is black

  std::vector<uint8_t> dir(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  std::vector<uint16_t> col(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  dir[2] = (1u << 3) | (1u << 7);  // right and left
  col[2] = 0x0444;
  g_serumData.dynashadowsdir.set(kFrameId, dir.data(), dir.size());
  g_serumData.dynashadowscol.set(kFrameId, col.data(), col.size());

  Colorize_Framev2(rom.data(), kFrameId, false, false,
                   /*replaceDynamicBlackContent=*/true);
  CHECK_EQ(mySerum.frame32[first], 0x0444);
  CHECK_EQ(mySerum.frame32[first + 1], 0x0333);
  CHECK_EQ(mySerum.frame32[first + 2], 0x0333);
  CHECK_EQ(mySerum.frame32[first + 3], 0x0444);

  // Without the flag the roles are the historical ones: the lit pixels are
  // the content, painted black, and it is they that cast the shadow.
  Colorize_Framev2(rom.data(), kFrameId);
  CHECK_EQ(mySerum.frame32[first], 0);
  CHECK_EQ(mySerum.frame32[first + 1], 0x0444);
  CHECK_EQ(mySerum.frame32[first + 2], 0x0444);
  CHECK_EQ(mySerum.frame32[first + 3], 0);
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

// A colorization turns many slightly different ROM frames into one stable
// picture, often through different frame ids. Announcing each of them as a new
// frame makes the host re-render and re-send a picture it already has, and
// restarts anything keyed on a frame arriving.
static void Test_FinishedFrameAlreadyOnDisplay(void) {
  SetUpFrame(128, 32);
  const size_t px = (size_t)g_serumData.fwidth * g_serumData.fheight;
  mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
  mySerum.width32 = g_serumData.fwidth;
  mySerum.width64 = 0;
  memset(mySerum.frame32, 0, px * sizeof(uint16_t));
  mySerum.frame32[10] = 0x1234;
  // The rotation tables are the caller's copy, which a load allocates; the
  // frame fixture does not.
  const size_t rotWords = MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION;
  mySerum.rotations32 = (uint16_t*)calloc(rotWords, sizeof(uint16_t));
  mySerum.rotations64 = (uint16_t*)calloc(rotWords, sizeof(uint16_t));

  // Nothing has been reported yet, so the first frame is always news.
  lastReportedOutputValid = false;
  CHECK(!FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                         CurrentRotationCrc()));

  lastReportedOutputCrc = CurrentOutputCrc();
  lastReportedRotationCrc = CurrentRotationCrc();
  lastReportedOutputValid = true;

  // The same picture and the same rotations: the caller has it already.
  CHECK(FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                        CurrentRotationCrc()));

  // One pixel different is a different picture.
  mySerum.frame32[10] = 0x1235;
  CHECK(!FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                         CurrentRotationCrc()));
  mySerum.frame32[10] = 0x1234;
  CHECK(FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                        CurrentRotationCrc()));

  // The same picture that rotates differently still has to be announced, or
  // its colours would never start moving.
  mySerum.rotations32[0] = 4;
  mySerum.rotations32[1] = 100;
  CHECK(!FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                         CurrentRotationCrc()));
  mySerum.rotations32[0] = 0;
  mySerum.rotations32[1] = 0;
  CHECK(FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                        CurrentRotationCrc()));

  // A scene frame or a frame that fired a trigger is news whatever it looks
  // like.
  CHECK(!FinishedFrameIsAlreadyOnDisplay(false, CurrentOutputCrc(),
                                         CurrentRotationCrc()));

  // The 64p plane counts as well: a frame whose 32p picture repeats can still
  // differ at 64p, and which plane the caller reads is its choice.
  mySerum.flags = FLAG_RETURNED_32P_FRAME_OK | FLAG_RETURNED_64P_FRAME_OK;
  mySerum.width64 = g_serumData.fwidth * 2;
  memset(mySerum.frame64, 0, px * 4 * sizeof(uint16_t));
  lastReportedOutputCrc = CurrentOutputCrc();
  lastReportedRotationCrc = CurrentRotationCrc();
  CHECK(FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                        CurrentRotationCrc()));
  mySerum.frame64[7] = 0x0abc;
  CHECK(!FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                         CurrentRotationCrc()));

  // And what a scene left on the display ends the record: the next frame that
  // happens to match it must still be announced.
  mySerum.frame64[7] = 0;
  CHECK(FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                        CurrentRotationCrc()));
  ReportedOutputNoLongerOnDisplay();
  CHECK(!FinishedFrameIsAlreadyOnDisplay(true, CurrentOutputCrc(),
                                         CurrentRotationCrc()));
  free(mySerum.rotations32);
  free(mySerum.rotations64);
  mySerum.rotations32 = NULL;
  mySerum.rotations64 = NULL;
  TearDownFrame();
}

// ---------------------------------------------------------------------------
// cRZ archives
// ---------------------------------------------------------------------------

static std::filesystem::path CrzTestDir(void) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "libserum_tests" / "crz";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

// Compressible, but not one repeated byte: a cROM is mostly runs, with detail.
static std::vector<uint8_t> CrzTestPayload(size_t size) {
  std::vector<uint8_t> bytes(size);
  uint32_t state = 12345;
  for (size_t i = 0; i < size; ++i) {
    state = state * 1664525u + 1013904223u;
    bytes[i] = (i / 97) % 3 ? (uint8_t)(i / 97) : (uint8_t)(state >> 24);
  }
  return bytes;
}

static void WriteTestFile(const std::filesystem::path& path,
                          const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

// The editor writes a cRZ next to every cROM and libserum has to load it, so
// what goes in must come out byte for byte, under the name it was given. The
// payload is larger than miniz's 64 KiB I/O buffer, so the streamed path loops.
static void Test_CrzWriteThenReadRoundTrips(void) {
  const std::filesystem::path dir = CrzTestDir();
  const std::string crom = (dir / "source.cROM").string();
  const std::string crz = (dir / "test_rom.cRZ").string();
  const std::vector<uint8_t> payload = CrzTestPayload(300000);
  WriteTestFile(crom, payload);

  CHECK(Serum_CRZ_WriteFromFile(crz.c_str(), "test_rom.cRom", crom.c_str()));
  CHECK_EQ(strlen(Serum_GetLastErrorMessage()), 0);
  CHECK(std::filesystem::file_size(crz) < payload.size());

  uint8_t* data = NULL;
  size_t size = 0;
  CHECK(Serum_CRZ_Read(crz.c_str(), &data, &size));
  CHECK_EQ(size, payload.size());
  CHECK(data && size == payload.size() &&
        memcmp(data, payload.data(), size) == 0);
  Serum_CRZ_Free(data);

  char name[64] = "untouched";
  CHECK(Serum_CRZ_GetEntryName(crz.c_str(), name, sizeof(name)));
  CHECK(strcmp(name, "test_rom.cRom") == 0);

  // The loader's own extraction sees the same bytes.
  std::vector<uint8_t> viaLoader;
  CHECK(ExtractCRZEntryToMemory(crz.c_str(), viaLoader));
  CHECK(viaLoader == payload);

  std::filesystem::remove_all(dir);
}

// A small cROM does not need zip64, and a reader that predates it -- the
// Win32 editor's -- must not be handed one. The sizes sit in the local header
// with no data descriptor, as that editor writes them.
static void Test_CrzIsAPlainDeflatedZip(void) {
  const std::filesystem::path dir = CrzTestDir();
  const std::string crom = (dir / "source.cROM").string();
  const std::string crz = (dir / "plain.cRZ").string();
  WriteTestFile(crom, CrzTestPayload(5000));
  CHECK(Serum_CRZ_WriteFromFile(crz.c_str(), "plain.cRom", crom.c_str()));

  mz_zip_archive zip = {0};
  CHECK(mz_zip_reader_init_file(&zip, crz.c_str(), 0));
  CHECK_EQ(mz_zip_reader_get_num_files(&zip), 1);
  CHECK(!mz_zip_is_zip64(&zip));
  mz_zip_archive_file_stat stat;
  CHECK(mz_zip_reader_file_stat(&zip, 0, &stat));
  CHECK_EQ(stat.m_method, MZ_DEFLATED);
  CHECK_EQ(stat.m_bit_flag & (1 << 3), 0);   // no data descriptor
  CHECK_EQ(stat.m_bit_flag & (1 << 11), 0);  // name not flagged as UTF-8
  CHECK_EQ(stat.m_uncomp_size, 5000);
  mz_zip_reader_end(&zip);

  std::filesystem::remove_all(dir);
}

// A failed read hands back nothing to free and says why; a failed write does
// not leave half an archive where a good one may have been expected.
static void Test_CrzFailuresAreClean(void) {
  const std::filesystem::path dir = CrzTestDir();
  const std::string missing = (dir / "missing.cRZ").string();
  const std::string garbage = (dir / "garbage.cRZ").string();
  WriteTestFile(garbage, CrzTestPayload(4096));

  uint8_t* data = (uint8_t*)&dir;
  size_t size = 99;
  CHECK(!Serum_CRZ_Read(missing.c_str(), &data, &size));
  CHECK(data == NULL);
  CHECK_EQ(size, 0);
  CHECK(strstr(Serum_GetLastErrorMessage(), "Serum_CRZ_Read") != NULL);

  data = (uint8_t*)&dir;
  size = 99;
  CHECK(!Serum_CRZ_Read(garbage.c_str(), &data, &size));
  CHECK(data == NULL);
  CHECK_EQ(size, 0);
  CHECK(!Serum_CRZ_Read(NULL, &data, &size));
  CHECK(!Serum_CRZ_Read(garbage.c_str(), NULL, &size));
  Serum_CRZ_Free(NULL);

  char name[8] = "x";
  CHECK(!Serum_CRZ_GetEntryName(garbage.c_str(), name, sizeof(name)));
  CHECK_EQ(name[0], 0);

  // A missing source must not cost the caller the archive it already has.
  const std::string crz = (dir / "kept.cRZ").string();
  const std::string crom = (dir / "source.cROM").string();
  WriteTestFile(crom, CrzTestPayload(2000));
  CHECK(Serum_CRZ_WriteFromFile(crz.c_str(), "kept.cRom", crom.c_str()));
  const auto keptSize = std::filesystem::file_size(crz);
  CHECK(!Serum_CRZ_WriteFromFile(crz.c_str(), "kept.cRom",
                                 (dir / "no_such.cROM").string().c_str()));
  CHECK(strstr(Serum_GetLastErrorMessage(), "Serum_CRZ_WriteFromFile") != NULL);
  CHECK(std::filesystem::exists(crz) &&
        std::filesystem::file_size(crz) == keptSize);
  CHECK(!Serum_CRZ_WriteFromFile(crom.c_str(), "kept.cRom", crom.c_str()));
  CHECK_EQ(std::filesystem::file_size(crom), 2000);

  // An entry name that reads as a path is refused, and nothing is created.
  const std::string refused = (dir / "refused.cRZ").string();
  for (const char* bad :
       {"/abs.cRom", "dir/x.cRom", "..\\x.cRom", "c:x.cRom", "..", ""}) {
    CHECK(!Serum_CRZ_WriteFromFile(refused.c_str(), bad, crom.c_str()));
    CHECK(!std::filesystem::exists(refused));
  }
  CHECK(!Serum_CRZ_WriteFromFile((dir / "no_dir" / "x.cRZ").string().c_str(),
                                 "x.cRom", crom.c_str()));

  // Too small a name buffer is an error, not a truncated name.
  CHECK(!Serum_CRZ_GetEntryName(crz.c_str(), name, sizeof(name)));
  CHECK_EQ(name[0], 0);

  // A success clears the message of the failure before it.
  CHECK(Serum_CRZ_Read(crz.c_str(), &data, &size));
  CHECK_EQ(strlen(Serum_GetLastErrorMessage()), 0);
  Serum_CRZ_Free(data);

  std::filesystem::remove_all(dir);
}

// Optional: SERUM_TEST_CRZ names a real cRZ, such as one written by the Win32
// editor. No colorization is kept in the repository, so without it this checks
// nothing. With it, the archive must read and survive being written again.
static void Test_CrzRealArchive(void) {
  const char* real = std::getenv("SERUM_TEST_CRZ");
  if (!real || !real[0]) return;

  uint8_t* data = NULL;
  size_t size = 0;
  CHECK(Serum_CRZ_Read(real, &data, &size));
  CHECK(size > 0);
  char name[512] = "";
  CHECK(Serum_CRZ_GetEntryName(real, name, sizeof(name)));
  if (!data) return;

  const std::filesystem::path dir = CrzTestDir();
  const std::string crom = (dir / "real.cROM").string();
  const std::string crz = (dir / "real.cRZ").string();
  {
    std::ofstream out(crom, std::ios::binary | std::ios::trunc);
    out.write((const char*)data, (std::streamsize)size);
  }
  CHECK(Serum_CRZ_WriteFromFile(crz.c_str(), name, crom.c_str()));
  uint8_t* again = NULL;
  size_t againSize = 0;
  CHECK(Serum_CRZ_Read(crz.c_str(), &again, &againSize));
  CHECK_EQ(againSize, size);
  CHECK(again && againSize == size && memcmp(again, data, size) == 0);
  printf("       %s: entry %s, %zu bytes, rewritten as %llu bytes\n", real,
         name, size, (unsigned long long)std::filesystem::file_size(crz));
  Serum_CRZ_Free(again);
  Serum_CRZ_Free(data);
  std::filesystem::remove_all(dir);
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
  SetUpFrame(kW, 32);
  mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
  mySerum.rotations32 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations64 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations32[0] = 4;   // four colours
  mySerum.rotations32[1] = 10;  // every 10 ms
  for (int i = 0; i < 4; ++i)
    mySerum.rotations32[2 + i] = (uint16_t)(0x100 + i);

  const size_t px = (size_t)kW * kH;
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
  SetUpFrame(kW, 32);
  mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
  mySerum.rotations32 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations64 = (uint16_t*)calloc(
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION, sizeof(uint16_t));
  mySerum.rotations32[0] = 3;
  mySerum.rotations32[1] = 1;
  for (int i = 0; i < 3; ++i)
    mySerum.rotations32[2 + i] = (uint16_t)(0x200 + i);
  const size_t px = (size_t)kW * kH;
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
// Rotations and dynamic content
//
// A rotating background (stars) under a dynamic zone whose glyph has a colour
// that is one of the rotation's colours, and a dynamic shadow above and below
// the glyph. Only the background moves: dynamic content is coloured from its
// set and a shadow is a flat colour, whatever colours the frame's rotations
// name. afm_113b frames 16 to 77 are this picture: white high-score digits
// over a sky whose stars rotate through white.
// ---------------------------------------------------------------------------

static const uint16_t kStarA = 0x0222, kStarB = 0x0333;  // the rotation
static const uint16_t kGlyphShadow = 0x0444;
static const uint32_t kGlyphX = 5, kGlyphY = 6;
static const uint32_t kStarX = 20, kStarY = 20;  // background, far from it

enum RotationRequest { kRequest32, kRequest64, kRequestBoth };

static void RenderGlyphOverRotatingBackground(RotationRequest request) {
  SetUpColorization();
  const size_t px = (size_t)kW * kCH;
  const std::vector<uint16_t> bg(px, kStarA);
  g_serumData.backgroundframes_v2.set(0, bg.data(), px);
  const std::vector<uint8_t> mask(px, 1);
  g_serumData.backgroundmask.set(kFrameId, mask.data(), px);
  const uint16_t background[1] = {0};
  g_serumData.backgroundIDs.set(kFrameId, background, 1);

  // A 3x3 zone, lit in the middle only; the glyph takes the rotation's
  // second colour from its set.
  std::vector<uint8_t> active(px, 0);
  for (uint32_t y = kGlyphY - 1; y <= kGlyphY + 1; ++y)
    for (uint32_t x = kGlyphX - 1; x <= kGlyphX + 1; ++x)
      active[(size_t)y * kW + x] = 1;
  std::vector<uint16_t> shadeToColour(16, 0);
  shadeToColour[5] = kStarB;
  GiveFrameADynamicZone(active, /*couche=*/0, shadeToColour);
  std::vector<uint8_t> dir(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  std::vector<uint16_t> col(MAX_DYNA_SETS_PER_FRAME_V2, 0);
  // Above, a pixel the raster has rendered already, and below, one it has
  // not reached yet.
  dir[0] = (1u << 1) | (1u << 5);
  col[0] = kGlyphShadow;
  g_serumData.dynashadowsdir.set(kFrameId, dir.data(), dir.size());
  g_serumData.dynashadowscol.set(kFrameId, col.data(), col.size());

  const size_t tableSize = MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION;
  std::vector<uint16_t> table(tableSize, 0);
  table[0] = 2;   // two colours
  table[1] = 10;  // every 10 ms
  table[2] = kStarA;
  table[3] = kStarB;
  g_serumData.colorrotations_v2.reserve(tableSize);
  g_serumData.colorrotations_v2_extra.reserve(tableSize);
  g_serumData.colorrotations_v2.set(kFrameId, table.data(), tableSize);
  g_serumData.BuildColorRotationLookup();
  mySerum.rotations32 = (uint16_t*)calloc(tableSize, sizeof(uint16_t));
  mySerum.rotations64 = (uint16_t*)calloc(tableSize, sizeof(uint16_t));
  memcpy(mySerum.rotations32, table.data(), tableSize * sizeof(uint16_t));
  memcpy(mySerum.rotations64, table.data(), tableSize * sizeof(uint16_t));

  upscaleExtraFromOriginal = request != kRequest32;
  originalPlaneRequestedByCaller = request != kRequest64;
  // What an earlier frame left behind, or the allocation on the first frame:
  // an entry nothing writes would name slot 0.
  memset(mySerum.rotationsinframe32, 0, px * 2 * sizeof(uint16_t));
  memset(mySerum.rotationsinframe64, 0, px * 8 * sizeof(uint16_t));
  memset(colorshifts32, 0, sizeof(colorshifts32));
  memset(colorshifts64, 0, sizeof(colorshifts64));

  g_rom.assign(px, 0);
  g_rom[(size_t)kGlyphY * kW + kGlyphX] = 5;
  romFrameForUpscale = g_rom.data();
  Colorize_Framev2(g_rom.data(), kFrameId);
}

// One rotation step on both planes, however long ago the last one was.
static void StepGlyphRotation(void) {
  colorshiftinittime32[0] = 0;
  colorshiftinittime64[0] = 0;
  Serum_ApplyRotationsv2();
}

static void Test_DynamicContentAndItsShadowDoNotRotate32(void) {
  RenderGlyphOverRotatingBackground(kRequest32);
  CHECK(mySerum.flags & FLAG_RETURNED_32P_FRAME_OK);
  const size_t glyph = (size_t)kGlyphY * kW + kGlyphX;
  const size_t above = glyph - kW, below = glyph + kW;
  const size_t star = (size_t)kStarY * kW + kStarX;
  CHECK_EQ(mySerum.frame32[glyph], kStarB);
  CHECK_EQ(mySerum.frame32[above], kGlyphShadow);
  CHECK_EQ(mySerum.frame32[below], kGlyphShadow);
  CHECK_EQ(mySerum.frame32[star], kStarA);
  CHECK_EQ(mySerum.rotationsinframe32[glyph * 2], 0xffff);
  CHECK_EQ(mySerum.rotationsinframe32[above * 2], 0xffff);
  CHECK_EQ(mySerum.rotationsinframe32[above * 2 + 1], 0xffff);
  CHECK_EQ(mySerum.rotationsinframe32[below * 2], 0xffff);
  CHECK_EQ(mySerum.rotationsinframe32[below * 2 + 1], 0xffff);
  CHECK_EQ(mySerum.rotationsinframe32[star * 2], 0);
  // Every pixel is either the background, in the rotation, or not in one.
  const size_t px = (size_t)kW * kCH;
  unsigned rotating = 0;
  for (size_t i = 0; i < px; ++i)
    if (mySerum.rotationsinframe32[i * 2] != 0xffff) ++rotating;
  CHECK_EQ(rotating, px - 3);

  for (int step = 1; step <= 4; ++step) {
    StepGlyphRotation();
    CHECK_EQ(mySerum.frame32[glyph], kStarB);
    CHECK_EQ(mySerum.frame32[above], kGlyphShadow);
    CHECK_EQ(mySerum.frame32[below], kGlyphShadow);
    CHECK_EQ(mySerum.frame32[star], (step & 1) ? kStarB : kStarA);
  }
  TearDownFrame();
}

// The upscaled plane, for a caller that asked for it alone and for one that
// asked for both: the glyph's 2x2 block and the shadows generated around it
// stand still while the background behind them rotates.
static void CheckUpscaledGlyphDoesNotRotate(RotationRequest request) {
  RenderGlyphOverRotatingBackground(request);
  CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
  const uint32_t w = kW * 2;
  const size_t px = (size_t)w * kCH * 2;
  const std::vector<uint16_t> first(mySerum.frame64, mySerum.frame64 + px);
  unsigned glyph = 0, shadow = 0;
  for (size_t i = 0; i < px; ++i) {
    if (first[i] == kStarA) {
      CHECK_EQ(mySerum.rotationsinframe64[i * 2], 0);
      continue;
    }
    // Not the background: the glyph, a shadow, or a frame corner the
    // upscale rounded away.
    if (first[i] == kStarB) ++glyph;
    if (first[i] == kGlyphShadow) ++shadow;
    CHECK_EQ(mySerum.rotationsinframe64[i * 2], 0xffff);
  }
  CHECK_EQ(glyph, 4);
  CHECK(shadow > 0);
  CHECK_EQ(first[(size_t)(kGlyphY * 2) * w + kGlyphX * 2], kStarB);

  for (int step = 1; step <= 4; ++step) {
    StepGlyphRotation();
    for (size_t i = 0; i < px; ++i) {
      if (first[i] == kStarA)
        CHECK_EQ(mySerum.frame64[i], (step & 1) ? kStarB : kStarA);
      else
        CHECK_EQ(mySerum.frame64[i], first[i]);
    }
    if (request == kRequestBoth) {
      const size_t at = (size_t)kGlyphY * kW + kGlyphX;
      CHECK_EQ(mySerum.frame32[at], kStarB);
      CHECK_EQ(mySerum.frame32[at - kW], kGlyphShadow);
      CHECK_EQ(mySerum.frame32[at + kW], kGlyphShadow);
      CHECK_EQ(mySerum.frame32[(size_t)kStarY * kW + kStarX],
               (step & 1) ? kStarB : kStarA);
    }
  }
  TearDownFrame();
}

static void Test_DynamicContentAndItsShadowDoNotRotate64(void) {
  CheckUpscaledGlyphDoesNotRotate(kRequest64);
}

static void Test_DynamicContentAndItsShadowDoNotRotateBoth(void) {
  CheckUpscaledGlyphDoesNotRotate(kRequestBoth);
}

// ---------------------------------------------------------------------------
// is256x64 projects
//
// A cROM whose ROM frame is 256x64 while its header describes a 128x32
// original plane and a 256x64 extra plane. libserum renders the 256x64 plane
// only; what the file stores for the 128x32 one is loaded and never used, and
// the synthetic project gives it colours of its own (0x08xx, 0x2xxx, 0xDxxx,
// 0xE0xx, 0x01xx, 0xF000) so that any of it reaching the output shows.
// These go through Serum_Load() on a cROM written to a
// temporary directory, so they cover the stream loader, the concentrate round
// trip and the public colorize call rather than a hand-built g_serumData.
// ---------------------------------------------------------------------------

namespace n256 {

static const uint32_t kNW = 256, kNH = 64, kNPx = kNW * kNH;  // ROM frame
static const uint32_t kOW = 128, kOH = 32, kOPx = kOW * kOH;  // original plane
static const uint32_t kSprPx = MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT;
static const uint32_t kColors = 4;
static const char* const kRom = "n256tst";
static const uint8_t kT = 255;  // transparent sprite pixel

enum : uint32_t {
  kStatic = 0,
  kDynamic,
  kBackground,
  kMasked,
  kSprite,
  kShapeSprite,
  kRotation,
  kMonoTrigger,
  kBlackTop,
  kScene,
  kFrameCount
};

// Where the two sprites sit in their frames, in ROM-frame coordinates. Odd,
// past column 128 or past row 32: none of it exists in a 128x32 frame.
static const uint32_t kSprX = 61, kSprY = 37, kSprW = 6, kSprH = 4;
static const uint32_t kShapeX = 200, kShapeY = 50, kShapeW = 5, kShapeH = 3;
static const uint8_t kSprite0[kSprH][kSprW] = {{1, 1, 2, 3, 3, 1},
                                               {2, 1, 2, 3, 1, 3},
                                               {3, 3, kT, 1, 2, 2},
                                               {1, 2, 1, 1, 3, kT}};
static const uint8_t kSprite1[kShapeH][kShapeW] = {
    {1, 0, 1, 1, 0}, {1, 1, 0, 1, 1}, {0, 1, 1, 1, 0}};

// A shade field with no period of 2 or 128, so reading it with the wrong
// stride or at the wrong scale cannot land on the right values by accident.
static uint8_t Shade(uint32_t f, uint32_t x, uint32_t y) {
  return (uint8_t)((x * 5u + y * 3u + ((x * y) % 7u) +
                    ((x / 8u + y / 4u) % (2u * f + 3u))) %
                   kColors);
}
// Every pixel of every plane of every frame gets its own colour.
static uint16_t C32(uint32_t f, uint32_t i) {
  return (uint16_t)(0x0800 + ((i + f * 977u) % kOPx));
}
static uint16_t C64(uint32_t f, uint32_t i) {
  return (uint16_t)(0x8000 + ((i + f * 977u) % kNPx));
}
static uint16_t Bg32(uint32_t i) { return (uint16_t)(0x2000 + i); }
static uint16_t Bg64(uint32_t i) { return (uint16_t)(0x4000 + i); }

struct Synth {
  uint32_t nframes = kFrameCount, nsprites = 2, ncompmasks = 3;
  uint16_t nbackgrounds = 1;
  bool is256x64 = true;
  uint32_t headerWidth = kOW;  // what the header claims the original plane is
  // The cROM format: 0 is header revision 20 as the editor writes it for
  // libserum 2.8 (LEGACY_SPRITES_PER_FRAME slots of 8-bit sprite IDs); from 1
  // on, header revision 21 with the format version and the slots per frame
  // (16-bit sprite IDs). SetFormat() changes both.
  uint32_t formatVersion = 0;
  uint32_t slotsPerFrame = LEGACY_SPRITES_PER_FRAME;
  std::vector<uint8_t> native;  // the ROM frames; hashed, not stored
  std::vector<uint8_t> shapemode, compmaskid, compmasks, isextraframe;
  std::vector<uint16_t> cframes, cframesx;
  std::vector<uint8_t> dynamasks, dynamasksx;
  std::vector<uint16_t> dyna4cols, dyna4colsx;
  std::vector<uint8_t> isextrasprite, spriteoriginal;
  std::vector<uint16_t> framesprites;  // SERUM_NO_SPRITE for an empty slot
  std::vector<uint16_t> spritecolored;
  std::vector<uint8_t> spritemaskx;
  std::vector<uint16_t> spritecoloredx;
  std::vector<uint8_t> activeframes;
  std::vector<uint16_t> colorrot, colorrotx;
  std::vector<uint32_t> detdwords;
  std::vector<uint16_t> detdwordpos, detareas;
  std::vector<uint32_t> triggers;
  std::vector<uint16_t> spritebb;
  std::vector<uint8_t> isextrabg;
  std::vector<uint16_t> bgframes, bgframesx, bgids;
  std::vector<uint8_t> bgmask, bgmaskx;
  std::vector<uint8_t> shdir, shdirx;
  std::vector<uint16_t> shcol, shcolx;
  std::vector<uint16_t> dynspr4cols, dynspr4colsx;
  std::vector<uint8_t> dynsprmask, dynsprmaskx;
  std::vector<uint8_t> sprshapemode;

  uint8_t* Native(uint32_t f) { return &native[(size_t)f * kNPx]; }
  const uint8_t* Native(uint32_t f) const { return &native[(size_t)f * kNPx]; }
  std::vector<uint8_t> Frame(uint32_t f) const {
    return std::vector<uint8_t>(Native(f), Native(f) + kNPx);
  }

  Synth() {
    native.assign((size_t)nframes * kNPx, 0);
    shapemode.assign(nframes, 0);
    compmaskid.assign(nframes, 255);
    compmasks.assign((size_t)ncompmasks * kNPx, 0);
    isextraframe.assign(nframes, 1);
    cframes.resize((size_t)nframes * kOPx);
    cframesx.resize((size_t)nframes * kNPx);
    dynamasks.assign((size_t)nframes * kOPx, 255);
    dynamasksx.assign((size_t)nframes * kNPx, 255);
    dyna4cols.assign((size_t)nframes * MAX_DYNA_SETS_PER_FRAME_V2 * kColors, 0);
    dyna4colsx = dyna4cols;
    isextrasprite.assign(nsprites, 1);
    framesprites.assign((size_t)nframes * slotsPerFrame, SERUM_NO_SPRITE);
    spriteoriginal.assign((size_t)nsprites * kSprPx, kT);
    spritecolored.assign((size_t)nsprites * kSprPx, 0);
    spritemaskx.assign((size_t)nsprites * kSprPx, kT);
    spritecoloredx.assign((size_t)nsprites * kSprPx, 0);
    activeframes.assign(nframes, 1);
    colorrot.assign(
        (size_t)nframes * MAX_LENGTH_COLOR_ROTATION * MAX_COLOR_ROTATION_V2, 0);
    colorrotx = colorrot;
    detdwords.assign((size_t)nsprites * MAX_SPRITE_DETECT_AREAS, 0);
    detdwordpos.assign((size_t)nsprites * MAX_SPRITE_DETECT_AREAS, 0);
    detareas.assign((size_t)nsprites * 4 * MAX_SPRITE_DETECT_AREAS, 0xffff);
    triggers.assign(nframes, 0xffffffffu);
    spritebb.assign((size_t)nframes * slotsPerFrame * 4, 0);
    isextrabg.assign(nbackgrounds, 1);
    bgframes.resize((size_t)nbackgrounds * kOPx);
    bgframesx.resize((size_t)nbackgrounds * kNPx);
    bgids.assign(nframes, 0xffff);
    bgmask.assign((size_t)nframes * kOPx, 0);
    bgmaskx.assign((size_t)nframes * kNPx, 0);
    shdir.assign((size_t)nframes * MAX_DYNA_SETS_PER_FRAME_V2, 0);
    shdirx = shdir;
    shcol.assign((size_t)nframes * MAX_DYNA_SETS_PER_FRAME_V2, 0);
    shcolx = shcol;
    dynspr4cols.assign((size_t)nsprites * MAX_DYNA_SETS_PER_SPRITE * kColors,
                       0);
    dynspr4colsx = dynspr4cols;
    dynsprmask.assign((size_t)nsprites * kSprPx, 255);
    dynsprmaskx = dynsprmask;
    sprshapemode.assign(nsprites, 0);

    for (uint32_t f = 0; f < nframes; ++f) {
      for (uint32_t i = 0; i < kOPx; ++i)
        cframes[(size_t)f * kOPx + i] = C32(f, i);
      for (uint32_t i = 0; i < kNPx; ++i)
        cframesx[(size_t)f * kNPx + i] = C64(f, i);
      for (uint32_t y = 0; y < kNH; ++y)
        for (uint32_t x = 0; x < kNW; ++x)
          Native(f)[y * kNW + x] = Shade(f, x, y);
    }
    for (uint32_t i = 0; i < kOPx; ++i) bgframes[i] = Bg32(i);
    for (uint32_t i = 0; i < kNPx; ++i) bgframesx[i] = Bg64(i);

    // kDynamic: one zone over the whole frame on both planes, set 0.
    memset(&dynamasks[(size_t)kDynamic * kOPx], 0, kOPx);
    memset(&dynamasksx[(size_t)kDynamic * kNPx], 0, kNPx);
    for (uint32_t s = 0; s < kColors; ++s) {
      dyna4cols[(size_t)kDynamic * MAX_DYNA_SETS_PER_FRAME_V2 * kColors + s] =
          (uint16_t)(0x0100 + s);
      dyna4colsx[(size_t)kDynamic * MAX_DYNA_SETS_PER_FRAME_V2 * kColors + s] =
          (uint16_t)(0x0200 + s);
    }

    // kBackground: mostly unlit, background 0 masked in everywhere.
    for (uint32_t y = 0; y < kNH; ++y)
      for (uint32_t x = 0; x < kNW; ++x)
        Native(kBackground)[y * kNW + x] = ((x + 2 * y) % 5 == 0) ? 2 : 0;
    bgids[kBackground] = 0;
    memset(&bgmask[(size_t)kBackground * kOPx], 1, kOPx);
    memset(&bgmaskx[(size_t)kBackground * kNPx], 1, kNPx);

    // kMasked: comparison mask 0 hides the lower right corner.
    compmaskid[kMasked] = 0;
    FillMask(0, 100, 40, 255, 63);

    // kSprite: a dark frame, sprite 0 somewhere inside mask 1 / its bounding
    // box, one lit pixel far outside to tell the frame apart.
    memset(Native(kSprite), 0, kNPx);
    Native(kSprite)[60 * kNW + 250] = 2;
    compmaskid[kSprite] = 1;
    FillMask(1, 32, 20, 95, 43);
    framesprites[(size_t)kSprite * slotsPerFrame] = 0;
    SetBB(kSprite, 32, 20, 95, 43);
    for (uint32_t y = 0; y < kSprH; ++y)
      for (uint32_t x = 0; x < kSprW; ++x) {
        const uint32_t i = y * MAX_SPRITE_WIDTH + x;
        spriteoriginal[i] = kSprite0[y][x];
        spritemaskx[i] = kSprite0[y][x];
        spritecoloredx[i] = (uint16_t)(0xC000 + y * 16 + x);
        // The original plane's sprite picture, as the editor stores it: at
        // that plane's own scale, half the sprite, in the top left of the
        // array. The runtime never reads it.
        if (x < kSprW / 2 && y < kSprH / 2)
          spritecolored[i] = (uint16_t)(0xD000 + y * 16 + x);
      }
    detareas[0] = 0, detareas[1] = 0, detareas[2] = kSprW, detareas[3] = kSprH;
    detdwords[0] = 1u | (2u << 8) | (3u << 16) | (1u << 24);  // row 1, x 1..4
    detdwordpos[0] = 1 * MAX_SPRITE_WIDTH + 1;
    dynsprmask[0] = 0;   // original plane: sprite pixel (0,0) is dynamic
    dynsprmaskx[1] = 0;  // extra plane: sprite pixel (1,0) is dynamic
    for (uint32_t s = 0; s < kColors; ++s) {
      dynspr4cols[s] = (uint16_t)(0xE000 + s);
      dynspr4colsx[s] = (uint16_t)(0xE100 + s);
    }
    PlaceSprite0(Native(kSprite), kSprX, kSprY);

    // kShapeSprite: sprite 1 is matched by shape, in the lower right.
    memset(Native(kShapeSprite), 0, kNPx);
    Native(kShapeSprite)[61 * kNW + 3] = 1;
    compmaskid[kShapeSprite] = 2;
    FillMask(2, 180, 44, 239, 60);
    framesprites[(size_t)kShapeSprite * slotsPerFrame] = 1;
    SetBB(kShapeSprite, 180, 44, 239, 60);
    sprshapemode[1] = 1;
    for (uint32_t y = 0; y < kShapeH; ++y)
      for (uint32_t x = 0; x < kShapeW; ++x) {
        const uint32_t i = kSprPx + y * MAX_SPRITE_WIDTH + x;
        spriteoriginal[i] = kSprite1[y][x] ? 3 : 0;
        spritemaskx[i] = spriteoriginal[i];
        spritecoloredx[i] = (uint16_t)(0xC800 + y * 16 + x);
        if (x < kShapeW / 2 && y < kShapeH / 2)
          spritecolored[i] = (uint16_t)(0xD800 + y * 16 + x);
      }
    detareas[4 * MAX_SPRITE_DETECT_AREAS + 0] = 0;
    detareas[4 * MAX_SPRITE_DETECT_AREAS + 1] = 0;
    detareas[4 * MAX_SPRITE_DETECT_AREAS + 2] = kShapeW;
    detareas[4 * MAX_SPRITE_DETECT_AREAS + 3] = kShapeH;
    detdwords[MAX_SPRITE_DETECT_AREAS] = 3u | (3u << 8) | (3u << 24);
    detdwordpos[MAX_SPRITE_DETECT_AREAS] = 1 * MAX_SPRITE_WIDTH + 0;
    for (uint32_t y = 0; y < kShapeH; ++y)
      for (uint32_t x = 0; x < kShapeW; ++x)
        Native(kShapeSprite)[(kShapeY + y) * kNW + kShapeX + x] =
            kSprite1[y][x] ? (uint8_t)(1 + (x + y) % 3) : 0;

    // kRotation: one colour per plane, each in a two-colour rotation.
    for (uint32_t i = 0; i < kOPx; ++i)
      cframes[(size_t)kRotation * kOPx + i] = 0xF000;
    for (uint32_t i = 0; i < kNPx; ++i)
      cframesx[(size_t)kRotation * kNPx + i] = 0xF100;
    uint16_t* rot = &colorrot[(size_t)kRotation * MAX_LENGTH_COLOR_ROTATION *
                              MAX_COLOR_ROTATION_V2];
    rot[0] = 2, rot[1] = 100, rot[2] = 0xF000, rot[3] = 0xF001;
    rot = &colorrotx[(size_t)kRotation * MAX_LENGTH_COLOR_ROTATION *
                     MAX_COLOR_ROTATION_V2];
    rot[0] = 2, rot[1] = 100, rot[2] = 0xF100, rot[3] = 0xF101;

    triggers[kMonoTrigger] = MONOCHROME_TRIGGER_ID;

    // kBlackTop: nothing in the first 4096 bytes, which is all a 128x32
    // reading of the frame would look at.
    memset(Native(kBlackTop), 0, kNPx);
    for (uint32_t y = 40; y < 50; ++y)
      for (uint32_t x = 10; x < 20; ++x) Native(kBlackTop)[y * kNW + x] = 3;
  }

  void FillMask(uint32_t mask, uint32_t x0, uint32_t y0, uint32_t x1,
                uint32_t y1) {
    for (uint32_t y = y0; y <= y1; ++y)
      for (uint32_t x = x0; x <= x1; ++x)
        compmasks[(size_t)mask * kNPx + y * kNW + x] = 1;
  }
  void SetBB(uint32_t f, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
             uint32_t slot = 0) {
    uint16_t* bb = &spritebb[((size_t)f * slotsPerFrame + slot) * 4];
    bb[0] = x0, bb[1] = y0, bb[2] = x1, bb[3] = y1;
  }
  // Sprite `from` again as sprites nsprites..to: more sprites, `to` the last.
  void CopySprite(uint32_t from, uint32_t to) {
    const uint32_t count = to + 1;
    const auto grow = [&](auto& v) {
      const size_t per = v.size() / nsprites;
      const size_t start = (size_t)from * per;
      for (uint32_t id = nsprites; id < count; ++id)
        v.insert(v.end(), v.begin() + start, v.begin() + start + per);
    };
    grow(isextrasprite);
    grow(spriteoriginal);
    grow(spritecolored);
    grow(spritemaskx);
    grow(spritecoloredx);
    grow(detdwords);
    grow(detdwordpos);
    grow(detareas);
    grow(dynspr4cols);
    grow(dynspr4colsx);
    grow(dynsprmask);
    grow(dynsprmaskx);
    grow(sprshapemode);
    nsprites = count;
  }
  // Another format and slot count, the slots a frame has kept.
  void SetFormat(uint32_t version, uint32_t slots) {
    std::vector<uint16_t> sprites((size_t)nframes * slots, SERUM_NO_SPRITE);
    std::vector<uint16_t> boxes((size_t)nframes * slots * 4, 0);
    const uint32_t kept = std::min(slots, slotsPerFrame);
    for (uint32_t f = 0; f < nframes; ++f)
      for (uint32_t i = 0; i < kept; ++i) {
        sprites[(size_t)f * slots + i] =
            framesprites[(size_t)f * slotsPerFrame + i];
        for (uint32_t k = 0; k < 4; ++k)
          boxes[((size_t)f * slots + i) * 4 + k] =
              spritebb[((size_t)f * slotsPerFrame + i) * 4 + k];
      }
    framesprites.swap(sprites);
    spritebb.swap(boxes);
    formatVersion = version;
    slotsPerFrame = slots;
  }
  static void PlaceSprite0(uint8_t* frame, uint32_t x0, uint32_t y0) {
    for (uint32_t y = 0; y < kSprH; ++y)
      for (uint32_t x = 0; x < kSprW; ++x)
        // A transparent sprite pixel matches anything; make it something.
        frame[(y0 + y) * kNW + x0 + x] =
            kSprite0[y][x] == kT ? 3 : kSprite0[y][x];
  }

  template <typename T>
  static void Put(std::vector<uint8_t>& out, const std::vector<T>& v) {
    const uint8_t* p = (const uint8_t*)v.data();
    out.insert(out.end(), p, p + v.size() * sizeof(T));
  }
  template <typename T>
  static void PutValue(std::vector<uint8_t>& out, T v) {
    const uint8_t* p = (const uint8_t*)&v;
    out.insert(out.end(), p, p + sizeof(T));
  }

  // The cROM exactly as the editor writes it: header revision 20 for format
  // version 0, revision 21 from format version 1 on.
  std::vector<uint8_t> Serialize() const {
    std::vector<uint32_t> hashes(nframes);
    const uint32_t hashPx = is256x64 ? kNPx : kOPx;
    for (uint32_t f = 0; f < nframes; ++f) {
      uint8_t* fr = const_cast<uint8_t*>(Native(f));
      uint8_t* mask =
          compmaskid[f] < 255
              ? const_cast<uint8_t*>(&compmasks[(size_t)compmaskid[f] * hashPx])
              : NULL;
      hashes[f] =
          mask ? crc32_fast_mask(fr, mask, hashPx) : crc32_fast(fr, hashPx);
    }
    std::vector<uint8_t> out;
    char name[64] = {0};
    snprintf(name, sizeof(name), "%s", kRom);
    out.insert(out.end(), name, name + 64);
    PutValue<uint32_t>(out, (formatVersion >= 1 ? 21 : 20) * sizeof(uint32_t));
    PutValue<uint32_t>(out, headerWidth);
    PutValue<uint32_t>(out, kOH);
    PutValue<uint32_t>(out, kNW);
    PutValue<uint32_t>(out, kNH);
    PutValue<uint32_t>(out, nframes);
    PutValue<uint32_t>(out, kColors);
    PutValue<uint32_t>(out, ncompmasks);
    PutValue<uint32_t>(out, nsprites);
    PutValue<uint16_t>(out, nbackgrounds);
    PutValue<int32_t>(out, is256x64 ? 1 : 0);
    if (formatVersion >= 1) {
      PutValue<uint32_t>(out, formatVersion);
      PutValue<uint32_t>(out, slotsPerFrame);
    }
    Put(out, hashes);
    Put(out, shapemode);
    Put(out, compmaskid);
    Put(out, compmasks);
    Put(out, isextraframe);
    Put(out, cframes);
    Put(out, cframesx);
    Put(out, dynamasks);
    Put(out, dynamasksx);
    Put(out, dyna4cols);
    Put(out, dyna4colsx);
    Put(out, isextrasprite);
    if (formatVersion >= 1) {
      Put(out, framesprites);
    } else {
      std::vector<uint8_t> narrow(framesprites.size());
      for (size_t i = 0; i < narrow.size(); ++i)
        narrow[i] = framesprites[i] == SERUM_NO_SPRITE
                        ? 255
                        : static_cast<uint8_t>(framesprites[i]);
      Put(out, narrow);
    }
    Put(out, spriteoriginal);
    Put(out, spritecolored);
    Put(out, spritemaskx);
    Put(out, spritecoloredx);
    Put(out, activeframes);
    Put(out, colorrot);
    Put(out, colorrotx);
    Put(out, detdwords);
    Put(out, detdwordpos);
    Put(out, detareas);
    Put(out, triggers);
    Put(out, spritebb);
    Put(out, isextrabg);
    Put(out, bgframes);
    Put(out, bgframesx);
    Put(out, bgids);
    Put(out, bgmask);
    Put(out, bgmaskx);
    Put(out, shdir);
    Put(out, shcol);
    Put(out, shdirx);
    Put(out, shcolx);
    Put(out, dynspr4cols);
    Put(out, dynspr4colsx);
    Put(out, dynsprmask);
    Put(out, dynsprmaskx);
    Put(out, sprshapemode);
    return out;
  }
};

static std::filesystem::path Dir(void) {
  return std::filesystem::temp_directory_path() / "libserum_tests" / "n256";
}

static Serum_Frame_Struc* LoadUnchecked(const Synth& s, uint8_t flags,
                                        bool viaConcentrate, const char* csv) {
  const std::filesystem::path dir = Dir();
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / kRom);
  WriteTestFile(dir / kRom / (std::string(kRom) + ".cROM"), s.Serialize());
  if (csv) {
    std::ofstream out(dir / kRom / (std::string(kRom) + ".pup.csv"));
    out << csv;
  }
  Serum_SetGenerateCRomC(viaConcentrate);
  Serum_SetMaximumUnknownFramesToSkip(0);
  Serum_SetIgnoreUnknownFramesTimeout(0);
  Serum_Frame_Struc* loaded = Serum_Load(dir.string().c_str(), kRom, flags);
  Serum_SetGenerateCRomC(true);
  return loaded;
}

// Load the project through the public call. With `viaConcentrate` the raw
// cROM is converted and the generated cROMc is what ends up loaded, which is
// the path a player takes from its second start on.
static bool Load(const Synth& s, uint8_t flags, bool viaConcentrate,
                 const char* csv = NULL) {
  const std::filesystem::path dir = Dir();
  Serum_Frame_Struc* loaded = LoadUnchecked(s, flags, viaConcentrate, csv);
  CHECK(loaded != NULL);
  CHECK_EQ(std::filesystem::exists(dir / kRom / (std::string(kRom) + ".cROMc")),
           viaConcentrate);
  // The trigger last reported outlives a colorization and is not repeated
  // within its timeout; every load here starts as if none had been.
  lastTriggerID = 0xffffffff;
  return loaded != NULL;
}

// Load the cROMc a previous Load(..., true) left behind, directly and with
// exactly these flags: the path a player takes from its second start on, where
// nothing widens the request the way a conversion from the cROM does.
static bool Reload(uint8_t flags) {
  Serum_Dispose();
  Serum_SetMaximumUnknownFramesToSkip(0);
  Serum_SetIgnoreUnknownFramesTimeout(0);
  const std::filesystem::path dir = Dir();
  std::filesystem::remove(dir / kRom / (std::string(kRom) + ".cROM"));
  Serum_Frame_Struc* loaded = Serum_Load(dir.string().c_str(), kRom, flags);
  CHECK(loaded != NULL);
  lastTriggerID = 0xffffffff;
  return loaded != NULL;
}

static void Unload(void) {
  Serum_Dispose();
  Serum_SetMaximumUnknownFramesToSkip(0);
  std::error_code ec;
  std::filesystem::remove_all(Dir(), ec);
}

// The caller's buffer is exactly one ROM frame, on the heap, so a read past
// it is something the sanitizer build reports.
static uint32_t Colorize(const std::vector<uint8_t>& native) {
  std::vector<uint8_t> frame(native.begin(), native.begin() + kNPx);
  frame.shrink_to_fit();
  return Serum_Colorize(frame.data());
}

template <typename Expected>
static void ExpectPlaneAt(const char* file, int line, const uint16_t* plane,
                          uint32_t w, uint32_t h, Expected expected) {
  ++g_checks;
  if (!plane) {
    Fail(file, line, "plane is missing");
    return;
  }
  uint32_t bad = 0, bx = 0, by = 0;
  uint16_t got = 0, want = 0;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const uint16_t e = expected(x, y);
      if (plane[y * w + x] == e) continue;
      if (!bad++) bx = x, by = y, got = plane[y * w + x], want = e;
    }
  if (bad)
    Fail(file, line,
         "%u of %u pixels differ; first at (%u,%u): got 0x%04x, want 0x%04x",
         bad, w * h, bx, by, got, want);
}
// Variadic because a lambda's capture list has commas in it.
#define EXPECT_PLANE32(...)                                           \
  n256::ExpectPlaneAt(__FILE__, __LINE__, mySerum.frame32, n256::kOW, \
                      n256::kOH, __VA_ARGS__)
#define EXPECT_PLANE64(...)                                           \
  n256::ExpectPlaneAt(__FILE__, __LINE__, mySerum.frame64, n256::kNW, \
                      n256::kNH, __VA_ARGS__)

static const uint8_t kBothPlanes =
    FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_64P_FRAMES;

// The call produced the 256x64 plane and nothing else.
#define EXPECT_ONLY_PLANE64()                             \
  do {                                                    \
    CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);    \
    CHECK(!(mySerum.flags & FLAG_RETURNED_32P_FRAME_OK)); \
    CHECK_EQ(mySerum.width64, 256);                       \
    CHECK_EQ(mySerum.width32, 0);                         \
  } while (0)
// The call produced no plane at all.
#define EXPECT_NO_PLANE()                                                \
  do {                                                                   \
    CHECK(!(mySerum.flags &                                              \
            (FLAG_RETURNED_32P_FRAME_OK | FLAG_RETURNED_64P_FRAME_OK))); \
    CHECK_EQ(mySerum.width64, 0);                                        \
    CHECK_EQ(mySerum.width32, 0);                                        \
  } while (0)

}  // namespace n256

// The flag survives both load paths and leaves one plane: 256x64. There is no
// 128x32 plane, not even an allocated one. It must not outlive the
// colorization it came with -- a cROM with an older header, or a v1 one, never
// mentions it.
static void Test_N256LoadsAndForgets(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    CHECK(g_serumData.is256x64);
    CHECK_EQ(NativeFrameWidth(), 256);
    CHECK_EQ(NativeFrameHeight(), 64);
    CHECK(ExtraPlaneIsNative());
    CHECK_EQ(g_serumData.fwidth, 128);
    CHECK_EQ(g_serumData.fheight, 32);
    CHECK_EQ(mySerum.width32, 0);
    CHECK_EQ(mySerum.width64, 256);
    CHECK(mySerum.frame32 == NULL);
    CHECK(mySerum.rotations32 == NULL);
    CHECK(mySerum.rotationsinframe32 == NULL);
    CHECK(mySerum.frame64 != NULL);
    CHECK(mySerum.flags & FLAG_RETURNED_EXTRA_AVAILABLE);
    // Nothing is scaled for these projects, in either direction.
    CHECK(!upscaleExtraFromOriginal);
    CHECK(!isoriginalrequested);
    CHECK(!isoriginalfallbackrequested);
    n256::Unload();
    CHECK(!g_serumData.is256x64);
  }
}

// The flag means one geometry only. Anything else is refused at load rather
// than rendered with buffers sized for something the file is not.
static void Test_N256RejectsOtherGeometry(void) {
  n256::Synth s;
  s.headerWidth = 192;
  CHECK(n256::LoadUnchecked(s, n256::kBothPlanes, false, NULL) == NULL);
  n256::Unload();
}

// Identification hashes the whole 256x64 frame: a difference in the last rows
// is a different frame, and a comparison mask is 256x64 too.
static void Test_N256Identification(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    for (uint32_t f = 0; f < n256::kScene; ++f) {
      // Its trigger starts a monochrome stream, which would then show the
      // unknown frames below; it has a test of its own.
      if (f == n256::kMonoTrigger) continue;
      const uint32_t r = n256::Colorize(s.Frame(f));
      CHECK(r != IDENTIFY_NO_FRAME && r != IDENTIFY_SAME_FRAME);
      CHECK_EQ(mySerum.frameID, f);
    }
    // No mask: one pixel in row 50 makes it a frame nobody knows.
    std::vector<uint8_t> frame = s.Frame(n256::kStatic);
    frame[50 * n256::kNW + 200] ^= 1;
    CHECK_EQ(n256::Colorize(frame), IDENTIFY_NO_FRAME);
    // Masked: the same change under the mask is still the frame...
    frame = s.Frame(n256::kMasked);
    frame[50 * n256::kNW + 200] ^= 1;
    frame[63 * n256::kNW + 255] ^= 1;
    CHECK(n256::Colorize(frame) != IDENTIFY_NO_FRAME);
    CHECK_EQ(mySerum.frameID, n256::kMasked);
    // ...and outside it, in the same rows, it is not.
    frame = s.Frame(n256::kMasked);
    frame[60 * n256::kNW + 5] ^= 1;
    CHECK_EQ(n256::Colorize(frame), IDENTIFY_NO_FRAME);
    n256::Unload();
  }
}

// Static content: the 256x64 plane shows the frame authored for it, and that
// is the only plane.
static void Test_N256StaticContent(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, FLAG_REQUEST_64P_FRAMES, viaConcentrate != 0)) continue;
    n256::Colorize(s.Frame(n256::kStatic));
    CHECK_EQ(mySerum.frameID, n256::kStatic);
    EXPECT_ONLY_PLANE64();
    EXPECT_PLANE64([](uint32_t x, uint32_t y) {
      return n256::C64(n256::kStatic, y * n256::kNW + x);
    });
    n256::Unload();
  }
}

// Dynamic content: the plane takes the ROM shade of its own pixel and the
// extra plane's colour set. Nothing is scaled.
static void Test_N256DynamicContent(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, FLAG_REQUEST_64P_FRAMES, viaConcentrate != 0)) continue;
    n256::Colorize(s.Frame(n256::kDynamic));
    CHECK_EQ(mySerum.frameID, n256::kDynamic);
    EXPECT_ONLY_PLANE64();
    EXPECT_PLANE64([](uint32_t x, uint32_t y) {
      return (uint16_t)(0x0200 + n256::Shade(n256::kDynamic, x, y));
    });
    // The copy kept for re-rendering the frame later is the whole frame.
    CHECK(memcmp(lastFrame, s.Native(n256::kDynamic), n256::kNPx) == 0);
    n256::Unload();
  }
}

// The background shows where its mask is set and the ROM lit nothing, judged
// on the plane's own ROM pixel.
static void Test_N256BackgroundOverBlack(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, FLAG_REQUEST_64P_FRAMES, viaConcentrate != 0)) continue;
    n256::Colorize(s.Frame(n256::kBackground));
    CHECK_EQ(mySerum.frameID, n256::kBackground);
    const uint8_t* rom = s.Native(n256::kBackground);
    EXPECT_ONLY_PLANE64();
    EXPECT_PLANE64([rom](uint32_t x, uint32_t y) {
      const uint32_t i = y * n256::kNW + x;
      return rom[i] == 0 ? n256::Bg64(i) : n256::C64(n256::kBackground, i);
    });
    n256::Unload();
  }
}

// What the plane must show for sprite 0 found at ROM position (sx, sy).
static uint16_t N256SpritePlane64(uint32_t sx, uint32_t sy, uint32_t x,
                                  uint32_t y) {
  const uint16_t under = n256::C64(n256::kSprite, y * n256::kNW + x);
  if (x < sx || y < sy || x >= sx + n256::kSprW || y >= sy + n256::kSprH)
    return under;
  const uint32_t px = x - sx, py = y - sy;
  if (n256::kSprite0[py][px] == n256::kT) return under;
  // Sprite pixel (1,0) is dynamic on this plane: its own ROM shade picks.
  if (px == 1 && py == 0) return (uint16_t)(0xE100 + n256::kSprite0[0][1]);
  return (uint16_t)(0xC000 + py * 16 + px);
}

// Sprites are looked for in the ROM frame as it is: the bounding box, the
// detection area and the position found are all 256x64 coordinates. This one
// sits at an odd position in row 37, which a 128x32 frame does not have.
static void Test_N256SpriteDetection(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    uint16_t which[MAX_SPRITES_PER_FRAME];
    uint8_t count = 0;
    uint16_t frx[MAX_SPRITES_PER_FRAME], fry[MAX_SPRITES_PER_FRAME],
        spx[MAX_SPRITES_PER_FRAME], spy[MAX_SPRITES_PER_FRAME],
        wid[MAX_SPRITES_PER_FRAME], hei[MAX_SPRITES_PER_FRAME];
    std::vector<uint8_t> frame = s.Frame(n256::kSprite);
    CHECK(Check_Spritesv2(frame.data(), n256::kSprite, which, &count, frx, fry,
                          spx, spy, wid, hei));
    CHECK_EQ(count, 1);
    if (count == 1) {
      CHECK_EQ(which[0], 0);
      CHECK_EQ(frx[0], n256::kSprX);
      CHECK_EQ(fry[0], n256::kSprY);
      CHECK_EQ(spx[0], 0);
      CHECK_EQ(spy[0], 0);
      CHECK_EQ(wid[0], n256::kSprW);
      CHECK_EQ(hei[0], n256::kSprH);
    }
    // Moved somewhere else inside the bounding box, it is found there.
    memset(frame.data(), 0, n256::kNPx);
    frame[60 * n256::kNW + 250] = 2;
    n256::Synth::PlaceSprite0(frame.data(), 88, 21);
    count = 0;
    CHECK(Check_Spritesv2(frame.data(), n256::kSprite, which, &count, frx, fry,
                          spx, spy, wid, hei));
    CHECK_EQ(count, 1);
    if (count == 1) {
      CHECK_EQ(frx[0], 88);
      CHECK_EQ(fry[0], 21);
    }
    // Sticking out of the bounding box, it is not a match.
    memset(frame.data(), 0, n256::kNPx);
    n256::Synth::PlaceSprite0(frame.data(), 93, 21);
    count = 0;
    CHECK(!Check_Spritesv2(frame.data(), n256::kSprite, which, &count, frx, fry,
                           spx, spy, wid, hei));
    CHECK_EQ(count, 0);

    // Shape mode goes through its own copy of the frame, which is a whole
    // ROM frame as well.
    frame = s.Frame(n256::kShapeSprite);
    count = 0;
    CHECK(Check_Spritesv2(frame.data(), n256::kShapeSprite, which, &count, frx,
                          fry, spx, spy, wid, hei));
    CHECK_EQ(count, 1);
    if (count == 1) {
      CHECK_EQ(which[0], 1);
      CHECK_EQ(frx[0], n256::kShapeX);
      CHECK_EQ(fry[0], n256::kShapeY);
      CHECK_EQ(wid[0], n256::kShapeW);
      CHECK_EQ(hei[0], n256::kShapeH);
    }
    n256::Unload();
  }
}

// A sprite found at ROM position (x, y) is drawn there, 1:1, from the
// extra-plane sprite arrays. The half-size picture the file also stores for
// the 128x32 plane goes nowhere.
static void Test_N256SpriteDrawing(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, FLAG_REQUEST_64P_FRAMES, viaConcentrate != 0)) continue;
    const uint32_t positions[2][2] = {{n256::kSprX, n256::kSprY}, {88, 21}};
    for (const auto& pos : positions) {
      std::vector<uint8_t> frame(n256::kNPx, 0);
      frame[60 * n256::kNW + 250] = 2;
      n256::Synth::PlaceSprite0(frame.data(), pos[0], pos[1]);
      const uint32_t r = n256::Colorize(frame);
      CHECK(r != IDENTIFY_NO_FRAME && r != IDENTIFY_SAME_FRAME);
      CHECK_EQ(mySerum.frameID, n256::kSprite);
      const uint32_t sx = pos[0], sy = pos[1];
      EXPECT_ONLY_PLANE64();
      EXPECT_PLANE64([sx, sy](uint32_t x, uint32_t y) {
        return N256SpritePlane64(sx, sy, x, y);
      });
    }
    n256::Unload();
  }
}

// The plane rotates with the extra plane's table, all of it, and nothing is
// reported for a 32p plane.
static void Test_N256Rotation(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    const uint32_t timer = n256::Colorize(s.Frame(n256::kRotation));
    CHECK(timer > 0 && timer <= 100);
    EXPECT_ONLY_PLANE64();
    EXPECT_PLANE64([](uint32_t, uint32_t) { return (uint16_t)0xF100; });
    // Long overdue.
    colorshiftinittime32[0] = GetMonotonicTimeMs() - 1000;
    colorshiftinittime64[0] = GetMonotonicTimeMs() - 1000;
    const uint32_t rotated = Serum_Rotate();
    CHECK(!(rotated & FLAG_RETURNED_V2_ROTATED32));
    CHECK(rotated & FLAG_RETURNED_V2_ROTATED64);
    EXPECT_PLANE64([](uint32_t, uint32_t) { return (uint16_t)0xF101; });
    n256::Unload();
  }
}

// 32p and 64p requested together: the 256x64 plane, and no 32p plane beside
// it -- no buffer, no flag, no width. A trigger on the frame is reported.
static void Test_N256RequestBoth(void) {
  n256::Synth s;
  s.triggers[n256::kStatic] = 1234;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    CHECK(mySerum.frame32 == NULL);
    CHECK_EQ(mySerum.width32, 0);
    CHECK_EQ(mySerum.width64, 256);
    const uint32_t r = n256::Colorize(s.Frame(n256::kStatic));
    CHECK_EQ(r, 0);
    CHECK_EQ(mySerum.frameID, n256::kStatic);
    CHECK_EQ(mySerum.triggerID, 1234);
    EXPECT_ONLY_PLANE64();
    EXPECT_PLANE64([](uint32_t x, uint32_t y) {
      return n256::C64(n256::kStatic, y * n256::kNW + x);
    });
    n256::Unload();
  }
}

// 32p alone asks for a plane these projects do not have. The colorization
// still loads, frames are still identified and their triggers still reported;
// there is just no picture, known frame or not.
static void Test_N256Request32pOnly(void) {
  n256::Synth s;
  s.triggers[n256::kStatic] = 1234;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    for (int direct = 0; direct <= viaConcentrate; ++direct) {
      if (!n256::Load(s, FLAG_REQUEST_32P_FRAMES, viaConcentrate != 0))
        continue;
      if (direct && !n256::Reload(FLAG_REQUEST_32P_FRAMES)) continue;
      CHECK(g_serumData.is256x64);
      CHECK(mySerum.frame32 == NULL);
      CHECK(mySerum.frame64 == NULL);
      CHECK_EQ(mySerum.width32, 0);
      CHECK_EQ(mySerum.width64, 0);
      CHECK(!isextrarequested);

      uint32_t r = n256::Colorize(s.Frame(n256::kStatic));
      CHECK_EQ(r, 0);
      CHECK_EQ(mySerum.frameID, n256::kStatic);
      CHECK_EQ(mySerum.triggerID, 1234);
      EXPECT_NO_PLANE();

      // Further frames go through identification, sprite detection and the
      // render calls with no plane to write to. With no trigger and no
      // picture they have nothing new to tell the caller, which is what
      // "already on the display" reports.
      r = n256::Colorize(s.Frame(n256::kSprite));
      CHECK_EQ(r, IDENTIFY_SAME_FRAME);
      CHECK_EQ(lastfound, n256::kSprite);
      EXPECT_NO_PLANE();
      r = n256::Colorize(s.Frame(n256::kRotation));
      CHECK_EQ(r, IDENTIFY_SAME_FRAME);
      CHECK_EQ(lastfound, n256::kRotation);
      EXPECT_NO_PLANE();
      CHECK_EQ(Serum_Rotate() &
                   (FLAG_RETURNED_V2_ROTATED32 | FLAG_RETURNED_V2_ROTATED64),
               0);

      // An unknown frame is "shown" in grey on no plane either.
      Serum_SetMaximumUnknownFramesToSkip(1);
      std::vector<uint8_t> frame(n256::kNPx);
      for (uint32_t i = 0; i < n256::kNPx; ++i)
        frame[i] = n256::Shade(77, i % n256::kNW, i / n256::kNW);
      CHECK_EQ(n256::Colorize(frame), 0);
      CHECK_EQ(mySerum.frameID, 0xfffffffd);
      EXPECT_NO_PLANE();
      n256::Unload();
    }
  }
}

// FLAG_REQUEST_FALLBACK: "the other plane will do". With a 32p-only request
// that is the 256x64 plane, for every frame. With a 64p request there is
// nothing to fall back to and the flag changes nothing.
static void Test_N256RequestFallback(void) {
  const n256::Synth s;
  // FLAG_REQUEST_FORCE keeps a load from the cROM from widening the request,
  // so there the stream loader has to keep the extra-plane data on its own.
  const uint8_t requests[4] = {
      FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_FALLBACK,
      FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_FALLBACK | FLAG_REQUEST_FORCE,
      FLAG_REQUEST_64P_FRAMES | FLAG_REQUEST_FALLBACK,
      n256::kBothPlanes | FLAG_REQUEST_FALLBACK,
  };
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    for (uint8_t request : requests) {
      // `direct` loads the generated cROMc on its own, where the extra-plane
      // data is kept or dropped by these flags alone.
      for (int direct = 0; direct <= viaConcentrate; ++direct) {
        if (!n256::Load(s, request, viaConcentrate != 0)) continue;
        if (direct && !n256::Reload(request)) continue;
        CHECK(mySerum.frame32 == NULL);
        CHECK(mySerum.frame64 != NULL);
        CHECK_EQ(mySerum.width32, 0);
        CHECK_EQ(mySerum.width64, 256);
        CHECK(!isoriginalfallbackrequested);

        n256::Colorize(s.Frame(n256::kDynamic));
        CHECK_EQ(mySerum.frameID, n256::kDynamic);
        EXPECT_ONLY_PLANE64();
        EXPECT_PLANE64([](uint32_t x, uint32_t y) {
          return (uint16_t)(0x0200 + n256::Shade(n256::kDynamic, x, y));
        });

        n256::Colorize(s.Frame(n256::kSprite));
        CHECK_EQ(mySerum.frameID, n256::kSprite);
        EXPECT_ONLY_PLANE64();
        EXPECT_PLANE64([](uint32_t x, uint32_t y) {
          return N256SpritePlane64(n256::kSprX, n256::kSprY, x, y);
        });
        n256::Unload();
      }
    }
  }
}

// A frame nobody knows is shown in grey on the 256x64 plane, pixel for pixel,
// and on that plane only -- whichever request produced the plane.
static void Test_N256UnknownFrameFallback(void) {
  const n256::Synth s;
  const uint8_t requests[3] = {n256::kBothPlanes, FLAG_REQUEST_64P_FRAMES,
                               FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_FALLBACK};
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    for (uint8_t request : requests) {
      if (!n256::Load(s, request, viaConcentrate != 0)) continue;
      Serum_SetMaximumUnknownFramesToSkip(1);
      std::vector<uint8_t> frame(n256::kNPx);
      for (uint32_t y = 0; y < n256::kNH; ++y)
        for (uint32_t x = 0; x < n256::kNW; ++x)
          frame[y * n256::kNW + x] = n256::Shade(77, x, y);
      CHECK_EQ(n256::Colorize(frame), 0);
      CHECK_EQ(mySerum.frameID, 0xfffffffd);
      EXPECT_ONLY_PLANE64();
      EXPECT_PLANE64([](uint32_t x, uint32_t y) {
        return grayscale_4[n256::Shade(77, x, y)];
      });
      n256::Unload();
    }
  }
}

// The monochrome palette trigger takes its palette from the frame's
// extra-plane colour set, the only one these projects are rendered from.
static void Test_N256MonochromePalette(void) {
  n256::Synth s;
  s.triggers[n256::kDynamic] = MONOCHROME_PALETTE_TRIGGER_ID;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, FLAG_REQUEST_64P_FRAMES, viaConcentrate != 0)) continue;
    n256::Colorize(s.Frame(n256::kDynamic));
    CHECK_EQ(mySerum.frameID, n256::kDynamic);
    CHECK(monochromePaletteMode);
    std::vector<uint8_t> frame(n256::kNPx);
    for (uint32_t i = 0; i < n256::kNPx; ++i)
      frame[i] = n256::Shade(77, i % n256::kNW, i / n256::kNW);
    CHECK_EQ(n256::Colorize(frame), 0);
    CHECK_EQ(mySerum.frameID, 0xfffffffd);
    EXPECT_ONLY_PLANE64();
    EXPECT_PLANE64([](uint32_t x, uint32_t y) {
      return (uint16_t)(0x0200 + n256::Shade(77, x, y));
    });
    n256::Unload();
  }
  // Without extra content the frame has no such set, and the 128x32 plane's
  // is not taken in its place.
  s.isextraframe[n256::kDynamic] = 0;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, FLAG_REQUEST_64P_FRAMES, viaConcentrate != 0)) continue;
    n256::Colorize(s.Frame(n256::kDynamic));
    CHECK_EQ(mySerum.frameID, n256::kDynamic);
    CHECK(!monochromePaletteMode);
    CHECK_EQ(monochromePaletteV2Length, 0);
    n256::Unload();
  }
}

// A frame, a background or a sprite without extra-plane data leaves its frame
// with nothing to render: the frame is identified, its trigger is reported,
// and no plane is produced. What the file stores for the 128x32 plane is not
// scaled up in its place, with or without FLAG_REQUEST_FALLBACK, and the
// plane keeps the picture it had.
static void Test_N256WithoutExtraData(void) {
  const uint8_t requests[3] = {FLAG_REQUEST_64P_FRAMES,
                               n256::kBothPlanes | FLAG_REQUEST_FALLBACK,
                               FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_FALLBACK};
  // Which piece is missing, and the frame that then has no plane.
  const uint32_t victims[3] = {n256::kStatic, n256::kBackground, n256::kSprite};
  for (int missing = 0; missing < 3; ++missing) {
    n256::Synth s;
    if (missing == 0) s.isextraframe[n256::kStatic] = 0;
    if (missing == 1) s.isextrabg[0] = 0;
    if (missing == 2) s.isextrasprite[0] = 0;
    const uint32_t victim = victims[missing];
    s.triggers[victim] = 1234;
    for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
      for (uint8_t request : requests) {
        if (!n256::Load(s, request, viaConcentrate != 0)) continue;
        // A complete frame first, so the plane holds a known picture.
        n256::Colorize(s.Frame(n256::kDynamic));
        EXPECT_ONLY_PLANE64();

        const uint32_t r = n256::Colorize(s.Frame(victim));
        CHECK_EQ(r, 0);
        CHECK_EQ(mySerum.frameID, victim);
        CHECK_EQ(mySerum.triggerID, 1234);
        EXPECT_NO_PLANE();
        CHECK(mySerum.frame32 == NULL);
        EXPECT_PLANE64([](uint32_t x, uint32_t y) {
          return (uint16_t)(0x0200 + n256::Shade(n256::kDynamic, x, y));
        });

        // The next complete frame is rendered as usual.
        n256::Colorize(s.Frame(n256::kRotation));
        CHECK_EQ(mySerum.frameID, n256::kRotation);
        EXPECT_ONLY_PLANE64();
        EXPECT_PLANE64([](uint32_t, uint32_t) { return (uint16_t)0xF100; });
        n256::Unload();
      }
    }
  }
}

// A black frame does not end a monochrome stream -- but only a frame that is
// black all the way down. This one is black in its first 4096 bytes and lit
// below, so it does end it, and the unknown frame after it is not shown.
static void Test_N256BlackFrameIsJudgedWhole(void) {
  const n256::Synth s;
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    std::vector<uint8_t> unknown(n256::kNPx);
    for (uint32_t i = 0; i < n256::kNPx; ++i)
      unknown[i] = n256::Shade(91, i % n256::kNW, i / n256::kNW);

    n256::Colorize(s.Frame(n256::kMonoTrigger));
    CHECK_EQ(mySerum.frameID, n256::kMonoTrigger);
    CHECK(monochromeMode);
    CHECK_EQ(n256::Colorize(unknown), 0);
    CHECK_EQ(mySerum.frameID, 0xfffffffd);

    n256::Colorize(s.Frame(n256::kBlackTop));
    CHECK_EQ(mySerum.frameID, n256::kBlackTop);
    CHECK(!monochromeMode);
    CHECK_EQ(n256::Colorize(unknown), IDENTIFY_NO_FRAME);
    n256::Unload();
  }
}

// Scene markers are 128x32 frames from the scene generator. In these projects
// they are identified like any other frame, over 16384 bytes: the marker
// followed by zeros. The lookup built at load time has to hash them the same
// way, and must not read past the marker it generated.
static void Test_N256SceneLookup(void) {
  const char* const csv = "7,2,100\n";
  n256::Synth s;
  {
    const std::filesystem::path dir = n256::Dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "scene.csv").string();
    {
      std::ofstream out(path);
      out << csv;
    }
    SceneGenerator generator;
    CHECK(generator.parseCSV(path));
    generator.setDepth(2);
    memset(s.Native(n256::kScene), 0, n256::kNPx);
    CHECK_EQ(generator.generateFrame(7, 1, s.Native(n256::kScene), 1, true),
             0xffff);
  }
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0, csv)) continue;
    CHECK_EQ(g_serumData.frameIsScene.size(), n256::kFrameCount);
    if (g_serumData.frameIsScene.size() == n256::kFrameCount) {
      CHECK_EQ(g_serumData.frameIsScene[n256::kScene], 1);
      CHECK_EQ(g_serumData.frameIsScene[n256::kStatic], 0);
    }
    if (viaConcentrate) {
      const auto it =
          g_serumData.sceneFrameIdByTriplet.find(MakeSceneTripletKey(7, 1, 1));
      CHECK(it != g_serumData.sceneFrameIdByTriplet.end());
      if (it != g_serumData.sceneFrameIdByTriplet.end())
        CHECK_EQ(it->second, n256::kScene);
    }
    n256::Unload();
  }
}

// The scene search hashes a ROM frame once per mask and shape mode, not once
// per scene frame. Scene frames sharing a pair can still be told apart, a pair
// that found nothing is passed over for the next one, and the search keeps its
// order and its "same frame" result.
static void Test_N256SceneSearchByMaskAndShape(void) {
  const char* const csv = "7,3,100\n";
  // Scene frames in search order: two share the pair (255, 0), the one
  // between them has comparison mask 0.
  const uint32_t sceneFrames[3] = {n256::kMonoTrigger, n256::kScene,
                                   n256::kBlackTop};
  n256::Synth s;
  std::vector<std::vector<uint8_t>> generated;
  {
    const std::filesystem::path dir = n256::Dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "scene.csv").string();
    {
      std::ofstream out(path);
      out << csv;
    }
    SceneGenerator generator;
    CHECK(generator.parseCSV(path));
    generator.setDepth(2);
    for (uint16_t frameIndex = 0; frameIndex < 3; ++frameIndex) {
      std::vector<uint8_t> frame(n256::kNPx, 0);
      CHECK_EQ(generator.generateFrame(7, frameIndex, frame.data(), 1, true),
               0xffff);
      memcpy(s.Native(sceneFrames[frameIndex]), frame.data(), n256::kNPx);
      generated.push_back(frame);
    }
  }
  CHECK(generated[0] != generated[1]);
  CHECK(generated[1] != generated[2]);
  s.compmaskid[n256::kMonoTrigger] = 255;
  s.compmaskid[n256::kScene] = 255;
  s.compmaskid[n256::kBlackTop] = 0;
  std::vector<uint8_t> unknown(n256::kNPx, 1);
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0, csv)) continue;
    for (uint32_t frameId : sceneFrames)
      CHECK_EQ(g_serumData.frameIsScene[frameId], 1);
    if (viaConcentrate) {
      for (uint16_t frameIndex = 0; frameIndex < 3; ++frameIndex) {
        const auto it = g_serumData.sceneFrameIdByTriplet.find(
            MakeSceneTripletKey(7, 1, frameIndex));
        CHECK(it != g_serumData.sceneFrameIdByTriplet.end());
        if (it != g_serumData.sceneFrameIdByTriplet.end())
          CHECK_EQ(it->second, sceneFrames[frameIndex]);
      }
    }
    CHECK_EQ(Identify_Frame(generated[1].data(), true), n256::kScene);
    // From kScene: (255, 0) finds nothing, kMonoTrigger has the same pair and
    // is passed over, mask 0 finds kBlackTop.
    CHECK_EQ(Identify_Frame(generated[2].data(), true), n256::kBlackTop);
    CHECK_EQ(Identify_Frame(generated[0].data(), true), n256::kMonoTrigger);
    CHECK_EQ(Identify_Frame(generated[0].data(), true), IDENTIFY_SAME_FRAME);
    CHECK_EQ(Identify_Frame(unknown.data(), true), IDENTIFY_NO_FRAME);
    n256::Unload();
  }
}

// The same file without the flag is an ordinary 128x32 project with a 256x64
// extra plane, and nothing about it may change: the ROM frame is 128x32, the
// 128x32 plane is rendered from its own data, the extra plane is the scaled
// layer, and sprites are looked for at 128x32.
static void Test_N256FlagOffIsUnchanged(void) {
  n256::Synth s;
  s.is256x64 = false;
  // Without the flag the ROM frame is the first 128x32 of each buffer, the
  // masks are 128x32, and there are no sprites to speak of.
  s.compmasks.assign((size_t)s.ncompmasks * n256::kOPx, 0);
  s.compmaskid.assign(s.nframes, 255);
  s.framesprites.assign(s.framesprites.size(), 255);
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    CHECK(!g_serumData.is256x64);
    CHECK_EQ(NativeFrameWidth(), 128);
    CHECK_EQ(NativeFrameHeight(), 32);
    CHECK(!ExtraPlaneIsNative());
    CHECK(upscaleExtraFromOriginal);
    CHECK(mySerum.frame32 != NULL);
    CHECK_EQ(mySerum.width32, 128);
    std::vector<uint8_t> frame(s.Native(n256::kDynamic),
                               s.Native(n256::kDynamic) + n256::kOPx);
    frame.shrink_to_fit();
    Serum_Colorize(frame.data());
    CHECK_EQ(mySerum.frameID, n256::kDynamic);
    CHECK(mySerum.flags & FLAG_RETURNED_32P_FRAME_OK);
    CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
    CHECK_EQ(mySerum.width32, 128);
    const uint8_t* rom = s.Native(n256::kDynamic);
    EXPECT_PLANE32([rom](uint32_t x, uint32_t y) {
      return (uint16_t)(0x0100 + rom[y * n256::kOW + x]);
    });
    n256::Unload();
  }
}

// ---------------------------------------------------------------------------
// Sprites on a 128x32 project whose 64-row output libserum upscales
//
// In layer mode a sprite reaches the 256x64 plane through the 128x32 one: it is
// drawn there, and its region is composited into the extra plane afterwards.
// A sprite with HD art still sends its dynamic pixels that way. So the sprite
// pass has to draw into the 128x32 plane whether or not the caller is given
// that plane, and what it leaves in the extra plane is not something a later
// frame may keep.
// ---------------------------------------------------------------------------

namespace spr32 {

// Where the sprite is found in the ROM frame, and its size there.
static const uint32_t kX = 5, kY = 3, kWid = 4, kHei = 3;
static const uint32_t kTransX = 3, kTransY = 2;  // a transparent sprite pixel
static const uint32_t kDynX = 1, kDynY = 1;      // a dynamic sprite pixel
static const uint8_t kDynShade = 9;              // the ROM shade under it
static const uint16_t kFrameColour = 0x0111;

static uint16_t SpriteColour(uint32_t px, uint32_t py) {
  return (uint16_t)(0x5000 + py * 16 + px);
}
static uint16_t HdColour(uint32_t hx, uint32_t hy) {
  return (uint16_t)(0x9000 + hy * 16 + hx);
}
static uint16_t DynColour(uint8_t shade) { return (uint16_t)(0x7000 + shade); }

static std::vector<uint8_t> g_spriteRom;

// One frame of a single static colour and one sprite, requested the way
// ConfigureRequestedOutputMode() leaves a 128x32 project: `both` is
// `32P | 64P`, otherwise `64P` alone, where the 128x32 plane is rendered as the
// upscale source and not handed to the caller.
static void SetUp(bool both, bool hdArt, uint8_t algorithm) {
  SetUpColorization();
  upscaleExtraFromOriginal = true;
  originalPlaneRequestedByCaller = both;
  isoriginalrequested = both;
  isoriginalfallbackrequested = !both;
  isextrarequested = true;
  runtimeScalingAlgorithm = algorithm;
  const size_t px = (size_t)kW * kCH;
  const std::vector<uint16_t> colours(px, kFrameColour);
  g_serumData.cframes_v2.set(kFrameId, colours.data(), px);

  g_spriteRom.assign(px, 0);
  g_spriteRom[(size_t)(kY + kDynY) * kW + kX + kDynX] = kDynShade;
  romFrameForUpscale = g_spriteRom.data();

  const size_t spritePixels = (size_t)MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT;
  g_serumData.nsprites = 1;
  std::vector<uint8_t> opaque(spritePixels, 0), dynActive(spritePixels, 0);
  const std::vector<uint8_t> dynLayer(spritePixels, 0);
  std::vector<uint16_t> art(spritePixels, 0);
  for (uint32_t y = 0; y < kHei; ++y)
    for (uint32_t x = 0; x < kWid; ++x) {
      if (x == kTransX && y == kTransY) continue;
      opaque[(size_t)y * MAX_SPRITE_WIDTH + x] = 1;
      art[(size_t)y * MAX_SPRITE_WIDTH + x] = SpriteColour(x, y);
    }
  dynActive[(size_t)kDynY * MAX_SPRITE_WIDTH + kDynX] = 1;
  g_serumData.spriteoriginal_opaque.set(0, opaque.data(), spritePixels);
  g_serumData.spritecolored.set(0, art.data(), spritePixels);
  g_serumData.dynaspritemasks.set(0, dynLayer.data(), spritePixels);
  g_serumData.dynaspritemasks_active.set(0, dynActive.data(), spritePixels);
  std::vector<uint16_t> sets(
      (size_t)MAX_DYNA_SETS_PER_SPRITE * g_serumData.nocolors, 0);
  for (uint32_t shade = 0; shade < g_serumData.nocolors; ++shade)
    sets[shade] = DynColour((uint8_t)shade);
  g_serumData.dynasprite4cols.set(0, sets.data(), sets.size());
  if (!hdArt) return;

  FillIndexed<uint8_t>(g_serumData.isextrasprite, {1}, 1, 1);
  std::vector<uint8_t> opaqueHd(spritePixels, 0);
  std::vector<uint16_t> artHd(spritePixels, 0);
  for (uint32_t y = 0; y < kHei * 2; ++y)
    for (uint32_t x = 0; x < kWid * 2; ++x) {
      if (x / 2 == kTransX && y / 2 == kTransY) continue;
      opaqueHd[(size_t)y * MAX_SPRITE_WIDTH + x] = 1;
      artHd[(size_t)y * MAX_SPRITE_WIDTH + x] = HdColour(x, y);
    }
  g_serumData.spritemask_extra_opaque.set(0, opaqueHd.data(), spritePixels,
                                          &g_serumData.isextrasprite);
  g_serumData.spritecolored_extra.set(0, artHd.data(), spritePixels,
                                      &g_serumData.isextrasprite);
}

static void DrawSprite(void) {
  Colorize_Spritev2(g_spriteRom.data(), 0, kX, kY, /*spx=*/0, /*spy=*/0, kWid,
                    kHei, kFrameId);
}

// The sprite pixel a 128x32 position belongs to, or false.
static bool InSprite(uint32_t sx, uint32_t sy, uint32_t* px, uint32_t* py) {
  if (sx < kX || sy < kY || sx >= kX + kWid || sy >= kY + kHei) return false;
  *px = sx - kX;
  *py = sy - kY;
  return !(*px == kTransX && *py == kTransY);
}

static uint16_t Expected32(uint32_t x, uint32_t y) {
  uint32_t px, py;
  if (!InSprite(x, y, &px, &py)) return kFrameColour;
  if (px == kDynX && py == kDynY) return DynColour(kDynShade);
  return SpriteColour(px, py);
}

// With line doubling: twice the position, twice the size, every 128x32 sprite
// pixel a 2x2 block -- or the sprite's own 256x64 picture where it has one,
// except for its dynamic pixel, which always comes through the scaled layer.
static uint16_t Expected64(bool hdArt, uint32_t x, uint32_t y) {
  uint32_t px, py;
  if (!InSprite(x / 2, y / 2, &px, &py)) return kFrameColour;
  if (px == kDynX && py == kDynY) return DynColour(kDynShade);
  return hdArt ? HdColour(x - kX * 2, y - kY * 2) : SpriteColour(px, py);
}

static unsigned Mismatches64(bool hdArt) {
  unsigned bad = 0;
  for (uint32_t y = 0; y < kCH * 2; ++y)
    for (uint32_t x = 0; x < kW * 2; ++x)
      if (mySerum.frame64[(size_t)y * kW * 2 + x] != Expected64(hdArt, x, y))
        ++bad;
  return bad;
}

}  // namespace spr32

// A caller that asks for the 64-row plane only is not handed the 128x32 one,
// but the sprite still has to be drawn into it: that is where the extra plane
// gets it from. Whether the plane is advertised and whether it was rendered
// are different questions, and the sprite pass used to ask the first. A sprite
// without HD art then never appeared at all, and one with HD art lost its
// dynamic pixels.
static void Test_SpriteIsDrawnFor64pOnlyRequest(void) {
  const size_t px = (size_t)kW * kCH;
  for (int hdArt = 0; hdArt < 2; ++hdArt) {
    for (uint8_t algorithm : {(uint8_t)SERUM_SCALING_LINE_DOUBLING,
                              (uint8_t)SERUM_SCALING_SCALE2X}) {
      const bool exact = algorithm == SERUM_SCALING_LINE_DOUBLING;
      // Both planes requested: the reference.
      spr32::SetUp(/*both=*/true, hdArt != 0, algorithm);
      Colorize_Framev2(spr32::g_spriteRom.data(), kFrameId);
      spr32::DrawSprite();
      CHECK(mySerum.flags & FLAG_RETURNED_32P_FRAME_OK);
      CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
      unsigned bad32 = 0;
      for (uint32_t y = 0; y < kCH; ++y)
        for (uint32_t x = 0; x < kW; ++x)
          if (mySerum.frame32[(size_t)y * kW + x] != spr32::Expected32(x, y))
            ++bad32;
      CHECK_EQ(bad32, 0);
      if (exact) CHECK_EQ(spr32::Mismatches64(hdArt != 0), 0);
      const std::vector<uint16_t> reference(mySerum.frame64,
                                            mySerum.frame64 + px * 4);
      TearDownFrame();

      // The 64-row plane alone: the same picture, and still only that plane.
      spr32::SetUp(/*both=*/false, hdArt != 0, algorithm);
      Colorize_Framev2(spr32::g_spriteRom.data(), kFrameId);
      CHECK(!(mySerum.flags & FLAG_RETURNED_32P_FRAME_OK));
      CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
      spr32::DrawSprite();
      CHECK(!(mySerum.flags & FLAG_RETURNED_32P_FRAME_OK));
      CHECK_EQ(mySerum.width32, 0);
      CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
      CHECK_EQ(mySerum.width64, kW * 2);
      if (exact) CHECK_EQ(spr32::Mismatches64(hdArt != 0), 0);
      unsigned differs = 0;
      for (size_t i = 0; i < px * 4; ++i)
        if (mySerum.frame64[i] != reference[i]) ++differs;
      CHECK_EQ(differs, 0);
      TearDownFrame();
    }
  }
}

// A frame's dynamic shadow that falls inside a sprite's rectangle is repainted
// by the sprite's own composite, out of the 128x32 plane. So that plane has to
// hold the shadow by then, for a caller that never sees the plane as well --
// otherwise the composite paints the shadow away wherever the sprite is
// transparent, and the two requests show different pictures.
static void Test_FrameShadowInsideSpriteRegion(void) {
  const size_t px = (size_t)kW * kCH;
  const uint16_t kShadow = 0x0444, kGlyph = 0x0333;
  // A lit dynamic pixel just under the sprite's rectangle, casting its shadow
  // upwards onto the sprite's transparent pixel.
  const uint32_t gx = spr32::kX + spr32::kTransX, gy = spr32::kY + spr32::kHei;
  for (uint8_t algorithm :
       {(uint8_t)SERUM_SCALING_LINE_DOUBLING, (uint8_t)SERUM_SCALING_SCALE2X}) {
    std::vector<uint16_t> reference;
    for (int both = 1; both >= 0; --both) {
      spr32::SetUp(both != 0, /*hdArt=*/false, algorithm);
      std::vector<uint8_t> active(px, 0);
      active[(size_t)gy * kW + gx] = 1;
      std::vector<uint16_t> shadeToColour(16, kGlyph);
      shadeToColour[0] = spr32::kFrameColour;
      GiveFrameADynamicZone(active, /*couche=*/2, shadeToColour);
      std::vector<uint8_t> dir(MAX_DYNA_SETS_PER_FRAME_V2, 0);
      std::vector<uint16_t> col(MAX_DYNA_SETS_PER_FRAME_V2, 0);
      dir[2] = 1u << 1;  // up
      col[2] = kShadow;
      g_serumData.dynashadowsdir.set(kFrameId, dir.data(), dir.size());
      g_serumData.dynashadowscol.set(kFrameId, col.data(), col.size());
      spr32::g_spriteRom[(size_t)gy * kW + gx] = 5;

      Colorize_Framev2(spr32::g_spriteRom.data(), kFrameId);
      spr32::DrawSprite();
      CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
      CHECK_EQ((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) != 0, both != 0);
      // The shadow is there, in the block of the transparent sprite pixel.
      unsigned shadow = 0;
      for (uint32_t dy = 0; dy < 2; ++dy)
        for (uint32_t dx = 0; dx < 2; ++dx)
          if (mySerum.frame64[(size_t)((gy - 1) * 2 + dy) * kW * 2 + gx * 2 +
                              dx] == kShadow)
            ++shadow;
      CHECK(shadow > 0);
      if (both) {
        reference.assign(mySerum.frame64, mySerum.frame64 + px * 4);
      } else {
        unsigned differs = 0;
        for (size_t i = 0; i < px * 4; ++i)
          if (mySerum.frame64[i] != reference[i]) ++differs;
        CHECK_EQ(differs, 0);
      }
      TearDownFrame();
    }
  }
}

// A sprite without HD art lands in the extra plane after the frame's composite,
// exactly as HD sprite art does, so the plane is no longer the upscale of the
// frame's own 128x32 picture. The next frame with that same picture -- the
// sprite gone, or somewhere else -- has to derive the plane again, or the
// sprite stays where it was.
static void Test_ScaledSpriteBlocksReuse(void) {
  const size_t at = (size_t)(spr32::kY * 2) * (kW * 2) + spr32::kX * 2;
  for (int both = 0; both < 2; ++both) {
    spr32::SetUp(both != 0, /*hdArt=*/false, SERUM_SCALING_SCALE2X);
    uint8_t* rom = spr32::g_spriteRom.data();
    extraPlaneReuseCount = 0;
    Colorize_Framev2(rom, kFrameId);
    CHECK_EQ(extraPlaneReuseCount, 0);
    // The same picture again, with no sprite: kept, as the rule says.
    Colorize_Framev2(rom, kFrameId);
    CHECK_EQ(extraPlaneReuseCount, 1);
    CHECK_EQ(mySerum.frame64[at], spr32::kFrameColour);
    const std::vector<uint16_t> withoutSprite(
        mySerum.frame64, mySerum.frame64 + (size_t)kW * kCH * 4);

    // Now a frame whose sprite is composited into the plane.
    Colorize_Framev2(rom, kFrameId);
    CHECK_EQ(extraPlaneReuseCount, 2);
    spr32::DrawSprite();
    CHECK_EQ(mySerum.frame64[at + 2], spr32::SpriteColour(1, 0));

    // The sprite is gone and the 128x32 picture is the one from before it.
    Colorize_Framev2(rom, kFrameId);
    CHECK_EQ(extraPlaneReuseCount, 2);
    unsigned left = 0;
    for (size_t i = 0; i < withoutSprite.size(); ++i)
      if (mySerum.frame64[i] != withoutSprite[i]) ++left;
    CHECK_EQ(left, 0);
    TearDownFrame();
  }
}

// The same through the public calls, on a cROM and on the cROMc made from it:
// a 128x32 project, a frame with no HD content of its own, and a sprite that
// moves inside its bounding box and then goes away while the frame stays the
// same one. Every colour is unique, so nothing is rounded and each 128x32
// pixel is a 2x2 block of the plane, the frame's corners aside.
static void Test_SpriteInLayerModeEndToEnd(void) {
  using namespace n256;
  Synth s;
  s.is256x64 = false;
  // Without the flag the ROM frame is the first 128x32 of each buffer and the
  // masks are 128x32.
  s.compmasks.assign((size_t)s.ncompmasks * kOPx, 0);
  s.compmaskid.assign(s.nframes, 255);
  s.framesprites.assign(s.framesprites.size(), 255);
  // The sprite frame: nothing lit but one pixel outside the comparison mask,
  // which hides the sprite's bounding box. No HD frame, no HD sprite art.
  const uint32_t bx0 = 10, by0 = 4, bx1 = 100, by1 = 28;
  s.isextraframe[kSprite] = 0;
  s.isextrasprite[0] = 0;
  s.compmaskid[kSprite] = 1;
  for (uint32_t y = by0; y <= by1; ++y)
    for (uint32_t x = bx0; x <= bx1; ++x)
      s.compmasks[(size_t)1 * kOPx + y * kOW + x] = 1;
  s.framesprites[(size_t)kSprite * s.slotsPerFrame] = 0;
  s.SetBB(kSprite, bx0, by0, bx1, by1);
  for (uint32_t y = 0; y < kSprH; ++y)
    for (uint32_t x = 0; x < kSprW; ++x)
      s.spritecolored[y * MAX_SPRITE_WIDTH + x] =
          (uint16_t)(0xD000 + y * 16 + x);
  memset(s.Native(kSprite), 0, kOPx);
  s.Native(kSprite)[30 * kOW + 120] = 2;

  // The ROM frame with the sprite at (sx, sy), or without it.
  const auto romFrame = [&s](bool withSprite, uint32_t sx, uint32_t sy) {
    std::vector<uint8_t> frame(s.Native(kSprite), s.Native(kSprite) + kOPx);
    if (withSprite)
      for (uint32_t y = 0; y < kSprH; ++y)
        for (uint32_t x = 0; x < kSprW; ++x)
          frame[(sy + y) * kOW + sx + x] =
              kSprite0[y][x] == kT ? 3 : kSprite0[y][x];
    frame.shrink_to_fit();
    return frame;
  };
  // What a 128x32 position shows. Sprite pixel (0,0) is dynamic.
  const auto expected32 = [](bool withSprite, uint32_t sx, uint32_t sy,
                             uint32_t x, uint32_t y) {
    const uint16_t under = C32(kSprite, y * kOW + x);
    if (!withSprite || x < sx || y < sy || x >= sx + kSprW || y >= sy + kSprH)
      return under;
    const uint32_t px = x - sx, py = y - sy;
    if (kSprite0[py][px] == kT) return under;
    if (px == 0 && py == 0) return (uint16_t)(0xE000 + kSprite0[0][0]);
    return (uint16_t)(0xD000 + py * 16 + px);
  };

  for (uint8_t flags : {(uint8_t)FLAG_REQUEST_64P_FRAMES, kBothPlanes}) {
    const bool both = flags == kBothPlanes;
    for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
      if (!Load(s, flags, viaConcentrate != 0)) continue;
      CHECK(upscaleExtraFromOriginal);
      // Found at one place, then at another, then not at all.
      const struct {
        bool withSprite;
        uint32_t sx, sy;
      } steps[] = {
          {true, 41, 13}, {true, 70, 9}, {false, 0, 0}, {true, 41, 13}};
      for (const auto& step : steps) {
        std::vector<uint8_t> frame =
            romFrame(step.withSprite, step.sx, step.sy);
        const uint32_t r = Serum_Colorize(frame.data());
        CHECK(r != IDENTIFY_NO_FRAME && r != IDENTIFY_SAME_FRAME);
        CHECK_EQ(mySerum.frameID, kSprite);
        CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
        CHECK_EQ(mySerum.width64, 256);
        CHECK_EQ((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) != 0, both);
        CHECK_EQ(mySerum.width32, both ? 128 : 0);
        const bool with = step.withSprite;
        const uint32_t sx = step.sx, sy = step.sy;
        EXPECT_PLANE64([&](uint32_t x, uint32_t y) {
          // Outside the frame is black, so the frame's own four corners are
          // rounded into it.
          if ((x == 0 || x == kNW - 1) && (y == 0 || y == kNH - 1))
            return (uint16_t)0;
          return expected32(with, sx, sy, x / 2, y / 2);
        });
        if (both)
          EXPECT_PLANE32([&](uint32_t x, uint32_t y) {
            return expected32(with, sx, sy, x, y);
          });
      }
      Unload();
    }
  }
}

// A background scene with the game's dynamic text in front of it: a 128x32
// project upscaled to 64 rows. The text has to scale the same in every scene
// step, whatever the scene shows behind it -- a movie changes the colours
// beside the text with every step, and Scale2x rounds a corner or not by
// comparing exactly those, so the text flickered while the scene played. It is
// scaled as on black: a solid corner rounds, a corner of a one pixel stroke
// stays, both judged on the game's ROM frame (the scene steps colorize the
// scene's marker frame, which the corner rule used to read). Scale2x only; a
// real machine line doubles.
static void Test_BackgroundSceneKeepsTextShape(void) {
  using namespace n256;
  const char* const csv = "7,1,100,0,1,1,1,0,0,4\n";  // looping, as background
  Synth s;
  s.is256x64 = false;
  s.compmasks.assign((size_t)s.ncompmasks * kOPx, 0);
  s.compmaskid.assign(s.nframes, 255);
  s.framesprites.assign(s.framesprites.size(), 255);
  s.isextraframe.assign(s.nframes, 0);
  {
    const std::filesystem::path dir = Dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "scene.csv").string();
    {
      std::ofstream out(path);
      out << csv;
    }
    SceneGenerator generator;
    CHECK(generator.parseCSV(path));
    generator.setDepth(2);
    memset(s.Native(kScene), 0, kNPx);
    CHECK_EQ(generator.generateFrame(7, 0, s.Native(kScene), 1, true), 0xffff);
  }
  // The scene keeps its colours: one of its own on every pixel, so no two
  // pixels beside the text match.
  //
  // The game frame, dynamic over the whole frame with the scene behind
  // everything unlit, starting scene 7: an L of one pixel wide strokes with
  // its corner at (lx, ly), and a solid block with its top left corner at
  // (bx, by).
  const uint32_t lx = 20, ly = 10, bx = 40, by = 10;
  memset(s.Native(kDynamic), 0, kNPx);
  for (uint32_t i = 0; i < 6; ++i) {
    s.Native(kDynamic)[ly * kOW + lx + i] = 3;
    s.Native(kDynamic)[(ly + i) * kOW + lx] = 3;
  }
  for (uint32_t y = by; y < by + 6; ++y)
    for (uint32_t x = bx; x < bx + 6; ++x) s.Native(kDynamic)[y * kOW + x] = 3;
  s.bgids[kDynamic] = 0;
  memset(&s.bgmask[(size_t)kDynamic * kOPx], 1, kOPx);
  s.triggers[kDynamic] = 7;
  const uint16_t kText = (uint16_t)(0x0100 + 3);

  if (!Load(s, FLAG_REQUEST_64P_FRAMES, false, csv)) return;
  CHECK(upscaleExtraFromOriginal);
  std::vector<uint8_t> game(s.Native(kDynamic), s.Native(kDynamic) + kOPx);
  game.shrink_to_fit();
  Serum_Colorize(game.data());
  CHECK(g_serumData.sceneGenerator->isActive());
  // The outer quadrant of a corner: the text colour where it is kept.
  const auto quadrant = [](uint32_t x, uint32_t y) {
    return mySerum.frame64[(size_t)(y * 2) * (kOW * 2) + x * 2];
  };
  int steps = 0;
  for (int i = 0; i < 4; ++i) {
    sceneNextFrameAtMs = 0;
    const uint32_t r = Serum_Rotate();
    if (!(r & (FLAG_RETURNED_V2_ROTATED32 | FLAG_RETURNED_V2_ROTATED64)))
      continue;
    ++steps;
    // The scene shows behind the text.
    CHECK_EQ(mySerum.frame64[(size_t)(ly * 2 - 2) * (kOW * 2) + lx * 2],
             C32(kScene, (ly - 1) * kOW + lx));
    // The stroke's corner stands, the block's corner is rounded.
    CHECK_EQ(quadrant(lx, ly), kText);
    CHECK(quadrant(bx, by) != kText);
    CHECK_EQ(quadrant(bx + 1, by + 1), kText);
  }
  CHECK(steps > 0);
  Unload();
}

// A monochrome frame paints both planes itself. On a project with an authored
// 64-row plane it writes that plane directly, and the plane is then not the
// upscale of the last colorized picture any more: the frame that ends the
// monochrome stretch -- the same picture again, reached from a ROM frame that
// differs only under the comparison mask -- has to derive it afresh instead of
// keeping the grey one.
static void Test_MonochromeFrameBlocksReuse(void) {
  using namespace n256;
  Synth s;
  s.is256x64 = false;
  s.compmasks.assign((size_t)s.ncompmasks * kOPx, 0);
  s.compmaskid.assign(s.nframes, 255);
  s.framesprites.assign(s.framesprites.size(), 255);
  // A frame with no HD content of its own, so its 64-row plane is derived,
  // and a comparison mask that hides one corner of it.
  s.isextraframe[kStatic] = 0;
  s.compmaskid[kStatic] = 1;
  for (uint32_t y = 20; y < 30; ++y)
    for (uint32_t x = 100; x < 120; ++x)
      s.compmasks[(size_t)1 * kOPx + y * kOW + x] = 1;

  const auto known = [&s](uint8_t hidden) {
    std::vector<uint8_t> frame(s.Native(kStatic), s.Native(kStatic) + kOPx);
    frame[25 * kOW + 110] = hidden;  // under the mask: same frame, new CRC
    frame.shrink_to_fit();
    return frame;
  };
  const auto expectColorized = [&](bool both) {
    CHECK_EQ(mySerum.frameID, kStatic);
    CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
    CHECK_EQ(mySerum.width64, 256);
    EXPECT_PLANE64([](uint32_t x, uint32_t y) {
      if ((x == 0 || x == kNW - 1) && (y == 0 || y == kNH - 1))
        return (uint16_t)0;
      return C32(kStatic, (y / 2) * kOW + x / 2);
    });
    if (both)
      EXPECT_PLANE32(
          [](uint32_t x, uint32_t y) { return C32(kStatic, y * kOW + x); });
  };

  for (uint8_t flags : {(uint8_t)FLAG_REQUEST_64P_FRAMES, kBothPlanes}) {
    const bool both = flags == kBothPlanes;
    for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
      if (!Load(s, flags, viaConcentrate != 0)) continue;
      CHECK(upscaleExtraFromOriginal);
      std::vector<uint8_t> first = known(0);
      uint32_t r = Serum_Colorize(first.data());
      CHECK(r != IDENTIFY_NO_FRAME && r != IDENTIFY_SAME_FRAME);
      expectColorized(both);

      // An unknown frame, shown in grey on both planes.
      Serum_SetMaximumUnknownFramesToSkip(1);
      std::vector<uint8_t> unknown(kOPx, 3);
      unknown[0] = 0;
      unknown.shrink_to_fit();
      r = Serum_Colorize(unknown.data());
      CHECK_EQ(r, 0);
      CHECK_EQ(mySerum.frameID, 0xfffffffd);
      CHECK(mySerum.flags & FLAG_RETURNED_64P_FRAME_OK);
      CHECK_EQ(mySerum.frame64[20 * kNW + 20], grayscale_4[3]);

      // The colorized picture again. Its 128x32 plane is what the 64-row
      // plane was last derived from; the 64-row plane itself is grey.
      std::vector<uint8_t> again = known(1);
      r = Serum_Colorize(again.data());
      CHECK(r != IDENTIFY_NO_FRAME && r != IDENTIFY_SAME_FRAME);
      expectColorized(both);
      Unload();
    }
  }
}

// ---------------------------------------------------------------------------
// Serum_LoadFromMemory()
//
// The same cROM bytes, once as a file through Serum_Load() and once as a
// buffer, have to give the same loaded state and the same output.

namespace memload {

struct Capture {
  uint8_t loadFlags = 0;
  uint32_t width32 = 0, width64 = 0, ntriggers = 0, nocolors = 0;
  bool has32 = false, has64 = false;
  std::vector<uint8_t> frameIsScene;
  std::vector<uint16_t> spriteCandidateIds;
  std::vector<uint32_t> frameToNormalBucket;
  size_t rotationLookupSize = 0, normalBuckets = 0;
  std::vector<uint32_t> results;
  std::vector<uint8_t> output;

  bool operator==(const Capture& o) const {
    return loadFlags == o.loadFlags && width32 == o.width32 &&
           width64 == o.width64 && ntriggers == o.ntriggers &&
           nocolors == o.nocolors && has32 == o.has32 && has64 == o.has64 &&
           frameIsScene == o.frameIsScene &&
           spriteCandidateIds == o.spriteCandidateIds &&
           frameToNormalBucket == o.frameToNormalBucket &&
           rotationLookupSize == o.rotationLookupSize &&
           normalBuckets == o.normalBuckets && results == o.results &&
           output == o.output;
  }
};

template <typename T>
static void Append(std::vector<uint8_t>& out, const T* data, size_t count) {
  if (!data) return;
  const uint8_t* p = (const uint8_t*)data;
  out.insert(out.end(), p, p + count * sizeof(T));
}

// Everything a caller can see after the load and after each frame.
static Capture Run(const n256::Synth& s, uint32_t framePixels) {
  Capture c;
  c.loadFlags = mySerum.flags;
  c.width32 = mySerum.width32;
  c.width64 = mySerum.width64;
  c.ntriggers = mySerum.ntriggers;
  c.nocolors = mySerum.nocolors;
  c.has32 = mySerum.frame32 != NULL;
  c.has64 = mySerum.frame64 != NULL;
  c.frameIsScene = g_serumData.frameIsScene;
  c.spriteCandidateIds = g_serumData.spriteCandidateIds;
  c.frameToNormalBucket = g_serumData.frameToNormalBucket;
  c.rotationLookupSize = g_serumData.colorRotationLookupByFrameAndColor.size();
  c.normalBuckets = g_serumData.normalIdentifyBuckets.size();
  const uint32_t rotationWords =
      MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION;
  // The monochrome trigger goes last: it changes what follows.
  std::vector<uint32_t> order;
  for (uint32_t f = 0; f < s.nframes; ++f)
    if (f != n256::kMonoTrigger) order.push_back(f);
  order.push_back(n256::kMonoTrigger);
  order.push_back(n256::kStatic);
  lastTriggerID = 0xffffffff;
  for (uint32_t f : order) {
    std::vector<uint8_t> frame(s.Native(f), s.Native(f) + framePixels);
    frame.shrink_to_fit();
    // The time to the first rotation step depends on the clock; whether there
    // is one does not.
    const uint32_t result = Serum_Colorize(frame.data());
    c.results.push_back(result == IDENTIFY_NO_FRAME ||
                                result == IDENTIFY_SAME_FRAME
                            ? result
                            : (result != 0 ? 1u : 0u));
    c.results.push_back(mySerum.frameID);
    c.results.push_back(mySerum.triggerID);
    c.results.push_back(mySerum.flags);
    c.results.push_back(mySerum.width32);
    c.results.push_back(mySerum.width64);
    if (mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) {
      Append(c.output, mySerum.frame32, (size_t)32 * mySerum.width32);
      Append(c.output, mySerum.rotations32, rotationWords);
      Append(c.output, mySerum.rotationsinframe32,
             (size_t)2 * 32 * mySerum.width32);
    }
    if (mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) {
      Append(c.output, mySerum.frame64, (size_t)64 * mySerum.width64);
      Append(c.output, mySerum.rotations64, rotationWords);
      Append(c.output, mySerum.rotationsinframe64,
             (size_t)2 * 64 * mySerum.width64);
    }
  }
  return c;
}

static n256::Synth Plain128x32(void) {
  n256::Synth s;
  s.is256x64 = false;
  s.compmasks.assign((size_t)s.ncompmasks * n256::kOPx, 0);
  s.compmaskid.assign(s.nframes, 255);
  s.framesprites.assign(s.framesprites.size(), 255);
  return s;
}

}  // namespace memload

static void Test_MemoryLoadMatchesFileLoad(void) {
  const uint8_t requests[] = {
      0,
      FLAG_REQUEST_32P_FRAMES,
      FLAG_REQUEST_64P_FRAMES,
      n256::kBothPlanes,
      FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_FALLBACK,
      FLAG_REQUEST_64P_FRAMES | FLAG_REQUEST_FALLBACK,
      (uint8_t)(n256::kBothPlanes | FLAG_REQUEST_FILL_MODIFIED_ELEMENTS),
  };
  const n256::Synth projects[] = {n256::Synth(), memload::Plain128x32()};
  for (const n256::Synth& s : projects) {
    const uint32_t framePixels = s.is256x64 ? n256::kNPx : n256::kOPx;
    const std::vector<uint8_t> crom = s.Serialize();
    for (uint8_t flags : requests) {
      if (!n256::Load(s, flags, false)) continue;
      const memload::Capture fromFile = memload::Run(s, framePixels);
      n256::Unload();

      // A copy of exactly the cROM's size, released right after the call.
      uint8_t* buffer = (uint8_t*)malloc(crom.size());
      memcpy(buffer, crom.data(), crom.size());
      Serum_SetMaximumUnknownFramesToSkip(0);
      Serum_SetIgnoreUnknownFramesTimeout(0);
      Serum_Frame_Struc* loaded =
          Serum_LoadFromMemory(buffer, crom.size(), flags);
      free(buffer);
      CHECK(loaded == &mySerum);
      CHECK_EQ(Serum_GetLastErrorMessage()[0], '\0');
      if (loaded) {
        CHECK_EQ(g_serumData.is256x64, s.is256x64);
        CHECK(!g_serumData.sceneGenerator->isActive());
        const memload::Capture fromMemory = memload::Run(s, framePixels);
        CHECK(fromMemory == fromFile);
        CHECK(!fromMemory.output.empty() ||
              (s.is256x64 && flags == FLAG_REQUEST_32P_FRAMES));
      }
      Serum_Dispose();
      CHECK(!cromloaded);
    }
  }
  Serum_SetMaximumUnknownFramesToSkip(0);
}

// Scenes set from stored data, as a cROMc load does, start a scene after
// inactivity and hold a last frame as the pup.csv they came from does.
static void Test_SceneDataKeepsAutoStartAndEndHold(void) {
  const std::filesystem::path dir = n256::Dir();
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "scene.csv").string();
  {
    std::ofstream out(path);
    out << "5,2,100,1,0,0,1,0,7,0\n6,2,100,0,0,0,1,0,3,0\n";
  }
  SceneGenerator parsed;
  CHECK(parsed.parseCSV(path));
  CHECK_EQ(parsed.getAutoStartTimer(), 7000u);
  CHECK_EQ(parsed.getAutoStartSceneId(), 5);
  SceneGenerator stored;
  stored.setSceneData(parsed.getSceneData());
  CHECK(stored.isActive());
  CHECK_EQ(stored.getAutoStartTimer(), parsed.getAutoStartTimer());
  CHECK_EQ(stored.getAutoStartSceneId(), parsed.getAutoStartSceneId());
  for (uint16_t sceneId : {5, 6}) {
    uint32_t parsedMs = 0, storedMs = 0;
    CHECK_EQ(stored.getSceneEndHoldDurationMs(sceneId, storedMs),
             parsed.getSceneEndHoldDurationMs(sceneId, parsedMs));
    CHECK_EQ(storedMs, parsedMs);
  }
  uint32_t holdMs = 0;
  CHECK(stored.getSceneEndHoldDurationMs(6, holdMs));
  CHECK_EQ(holdMs, 3000u);
  std::filesystem::remove_all(dir);
}

namespace settings {

// A cROM settings block: the scene of "7,2,100" and the given scaling.
static std::vector<uint8_t> Block(uint32_t contents, uint8_t algorithm,
                                  uint8_t shadowOffset) {
  std::vector<uint8_t> payload;
  const auto put = [&payload](const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    payload.insert(payload.end(), p, p + bytes);
  };
  put(&contents, 4);
  if (contents & kCromSettingsScenes) {
    const uint32_t count = 1;
    const uint16_t scene[3] = {7, 2, 100};
    const uint8_t flags[7] = {0, 0, 0, 1, 0, 0, 0};
    put(&count, 4);
    put(scene, sizeof(scene));
    put(flags, sizeof(flags));
  }
  if (contents & kCromSettingsScaling) {
    put(&algorithm, 1);
    put(&shadowOffset, 1);
  }
  std::vector<uint8_t> block;
  const uint32_t magic = kCromSettingsMagic;
  const uint32_t size = (uint32_t)payload.size();
  block.insert(block.end(), (const uint8_t*)&magic, (const uint8_t*)&magic + 4);
  block.insert(block.end(), (const uint8_t*)&size, (const uint8_t*)&size + 4);
  block.insert(block.end(), payload.begin(), payload.end());
  return block;
}

// Writes the cROM with the block appended, a pup.csv naming another scene and a
// scaling.txt asking for the other settings, and loads it.
static Serum_Frame_Struc* Load(const std::vector<uint8_t>& crom,
                               bool viaConcentrate) {
  const std::filesystem::path dir = n256::Dir();
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / n256::kRom);
  WriteTestFile(dir / n256::kRom / (std::string(n256::kRom) + ".cROM"), crom);
  {
    std::ofstream out(dir / n256::kRom /
                      (std::string(n256::kRom) + ".pup.csv"));
    out << "9,2,100\n";
  }
  {
    std::ofstream out(dir / n256::kRom / "scaling.txt");
    out << "scale2x\nshadow-offset: native\n";
  }
  Serum_SetGenerateCRomC(viaConcentrate);
  Serum_SetMaximumUnknownFramesToSkip(0);
  Serum_SetIgnoreUnknownFramesTimeout(0);
  Serum_Frame_Struc* loaded =
      Serum_Load(dir.string().c_str(), n256::kRom, n256::kBothPlanes);
  Serum_SetGenerateCRomC(true);
  return loaded;
}

static bool HasScene(uint16_t sceneId) {
  uint16_t frameCount, duration;
  bool interruptable, immediate;
  uint8_t repeat, options;
  return g_serumData.sceneGenerator->getSceneInfo(
      sceneId, frameCount, duration, interruptable, immediate, repeat, options);
}

}  // namespace settings

// cROM format version 1: up to MAX_SPRITES_PER_FRAME slots per frame, the
// count in the header, and 16-bit sprite IDs. A sprite with an ID beyond the
// old 8-bit range in a slot beyond the old 32 is found, from the cROM and from
// the cROMc made from it.
static void Test_CromFormat1Sprites(void) {
  n256::Synth s;
  s.SetFormat(1, MAX_SPRITES_PER_FRAME);
  s.CopySprite(0, 256);
  // Slots are filled from the first on (a frame's first empty slot ends
  // them): the shape sprite, not in this frame, in the slots before.
  const uint32_t slot = 100;
  for (uint32_t i = 0; i < slot; ++i)
    s.framesprites[(size_t)n256::kSprite * s.slotsPerFrame + i] = 1;
  s.framesprites[(size_t)n256::kSprite * s.slotsPerFrame + slot] = 256;
  s.SetBB(n256::kSprite, 32, 20, 95, 43, slot);
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    if (!n256::Load(s, n256::kBothPlanes, viaConcentrate != 0)) continue;
    CHECK_EQ(g_serumData.SpritesPerFrame(), (uint32_t)MAX_SPRITES_PER_FRAME);
    CHECK_EQ(g_serumData.nsprites, 257u);
    uint16_t which[MAX_SPRITES_PER_FRAME];
    uint8_t count = 0;
    uint16_t frx[MAX_SPRITES_PER_FRAME], fry[MAX_SPRITES_PER_FRAME],
        spx[MAX_SPRITES_PER_FRAME], spy[MAX_SPRITES_PER_FRAME],
        wid[MAX_SPRITES_PER_FRAME], hei[MAX_SPRITES_PER_FRAME];
    std::vector<uint8_t> frame = s.Frame(n256::kSprite);
    CHECK(Check_Spritesv2(frame.data(), n256::kSprite, which, &count, frx, fry,
                          spx, spy, wid, hei));
    CHECK_EQ(count, 1);
    if (count == 1) {
      CHECK_EQ(which[0], 256);
      CHECK_EQ(frx[0], n256::kSprX);
      CHECK_EQ(fry[0], n256::kSprY);
    }
    n256::Unload();
  }
  // A format version 0 cROM has the old 32 slots.
  n256::Synth old;
  if (n256::Load(old, n256::kBothPlanes, false)) {
    CHECK_EQ(g_serumData.SpritesPerFrame(), (uint32_t)LEGACY_SPRITES_PER_FRAME);
  }
  n256::Unload();
  // A newer format than the library reads is refused, not misread.
  n256::Synth newer;
  newer.SetFormat(SERUM_CROM_FORMAT_VERSION + 1, MAX_SPRITES_PER_FRAME);
  CHECK(n256::LoadUnchecked(newer, n256::kBothPlanes, false, NULL) == NULL);
  n256::Unload();
}

// A cROMc before concentrate version 10 has 8-bit sprite IDs and 32 slots per
// frame; it still loads, and its sprites are found.
static void Test_ConcentrateVersion9Sprites(void) {
  const n256::Synth s;
  if (!n256::Load(s, n256::kBothPlanes, true)) return;
  CHECK_EQ(g_serumData.concentrateFileVersion, SERUM_CONCENTRATE_VERSION);
  const std::filesystem::path dir = n256::Dir() / n256::kRom;
  const std::string cromc =
      (dir / (std::string(n256::kRom) + ".cROMc")).string();
  g_serumData.concentrateFileVersion = 9;
  CHECK(g_serumData.SaveToFile(cromc.c_str()));
  Serum_Dispose();
  std::filesystem::remove(dir / (std::string(n256::kRom) + ".cROM"));
  Serum_SetGenerateCRomC(false);
  CHECK(Serum_Load(n256::Dir().string().c_str(), n256::kRom,
                   n256::kBothPlanes) != NULL);
  Serum_SetGenerateCRomC(true);
  CHECK_EQ(g_serumData.concentrateFileVersion, 9);
  CHECK_EQ(g_serumData.SpritesPerFrame(), (uint32_t)LEGACY_SPRITES_PER_FRAME);
  uint16_t which[MAX_SPRITES_PER_FRAME];
  uint8_t count = 0;
  uint16_t frx[MAX_SPRITES_PER_FRAME], fry[MAX_SPRITES_PER_FRAME],
      spx[MAX_SPRITES_PER_FRAME], spy[MAX_SPRITES_PER_FRAME],
      wid[MAX_SPRITES_PER_FRAME], hei[MAX_SPRITES_PER_FRAME];
  std::vector<uint8_t> frame = s.Frame(n256::kSprite);
  CHECK(Check_Spritesv2(frame.data(), n256::kSprite, which, &count, frx, fry,
                        spx, spy, wid, hei));
  CHECK_EQ(count, 1);
  if (count == 1) CHECK_EQ(which[0], 0);
  n256::Unload();
}

// The scenes and scaling settings of a cROM's settings block are the
// colorization's own: a pup.csv or scaling.txt beside it changes neither, in
// the cROM, in the cROMc made from it, in that cROMc loaded again, or in a
// load from memory. Without the block, both sidecars apply as before.
static void Test_CromSettingsBlock(void) {
  n256::Synth s;
  {
    const std::filesystem::path dir = n256::Dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "scene.csv").string();
    {
      std::ofstream out(path);
      out << "7,2,100\n";
    }
    SceneGenerator generator;
    CHECK(generator.parseCSV(path));
    generator.setDepth(2);
    memset(s.Native(n256::kScene), 0, n256::kNPx);
    CHECK_EQ(generator.generateFrame(7, 1, s.Native(n256::kScene), 1, true),
             0xffff);
  }
  std::vector<uint8_t> crom = s.Serialize();
  const std::vector<uint8_t> block = settings::Block(
      kCromSettingsScenes | kCromSettingsScaling, SERUM_SCALING_LINE_DOUBLING,
      SERUM_SHADOW_OFFSET_PROPORTIONAL);
  crom.insert(crom.end(), block.begin(), block.end());
  const auto expectBlock = [](void) {
    CHECK(g_serumData.scenesFromColorization);
    CHECK(g_serumData.scalingFromColorization);
    CHECK(settings::HasScene(7));
    CHECK(!settings::HasScene(9));
    CHECK_EQ(g_serumData.frameIsScene.size(), n256::kFrameCount);
    if (g_serumData.frameIsScene.size() == n256::kFrameCount)
      CHECK_EQ(g_serumData.frameIsScene[n256::kScene], 1);
    CHECK_EQ(g_serumData.scalingAlgorithm, SERUM_SCALING_LINE_DOUBLING);
    CHECK_EQ(g_serumData.shadowOffsetMode, SERUM_SHADOW_OFFSET_PROPORTIONAL);
  };
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    CHECK(settings::Load(crom, viaConcentrate != 0) != NULL);
    expectBlock();
    if (viaConcentrate) {
      CHECK_EQ(g_serumData.concentrateFileVersion, SERUM_CONCENTRATE_VERSION);
      // The cROMc alone, with both sidecars still beside it.
      Serum_Dispose();
      std::filesystem::remove(n256::Dir() / n256::kRom /
                              (std::string(n256::kRom) + ".cROM"));
      CHECK(Serum_Load(n256::Dir().string().c_str(), n256::kRom,
                       n256::kBothPlanes) != NULL);
      expectBlock();
    }
    Serum_Dispose();
  }
  const std::string csvPath =
      (n256::Dir() / n256::kRom / (std::string(n256::kRom) + ".pup.csv"))
          .string();
  CHECK(Serum_LoadFromMemoryWithScenes(crom.data(), crom.size(),
                                       csvPath.c_str(),
                                       n256::kBothPlanes) != NULL);
  expectBlock();
  Serum_Dispose();

  // A block that sets nothing leaves both sidecars in charge.
  std::vector<uint8_t> empty = s.Serialize();
  const std::vector<uint8_t> nothing = settings::Block(0, 0, 0);
  empty.insert(empty.end(), nothing.begin(), nothing.end());
  CHECK(settings::Load(empty, false) != NULL);
  CHECK(!g_serumData.scenesFromColorization);
  CHECK(!g_serumData.scalingFromColorization);
  CHECK(settings::HasScene(9));
  CHECK(!settings::HasScene(7));
  Serum_Dispose();

  // A block cut short fails the load.
  std::vector<uint8_t> cut = crom;
  cut.resize(cut.size() - 3);
  CHECK(Serum_LoadFromMemory(cut.data(), cut.size(), n256::kBothPlanes) ==
        NULL);
  n256::Unload();
}

namespace lights {

// The lights part of a settings block: two effects, channel 0 defaults to
// effect 1; frame kStatic starts effect 0 on channel 0 and a static color on
// channel 1, frame kDynamic turns channel 1 off and leaves channel 0 alone.
static std::vector<uint8_t> Block(void) {
  std::vector<uint8_t> payload;
  const auto put = [&payload](const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    payload.insert(payload.end(), p, p + bytes);
  };
  const auto putU32 = [&put](uint32_t value) { put(&value, 4); };
  const auto putAction = [&put](uint8_t kind, uint16_t effect, uint32_t color) {
    const uint8_t head[2] = {kind, 0};
    put(head, 2);
    put(&effect, 2);
    put(&color, 4);
  };
  putU32(kCromSettingsLights);
  putU32(2);
  for (uint8_t mode : {12, 33}) {
    const uint8_t head[2] = {mode, 0x80};
    const uint16_t speed = 1500;
    const uint32_t duration = mode == 12 ? 2000 : 0;
    const uint32_t colors[3] = {0x00ff0000, 0x0000ff00, 0xff000000};
    const uint8_t tail[4] = {128, 0, 0, 0};
    put(head, 2);
    put(&speed, 2);
    put(&duration, 4);
    put(colors, sizeof(colors));
    put(tail, 4);
  }
  putAction(SERUM_LIGHT_EFFECT, 1, 0);
  for (int i = 1; i < SERUM_LIGHT_CHANNELS; ++i) putAction(0, 0, 0);
  putU32(2);
  putU32(n256::kStatic);
  putAction(SERUM_LIGHT_EFFECT, 0, 0);
  putAction(SERUM_LIGHT_COLOR, 0, 0x00123456);
  putAction(0, 0, 0);
  putAction(0, 0, 0);
  putU32(n256::kDynamic);
  putAction(0, 0, 0);
  putAction(SERUM_LIGHT_OFF, 0, 0);
  putAction(0, 0, 0);
  putAction(0, 0, 0);
  std::vector<uint8_t> block;
  const uint32_t magic = kCromSettingsMagic;
  const uint32_t size = (uint32_t)payload.size();
  block.insert(block.end(), (const uint8_t*)&magic, (const uint8_t*)&magic + 4);
  block.insert(block.end(), (const uint8_t*)&size, (const uint8_t*)&size + 4);
  block.insert(block.end(), payload.begin(), payload.end());
  return block;
}

}  // namespace lights

// Speaker lights from the settings block: the defaults, and the effects the
// frames start, each once while it plays; through a cROMc as well.
static void Test_CromSettingsLights(void) {
  n256::Synth s;
  std::vector<uint8_t> crom = s.Serialize();
  const std::vector<uint8_t> block = lights::Block();
  crom.insert(crom.end(), block.begin(), block.end());
  for (int viaConcentrate = 0; viaConcentrate < 2; ++viaConcentrate) {
    CHECK(settings::Load(crom, viaConcentrate != 0) != NULL);
    if (viaConcentrate) {
      // The cROMc alone.
      Serum_Dispose();
      std::filesystem::remove(n256::Dir() / n256::kRom /
                              (std::string(n256::kRom) + ".cROM"));
      CHECK(Serum_Load(n256::Dir().string().c_str(), n256::kRom,
                       n256::kBothPlanes) != NULL);
    }
    Serum_Light_Effect effects[SERUM_LIGHT_CHANNELS] = {};
    CHECK_EQ(Serum_GetDefaultLightEffects(effects), 0xfu);
    CHECK_EQ(effects[0].mode, 33);
    CHECK_EQ(effects[0].speed, 1500);
    CHECK_EQ(effects[0].options, 0x80);
    CHECK_EQ(effects[0].brightness, 128);
    CHECK_EQ(effects[0].colors[2], 0xff000000u);
    // A channel without a default is off: static black.
    CHECK_EQ(effects[1].mode, 0);
    CHECK_EQ(effects[1].colors[0], 0u);
    CHECK_EQ(effects[1].brightness, 255);

    CHECK_EQ(Serum_GetLightEffects(effects), 0u);
    n256::Colorize(s.Frame(n256::kStatic));
    CHECK_EQ(Serum_GetLightEffects(effects), 0x3u);
    CHECK_EQ(effects[0].mode, 12);
    CHECK_EQ(effects[0].durationMs, 2000u);
    CHECK_EQ(effects[1].mode, 0);
    CHECK_EQ(effects[1].colors[0], 0x00123456u);
    CHECK_EQ(Serum_GetLightEffects(effects), 0u);
    // Channel 1 off; channel 0 keeps its effect.
    n256::Colorize(s.Frame(n256::kDynamic));
    CHECK_EQ(Serum_GetLightEffects(effects), 0x2u);
    CHECK_EQ(effects[1].colors[0], 0u);
    // kStatic again: channel 0 still plays effect 0, only channel 1 changes.
    n256::Colorize(s.Frame(n256::kStatic));
    CHECK_EQ(Serum_GetLightEffects(effects), 0x2u);
    // A frame without lights changes nothing.
    n256::Colorize(s.Frame(n256::kBackground));
    CHECK_EQ(Serum_GetLightEffects(effects), 0u);
    Serum_Dispose();
  }

  // Without lights: no defaults.
  CHECK(Serum_LoadFromMemory(s.Serialize().data(), s.Serialize().size(),
                             n256::kBothPlanes) != NULL);
  Serum_Light_Effect none[SERUM_LIGHT_CHANNELS] = {};
  CHECK_EQ(Serum_GetDefaultLightEffects(none), 0u);
  Serum_Dispose();

  // An effect index past the effects fails the load.
  std::vector<uint8_t> broken = s.Serialize();
  std::vector<uint8_t> badBlock = block;
  // The default of channel 0: kind at payload offset 4 + 4 + 2 * 24.
  badBlock[8 + 4 + 4 + 2 * 24 + 2] = 7;
  broken.insert(broken.end(), badBlock.begin(), badBlock.end());
  CHECK(Serum_LoadFromMemory(broken.data(), broken.size(), n256::kBothPlanes) ==
        NULL);
  n256::Unload();
}

// Serum_LoadFromMemoryWithScenes(): the scenes of a pup.csv, parsed before the
// lookups are built, give the state and output of Serum_Load() with the same
// pup.csv beside the cROM; without a path it is Serum_LoadFromMemory().
static void Test_MemoryLoadWithScenes(void) {
  const char* const csv = "7,2,100\n";
  n256::Synth s;
  {
    const std::filesystem::path dir = n256::Dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "scene.csv").string();
    {
      std::ofstream out(path);
      out << csv;
    }
    SceneGenerator generator;
    CHECK(generator.parseCSV(path));
    generator.setDepth(2);
    memset(s.Native(n256::kScene), 0, n256::kNPx);
    CHECK_EQ(generator.generateFrame(7, 1, s.Native(n256::kScene), 1, true),
             0xffff);
  }
  const uint32_t framePixels = s.is256x64 ? n256::kNPx : n256::kOPx;
  const std::vector<uint8_t> crom = s.Serialize();
  if (!n256::Load(s, n256::kBothPlanes, false, csv)) return;
  CHECK(g_serumData.sceneGenerator->isActive());
  const memload::Capture fromFile = memload::Run(s, framePixels);
  Serum_Dispose();

  // The pup.csv stays in the test folder; the cROM comes from memory.
  const std::string csvPath =
      (n256::Dir() / n256::kRom / (std::string(n256::kRom) + ".pup.csv"))
          .string();
  Serum_SetMaximumUnknownFramesToSkip(0);
  Serum_SetIgnoreUnknownFramesTimeout(0);
  CHECK(Serum_LoadFromMemoryWithScenes(crom.data(), crom.size(),
                                       csvPath.c_str(),
                                       n256::kBothPlanes) == &mySerum);
  lastTriggerID = 0xffffffff;
  CHECK(g_serumData.sceneGenerator->isActive());
  CHECK_EQ(g_serumData.frameIsScene.size(), n256::kFrameCount);
  if (g_serumData.frameIsScene.size() == n256::kFrameCount)
    CHECK_EQ(g_serumData.frameIsScene[n256::kScene], 1);
  CHECK(memload::Run(s, framePixels) == fromFile);
  Serum_Dispose();

  // No path: as Serum_LoadFromMemory(), without scenes.
  CHECK(Serum_LoadFromMemoryWithScenes(crom.data(), crom.size(), NULL,
                                       n256::kBothPlanes) == &mySerum);
  CHECK(!g_serumData.sceneGenerator->isActive());
  n256::Unload();
}

namespace callbackload {

// Hands out the bytes in pieces of at most `piece`, and ends early at `end`.
struct Source {
  const std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;
  size_t piece = 0;
  size_t end = 0;
  size_t largestRequest = 0;
};

static size_t SERUM_CALLBACK Read(void* userData, uint8_t* buffer,
                                  size_t size) {
  Source& source = *static_cast<Source*>(userData);
  source.largestRequest = std::max(source.largestRequest, size);
  const size_t count =
      std::min({size, source.piece, source.end - source.offset});
  memcpy(buffer, source.bytes->data() + source.offset, count);
  source.offset += count;
  return count;
}

}  // namespace callbackload

// Serum_LoadFromCallback(): the same colorization as from memory, however the
// callback cuts the bytes, without asking for more than the cROM has; one that
// ends early fails as a truncated cROM does, and leaves nothing loaded.
static void Test_CallbackLoadMatchesMemoryLoad(void) {
  const n256::Synth projects[] = {n256::Synth(), memload::Plain128x32()};
  for (const n256::Synth& s : projects) {
    const uint32_t framePixels = s.is256x64 ? n256::kNPx : n256::kOPx;
    const std::vector<uint8_t> crom = s.Serialize();
    Serum_SetMaximumUnknownFramesToSkip(0);
    Serum_SetIgnoreUnknownFramesTimeout(0);
    CHECK(Serum_LoadFromMemory(crom.data(), crom.size(), n256::kBothPlanes) ==
          &mySerum);
    lastTriggerID = 0xffffffff;
    const memload::Capture fromMemory = memload::Run(s, framePixels);
    Serum_Dispose();
    CHECK(!SparseVectorFastPacking());

    for (size_t piece : {(size_t)1 << 30, (size_t)4097, (size_t)7}) {
      callbackload::Source source{&crom, 0, piece, crom.size(), 0};
      Serum_SetMaximumUnknownFramesToSkip(0);
      Serum_SetIgnoreUnknownFramesTimeout(0);
      CHECK(Serum_LoadFromCallback(callbackload::Read, &source, crom.size(),
                                   NULL, n256::kBothPlanes) == &mySerum);
      CHECK_EQ(source.offset, crom.size());
      CHECK(source.largestRequest <= crom.size());
      lastTriggerID = 0xffffffff;
      CHECK(memload::Run(s, framePixels) == fromMemory);
      Serum_Dispose();
    }

    callbackload::Source cut{&crom, 0, 4096, crom.size() / 2, 0};
    CHECK(Serum_LoadFromCallback(callbackload::Read, &cut, crom.size(), NULL,
                                 n256::kBothPlanes) == NULL);
    CHECK(!cromloaded);
    CHECK(Serum_GetLastErrorMessage()[0] != '\0');
    CHECK(!SparseVectorFastPacking());
    callbackload::Source whole{&crom, 0, 4096, crom.size(), 0};
    CHECK(Serum_LoadFromCallback(callbackload::Read, &whole, crom.size() / 2,
                                 NULL, n256::kBothPlanes) == NULL);
    CHECK(whole.offset <= crom.size() / 2);
    CHECK(Serum_LoadFromCallback(NULL, NULL, crom.size(), NULL,
                                 n256::kBothPlanes) == NULL);
  }
  n256::Unload();
}

// A buffer has no directory: nothing is written, whatever the cROMc setting.
static void Test_MemoryLoadWritesNoFile(void) {
  const n256::Synth s;
  const std::vector<uint8_t> crom = s.Serialize();
  const std::filesystem::path dir = n256::Dir();
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::filesystem::path before = std::filesystem::current_path();
  std::filesystem::current_path(dir);
  Serum_SetGenerateCRomC(true);
  CHECK(Serum_LoadFromMemory(crom.data(), crom.size(), n256::kBothPlanes) !=
        NULL);
  CHECK(std::filesystem::is_empty(dir));
  std::filesystem::current_path(before);
  n256::Unload();
}

static void Test_MemoryLoadFailuresAreClean(void) {
  const n256::Synth s;
  const std::vector<uint8_t> crom = s.Serialize();
  const auto expectNothingLoaded = [](void) {
    CHECK(!cromloaded);
    CHECK(!g_serumData.is256x64);
    CHECK(mySerum.frame32 == NULL);
    CHECK(mySerum.frame64 == NULL);
    CHECK(framechecked == NULL);
    CHECK(Serum_GetLastErrorMessage()[0] != '\0');
  };
  const auto expectFailure = [&](const std::vector<uint8_t>& data) {
    // An exact-size heap copy, so a read past the end is a sanitizer report.
    uint8_t* buffer = (uint8_t*)malloc(data.empty() ? 1 : data.size());
    if (!data.empty()) memcpy(buffer, data.data(), data.size());
    CHECK(Serum_LoadFromMemory(buffer, data.size(), n256::kBothPlanes) == NULL);
    free(buffer);
    expectNothingLoaded();
  };

  // A failed call also releases what was loaded before it.
  CHECK(Serum_LoadFromMemory(crom.data(), crom.size(), n256::kBothPlanes) !=
        NULL);
  CHECK(Serum_LoadFromMemory(NULL, 100, n256::kBothPlanes) == NULL);
  expectNothingLoaded();
  CHECK(Serum_LoadFromMemory(crom.data(), crom.size(), n256::kBothPlanes) !=
        NULL);
  expectFailure({});

  // Truncated anywhere: in the name, the header, and every block after it.
  std::vector<size_t> cuts = {1, 63, 64, 67, 68, 72, 84, 100, 104, 108};
  for (size_t cut = 200; cut < crom.size(); cut += crom.size() / 37 + 1)
    cuts.push_back(cut);
  cuts.push_back(crom.size() - 1);
  for (size_t cut : cuts)
    expectFailure(std::vector<uint8_t>(crom.begin(), crom.begin() + cut));

  // Counts the data cannot hold: frames, masks, sprites.
  for (size_t offset : {(size_t)84, (size_t)92, (size_t)96}) {
    std::vector<uint8_t> bad = crom;
    const uint32_t huge = 0xfffffff0u;
    memcpy(&bad[offset], &huge, sizeof(huge));
    expectFailure(bad);
  }
  // A geometry the loader refuses, and bytes that are no cROM at all.
  {
    std::vector<uint8_t> bad = crom;
    const uint32_t width = 192;
    memcpy(&bad[68], &width, sizeof(width));
    expectFailure(bad);
    expectFailure(std::vector<uint8_t>(4096, 0xa5));
    expectFailure(std::vector<uint8_t>(4096, 0));
  }

  // The next valid load is not affected by any of it.
  CHECK(Serum_LoadFromMemory(crom.data(), crom.size(),
                             FLAG_REQUEST_64P_FRAMES) != NULL);
  CHECK(g_serumData.is256x64);
  n256::Colorize(s.Frame(n256::kStatic));
  CHECK_EQ(mySerum.frameID, n256::kStatic);
  EXPECT_ONLY_PLANE64();
  EXPECT_PLANE64([](uint32_t x, uint32_t y) {
    return n256::C64(n256::kStatic, y * n256::kNW + x);
  });
  n256::Unload();
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
    {"upscale/no_yield_to_hd_nothing", Test_LayerDoesNotYieldToHdNothing},
    {"upscale/sprite_region_not_left_stale", Test_SpriteRegionIsNotLeftStale},
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
    {"separator/survives_a_row_wipe", Test_SeparatorSurvivesARowWipe},
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
    {"colorize/finished_frame_already_on_display",
     Test_FinishedFrameAlreadyOnDisplay},
    {"colorize/background_mask", Test_BackgroundShowsThroughItsMask},
    {"colorize/dynamic_zone_colours", Test_DynamicZoneColoursComeFromItsSet},
    {"colorize/dynamic_zone_active_mask",
     Test_DynamicZoneOnlyAppliesWhereActive},
    {"colorize/unlit_dynamic_colour_on_request",
     Test_UnlitDynamicColourBeatsTheBackgroundOnRequest},
    {"colorize/unlit_dynamic_colour_casts_shadow",
     Test_UnlitDynamicColourCastsItsShadow},
    {"crz/write_then_read_round_trips", Test_CrzWriteThenReadRoundTrips},
    {"crz/plain_deflated_zip", Test_CrzIsAPlainDeflatedZip},
    {"crz/failures_are_clean", Test_CrzFailuresAreClean},
    {"crz/real_archive", Test_CrzRealArchive},
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
    {"rotation/dynamic_content_and_shadow_32p",
     Test_DynamicContentAndItsShadowDoNotRotate32},
    {"rotation/dynamic_content_and_shadow_64p",
     Test_DynamicContentAndItsShadowDoNotRotate64},
    {"rotation/dynamic_content_and_shadow_both",
     Test_DynamicContentAndItsShadowDoNotRotateBoth},
    {"n256/loads_and_forgets", Test_N256LoadsAndForgets},
    {"n256/rejects_other_geometry", Test_N256RejectsOtherGeometry},
    {"n256/identification", Test_N256Identification},
    {"n256/static_content", Test_N256StaticContent},
    {"n256/dynamic_content", Test_N256DynamicContent},
    {"n256/background_over_black", Test_N256BackgroundOverBlack},
    {"n256/sprite_detection", Test_N256SpriteDetection},
    {"crom/format_1_sprites", Test_CromFormat1Sprites},
    {"cromc/version_9_sprites", Test_ConcentrateVersion9Sprites},
    {"n256/sprite_drawing", Test_N256SpriteDrawing},
    {"n256/rotation", Test_N256Rotation},
    {"n256/request_both", Test_N256RequestBoth},
    {"n256/request_32p_only", Test_N256Request32pOnly},
    {"n256/request_fallback", Test_N256RequestFallback},
    {"n256/unknown_frame_fallback", Test_N256UnknownFrameFallback},
    {"n256/monochrome_palette", Test_N256MonochromePalette},
    {"n256/without_extra_data", Test_N256WithoutExtraData},
    {"n256/black_frame_is_judged_whole", Test_N256BlackFrameIsJudgedWhole},
    {"n256/scene_lookup", Test_N256SceneLookup},
    {"n256/scene_search_by_mask_and_shape", Test_N256SceneSearchByMaskAndShape},
    {"n256/flag_off_is_unchanged", Test_N256FlagOffIsUnchanged},
    {"sprite/drawn_for_64p_only_request", Test_SpriteIsDrawnFor64pOnlyRequest},
    {"sprite/frame_shadow_inside_sprite_region",
     Test_FrameShadowInsideSpriteRegion},
    {"sprite/scaled_sprite_blocks_reuse", Test_ScaledSpriteBlocksReuse},
    {"sprite/layer_mode_end_to_end", Test_SpriteInLayerModeEndToEnd},
    {"colorize/monochrome_frame_blocks_reuse", Test_MonochromeFrameBlocksReuse},
    {"memory/matches_file_load", Test_MemoryLoadMatchesFileLoad},
    {"scenes/stored_data_keeps_timers", Test_SceneDataKeepsAutoStartAndEndHold},
    {"memory/with_scenes", Test_MemoryLoadWithScenes},
    {"memory/callback", Test_CallbackLoadMatchesMemoryLoad},
    {"crom/settings_block", Test_CromSettingsBlock},
    {"crom/settings_lights", Test_CromSettingsLights},
    {"upscale/background_scene_keeps_text_shape",
     Test_BackgroundSceneKeepsTextShape},
    {"memory/writes_no_file", Test_MemoryLoadWritesNoFile},
    {"memory/failures_are_clean", Test_MemoryLoadFailuresAreClean},
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
