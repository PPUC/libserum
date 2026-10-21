#pragma once

// Internal helper functions of the decoder. Not API: nothing here is exported
// or installed.
//
// They are defined in `serum-accel-ref.cpp`, or in the sources the CMake
// variable `SERUM_ACCEL_DIR` names. Nothing they compute is persisted.
//
// The CRC-32 functions and UnpackValues() may be called from anywhere. All
// others are called with the API mutex held.

#include <cstddef>
#include <cstdint>

class SerumData;

// To be raised when a declaration or a contract in this header changes.
// CMakeLists.txt compares it with the version `SERUM_ACCEL_DIR` states.
// 2: sprite IDs are uint16_t (more than 255 sprites).
#define SERUM_ACCEL_SEAM_VERSION 2

namespace serum_accel {

// ---------------------------------------------------------------------------
// CRC-32
//
// The standard reflected CRC-32 (polynomial 0xEDB88320, initial value and
// final inversion 0xffffffff) of a byte sequence. The stored frame hashes are
// these values, so they must be the same on every platform.
// ---------------------------------------------------------------------------

// Of the `n` bytes at `s`.
uint32_t Crc32(const uint8_t* s, uint32_t n);
// Of the same bytes reduced to lit/unlit: a byte above 1 counts as 1.
uint32_t Crc32Shape(const uint8_t* s, uint32_t n);
// Of the bytes `s[i]` for which `mask[i] == 0`, in order.
uint32_t Crc32Masked(const uint8_t* s, const uint8_t* mask, uint32_t n);
// Of those bytes reduced to lit/unlit.
uint32_t Crc32MaskedShape(const uint8_t* s, const uint8_t* mask, uint32_t n);

// ---------------------------------------------------------------------------
// Sparse vectors
// ---------------------------------------------------------------------------

// Unpacks `count` values of `bitsPerValue` bits (1, 2 or 4) each: value `i` is
// the bits `i * bitsPerValue` and up of `packed`, the lowest bit of a byte
// first. `packed` holds `ceil(count * bitsPerValue / 8)` bytes and `out` takes
// `count`.
void UnpackValues(const uint8_t* packed, uint8_t bitsPerValue, uint8_t* out,
                  size_t count);

// ---------------------------------------------------------------------------
// Sprite detection
// ---------------------------------------------------------------------------

// Called at the start of each Check_Spritesv2(). The bytes behind a frame
// pointer handed to FindSpriteWord() do not change until the next call, and
// may have changed after it.
void BeginSpriteDetection();

struct SpriteWordSearch {
  // The frame the sprite is matched against: the ROM frame, or its lit/unlit
  // copy for a shape-mode sprite. `width * height` bytes, `width >= 4`.
  const uint8_t* frame;
  uint32_t width;
  uint32_t height;
  // Four pixels as one value, the leftmost in the lowest byte.
  uint32_t word;
  // The sprite's bounding box, limits included, with `minX <= maxX - 3` and
  // `minY <= maxY`. It may leave the frame.
  short minX, minY, maxX, maxY;
};
// Called with a place the word was found at. Returns true to end the search.
typedef bool (*SpriteWordVisitor)(void* context, short x, short y);

// Calls `visit` for every (x, y) with `minY <= y <= maxY` and
// `minX <= x <= maxX - 3` whose four bytes from `frame[y * width + x]` on are
// `word`, row by row from the top and from left to right within a row. The
// frame is read as one flat buffer, so in a box that leaves the frame on the
// right a window continues at the start of the next row. Returns true as soon
// as `visit` does, and false when the box is exhausted.
//
// A word that occurs in none of the windows lying within a row of the frame
// (x from 0 to `width - 4`) is not looked for: nothing is visited for it.
bool FindSpriteWord(const SpriteWordSearch& search, SpriteWordVisitor visit,
                    void* context);

// Called once per candidate sprite that has sprite data, before its detection
// areas are compared. Until the next call, SpriteAreaMatches() is asked about
// areas of `sprite` only, and the caller does not read that sprite's
// `spriteoriginal` / `spriteoriginal_opaque`.
void BeginSpriteCandidate(SerumData& data, uint16_t sprite);

// Why SpriteAreaMatches() returned false, for the sprite debug log: a constant
// string and four numbers whose meaning goes with it.
struct SpriteAreaMismatch {
  const char* reason = "";
  uint32_t a = 0, b = 0, c = 0, d = 0;
};

// Whether detection area `detectIndex` (an index into `data.spriteDetectMeta`,
// belonging to `sprite`, the current candidate) is in `frame` with its top
// left corner at (`areaX`, `areaY`). The caller has made sure the area lies
// inside the frame.
//
// The pixels compared are those of the detection rectangle that lie in one of
// the sprite's opaque row segments and that `spriteoriginal_opaque` marks,
// each against `spriteoriginal` -- or, with `shape`, against whether
// `spriteoriginal` is non-zero. An opacity sidecar that does not reach a row
// or a segment the rectangle needs gives false.
//
// `data` is the loaded colorization, unchanged since the last
// ColorizationUnloaded(). `mismatch` may be null.
bool SpriteAreaMatches(SerumData& data, uint32_t detectIndex, uint16_t sprite,
                       bool shape, const uint8_t* frame, uint32_t frameWidth,
                       int areaX, int areaY, SpriteAreaMismatch* mismatch);

// ---------------------------------------------------------------------------
// Colour rotations
// ---------------------------------------------------------------------------

// False only if `data.TryGetColorRotation(frameId, <any colour>, isextra, ...)`
// fails for every colour.
bool PlaneMayRotate(const SerumData& data, uint32_t frameId, bool isextra);

// ---------------------------------------------------------------------------
// The upscale composite
// ---------------------------------------------------------------------------

// One line-doubling composite of the original plane into the plane twice its
// size: destination pixel (x, y) is source pixel (x / 2, y / 2).
struct LineDoubleComposite {
  uint32_t srcWidth;  // the destination is `2 * srcWidth` wide
  // The destination rectangle, limits included. It always holds whole source
  // pixels: x0 and y0 are even, x1 and y1 odd.
  uint32_t x0, y0, x1, y1;
  const uint16_t* srcColours;
  uint16_t* dstColours;
  // One byte per source pixel, or null: a source pixel whose byte is 0 leaves
  // its four destination pixels untouched in every plane below.
  const uint8_t* srcCoverage;
  // Each pair is carried along with the colour unless its source is null. The
  // rotation planes hold two values per pixel.
  const uint8_t* srcLayers;
  uint8_t* dstLayers;
  const uint16_t* srcRotations;
  uint16_t* dstRotations;
  const uint8_t* srcModified;
  uint8_t* dstModified;
};

// Does the whole composite and returns true, or changes nothing and returns
// false; the caller then runs its composite loop.
bool CompositeLineDoubled(const LineDoubleComposite& job);

// ---------------------------------------------------------------------------
// Sprite colours
// ---------------------------------------------------------------------------

enum class SpriteColours {
  Colored,       // data.spritecolored
  Dynamic,       // data.dynasprite4cols
  ColoredExtra,  // data.spritecolored_extra
  DynamicExtra,  // data.dynasprite4cols_extra
};

// The colour array of `sprite` in that store, valid until the store is read
// again, or null; the caller then reads each colour from the store.
const uint16_t* SpriteColourArray(SerumData& data, SpriteColours which,
                                  uint16_t sprite);

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

// Called by Serum_free(): releases everything derived from the loaded
// colorization.
void ColorizationUnloaded();

// To be called after entries were put into
// `data.colorRotationLookupByFrameAndColor`.
void RotationLookupChanged(const SerumData& data);

}  // namespace serum_accel
