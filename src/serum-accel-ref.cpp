// The functions declared in `serum-accel.h`.

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "SerumData.h"
#include "serum-accel.h"
#include "serum.h"

namespace serum_accel {

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

namespace {

struct Crc32Table {
  uint32_t entry[256];
};
// What one byte does to the register: eight steps of the reflected division
// by 0xEDB88320.
constexpr Crc32Table MakeCrc32Table() {
  Crc32Table table{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t ch = i;
    uint32_t crc = 0;
    for (int j = 0; j < 8; j++) {
      const uint32_t b = (ch ^ crc) & 1;
      crc >>= 1;
      if (b != 0) crc = crc ^ 0xEDB88320;
      ch >>= 1;
    }
    table.entry[i] = crc;
  }
  return table;
}
constexpr Crc32Table kCrc32Table = MakeCrc32Table();

}  // namespace

uint32_t Crc32(const uint8_t* s, uint32_t n) {
  uint32_t crc = 0xffffffff;
  for (uint32_t i = 0; i < n; i++)
    crc = (crc >> 8) ^ kCrc32Table.entry[(s[i] ^ crc) & 0xFF];
  return ~crc;
}

uint32_t Crc32Shape(const uint8_t* s, uint32_t n) {
  uint32_t crc = 0xffffffff;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t val = s[i];
    if (val > 1) val = 1;
    crc = (crc >> 8) ^ kCrc32Table.entry[(val ^ crc) & 0xFF];
  }
  return ~crc;
}

uint32_t Crc32Masked(const uint8_t* s, const uint8_t* mask, uint32_t n) {
  uint32_t crc = 0xffffffff;
  for (uint32_t i = 0; i < n; i++) {
    if (mask[i] == 0) crc = (crc >> 8) ^ kCrc32Table.entry[(s[i] ^ crc) & 0xFF];
  }
  return ~crc;
}

uint32_t Crc32MaskedShape(const uint8_t* s, const uint8_t* mask, uint32_t n) {
  uint32_t crc = 0xffffffff;
  for (uint32_t i = 0; i < n; i++) {
    if (mask[i] == 0) {
      uint8_t val = s[i];
      if (val > 1) val = 1;
      crc = (crc >> 8) ^ kCrc32Table.entry[(val ^ crc) & 0xFF];
    }
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Sparse vectors
// ---------------------------------------------------------------------------

void UnpackValues(const uint8_t* packed, uint8_t bitsPerValue, uint8_t* out,
                  size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (bitsPerValue == 1) {
      const bool isSet = (packed[i / 8] & (1u << (i % 8))) != 0;
      out[i] = static_cast<uint8_t>(isSet ? 1 : 0);
    } else if (bitsPerValue == 2) {
      const size_t bitPos = i * 2;
      out[i] =
          static_cast<uint8_t>((packed[bitPos / 8] >> (bitPos % 8)) & 0x3u);
    } else {
      const size_t bitPos = i * 4;
      out[i] =
          static_cast<uint8_t>((packed[bitPos / 8] >> (bitPos % 8)) & 0xFu);
    }
  }
}

// ---------------------------------------------------------------------------
// Sprite detection
// ---------------------------------------------------------------------------

// The set of 4-byte windows present in a ROM frame, used by sprite
// detection to reject a sprite's detection word without scanning for it.
//
// This was a std::unordered_set rebuilt unconditionally on every frame: ~4000
// inserts plus a reserve, ~15 us on a 128x32 frame with GCC -mcpu=cortex-a53 --
// about as much as the entire RGB24 upscale, and paid whether or not any sprite
// was ever looked for. Open addressing over a table allocated once removes the
// heap traffic, and a generation stamp removes the clear, so starting a frame
// costs nothing: ~4 us when the index is needed. Building it lazily removes
// even that on frames with no sprite candidates -- 89% of bdk_294's frames.
struct FrameDwordSet {
  uint32_t* keys = NULL;
  uint32_t* stamps = NULL;  // == gen when the slot is in use for this frame
  uint32_t mask = 0;
  uint32_t shift = 0;
  uint32_t gen = 0;
  // The frame the set was filled from, and its size. `current` is taken back
  // by BeginSpriteDetection(): the bytes behind the pointer may have changed.
  const uint8_t* source = NULL;
  uint32_t width = 0;
  uint32_t height = 0;
  bool current = false;
};
// Detection looks through at most two frames per call: the ROM frame and its
// lit/unlit copy for shape-mode sprites. The second table is allocated the
// first time a frame needs it.
static FrameDwordSet frameDwordSets[2];

static void FreeFrameDwordSet(FrameDwordSet& set) {
  free(set.keys);
  free(set.stamps);
  set = FrameDwordSet();
}

// Sized so the table can never be more than half full: every dword window in
// the frame could be distinct, and a table that fills would loop forever in the
// probe below.
static bool AllocateFrameDwordSet(FrameDwordSet& set, uint32_t width,
                                  uint32_t height) {
  FreeFrameDwordSet(set);
  size_t cap = 1024;
  const size_t want = (size_t)height * (width > 3 ? width - 3 : 1) * 2;
  while (cap < want) cap <<= 1;
  set.mask = (uint32_t)(cap - 1);
  set.shift = 32;
  while (((size_t)1 << (32 - set.shift)) < cap) set.shift--;
  set.keys = (uint32_t*)malloc(cap * sizeof(uint32_t));
  set.stamps = (uint32_t*)calloc(cap, sizeof(uint32_t));
  set.gen = 0;
  set.width = width;
  set.height = height;
  if (!set.keys || !set.stamps) {
    FreeFrameDwordSet(set);
    return false;
  }
  return true;
}

static inline void FrameDwordsBegin(FrameDwordSet& set) {
  if (++set.gen == 0) {  // wrapped: old stamps are indistinguishable
    memset(set.stamps, 0, ((size_t)set.mask + 1) * sizeof(uint32_t));
    set.gen = 1;
  }
}
// Fibonacci hashing. The product's *high* bits are the mixed ones -- a
// multiply only propagates entropy upwards, so masking the low bits instead
// leaves them nearly as correlated as the raw dword. Frame dwords are four
// pixel bytes drawn from a handful of shades, so that mistake clusters them
// hard: it measured 22 us/frame on bdk_294 against 4.8 us for this shift.
static inline uint32_t FrameDwordSlot(const FrameDwordSet& set,
                                      uint32_t value) {
  return (value * 2654435761u) >> set.shift;
}
static inline void FrameDwordsInsert(FrameDwordSet& set, uint32_t value) {
  uint32_t h = FrameDwordSlot(set, value);
  while (set.stamps[h] == set.gen) {
    if (set.keys[h] == value) return;
    h = (h + 1) & set.mask;
  }
  set.stamps[h] = set.gen;
  set.keys[h] = value;
}
static inline bool FrameDwordsContains(const FrameDwordSet& set,
                                       uint32_t value) {
  uint32_t h = FrameDwordSlot(set, value);
  while (set.stamps[h] == set.gen) {
    if (set.keys[h] == value) return true;
    h = (h + 1) & set.mask;
  }
  return false;
}

// The set of `frame`'s windows, filled on the first request for that frame
// since BeginSpriteDetection(). Null when there is no table to hold it; the
// search then goes without.
static const FrameDwordSet* FrameDwordsOf(const uint8_t* frame, uint32_t width,
                                          uint32_t height) {
  FrameDwordSet* set = NULL;
  for (FrameDwordSet& candidate : frameDwordSets) {
    if (candidate.current && candidate.source == frame &&
        candidate.width == width && candidate.height == height)
      return &candidate;
    if (!set && !candidate.current) set = &candidate;
  }
  // Both describe other frames of this call: a third one takes the first.
  if (!set) set = &frameDwordSets[0];
  set->current = false;
  if ((!set->keys || set->width != width || set->height != height) &&
      !AllocateFrameDwordSet(*set, width, height))
    return NULL;
  FrameDwordsBegin(*set);
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t rowBase = y * width;
    uint32_t dword = (uint32_t)(frame[rowBase] << 8) |
                     (uint32_t)(frame[rowBase + 1] << 16) |
                     (uint32_t)(frame[rowBase + 2] << 24);
    for (uint32_t x = 0; x <= width - 4; ++x) {
      dword = (dword >> 8) | (uint32_t)(frame[rowBase + x + 3] << 24);
      FrameDwordsInsert(*set, dword);
    }
  }
  set->source = frame;
  set->current = true;
  return set;
}

void BeginSpriteDetection() {
  for (FrameDwordSet& set : frameDwordSets) set.current = false;
}

bool FindSpriteWord(const SpriteWordSearch& search, SpriteWordVisitor visit,
                    void* context) {
  // A word that is in none of the frame's own windows is not looked for, in
  // any box. Without a table to hold the set, every box is scanned.
  const FrameDwordSet* frameDwords =
      FrameDwordsOf(search.frame, search.width, search.height);
  if (frameDwords && !FrameDwordsContains(*frameDwords, search.word)) {
    return false;
  }
  const uint8_t* Frame = search.frame;
  const uint32_t width = search.width;
  for (short ty = search.minY; ty <= search.maxY; ty++) {
    uint32_t mdword = (uint32_t)(Frame[ty * width + search.minX] << 8) |
                      (uint32_t)(Frame[ty * width + search.minX + 1] << 16) |
                      (uint32_t)(Frame[ty * width + search.minX + 2] << 24);
    for (short tx = search.minX; tx <= search.maxX - 3; tx++) {
      const uint32_t tj = ty * width + tx;
      mdword = (mdword >> 8) | (uint32_t)(Frame[tj + 3] << 24);
      if (mdword == search.word && visit(context, tx, ty)) return true;
    }
  }
  return false;
}

// The candidate's sprite, as read from the sprite store by
// BeginSpriteCandidate().
static SerumData* candidateData = NULL;
static uint8_t candidateSprite = 0;
static const uint8_t* candidateOriginal = NULL;
static const uint8_t* candidateOpaque = NULL;

void BeginSpriteCandidate(SerumData& data, uint8_t sprite) {
  candidateData = &data;
  candidateSprite = sprite;
  candidateOriginal = data.spriteoriginal[sprite];
  candidateOpaque = data.spriteoriginal_opaque[sprite];
}

bool SpriteAreaMatches(SerumData& data, uint32_t detectIndex, uint8_t sprite,
                       bool shape, const uint8_t* frame, uint32_t frameWidth,
                       int areaX, int areaY, SpriteAreaMismatch* mismatch) {
  const auto& detMeta = data.spriteDetectMeta[detectIndex];
  const short detx = static_cast<short>(detMeta.detectX);
  const short dety = static_cast<short>(detMeta.detectY);
  const short detw = static_cast<short>(detMeta.detectWidth);
  const short deth = static_cast<short>(detMeta.detectHeight);
  if (candidateData != &data || candidateSprite != sprite ||
      !candidateOriginal) {
    BeginSpriteCandidate(data, sprite);
  }
  const uint8_t* spriteOriginal = candidateOriginal;
  const uint8_t* spriteOpaque = candidateOpaque;
  const auto no = [mismatch](const char* reason, uint32_t a, uint32_t b,
                             uint32_t c, uint32_t d) {
    if (mismatch) {
      mismatch->reason = reason;
      mismatch->a = a;
      mismatch->b = b;
      mismatch->c = c;
      mismatch->d = d;
    }
    return false;
  };
  for (uint16_t tk = 0; tk < deth; tk++) {
    const uint32_t spriteRow = static_cast<uint32_t>(dety + tk);
    const uint32_t rowIndex =
        static_cast<uint32_t>(sprite) * MAX_SPRITE_HEIGHT + spriteRow;
    if (rowIndex >= data.spriteOpaqueRowSegmentStart.size()) {
      return no("row-sidecar-oob", rowIndex,
                static_cast<uint32_t>(data.spriteOpaqueRowSegmentStart.size()),
                spriteRow, static_cast<uint32_t>(tk));
    }
    const uint32_t segStartIndex = data.spriteOpaqueRowSegmentStart[rowIndex];
    const uint16_t segCount = data.spriteOpaqueRowSegmentCount[rowIndex];
    for (uint16_t seg = 0; seg < segCount; ++seg) {
      const uint32_t segIndex = segStartIndex + seg * 2;
      if (segIndex + 1 >= data.spriteOpaqueSegments.size()) {
        return no("segment-sidecar-oob", segIndex,
                  static_cast<uint32_t>(data.spriteOpaqueSegments.size()),
                  segStartIndex, segCount);
      }
      const uint16_t segmentX = data.spriteOpaqueSegments[segIndex];
      const uint16_t segmentLen = data.spriteOpaqueSegments[segIndex + 1];
      const uint16_t segFrom =
          std::max<uint16_t>(segmentX, static_cast<uint16_t>(detx));
      const uint16_t segTo =
          std::min<uint16_t>(static_cast<uint16_t>(segmentX + segmentLen),
                             static_cast<uint16_t>(detx + detw));
      if (segFrom >= segTo) {
        continue;
      }

      const uint32_t spriteBase = spriteRow * MAX_SPRITE_WIDTH + segFrom;
      const uint32_t frameBase =
          static_cast<uint32_t>(tk + areaY) * frameWidth +
          static_cast<uint32_t>(segFrom - detx + areaX);
      for (uint16_t x = segFrom; x < segTo; ++x) {
        const uint32_t spriteOffset = spriteBase + (x - segFrom);
        const uint32_t frameOffset = frameBase + (x - segFrom);
        if (spriteOpaque[spriteOffset] == 0) {
          continue;
        }
        const uint8_t expectedValue =
            shape ? static_cast<uint8_t>(spriteOriginal[spriteOffset] > 0)
                  : spriteOriginal[spriteOffset];
        if (expectedValue != frame[frameOffset]) {
          return no("opaque-run-mismatch", spriteOffset, frameOffset,
                    expectedValue, frame[frameOffset]);
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Colour rotations
// ---------------------------------------------------------------------------

bool PlaneMayRotate(const SerumData&, uint32_t, bool) { return true; }

// ---------------------------------------------------------------------------
// The upscale composite
// ---------------------------------------------------------------------------

// The caller's composite loop does it.
bool CompositeLineDoubled(const LineDoubleComposite&) { return false; }

// ---------------------------------------------------------------------------
// Sprite colours
// ---------------------------------------------------------------------------

// The caller reads each colour from its store.
const uint16_t* SpriteColourArray(SerumData&, SpriteColours, uint8_t) {
  return nullptr;
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

void ColorizationUnloaded() {
  for (FrameDwordSet& set : frameDwordSets) FreeFrameDwordSet(set);
  candidateData = NULL;
  candidateOriginal = NULL;
  candidateOpaque = NULL;
}

void RotationLookupChanged(const SerumData&) {}

}  // namespace serum_accel
