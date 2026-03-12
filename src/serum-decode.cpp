#define __STDC_WANT_LIB_EXT1_ 1

#include "serum-decode.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SerumData.h"
#include "TimeUtils.h"
#include "serum-version.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#define strcasecmp _stricmp
#else

#if not defined(__STDC_LIB_EXT1__)

// trivial implementation of the secure string functions if not directly
// supported by the compiler these do not perform all security checks and can be
// improved for sure
int strcpy_s(char* dest, size_t destsz, const char* src) {
  if ((dest == NULL) || (src == NULL)) return 1;
  if (strlen(src) >= destsz) return 1;
  strcpy(dest, src);
  return 0;
}

int strcat_s(char* dest, size_t destsz, const char* src) {
  if ((dest == NULL) || (src == NULL)) return 1;
  if (strlen(dest) + strlen(src) >= destsz) return 1;
  strcat(dest, src);
  return 0;
}

#endif
#endif

#define PUP_TRIGGER_REPEAT_TIMEOUT 500  // 500 ms
#define PUP_TRIGGER_MAX_THRESHOLD 50000
#define MONOCHROME_TRIGGER_ID 65432
#define MONOCHROME_PALETTE_TRIGGER_ID 65431

#pragma warning(disable : 4996)

Serum_LogCallback logCallback = nullptr;
const void* logUserData = nullptr;

void Log(const char* format, ...) {
  if (!logCallback) {
    return;
  }

  va_list args;
  va_start(args, format);
  (*(logCallback))(format, args, logUserData);
  va_end(args);
}

static SerumData g_serumData;
uint16_t sceneFrameCount = 0;
uint16_t sceneCurrentFrame = 0;
uint16_t sceneDurationPerFrame = 0;
bool sceneInterruptable = false;
bool sceneStartImmediately = false;
bool sceneIsLastBackgroundFrame = false;
uint8_t sceneRepeatCount = 0;
uint8_t sceneOptionFlags = 0;
uint32_t sceneEndHoldUntilMs = 0;
uint32_t sceneEndHoldDurationMs = 0;
uint8_t sceneFrame[256 * 64] = {0};
uint8_t lastFrame[256 * 64] = {0};
uint32_t lastFrameId = 0;  // last frame ID identified
uint32_t lastFrameCrcForSpriteCache = 0;
bool lastFrameSpriteCacheValid = false;
uint32_t lastFrameSpriteCacheFrameId = IDENTIFY_NO_FRAME;
uint32_t lastFrameSpriteCacheCrc = 0;
uint8_t lastFrameSpriteCacheIds[MAX_SPRITES_PER_FRAME] = {255};
uint8_t lastFrameSpriteCacheCount = 0;
uint16_t lastFrameSpriteCacheFrameX[MAX_SPRITES_PER_FRAME] = {0};
uint16_t lastFrameSpriteCacheFrameY[MAX_SPRITES_PER_FRAME] = {0};
uint16_t lastFrameSpriteCacheSpriteX[MAX_SPRITES_PER_FRAME] = {0};
uint16_t lastFrameSpriteCacheSpriteY[MAX_SPRITES_PER_FRAME] = {0};
uint16_t lastFrameSpriteCacheWidth[MAX_SPRITES_PER_FRAME] = {0};
uint16_t lastFrameSpriteCacheHeight[MAX_SPRITES_PER_FRAME] = {0};
uint16_t sceneBackgroundFrame[256 * 64] = {0};
bool monochromeMode = false;
bool monochromePaletteMode = false;
bool showStatusMessages = false;
bool keepTriggersInternal = false;

const int pathbuflen = 4096;
const uint32_t MAX_FRAME_WIDTH = 256;
const uint32_t MAX_FRAME_HEIGHT = 64;

const uint32_t MAX_NUMBER_FRAMES = 0x7fffffff;

const uint16_t greyscale_4[4] = {
    0x0000,  // Black (0, 0, 0)
    0x528A,  // Dark grey (~1/3 intensity)
    0xAD55,  // Light grey (~2/3 intensity)
    0xFFFF   // White (31, 63, 31)
};

const uint16_t greyscale_16[16] = {
    0x0000,  // Black (0, 0, 0)
    0x1082,  // 1/15
    0x2104,  // 2/15
    0x3186,  // 3/15
    0x4208,  // 4/15
    0x528A,  // 5/15
    0x630C,  // 6/15
    0x738E,  // 7/15
    0x8410,  // 8/15
    0x9492,  // 9/15
    0xA514,  // 10/15
    0xB596,  // 11/15
    0xC618,  // 12/15
    0xD69A,  // 13/15
    0xE71C,  // 14/15
    0xFFFF   // White (31, 63, 31)
};
uint16_t monochromePaletteV2[16] = {0};
uint8_t monochromePaletteV2Length = 0;

uint32_t Serum_RenderScene(void);
static void BuildFrameLookupVectors(void);
static uint64_t MakeFrameSignature(uint8_t mask, uint8_t shape, uint32_t hash);
static uint64_t MakeSceneFrameKey(uint16_t sceneId, uint8_t group,
                                  uint16_t frameIndex);
static uint32_t MakeSceneGroupKey(uint16_t sceneId, uint8_t group);
static void InitFrameLookupRuntimeStateFromStoredData(void);
static void StopV2ColorRotations(void);
static bool CaptureMonochromePaletteFromFrameV2(uint32_t frameId);
static bool IsFullBlackFrame(const uint8_t* frame, uint32_t size);
static void ConfigureSceneEndHold(uint16_t sceneId, bool interruptable,
                                  uint8_t sceneOptions);
static void ForceNormalFrameRefreshAfterSceneEnd(void);
static void EnsureValidOutputDimensions(void);
static bool ValidateLoadedGeometry(bool isV2, const char* sourceTag);
static void RebuildSpriteSizeCaches(void);
static void RebuildRotationLookupTablesV2(void);
static void RebuildSpriteDetectionPlanV2(void);

struct SceneResumeState {
  uint16_t nextFrame = 0;
  uint32_t timestampMs = 0;
};
static std::unordered_map<uint32_t, SceneResumeState> g_sceneResumeState;
static constexpr uint32_t SCENE_RESUME_WINDOW_MS = 8000;

// variables
bool cromloaded = false;  // is there a crom loaded?
bool generateCRomC = true;
uint32_t lastfound = 0;         // last frame ID identified (current stream)
uint32_t lastfound_normal = 0;  // last frame ID for non-scene frames
uint32_t lastfound_scene = 0;   // last frame ID for scene frames
uint32_t lastframe_full_crc_normal = 0;
uint32_t lastframe_found = GetMonotonicTimeMs();
uint32_t lastTriggerID = 0xffffffff;  // last trigger ID found
uint32_t lasttriggerTimestamp = 0;
bool isrotation = true;     // are there rotations to send
bool crc32_ready = false;   // is the crc32 table filled?
uint32_t crc32_table[256];  // initial table
bool* framechecked = NULL;  // are these frames checked?
uint16_t ignoreUnknownFramesTimeout = 0;
uint8_t maxFramesToSkip = 0;
uint8_t framesSkippedCounter = 0;
uint8_t standardPalette[PALETTE_SIZE];
uint8_t standardPaletteLength = 0;
uint32_t colorshifts[MAX_COLOR_ROTATIONS];         // how many color we shifted
uint32_t colorshiftinittime[MAX_COLOR_ROTATIONS];  // when was the tick for this
uint32_t colorshifts32[MAX_COLOR_ROTATION_V2];  // how many color we shifted for
                                                // extra res
uint32_t colorshiftinittime32[MAX_COLOR_ROTATION_V2];  // when was the tick for
                                                       // this for extra res
uint32_t colorshifts64[MAX_COLOR_ROTATION_V2];  // how many color we shifted for
                                                // extra res
uint32_t colorshiftinittime64[MAX_COLOR_ROTATION_V2];  // when was the tick for
                                                       // this for extra res
uint32_t colorrotseruminit;  // initial time when all the rotations started
uint32_t
    colorrotnexttime[MAX_COLOR_ROTATIONS];  // next time of the next rotation
uint32_t colorrotnexttime32[MAX_COLOR_ROTATION_V2];  // next time of the next
                                                     // rotation
uint32_t colorrotnexttime64[MAX_COLOR_ROTATION_V2];  // next time of the next
                                                     // rotation
// rotation
bool enabled = true;  // is colorization enabled?

bool isoriginalrequested =
    true;  // are the original resolution frames requested by the caller
bool isextrarequested =
    false;  // are the extra resolution frames requested by the caller

uint32_t
    rotationnextabsolutetime[MAX_COLOR_ROTATIONS];  // cumulative time for the
                                                    // next rotation for each
                                                    // color rotation

Serum_Frame_Struc mySerum;  // structure to keep communicate colorization data

uint8_t* frameshape = NULL;  // memory for shape mode conversion of ythe frame

SERUM_API void Serum_SetLogCallback(Serum_LogCallback callback,
                                    const void* userData) {
  g_serumData.SetLogCallback(callback, userData);
  logCallback = callback;
  logUserData = userData;
}

#if defined(_WIN32) || defined(_WIN64) || defined(__APPLE__) || \
    defined(__ANDROID__)
bool is_real_machine() { return false; }
#else
// On a real pinball machine, we have a lot of frames, the colorization might
// not handle or display correctly as they don't appear on a virtual pinball
// machine:
// - error messages
// - system diagnostics
// - settings menu
// - tools like motor adjustments
// - coin door open warnings
// - older or patched ROM versions
// - ...
// Falling back to monochrome in such situations might help.
// As a simpliefied approach, we assume that a Raspberry Pi running Linux is
// used to handle Serum on a real pinball machine. If there is other hardware,
// it needs to be added here.
bool is_real_machine() {
  static std::optional<bool> cached;
  if (cached.has_value()) return *cached;

  std::ifstream model_file("/proc/device-tree/model");
  if (model_file.is_open()) {
    std::string model;
    std::getline(model_file, model);
    cached = (model.find("Raspberry") != std::string::npos);
  } else {
    cached = false;
  }
  return *cached;
}
#endif

static bool ValidateLoadedGeometry(bool isV2, const char* sourceTag) {
  auto is_valid_frame = [](uint32_t width, uint32_t height) -> bool {
    return width > 0 && height > 0 && width <= MAX_FRAME_WIDTH &&
           height <= MAX_FRAME_HEIGHT;
  };

  if (!is_valid_frame(g_serumData.frameWidth, g_serumData.frameHeight)) {
    Log("Invalid frame size in %s: %ux%u", sourceTag, g_serumData.frameWidth,
        g_serumData.frameHeight);
    return false;
  }

  if (isV2) {
    if (g_serumData.frameHeight != 32 && g_serumData.frameHeight != 64) {
      Log("Invalid base frame height in %s: %u (expected 32 or 64)", sourceTag,
          g_serumData.frameHeight);
      return false;
    }

    const bool hasExtra =
        (g_serumData.extraFrameWidth > 0 || g_serumData.extraFrameHeight > 0);
    if (hasExtra) {
      if (!is_valid_frame(g_serumData.extraFrameWidth,
                          g_serumData.extraFrameHeight)) {
        Log("Invalid extra frame size in %s: %ux%u", sourceTag,
            g_serumData.extraFrameWidth, g_serumData.extraFrameHeight);
        return false;
      }
      if (g_serumData.extraFrameHeight != 32 &&
          g_serumData.extraFrameHeight != 64) {
        Log("Invalid extra frame height in %s: %u (expected 32 or 64)",
            sourceTag, g_serumData.extraFrameHeight);
        return false;
      }
    }
  } else {
    if (g_serumData.classicColorCount == 0 ||
        g_serumData.classicColorCount > 64) {
      Log("Invalid palette size in %s: nccolors=%u", sourceTag,
          g_serumData.classicColorCount);
      return false;
    }
  }

  return true;
}

static std::string to_lower(const std::string& str) {
  std::string lower_str;
  std::transform(str.begin(), str.end(), std::back_inserter(lower_str),
                 [](unsigned char c) { return std::tolower(c); });
  return lower_str;
}

static std::optional<std::string> find_case_insensitive_file(
    const std::string& dir_path, const std::string& filename) {
  std::string path_copy =
      dir_path;  // make a copy to avoid modifying the original string

  if (!std::filesystem::exists(path_copy) ||
      !std::filesystem::is_directory(path_copy)) {
    Log("Directory does not exist: %s", dir_path.c_str());
    return std::nullopt;
  }

  std::string lower_filename = to_lower(filename);

  try {
    for (const auto& entry : std::filesystem::directory_iterator(path_copy)) {
      if (entry.is_regular_file()) {
        std::string entry_filename = entry.path().filename().string();
        if (to_lower(entry_filename) == lower_filename)
          return entry.path().string();
      }
    }
  } catch (const std::filesystem::filesystem_error& e) {
    Log("Filesystem error when accessing %s: %s", dir_path.c_str(), e.what());
    return std::nullopt;
  }

  Log("File %s not found in directory %s", filename.c_str(), dir_path.c_str());
  return std::nullopt;
}

void Free_element(void** ppElement) {
  // free a malloc block and set its pointer to NULL
  if (ppElement && *ppElement) {
    free(*ppElement);
    *ppElement = NULL;
  }
}

static void RebuildSpriteSizeCaches(void) {
  if (g_serumData.spriteWidthV1.size() == g_serumData.spriteCount &&
      g_serumData.spriteHeightV1.size() == g_serumData.spriteCount &&
      g_serumData.spriteWidthV2.size() == g_serumData.spriteCount &&
      g_serumData.spriteHeightV2.size() == g_serumData.spriteCount) {
    return;
  }

  g_serumData.spriteWidthV1.assign(g_serumData.spriteCount, 0);
  g_serumData.spriteHeightV1.assign(g_serumData.spriteCount, 0);
  g_serumData.spriteWidthV2.assign(g_serumData.spriteCount, 0);
  g_serumData.spriteHeightV2.assign(g_serumData.spriteCount, 0);

  for (uint32_t i = 0; i < g_serumData.spriteCount; ++i) {
    if (g_serumData.spritedescriptionso.hasData(i)) {
      uint8_t* spriteData = g_serumData.spritedescriptionso[i];
      int maxW = 0;
      int maxH = 0;
      for (int y = 0; y < MAX_SPRITE_SIZE; ++y) {
        for (int x = 0; x < MAX_SPRITE_SIZE; ++x) {
          if (spriteData[y * MAX_SPRITE_SIZE + x] < 255) {
            if (x + 1 > maxW) maxW = x + 1;
            if (y + 1 > maxH) maxH = y + 1;
          }
        }
      }
      g_serumData.spriteWidthV1[i] = static_cast<uint16_t>(maxW);
      g_serumData.spriteHeightV1[i] = static_cast<uint16_t>(maxH);
    }

    if (g_serumData.spriteoriginal.hasData(i)) {
      uint8_t* spriteData = g_serumData.spriteoriginal[i];
      int maxW = 0;
      int maxH = 0;
      for (int y = 0; y < MAX_SPRITE_HEIGHT; ++y) {
        for (int x = 0; x < MAX_SPRITE_WIDTH; ++x) {
          if (spriteData[y * MAX_SPRITE_WIDTH + x] < 255) {
            if (x + 1 > maxW) maxW = x + 1;
            if (y + 1 > maxH) maxH = y + 1;
          }
        }
      }
      g_serumData.spriteWidthV2[i] = static_cast<uint16_t>(maxW);
      g_serumData.spriteHeightV2[i] = static_cast<uint16_t>(maxH);
    }
  }
}

static void RebuildRotationLookupTablesV2(void) {
  if (g_serumData.SerumVersion != SERUM_V2 || g_serumData.frameCount == 0) {
    g_serumData.rotationLookupColorsV2.clear();
    g_serumData.rotationLookupMetaV2.clear();
    g_serumData.rotationLookupColorsV2Extra.clear();
    g_serumData.rotationLookupMetaV2Extra.clear();
    return;
  }

  if (g_serumData.rotationLookupColorsV2.size() == g_serumData.frameCount &&
      g_serumData.rotationLookupMetaV2.size() == g_serumData.frameCount &&
      g_serumData.rotationLookupColorsV2Extra.size() ==
          g_serumData.frameCount &&
      g_serumData.rotationLookupMetaV2Extra.size() == g_serumData.frameCount) {
    return;
  }

  g_serumData.rotationLookupColorsV2.assign(g_serumData.frameCount, {});
  g_serumData.rotationLookupMetaV2.assign(g_serumData.frameCount, {});
  g_serumData.rotationLookupColorsV2Extra.assign(g_serumData.frameCount, {});
  g_serumData.rotationLookupMetaV2Extra.assign(g_serumData.frameCount, {});

  auto buildLookup = [](uint16_t* rotations, std::vector<uint16_t>& colors,
                        std::vector<uint16_t>& meta) {
    if (!rotations) return;
    colors.reserve(64);
    meta.reserve(64);
    for (uint16_t rotationId = 0; rotationId < MAX_COLOR_ROTATION_V2;
         ++rotationId) {
      const uint16_t length = rotations[rotationId * MAX_LENGTH_COLOR_ROTATION];
      if (length == 0) continue;
      for (uint16_t pos = 0; pos < length; ++pos) {
        const uint16_t color =
            rotations[rotationId * MAX_LENGTH_COLOR_ROTATION + 2 + pos];
        if (std::find(colors.begin(), colors.end(), color) != colors.end()) {
          continue;
        }
        colors.push_back(color);
        meta.push_back(static_cast<uint16_t>((rotationId << 8) | (pos & 0xff)));
      }
    }
  };

  for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
    buildLookup(g_serumData.colorrotations_v2[frameId],
                g_serumData.rotationLookupColorsV2[frameId],
                g_serumData.rotationLookupMetaV2[frameId]);
    buildLookup(g_serumData.colorrotations_v2_extra[frameId],
                g_serumData.rotationLookupColorsV2Extra[frameId],
                g_serumData.rotationLookupMetaV2Extra[frameId]);
  }
}

static void RebuildSpriteDetectionPlanV2(void) {
  if (g_serumData.SerumVersion != SERUM_V2 || g_serumData.frameCount == 0) {
    g_serumData.spriteDetectionPlanV2.clear();
    return;
  }

  if (g_serumData.spriteDetectionPlanV2.size() == g_serumData.frameCount) {
    return;
  }

  g_serumData.spriteDetectionPlanV2.assign(g_serumData.frameCount, {});
  for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
    uint8_t* frameSprites = g_serumData.framesprites[frameId];
    uint16_t* frameSpriteBB = g_serumData.framespriteBB[frameId];
    auto& plan = g_serumData.spriteDetectionPlanV2[frameId];
    for (uint8_t i = 0; i < MAX_SPRITES_PER_FRAME && frameSprites[i] < 255;
         ++i) {
      const uint8_t spriteId = frameSprites[i];
      SerumData::SpriteDetectionPlanEntryV2 entry;
      entry.spriteId = spriteId;
      entry.minX = frameSpriteBB[i * 4];
      entry.minY = frameSpriteBB[i * 4 + 1];
      entry.maxX = frameSpriteBB[i * 4 + 2];
      entry.maxY = frameSpriteBB[i * 4 + 3];
      entry.spriteWidth = (spriteId < g_serumData.spriteWidthV2.size())
                              ? g_serumData.spriteWidthV2[spriteId]
                              : 0;
      entry.spriteHeight = (spriteId < g_serumData.spriteHeightV2.size())
                               ? g_serumData.spriteHeightV2[spriteId]
                               : 0;
      entry.shapeCheck = g_serumData.sprshapemode[spriteId][0] > 0;
      plan.push_back(entry);
    }
  }
}

void Serum_free(void) {
  // Free the memory for a full Serum whatever the format version
  g_serumData.Clear();

  Free_element((void**)&framechecked);
  Free_element((void**)&mySerum.frame);
  Free_element((void**)&mySerum.frame32);
  Free_element((void**)&mySerum.frame64);
  Free_element((void**)&mySerum.palette);
  Free_element((void**)&mySerum.rotations);
  Free_element((void**)&mySerum.rotations32);
  Free_element((void**)&mySerum.rotations64);
  Free_element((void**)&mySerum.rotationsinframe32);
  Free_element((void**)&mySerum.rotationsinframe64);
  Free_element((void**)&mySerum.modifiedelements32);
  Free_element((void**)&mySerum.modifiedelements64);
  Free_element((void**)&frameshape);
  cromloaded = false;
  lastfound = 0;
  lastfound_normal = 0;
  lastfound_scene = 0;
  lastframe_full_crc_normal = 0;
  sceneEndHoldUntilMs = 0;
  sceneEndHoldDurationMs = 0;
  monochromeMode = false;
  monochromePaletteMode = false;
  monochromePaletteV2Length = 0;
  lastFrameCrcForSpriteCache = 0;
  lastFrameSpriteCacheValid = false;
  lastFrameSpriteCacheFrameId = IDENTIFY_NO_FRAME;
  lastFrameSpriteCacheCrc = 0;
  lastFrameSpriteCacheCount = 0;
  memset(lastFrameSpriteCacheIds, 255, sizeof(lastFrameSpriteCacheIds));
  g_sceneResumeState.clear();

  g_serumData.sceneGenerator->Reset();
}

SERUM_API const char* Serum_GetVersion() { return SERUM_VERSION; }

SERUM_API const char* Serum_GetMinorVersion() { return SERUM_MINOR_VERSION; }

void CRC32encode(void)  // initiating the CRC table, must be called at startup
{
  for (int i = 0; i < 256; i++) {
    uint32_t ch = i;
    uint32_t crc = 0;
    for (int j = 0; j < 8; j++) {
      uint32_t b = (ch ^ crc) & 1;
      crc >>= 1;
      if (b != 0) crc = crc ^ 0xEDB88320;
      ch >>= 1;
    }
    crc32_table[i] = crc;
  }
  crc32_ready = true;
}

uint32_t crc32_fast(uint8_t* s, uint32_t n)
// computing a buffer CRC32, "CRC32encode()" must have been called before the
// first use version with no mask nor shapemode
{
  uint32_t crc = 0xffffffff;
  for (int i = 0; i < (int)n; i++)
    crc = (crc >> 8) ^ crc32_table[(s[i] ^ crc) & 0xFF];
  return ~crc;
}

uint32_t crc32_fast_shape(uint8_t* s, uint32_t n)
// computing a buffer CRC32, "CRC32encode()" must have been called before the
// first use version with shapemode and no mask
{
  uint32_t crc = 0xffffffff;
  for (int i = 0; i < (int)n; i++) {
    uint8_t val = s[i];
    if (val > 1) val = 1;
    crc = (crc >> 8) ^ crc32_table[(val ^ crc) & 0xFF];
  }
  return ~crc;
}

uint32_t crc32_fast_mask(uint8_t* source, uint8_t* mask, uint32_t n)
// computing a buffer CRC32 on the non-masked area, "CRC32encode()" must have
// been called before the first use version with a mask and no shape mode
{
  uint32_t crc = 0xffffffff;
  for (uint32_t i = 0; i < n; i++) {
    if (mask[i] == 0) crc = (crc >> 8) ^ crc32_table[(source[i] ^ crc) & 0xFF];
  }
  return ~crc;
}

uint32_t crc32_fast_mask_shape(uint8_t* source, uint8_t* mask, uint32_t n)
// computing a buffer CRC32 on the non-masked area, "CRC32encode()" must have
// been called before the first use version with a mask and shape mode
{
  uint32_t crc = 0xffffffff;
  for (uint32_t i = 0; i < n; i++) {
    if (mask[i] == 0) {
      uint8_t val = source[i];
      if (val > 1) val = 1;
      crc = (crc >> 8) ^ crc32_table[(val ^ crc) & 0xFF];
    }
  }
  return ~crc;
}

uint32_t calc_crc32(uint8_t* source, uint8_t mask, uint32_t n, uint8_t Shape) {
  const uint32_t pixels =
      g_serumData.isNative256x64
          ? (256 * 64)
          : (g_serumData.frameWidth * g_serumData.frameHeight);
  if (mask < 255) {
    uint8_t* pmask = g_serumData.compmasks[mask];
    if (Shape == 1)
      return crc32_fast_mask_shape(source, pmask, pixels);
    else
      return crc32_fast_mask(source, pmask, pixels);
  } else if (Shape == 1)
    return crc32_fast_shape(source, pixels);
  return crc32_fast(source, pixels);
}

bool unzip_crz(const char* const filename, const char* const extractpath,
               char* cromname, int cromsize) {
  bool ok = true;
  mz_zip_archive zip_archive = {0};

  if (!mz_zip_reader_init_file(&zip_archive, filename, 0)) {
    return false;
  }

  int num_files = mz_zip_reader_get_num_files(&zip_archive);

  if (num_files == 0 ||
      !mz_zip_reader_get_filename(&zip_archive, 0, cromname, cromsize)) {
    mz_zip_reader_end(&zip_archive);
    return false;
  }

  for (int i = 0; i < num_files; i++) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat)) continue;

    char dstPath[pathbuflen];
    if (strcpy_s(dstPath, pathbuflen, extractpath)) goto fail;
    if (strcat_s(dstPath, pathbuflen, file_stat.m_filename)) goto fail;

    mz_zip_reader_extract_file_to_file(&zip_archive, file_stat.m_filename,
                                       dstPath, 0);
  }

  goto nofail;
fail:
  ok = false;
nofail:

  mz_zip_reader_end(&zip_archive);

  return ok;
}

void Full_Reset_ColorRotations(void) {
  memset(colorshifts, 0, MAX_COLOR_ROTATIONS * sizeof(uint32_t));
  colorrotseruminit = GetMonotonicTimeMs();
  for (int ti = 0; ti < MAX_COLOR_ROTATIONS; ti++)
    colorshiftinittime[ti] = colorrotseruminit;
  memset(colorshifts32, 0, MAX_COLOR_ROTATION_V2 * sizeof(uint32_t));
  memset(colorshifts64, 0, MAX_COLOR_ROTATION_V2 * sizeof(uint32_t));
  for (int ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
    colorshiftinittime32[ti] = colorrotseruminit;
    colorshiftinittime64[ti] = colorrotseruminit;
  }
}

uint32_t max(uint32_t v1, uint32_t v2) {
  if (v1 > v2) return v1;
  return v2;
}

uint32_t min(uint32_t v1, uint32_t v2) {
  if (v1 < v2) return v1;
  return v2;
}

long serum_file_length;

bool Serum_SaveConcentrate(const char* filename) {
  if (!cromloaded || is_real_machine()) return false;
  BuildFrameLookupVectors();

  std::string concentratePath;

  // Remove extension and add .cROMc
  if (const char* dot = strrchr(filename, '.')) {
    concentratePath = std::string(filename, dot);
  }

  concentratePath += ".cROMc";

  return g_serumData.SaveToFile(concentratePath.c_str());
}

static Serum_Frame_Struc* Serum_LoadConcentratePrepared(const uint8_t flags) {
  // Update mySerum structure
  mySerum.SerumVersion = g_serumData.SerumVersion;
  mySerum.flags = flags;
  mySerum.nocolors = g_serumData.colorCount;

  if (!ValidateLoadedGeometry(g_serumData.SerumVersion == SERUM_V2, "cROMc")) {
    Log("Failed to vaildate cROMc geometry.");
    enabled = false;
    return NULL;
  }

  // Set requested frame types
  isoriginalrequested = false;
  isextrarequested = false;
  mySerum.width32 = 0;
  mySerum.width64 = 0;

  if (SERUM_V2 == g_serumData.SerumVersion) {
    if (g_serumData.frameHeight == 32) {
      if (flags & FLAG_REQUEST_32P_FRAMES) {
        isoriginalrequested = true;
        mySerum.width32 = g_serumData.frameWidth;
      }
      if (flags & FLAG_REQUEST_64P_FRAMES) {
        isextrarequested = true;
        mySerum.width64 = g_serumData.extraFrameWidth;
      }
    } else {
      if (flags & FLAG_REQUEST_64P_FRAMES) {
        isoriginalrequested = true;
        mySerum.width64 = g_serumData.frameWidth;
      }
      if (flags & FLAG_REQUEST_32P_FRAMES) {
        isextrarequested = true;
        mySerum.width32 = g_serumData.extraFrameWidth;
      }
    }

    if (flags & FLAG_REQUEST_32P_FRAMES) {
      mySerum.frame32 =
          (uint16_t*)malloc(32 * mySerum.width32 * sizeof(uint16_t));
      mySerum.rotations32 = (uint16_t*)malloc(
          MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * sizeof(uint16_t));
      mySerum.rotationsinframe32 =
          (uint16_t*)malloc(2 * 32 * mySerum.width32 * sizeof(uint16_t));
      if (flags & FLAG_REQUEST_FILL_MODIFIED_ELEMENTS)
        mySerum.modifiedelements32 = (uint8_t*)malloc(32 * mySerum.width32);
    }

    if (flags & FLAG_REQUEST_64P_FRAMES) {
      mySerum.frame64 =
          (uint16_t*)malloc(64 * mySerum.width64 * sizeof(uint16_t));
      mySerum.rotations64 = (uint16_t*)malloc(
          MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * sizeof(uint16_t));
      mySerum.rotationsinframe64 =
          (uint16_t*)malloc(2 * 64 * mySerum.width64 * sizeof(uint16_t));
      if (flags & FLAG_REQUEST_FILL_MODIFIED_ELEMENTS)
        mySerum.modifiedelements64 = (uint8_t*)malloc(64 * mySerum.width64);
    }

    if (isextrarequested) {
      for (uint32_t ti = 0; ti < g_serumData.frameCount; ti++) {
        if (g_serumData.isextraframe[ti][0] > 0) {
          mySerum.flags |= FLAG_RETURNED_EXTRA_AVAILABLE;
          break;
        }
      }
    }

    frameshape =
        (uint8_t*)malloc(g_serumData.frameWidth * g_serumData.frameHeight);
    if (!frameshape) {
      Serum_free();
      enabled = false;
      return NULL;
    }
  } else if (SERUM_V1 == g_serumData.SerumVersion) {
    if (g_serumData.frameHeight == 64) {
      mySerum.width64 = g_serumData.frameWidth;
      mySerum.width32 = 0;
    } else {
      mySerum.width32 = g_serumData.frameWidth;
      mySerum.width64 = 0;
    }

    mySerum.frame =
        (uint8_t*)malloc(g_serumData.frameWidth * g_serumData.frameHeight);
    mySerum.palette = (uint8_t*)malloc(3 * 64);
    mySerum.rotations = (uint8_t*)malloc(MAX_COLOR_ROTATIONS * 3);
    if (!mySerum.frame || !mySerum.palette || !mySerum.rotations) {
      Serum_free();
      enabled = false;
      return NULL;
    }
  }

  mySerum.ntriggers = 0;
  for (uint32_t ti = 0; ti < g_serumData.frameCount; ti++) {
    // Every trigger ID greater than PUP_TRIGGER_MAX_THRESHOLD is an internal
    // trigger for rotation scenes and must not be communicated to the PUP
    // Player.
    if (g_serumData.triggerIDs[ti][0] < PUP_TRIGGER_MAX_THRESHOLD)
      mySerum.ntriggers++;
  }

  // Allocate framechecked array
  framechecked = (bool*)malloc(sizeof(bool) * g_serumData.frameCount);
  if (!framechecked) {
    Serum_free();
    enabled = false;
    return NULL;
  }

  Full_Reset_ColorRotations();
  cromloaded = true;
  enabled = true;
  RebuildSpriteSizeCaches();

  return &mySerum;
}

Serum_Frame_Struc* Serum_LoadConcentrate(const char* filename,
                                         const uint8_t flags) {
  if (!crc32_ready) CRC32encode();

  if (!g_serumData.LoadFromFile(filename, flags)) return NULL;

  return Serum_LoadConcentratePrepared(flags);
}

Serum_Frame_Struc* Serum_LoadFilev2(FILE* pfile, const uint8_t flags,
                                    bool uncompressedCROM, char* pathbuf,
                                    uint32_t sizeheader) {
  fread(&g_serumData.frameWidth, 4, 1, pfile);
  fread(&g_serumData.frameHeight, 4, 1, pfile);
  fread(&g_serumData.extraFrameWidth, 4, 1, pfile);
  fread(&g_serumData.extraFrameHeight, 4, 1, pfile);
  isoriginalrequested = false;
  isextrarequested = false;
  mySerum.width32 = 0;
  mySerum.width64 = 0;
  if (g_serumData.frameHeight == 32) {
    if (flags & FLAG_REQUEST_32P_FRAMES) {
      isoriginalrequested = true;
      mySerum.width32 = g_serumData.frameWidth;
    }
    if (flags & FLAG_REQUEST_64P_FRAMES) {
      isextrarequested = true;
      mySerum.width64 = g_serumData.extraFrameWidth;
    }

  } else {
    if (flags & FLAG_REQUEST_64P_FRAMES) {
      isoriginalrequested = true;
      mySerum.width64 = g_serumData.frameWidth;
    }
    if (flags & FLAG_REQUEST_32P_FRAMES) {
      isextrarequested = true;
      mySerum.width32 = g_serumData.extraFrameWidth;
    }
  }
  fread(&g_serumData.frameCount, 4, 1, pfile);
  fread(&g_serumData.colorCount, 4, 1, pfile);
  mySerum.nocolors = g_serumData.colorCount;
  if ((g_serumData.frameCount == 0) || (g_serumData.colorCount == 0) ||
      !ValidateLoadedGeometry(true, "cROM/v2")) {
    // incorrect file format
    fclose(pfile);
    enabled = false;
    return NULL;
  }
  fread(&g_serumData.comparisonMaskCount, 4, 1, pfile);
  fread(&g_serumData.spriteCount, 4, 1, pfile);
  fread(&g_serumData.backgroundCount, 2, 1,
        pfile);  // g_serumData.backgroundCount is a uint16_t
  if (sizeheader >= 20 * sizeof(uint32_t)) {
    int is256x64;
    fread(&is256x64, sizeof(int), 1, pfile);
    g_serumData.isNative256x64 = (is256x64 != 0);
  }

  frameshape =
      (uint8_t*)malloc(g_serumData.frameWidth * g_serumData.frameHeight);

  if (flags & FLAG_REQUEST_32P_FRAMES) {
    mySerum.frame32 =
        (uint16_t*)malloc(32 * mySerum.width32 * sizeof(uint16_t));
    mySerum.rotations32 = (uint16_t*)malloc(
        MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * sizeof(uint16_t));
    mySerum.rotationsinframe32 =
        (uint16_t*)malloc(2 * 32 * mySerum.width32 * sizeof(uint16_t));
    if (flags & FLAG_REQUEST_FILL_MODIFIED_ELEMENTS)
      mySerum.modifiedelements32 = (uint8_t*)malloc(32 * mySerum.width32);
    if (!mySerum.frame32 || !mySerum.rotations32 ||
        !mySerum.rotationsinframe32 ||
        (flags & FLAG_REQUEST_FILL_MODIFIED_ELEMENTS &&
         !mySerum.modifiedelements32)) {
      Serum_free();
      fclose(pfile);
      enabled = false;
      return NULL;
    }
  }
  if (flags & FLAG_REQUEST_64P_FRAMES) {
    mySerum.frame64 =
        (uint16_t*)malloc(64 * mySerum.width64 * sizeof(uint16_t));
    mySerum.rotations64 = (uint16_t*)malloc(
        MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * sizeof(uint16_t));
    mySerum.rotationsinframe64 =
        (uint16_t*)malloc(2 * 64 * mySerum.width64 * sizeof(uint16_t));
    if (flags & FLAG_REQUEST_FILL_MODIFIED_ELEMENTS)
      mySerum.modifiedelements64 = (uint8_t*)malloc(64 * mySerum.width64);
    if (!mySerum.frame64 || !mySerum.rotations64 ||
        !mySerum.rotationsinframe64 ||
        (flags & FLAG_REQUEST_FILL_MODIFIED_ELEMENTS &&
         !mySerum.modifiedelements64)) {
      Serum_free();
      fclose(pfile);
      enabled = false;
      return NULL;
    }
  }

  g_serumData.hashcodes.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.shapecompmode.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.compmaskID.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.compmasks.readFromCRomFile(
      g_serumData.isNative256x64
          ? (256 * 64)
          : (g_serumData.frameWidth * g_serumData.frameHeight),
      g_serumData.comparisonMaskCount, pfile);
  g_serumData.isextraframe.readFromCRomFile(1, g_serumData.frameCount, pfile);
  if (isextrarequested) {
    for (uint32_t ti = 0; ti < g_serumData.frameCount; ti++) {
      if (g_serumData.isextraframe[ti][0] > 0) {
        mySerum.flags |= FLAG_RETURNED_EXTRA_AVAILABLE;
        break;
      }
    }
  } else
    g_serumData.isextraframe.clearIndex();
  g_serumData.cframes_v2.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight, g_serumData.frameCount,
      pfile);
  g_serumData.cframes_v2_extra.readFromCRomFile(
      g_serumData.extraFrameWidth * g_serumData.extraFrameHeight,
      g_serumData.frameCount, pfile, &g_serumData.isextraframe);
  g_serumData.dynamasks.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight, g_serumData.frameCount,
      pfile);
  g_serumData.dynamasks_extra.readFromCRomFile(
      g_serumData.extraFrameWidth * g_serumData.extraFrameHeight,
      g_serumData.frameCount, pfile, &g_serumData.isextraframe);
  g_serumData.dyna4cols_v2.readFromCRomFile(
      MAX_DYNA_SETS_PER_FRAME_V2 * g_serumData.colorCount,
      g_serumData.frameCount, pfile);
  g_serumData.dyna4cols_v2_extra.readFromCRomFile(
      MAX_DYNA_SETS_PER_FRAME_V2 * g_serumData.colorCount,
      g_serumData.frameCount, pfile, &g_serumData.isextraframe);
  g_serumData.isextrasprite.readFromCRomFile(1, g_serumData.spriteCount, pfile);
  if (!isextrarequested) g_serumData.isextrasprite.clearIndex();
  g_serumData.framesprites.readFromCRomFile(MAX_SPRITES_PER_FRAME,
                                            g_serumData.frameCount, pfile);
  g_serumData.spriteoriginal.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.spriteCount, pfile);
  g_serumData.spritecolored.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.spriteCount, pfile);
  g_serumData.spritemask_extra.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.spriteCount, pfile,
      &g_serumData.isextrasprite);
  g_serumData.spritecolored_extra.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.spriteCount, pfile,
      &g_serumData.isextrasprite);
  g_serumData.activeframes.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.colorrotations_v2.readFromCRomFile(
      MAX_LENGTH_COLOR_ROTATION * MAX_COLOR_ROTATION_V2, g_serumData.frameCount,
      pfile);
  g_serumData.colorrotations_v2_extra.readFromCRomFile(
      MAX_LENGTH_COLOR_ROTATION * MAX_COLOR_ROTATION_V2, g_serumData.frameCount,
      pfile, &g_serumData.isextraframe);
  g_serumData.spritedetdwords.readFromCRomFile(MAX_SPRITE_DETECT_AREAS,
                                               g_serumData.spriteCount, pfile);
  g_serumData.spritedetdwordpos.readFromCRomFile(
      MAX_SPRITE_DETECT_AREAS, g_serumData.spriteCount, pfile);
  g_serumData.spritedetareas.readFromCRomFile(4 * MAX_SPRITE_DETECT_AREAS,
                                              g_serumData.spriteCount, pfile);
  g_serumData.triggerIDs.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.framespriteBB.readFromCRomFile(MAX_SPRITES_PER_FRAME * 4,
                                             g_serumData.frameCount, pfile,
                                             &g_serumData.framesprites);
  g_serumData.isextrabackground.readFromCRomFile(1, g_serumData.backgroundCount,
                                                 pfile);
  if (!isextrarequested) g_serumData.isextrabackground.clearIndex();
  g_serumData.backgroundframes_v2.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight,
      g_serumData.backgroundCount, pfile);
  g_serumData.backgroundframes_v2_extra.readFromCRomFile(
      g_serumData.extraFrameWidth * g_serumData.extraFrameHeight,
      g_serumData.backgroundCount, pfile, &g_serumData.isextrabackground);
  g_serumData.backgroundIDs.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.backgroundmask.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight, g_serumData.frameCount,
      pfile, &g_serumData.backgroundIDs);
  g_serumData.backgroundmask_extra.readFromCRomFile(
      g_serumData.extraFrameWidth * g_serumData.extraFrameHeight,
      g_serumData.frameCount, pfile, &g_serumData.backgroundIDs);

  if (sizeheader >= 15 * sizeof(uint32_t)) {
    g_serumData.dynashadowsdir.readFromCRomFile(MAX_DYNA_SETS_PER_FRAME_V2,
                                                g_serumData.frameCount, pfile);
    g_serumData.dynashadowscol.readFromCRomFile(MAX_DYNA_SETS_PER_FRAME_V2,
                                                g_serumData.frameCount, pfile);
    g_serumData.dynashadowsdir_extra.readFromCRomFile(
        MAX_DYNA_SETS_PER_FRAME_V2, g_serumData.frameCount, pfile,
        &g_serumData.isextraframe);
    g_serumData.dynashadowscol_extra.readFromCRomFile(
        MAX_DYNA_SETS_PER_FRAME_V2, g_serumData.frameCount, pfile,
        &g_serumData.isextraframe);
  } else {
    g_serumData.dynashadowsdir.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
    g_serumData.dynashadowscol.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
    g_serumData.dynashadowsdir_extra.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
    g_serumData.dynashadowscol_extra.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
  }

  if (sizeheader >= 18 * sizeof(uint32_t)) {
    g_serumData.dynasprite4cols.readFromCRomFile(
        MAX_DYNA_SETS_PER_SPRITE * g_serumData.colorCount,
        g_serumData.spriteCount, pfile);
    g_serumData.dynasprite4cols_extra.readFromCRomFile(
        MAX_DYNA_SETS_PER_SPRITE * g_serumData.colorCount,
        g_serumData.spriteCount, pfile, &g_serumData.isextraframe);
    g_serumData.dynaspritemasks.readFromCRomFile(
        MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.spriteCount, pfile);
    g_serumData.dynaspritemasks_extra.readFromCRomFile(
        MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.spriteCount, pfile,
        &g_serumData.isextraframe);
  } else {
    g_serumData.dynasprite4cols.reserve(MAX_DYNA_SETS_PER_SPRITE *
                                        g_serumData.colorCount);
    g_serumData.dynasprite4cols_extra.reserve(MAX_DYNA_SETS_PER_SPRITE *
                                              g_serumData.colorCount);
    g_serumData.dynaspritemasks.reserve(MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT);
    g_serumData.dynaspritemasks_extra.reserve(MAX_SPRITE_WIDTH *
                                              MAX_SPRITE_HEIGHT);
  }

  if (sizeheader >= 19 * sizeof(uint32_t)) {
    g_serumData.sprshapemode.readFromCRomFile(1, g_serumData.spriteCount,
                                              pfile);
    for (uint32_t i = 0; i < g_serumData.spriteCount; i++) {
      if (g_serumData.sprshapemode[i][0] > 0) {
        for (uint32_t j = 0; j < MAX_SPRITE_DETECT_AREAS; j++) {
          uint32_t detdwords = g_serumData.spritedetdwords[i][j];
          if ((detdwords & 0xFF000000) > 0)
            detdwords = (detdwords & 0x00FFFFFF) | 0x01000000;
          if ((detdwords & 0x00FF0000) > 0)
            detdwords = (detdwords & 0xFF00FFFF) | 0x00010000;
          if ((detdwords & 0x0000FF00) > 0)
            detdwords = (detdwords & 0xFFFF00FF) | 0x00000100;
          if ((detdwords & 0x000000FF) > 0)
            detdwords = (detdwords & 0xFFFFFF00) | 0x00000001;
          g_serumData.spritedetdwords[i][j] = detdwords;
        }
        for (uint32_t j = 0; j < MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT; j++) {
          if (g_serumData.spriteoriginal[i][j] > 0 &&
              g_serumData.spriteoriginal[i][j] != 255)
            g_serumData.spriteoriginal[i][j] = 1;
        }
      }
    }
  } else {
    g_serumData.sprshapemode.reserve(g_serumData.spriteCount);
  }

  fclose(pfile);

  mySerum.ntriggers = 0;
  uint32_t framespos = g_serumData.frameCount / 2;
  uint32_t framesspace = g_serumData.frameCount - framespos;
  uint32_t framescount = (framesspace + 9) / 10;

  if (framescount > 0) {
    std::vector<uint32_t> candidates;
    candidates.reserve(framesspace);
    for (uint32_t ti = framespos; ti < g_serumData.frameCount; ++ti) {
      if (g_serumData.triggerIDs[ti][0] == 0xffffffff) {
        candidates.push_back(ti);
      }
    }

    if (!candidates.empty()) {
      std::mt19937 rng(0xC0DE1234);
      std::shuffle(candidates.begin(), candidates.end(), rng);
      std::uniform_int_distribution<uint32_t> triggerDist(65433u, 0xfffffffeu);

      uint32_t toAssign = std::min<uint32_t>(framescount, candidates.size());
      for (uint32_t i = 0; i < toAssign; ++i) {
        uint32_t triggerValue = triggerDist(rng);
        g_serumData.triggerIDs.set(candidates[i], &triggerValue, 1);
      }

      for (uint32_t offset = 0; (framespos + offset) < g_serumData.frameCount;
           ++offset) {
        uint32_t idx = framespos + offset;
        if (g_serumData.triggerIDs[idx][0] == 0xffffffff) {
          uint32_t triggerValue = triggerDist(rng);
          g_serumData.triggerIDs.set(idx, &triggerValue, 1);
          break;
        }
      }
    }
  }
  for (uint32_t ti = 0; ti < g_serumData.frameCount; ti++) {
    // Every trigger ID greater than PUP_TRIGGER_MAX_THRESHOLD is an internal
    // trigger for rotation scenes and must not be communicated to the PUP
    // Player.
    if (g_serumData.triggerIDs[ti][0] < PUP_TRIGGER_MAX_THRESHOLD)
      mySerum.ntriggers++;
  }
  framechecked = (bool*)malloc(sizeof(bool) * g_serumData.frameCount);
  if (!framechecked) {
    Serum_free();
    enabled = false;
    return NULL;
  }
  if (flags & FLAG_REQUEST_32P_FRAMES) {
    if (g_serumData.frameHeight == 32)
      mySerum.width32 = g_serumData.frameWidth;
    else
      mySerum.width32 = g_serumData.extraFrameWidth;
  } else
    mySerum.width32 = 0;
  if (flags & FLAG_REQUEST_64P_FRAMES) {
    if (g_serumData.frameHeight == 32)
      mySerum.width64 = g_serumData.extraFrameWidth;
    else
      mySerum.width64 = g_serumData.frameWidth;
  } else
    mySerum.width64 = 0;

  mySerum.SerumVersion = g_serumData.SerumVersion = SERUM_V2;

  Full_Reset_ColorRotations();
  cromloaded = true;

  if (!uncompressedCROM) {
    // remove temporary file that had been extracted from compressed CRZ file
    remove(pathbuf);
  }

  enabled = true;
  RebuildSpriteSizeCaches();
  return &mySerum;
}

Serum_Frame_Struc* Serum_LoadFilev1(const char* const filename,
                                    const uint8_t flags) {
  char pathbuf[pathbuflen];
  if (!crc32_ready) CRC32encode();

  // check if we're using an uncompressed cROM file
  const char* ext;
  bool uncompressedCROM = false;
  if ((ext = strrchr(filename, '.')) != NULL) {
    if (strcasecmp(ext, ".cROM") == 0) {
      uncompressedCROM = true;
      if (strcpy_s(pathbuf, pathbuflen, filename)) return NULL;
    }
  }

  // extract file if it is compressed
  if (!uncompressedCROM) {
    char cromname[pathbuflen];
    if (getenv("TMPDIR") != NULL) {
      if (strcpy_s(pathbuf, pathbuflen, getenv("TMPDIR"))) return NULL;
      size_t len = strlen(pathbuf);
      if (len > 0 && pathbuf[len - 1] != '/') {
        if (strcat_s(pathbuf, pathbuflen, "/")) return NULL;
      }
    } else if (strcpy_s(pathbuf, pathbuflen, filename))
      return NULL;

    if (!unzip_crz(filename, pathbuf, cromname, pathbuflen)) return NULL;
    if (strcat_s(pathbuf, pathbuflen, cromname)) return NULL;
  }

  // Open cRom
  FILE* pfile;
  pfile = fopen(pathbuf, "rb");
  if (!pfile) {
    enabled = false;
    return NULL;
  }

  // read the header to know how much memory is needed
  fread(g_serumData.romName, 1, 64, pfile);
  uint32_t sizeheader;
  fread(&sizeheader, 4, 1, pfile);
  // if this is a new format file, we load with Serum_LoadNewFile()
  if (sizeheader >= 14 * sizeof(uint32_t))
    return Serum_LoadFilev2(pfile, flags, uncompressedCROM, pathbuf,
                            sizeheader);
  mySerum.SerumVersion = g_serumData.SerumVersion = SERUM_V1;
  fread(&g_serumData.frameWidth, 4, 1, pfile);
  fread(&g_serumData.frameHeight, 4, 1, pfile);
  // The serum file stored the number of frames as uint32_t, but in fact, the
  // number of frames will never exceed the size of uint16_t (65535)
  uint32_t nframes32;
  fread(&nframes32, 4, 1, pfile);
  g_serumData.frameCount = (uint16_t)nframes32;
  fread(&g_serumData.colorCount, 4, 1, pfile);
  mySerum.nocolors = g_serumData.colorCount;
  fread(&g_serumData.classicColorCount, 4, 1, pfile);
  if ((g_serumData.frameWidth == 0) || (g_serumData.frameHeight == 0) ||
      (g_serumData.frameCount == 0) || (g_serumData.colorCount == 0) ||
      (g_serumData.classicColorCount == 0)) {
    // incorrect file format
    fclose(pfile);
    enabled = false;
    return NULL;
  }
  if (!ValidateLoadedGeometry(false, "cROM/v1")) {
    fclose(pfile);
    enabled = false;
    return NULL;
  }
  fread(&g_serumData.comparisonMaskCount, 4, 1, pfile);
  fread(&g_serumData.movementMaskCount, 4, 1, pfile);
  fread(&g_serumData.spriteCount, 4, 1, pfile);
  if (sizeheader >= 13 * sizeof(uint32_t))
    fread(&g_serumData.backgroundCount, 2, 1, pfile);
  else
    g_serumData.backgroundCount = 0;
  // allocate memory for the serum format
  uint8_t* spritedescriptionso = (uint8_t*)malloc(
      g_serumData.spriteCount * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);
  uint8_t* spritedescriptionsc = (uint8_t*)malloc(
      g_serumData.spriteCount * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);

  mySerum.frame =
      (uint8_t*)malloc(g_serumData.frameWidth * g_serumData.frameHeight);
  mySerum.palette = (uint8_t*)malloc(3 * 64);
  mySerum.rotations = (uint8_t*)malloc(MAX_COLOR_ROTATIONS * 3);
  if (((g_serumData.spriteCount > 0) &&
       (!spritedescriptionso || !spritedescriptionsc)) ||
      !mySerum.frame || !mySerum.palette || !mySerum.rotations) {
    Serum_free();
    fclose(pfile);
    enabled = false;
    return NULL;
  }
  // read the cRom file
  g_serumData.hashcodes.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.shapecompmode.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.compmaskID.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.movrctID.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.movrctID.clear();  // we don't need this anymore, but we need to
                                 // read it to skip the data in the file
  g_serumData.compmasks.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight,
      g_serumData.comparisonMaskCount, pfile);
  g_serumData.movrcts.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight,
      g_serumData.movementMaskCount, pfile);
  g_serumData.movrcts.clear();  // we don't need this anymore, but we need to
                                // read it to skip the data in the file
  g_serumData.cpal.readFromCRomFile(3 * g_serumData.classicColorCount,
                                    g_serumData.frameCount, pfile);
  g_serumData.cframes.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight, g_serumData.frameCount,
      pfile);
  g_serumData.dynamasks.readFromCRomFile(
      g_serumData.frameWidth * g_serumData.frameHeight, g_serumData.frameCount,
      pfile);
  g_serumData.dyna4cols.readFromCRomFile(
      MAX_DYNA_4COLS_PER_FRAME * g_serumData.colorCount, g_serumData.frameCount,
      pfile);
  g_serumData.framesprites.readFromCRomFile(MAX_SPRITES_PER_FRAME,
                                            g_serumData.frameCount, pfile);

  for (int ti = 0;
       ti < (int)g_serumData.spriteCount * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE;
       ti++) {
    fread(&spritedescriptionsc[ti], 1, 1, pfile);
    fread(&spritedescriptionso[ti], 1, 1, pfile);
  }
  for (uint32_t i = 0; i < g_serumData.spriteCount; i++) {
    g_serumData.spritedescriptionsc.set(
        i, &spritedescriptionsc[i * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE],
        MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);
    g_serumData.spritedescriptionso.set(
        i, &spritedescriptionso[i * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE],
        MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);
  }
  Free_element((void**)&spritedescriptionso);
  Free_element((void**)&spritedescriptionsc);

  g_serumData.activeframes.readFromCRomFile(1, g_serumData.frameCount, pfile);
  g_serumData.colorrotations.readFromCRomFile(3 * MAX_COLOR_ROTATIONS,
                                              g_serumData.frameCount, pfile);
  g_serumData.spritedetdwords.readFromCRomFile(MAX_SPRITE_DETECT_AREAS,
                                               g_serumData.spriteCount, pfile);
  g_serumData.spritedetdwordpos.readFromCRomFile(
      MAX_SPRITE_DETECT_AREAS, g_serumData.spriteCount, pfile);
  g_serumData.spritedetareas.readFromCRomFile(4 * MAX_SPRITE_DETECT_AREAS,
                                              g_serumData.spriteCount, pfile);
  mySerum.ntriggers = 0;
  if (sizeheader >= 11 * sizeof(uint32_t)) {
    g_serumData.triggerIDs.readFromCRomFile(1, g_serumData.frameCount, pfile);
  }
  uint32_t framespos = g_serumData.frameCount / 2;
  uint32_t framesspace = g_serumData.frameCount - framespos;
  uint32_t framescount = (framesspace + 9) / 10;

  if (framescount > 0) {
    std::vector<uint32_t> candidates;
    candidates.reserve(framesspace);
    for (uint32_t ti = framespos; ti < g_serumData.frameCount; ++ti) {
      if (g_serumData.triggerIDs[ti][0] == 0xffffffff) {
        candidates.push_back(ti);
      }
    }

    if (!candidates.empty()) {
      std::mt19937 rng(0xC0DE1234);
      std::shuffle(candidates.begin(), candidates.end(), rng);
      std::uniform_int_distribution<uint32_t> triggerDist(65433u, 0xfffffffeu);

      uint32_t toAssign = std::min<uint32_t>(framescount, candidates.size());
      for (uint32_t i = 0; i < toAssign; ++i) {
        uint32_t triggerValue = triggerDist(rng);
        g_serumData.triggerIDs.set(candidates[i], &triggerValue, 1);
      }

      for (uint32_t offset = 0; (framespos + offset) < g_serumData.frameCount;
           ++offset) {
        uint32_t idx = framespos + offset;
        if (g_serumData.triggerIDs[idx][0] == 0xffffffff) {
          uint32_t triggerValue = triggerDist(rng);
          g_serumData.triggerIDs.set(idx, &triggerValue, 1);
          break;
        }
      }
    }
  }
  for (uint32_t ti = 0; ti < g_serumData.frameCount; ti++) {
    if (g_serumData.triggerIDs[ti][0] != 0xffffffff) mySerum.ntriggers++;
  }
  if (sizeheader >= 12 * sizeof(uint32_t))
    g_serumData.framespriteBB.readFromCRomFile(MAX_SPRITES_PER_FRAME * 4,
                                               g_serumData.frameCount, pfile,
                                               &g_serumData.framesprites);
  else {
    for (uint32_t tj = 0; tj < g_serumData.frameCount; tj++) {
      uint16_t tmp_framespriteBB[4 * MAX_SPRITES_PER_FRAME];
      for (uint32_t ti = 0; ti < MAX_SPRITES_PER_FRAME; ti++) {
        tmp_framespriteBB[ti * 4] = 0;
        tmp_framespriteBB[ti * 4 + 1] = 0;
        tmp_framespriteBB[ti * 4 + 2] = g_serumData.frameWidth - 1;
        tmp_framespriteBB[ti * 4 + 3] = g_serumData.frameHeight - 1;
      }
      g_serumData.framespriteBB.set(tj, tmp_framespriteBB,
                                    MAX_SPRITES_PER_FRAME * 4);
    }
  }
  if (sizeheader >= 13 * sizeof(uint32_t)) {
    g_serumData.backgroundframes.readFromCRomFile(
        g_serumData.frameWidth * g_serumData.frameHeight,
        g_serumData.backgroundCount, pfile);
    g_serumData.backgroundIDs.readFromCRomFile(1, g_serumData.frameCount,
                                               pfile);
    g_serumData.backgroundBB.readFromCRomFile(4, g_serumData.frameCount, pfile,
                                              &g_serumData.backgroundIDs);
  }
  fclose(pfile);

  // allocate memory for previous detected frame
  framechecked = (bool*)malloc(sizeof(bool) * g_serumData.frameCount);
  if (!framechecked) {
    Serum_free();
    enabled = false;
    return NULL;
  }
  if (g_serumData.frameHeight == 64) {
    mySerum.width64 = g_serumData.frameWidth;
    mySerum.width32 = 0;
  } else {
    mySerum.width32 = g_serumData.frameWidth;
    mySerum.width64 = 0;
  }
  Full_Reset_ColorRotations();
  cromloaded = true;

  if (!uncompressedCROM) {
    // remove temporary file that had been extracted from compressed CRZ file
    remove(pathbuf);
  }

  enabled = true;
  RebuildSpriteSizeCaches();
  return &mySerum;
}

SERUM_API Serum_Frame_Struc* Serum_Load(const char* const altcolorpath,
                                        const char* const romname,
                                        uint8_t flags) {
  Serum_free();

  mySerum.SerumVersion = g_serumData.SerumVersion = 0;
  mySerum.flags = 0;
  mySerum.frame = NULL;
  mySerum.frame32 = NULL;
  mySerum.frame64 = NULL;
  mySerum.palette = NULL;
  mySerum.rotations = NULL;
  mySerum.rotations32 = NULL;
  mySerum.rotations64 = NULL;
  mySerum.rotationsinframe32 = NULL;
  mySerum.rotationsinframe64 = NULL;
  mySerum.modifiedelements32 = NULL;
  mySerum.modifiedelements64 = NULL;

  std::string pathbuf = std::string(altcolorpath);
  if (pathbuf.empty() || (pathbuf.back() != '\\' && pathbuf.back() != '/'))
    pathbuf += '/';
  pathbuf += romname;
  pathbuf += '/';

  Log("Searching colorization file for %s in %s", romname, pathbuf.c_str());

  // If no specific frame tyoe is requested, activate both
  if ((flags & (FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_64P_FRAMES)) == 0) {
    flags |= FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_64P_FRAMES;
  }

  std::optional<std::string> csvFoundFile =
      find_case_insensitive_file(pathbuf, std::string(romname) + ".pup.csv");
  if (csvFoundFile) {
    Log("Found %s", csvFoundFile->c_str());
#ifdef WRITE_CROMC
    // request both frame types for updating concentrate
    flags |= FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_64P_FRAMES;
#endif
  }
  Serum_Frame_Struc* result = NULL;
  bool loadedFromConcentrate = false;
  bool sceneDataUpdatedFromCsv = false;
  std::optional<std::string> pFoundFile;
  std::optional<std::string> skipFoundFile =
      find_case_insensitive_file(pathbuf, "skip-cromc.txt");
  if (skipFoundFile) {
    Log("Skipping .cROMc load due to presence of %s", skipFoundFile->c_str());
  } else {
    pFoundFile =
        find_case_insensitive_file(pathbuf, std::string(romname) + ".cROMc");

    if (pFoundFile) {
      Log("Found %s", pFoundFile->c_str());
      result = Serum_LoadConcentrate(pFoundFile->c_str(), flags);
      loadedFromConcentrate = (result != NULL);
      if (result) {
        Log("Loaded %s", pFoundFile->c_str());
        if (csvFoundFile && g_serumData.SerumVersion == SERUM_V2 &&
            g_serumData.sceneGenerator->parseCSV(csvFoundFile->c_str())) {
          sceneDataUpdatedFromCsv = true;
#ifdef WRITE_CROMC
          // Update the concentrate file with new PUP data
          if (generateCRomC) Serum_SaveConcentrate(pFoundFile->c_str());
#endif
        }
      } else {
        Log("Failed to load %s", pFoundFile->c_str());
      }
    }
  }

  if (!result) {
#ifdef WRITE_CROMC
    // by default, we request both frame types
    flags |= FLAG_REQUEST_32P_FRAMES | FLAG_REQUEST_64P_FRAMES;
#endif
    pFoundFile =
        find_case_insensitive_file(pathbuf, std::string(romname) + ".cROM");
    if (!pFoundFile)
      pFoundFile =
          find_case_insensitive_file(pathbuf, std::string(romname) + ".cRZ");
    if (!pFoundFile) {
      enabled = false;
      return NULL;
    }
    Log("Found %s", pFoundFile->c_str());
    result = Serum_LoadFilev1(pFoundFile->c_str(), flags);
    if (result) {
      Log("Loaded %s", pFoundFile->c_str());
      if (csvFoundFile && g_serumData.SerumVersion == SERUM_V2) {
        sceneDataUpdatedFromCsv =
            g_serumData.sceneGenerator->parseCSV(csvFoundFile->c_str());
      }
#ifdef WRITE_CROMC
      if (generateCRomC) Serum_SaveConcentrate(pFoundFile->c_str());
#endif
    } else {
      Log("Failed to load %s", pFoundFile->c_str());
    }
  }
  if (result && g_serumData.sceneGenerator->isActive())
    g_serumData.sceneGenerator->setDepth(result->nocolors == 16 ? 4 : 2);
  if (result) {
    if (!loadedFromConcentrate || g_serumData.concentrateFileVersion < 6 ||
        sceneDataUpdatedFromCsv) {
      BuildFrameLookupVectors();
    } else {
      InitFrameLookupRuntimeStateFromStoredData();
    }
    RebuildRotationLookupTablesV2();
    RebuildSpriteDetectionPlanV2();
  }
  if (is_real_machine()) {
    monochromeMode = true;
  }

  return result;
}

SERUM_API void Serum_Dispose(void) { Serum_free(); }

static void BuildFrameLookupVectors(void) {
  uint32_t numSceneFrames = 0;
  g_serumData.frameIsScene.clear();
  g_serumData.sceneFramesBySignature.clear();
  g_serumData.sceneFrameIdsBySceneKey.clear();
  g_serumData.sceneGroupFrameTableOffset.clear();
  g_serumData.sceneGroupFrameTableLength.clear();
  g_serumData.sceneGroupFrameIdsFlat.clear();

  if (g_serumData.frameCount == 0) return;
  g_serumData.frameIsScene.resize(g_serumData.frameCount, 0);
  const uint32_t pixels =
      g_serumData.isNative256x64
          ? (256 * 64)
          : (g_serumData.frameWidth * g_serumData.frameHeight);

  // Build scene signatures in the same domain used by Identify_Frame:
  // (mask, shape, crc32 over original frame pixels).
  if (g_serumData.SerumVersion == SERUM_V2 && g_serumData.frameWidth == 128 &&
      g_serumData.frameHeight == 32 && g_serumData.sceneGenerator &&
      g_serumData.sceneGenerator->isActive()) {
    std::unordered_set<uint16_t> uniqueMaskShapeKeys;
    std::vector<std::pair<uint8_t, uint8_t>> uniqueMaskShapes;
    uniqueMaskShapes.reserve(g_serumData.frameCount);
    std::unordered_map<uint64_t, std::vector<uint32_t>> frameIdsBySignature;
    frameIdsBySignature.reserve(g_serumData.frameCount);

    for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
      const uint8_t mask = g_serumData.compmaskID[frameId][0];
      const uint8_t shape = g_serumData.shapecompmode[frameId][0];
      const uint16_t key = (uint16_t(mask) << 8) | shape;
      if (uniqueMaskShapeKeys.insert(key).second) {
        uniqueMaskShapes.emplace_back(mask, shape);
      }
      frameIdsBySignature[MakeFrameSignature(mask, shape,
                                             g_serumData.hashcodes[frameId][0])]
          .push_back(frameId);
    }

    std::unordered_set<uint64_t> sceneSignatures;
    sceneSignatures.reserve(uniqueMaskShapes.size() * 64);
    std::unordered_map<uint32_t, std::vector<uint32_t>> sceneGroupFrameIds;
    auto append_unique = [](std::vector<uint32_t>& ids, uint32_t frameId) {
      if (std::find(ids.begin(), ids.end(), frameId) == ids.end()) {
        ids.push_back(frameId);
      }
    };

    uint8_t generatedSceneFrame[128 * 32];
    const auto& scenes = g_serumData.sceneGenerator->getSceneData();
    for (const auto& scene : scenes) {
      const int groups = scene.frameGroups > 0 ? scene.frameGroups : 1;
      for (int group = 1; group <= groups; ++group) {
        for (uint16_t frameIndex = 0; frameIndex < scene.frameCount;
             ++frameIndex) {
          if (g_serumData.sceneGenerator->generateFrame(
                  scene.sceneId, frameIndex, generatedSceneFrame, group,
                  true) != 0xffff) {
            continue;
          }
          std::vector<uint32_t> matchedFrameIds;
          for (const auto& maskShape : uniqueMaskShapes) {
            uint32_t hash = calc_crc32(generatedSceneFrame, maskShape.first,
                                       pixels, maskShape.second);
            uint64_t signature =
                MakeFrameSignature(maskShape.first, maskShape.second, hash);
            sceneSignatures.insert(signature);
            auto frameSigIt = frameIdsBySignature.find(signature);
            if (frameSigIt == frameIdsBySignature.end()) {
              continue;
            }
            for (uint32_t frameId : frameSigIt->second) {
              append_unique(matchedFrameIds, frameId);
            }
          }

          if (!matchedFrameIds.empty()) {
            std::sort(matchedFrameIds.begin(), matchedFrameIds.end());
            matchedFrameIds.erase(
                std::unique(matchedFrameIds.begin(), matchedFrameIds.end()),
                matchedFrameIds.end());

            uint32_t primaryFrameId = matchedFrameIds.front();
            g_serumData.sceneFrameIdsBySceneKey[MakeSceneFrameKey(
                scene.sceneId, static_cast<uint8_t>(group), frameIndex)] =
                primaryFrameId;

            uint32_t sceneGroupKey =
                MakeSceneGroupKey(scene.sceneId, static_cast<uint8_t>(group));
            std::vector<uint32_t>& byGroup = sceneGroupFrameIds[sceneGroupKey];
            if (byGroup.size() < scene.frameCount) {
              byGroup.resize(scene.frameCount, IDENTIFY_NO_FRAME);
            }
            byGroup[frameIndex] = primaryFrameId;
          }
        }
      }
    }

    for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
      const uint8_t mask = g_serumData.compmaskID[frameId][0];
      const uint8_t shape = g_serumData.shapecompmode[frameId][0];
      const uint32_t hash = g_serumData.hashcodes[frameId][0];
      if (sceneSignatures.find(MakeFrameSignature(mask, shape, hash)) !=
          sceneSignatures.end()) {
        g_serumData.frameIsScene[frameId] = 1;
        g_serumData
            .sceneFramesBySignature[MakeFrameSignature(mask, shape, hash)]
            .push_back(frameId);
        numSceneFrames++;
      }
    }

    std::vector<uint32_t> sceneGroupKeys;
    sceneGroupKeys.reserve(sceneGroupFrameIds.size());
    for (const auto& entry : sceneGroupFrameIds) {
      sceneGroupKeys.push_back(entry.first);
    }
    std::sort(sceneGroupKeys.begin(), sceneGroupKeys.end());

    uint32_t offset = 0;
    for (uint32_t sceneGroupKey : sceneGroupKeys) {
      const std::vector<uint32_t>& byGroup = sceneGroupFrameIds[sceneGroupKey];
      g_serumData.sceneGroupFrameTableOffset[sceneGroupKey] = offset;
      g_serumData.sceneGroupFrameTableLength[sceneGroupKey] =
          static_cast<uint16_t>(std::min<size_t>(byGroup.size(), 0xffffu));
      g_serumData.sceneGroupFrameIdsFlat.insert(
          g_serumData.sceneGroupFrameIdsFlat.end(), byGroup.begin(),
          byGroup.end());
      offset += static_cast<uint32_t>(byGroup.size());
    }
  }

  Log("Loaded %d frames and %d rotation scene frames",
      g_serumData.frameCount - numSceneFrames, numSceneFrames);

  lastfound_scene = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
    if (g_serumData.frameIsScene[frameId]) {
      lastfound_scene = frameId;
      break;
    }
  }

  lastfound_normal = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
    if (!g_serumData.frameIsScene[frameId]) {
      lastfound_normal = frameId;
      break;
    }
  }
}

static uint64_t MakeFrameSignature(uint8_t mask, uint8_t shape, uint32_t hash) {
  return (uint64_t(mask) << 40) | (uint64_t(shape) << 32) | hash;
}

static uint64_t MakeSceneFrameKey(uint16_t sceneId, uint8_t group,
                                  uint16_t frameIndex) {
  return (uint64_t(sceneId) << 24) | (uint64_t(group) << 16) |
         uint64_t(frameIndex);
}

static uint32_t MakeSceneGroupKey(uint16_t sceneId, uint8_t group) {
  return (uint32_t(sceneId) << 8) | uint32_t(group);
}

static void InitFrameLookupRuntimeStateFromStoredData(void) {
  const bool needsSceneTable =
      g_serumData.sceneGenerator && g_serumData.sceneGenerator->isActive();
  if (g_serumData.frameIsScene.size() != g_serumData.frameCount ||
      (needsSceneTable && g_serumData.sceneGroupFrameTableOffset.empty())) {
    BuildFrameLookupVectors();
    return;
  }

  uint32_t numSceneFrames = 0;
  for (uint8_t isScene : g_serumData.frameIsScene) {
    if (isScene) numSceneFrames++;
  }
  Log("Loaded %d frames and %d rotation scene frames",
      g_serumData.frameCount - numSceneFrames, numSceneFrames);

  lastfound_scene = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
    if (g_serumData.frameIsScene[frameId]) {
      lastfound_scene = frameId;
      break;
    }
  }

  lastfound_normal = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.frameCount; ++frameId) {
    if (!g_serumData.frameIsScene[frameId]) {
      lastfound_normal = frameId;
      break;
    }
  }
}

uint32_t Identify_Frame(uint8_t* frame, bool sceneFrameRequested) {
  // Usually the first frame has the ID 0, but lastfound is also initialized
  // with 0. So we need a helper to be able to detect frame 0 as new.
  static bool first_match_normal = true;

  if (!cromloaded) return IDENTIFY_NO_FRAME;
  if (sceneFrameRequested) return IDENTIFY_NO_FRAME;
  memset(framechecked, false, g_serumData.frameCount);
  uint32_t& lastfound_stream = lastfound_normal;
  bool& first_match = first_match_normal;
  uint32_t& lastframe_full_crc = lastframe_full_crc_normal;
  uint32_t tj = lastfound_stream;  // we start from the last found normal frame
  const uint32_t pixels =
      g_serumData.isNative256x64
          ? (256 * 64)
          : (g_serumData.frameWidth * g_serumData.frameHeight);
  do {
    if (g_serumData.frameIsScene[tj]) {
      if (++tj >= g_serumData.frameCount) tj = 0;
      continue;
    }
    if (!framechecked[tj]) {
      // calculate the hashcode for the generated frame with the mask and
      // shapemode of the current crom frame
      uint8_t mask = g_serumData.compmaskID[tj][0];
      uint8_t Shape = g_serumData.shapecompmode[tj][0];
      uint32_t Hashc = calc_crc32(frame, mask, pixels, Shape);
      // now we can compare with all the crom frames that share these same mask
      // and shapemode
      uint32_t ti = tj;
      do {
        if (g_serumData.frameIsScene[ti]) {
          if (++ti >= g_serumData.frameCount) ti = 0;
          continue;
        }
        if (!framechecked[ti]) {
          if ((g_serumData.compmaskID[ti][0] == mask) &&
              (g_serumData.shapecompmode[ti][0] == Shape)) {
            if (Hashc == g_serumData.hashcodes[ti][0]) {
              if (first_match || ti != lastfound_stream || mask < 255) {
                // Reset_ColorRotations();
                lastfound_stream = ti;
                lastfound = ti;
                lastframe_full_crc = crc32_fast(frame, pixels);
                first_match = false;
                return ti;  // we found the frame, we return it
              }

              uint32_t full_crc = crc32_fast(frame, pixels);
              if (full_crc != lastframe_full_crc) {
                lastframe_full_crc = full_crc;
                lastfound = ti;
                return ti;  // we found the same frame with shape as before, but
                            // the full frame is different
              }
              lastfound = ti;
              return IDENTIFY_SAME_FRAME;  // we found the frame, but it is the
                                           // same full frame as before (no
                                           // mask)
            }
            framechecked[ti] = true;
          }
        }
        if (++ti >= g_serumData.frameCount) ti = 0;
      } while (ti != tj);
    }
    if (++tj >= g_serumData.frameCount) tj = 0;
  } while (tj != lastfound_stream);

  return IDENTIFY_NO_FRAME;  // we found no corresponding frame
}

void GetSpriteSize(uint8_t spriteIndex, int* spriteWidth, int* spriteHeight,
                   uint8_t* spriteData, int sourceWidth, int sourceHeight) {
  *spriteWidth = *spriteHeight = 0;
  if (spriteIndex >= g_serumData.spriteCount) return;
  if (!spriteData) return;
  for (int row = 0; row < sourceHeight; row++) {
    for (int col = 0; col < sourceWidth; col++) {
      if (spriteData[row * sourceWidth + col] < 255) {
        if (row > *spriteHeight) *spriteHeight = row;
        if (col > *spriteWidth) *spriteWidth = col;
      }
    }
  }
  (*spriteHeight)++;
  (*spriteWidth)++;
}

bool Check_Spritesv1(uint8_t* Frame, uint32_t quelleframe,
                     uint8_t* pquelsprites, uint8_t* nspr, uint16_t* pfrx,
                     uint16_t* pfry, uint16_t* pspx, uint16_t* pspy,
                     uint16_t* pwid, uint16_t* phei) {
  uint8_t* frameSprites = g_serumData.framesprites[quelleframe];
  uint16_t* frameSpriteBB = g_serumData.framespriteBB[quelleframe];
  uint8_t ti = 0;
  uint32_t mdword;
  *nspr = 0;
  while ((ti < MAX_SPRITES_PER_FRAME) && (frameSprites[ti] < 255)) {
    uint8_t qspr = frameSprites[ti];
    int spw = (qspr < g_serumData.spriteWidthV1.size())
                  ? g_serumData.spriteWidthV1[qspr]
                  : 0;
    int sph = (qspr < g_serumData.spriteHeightV1.size())
                  ? g_serumData.spriteHeightV1[qspr]
                  : 0;
    uint8_t* spriteDesc = g_serumData.spritedescriptionso[qspr];
    uint16_t* spriteDetAreas = g_serumData.spritedetareas[qspr];
    uint32_t* spriteDetDwords = g_serumData.spritedetdwords[qspr];
    uint16_t* spriteDetDwordPos = g_serumData.spritedetdwordpos[qspr];
    if (spw <= 0 || sph <= 0) {
      GetSpriteSize(qspr, &spw, &sph, spriteDesc, MAX_SPRITE_SIZE,
                    MAX_SPRITE_SIZE);
    }
    short minxBB = (short)(frameSpriteBB[ti * 4]);
    short minyBB = (short)(frameSpriteBB[ti * 4 + 1]);
    short maxxBB = (short)(frameSpriteBB[ti * 4 + 2]);
    short maxyBB = (short)(frameSpriteBB[ti * 4 + 3]);
    for (uint32_t tm = 0; tm < MAX_SPRITE_DETECT_AREAS; tm++) {
      if (spriteDetAreas[tm * 4] == 0xffff) continue;
      // we look for the sprite in the frame sent
      for (short ty = minyBB; ty <= maxyBB; ty++) {
        mdword =
            (uint32_t)(Frame[ty * g_serumData.frameWidth + minxBB] << 8) |
            (uint32_t)(Frame[ty * g_serumData.frameWidth + minxBB + 1] << 16) |
            (uint32_t)(Frame[ty * g_serumData.frameWidth + minxBB + 2] << 24);
        for (short tx = minxBB; tx <= maxxBB - 3; tx++) {
          uint32_t tj = ty * g_serumData.frameWidth + tx;
          mdword = (mdword >> 8) | (uint32_t)(Frame[tj + 3] << 24);
          // we look for the magic dword first:
          uint16_t sddp = spriteDetDwordPos[tm];
          if (mdword == spriteDetDwords[tm]) {
            short frax =
                (short)tx;  // position in the frame of the detection dword
            short fray = (short)ty;
            short sprx =
                (short)(sddp % MAX_SPRITE_SIZE);  // position in the sprite of
                                                  // the detection dword
            short spry = (short)(sddp / MAX_SPRITE_SIZE);
            // details of the det area:
            short detx =
                (short)spriteDetAreas[tm * 4];  // position of the detection
                                                // area in the sprite
            short dety = (short)spriteDetAreas[tm * 4 + 1];
            short detw = (short)
                spriteDetAreas[tm * 4 + 2];  // size of the detection area
            short deth = (short)spriteDetAreas[tm * 4 + 3];
            // if the detection area starts before the frame (left or top),
            // continue:
            if ((frax - minxBB < sprx - detx) || (fray - minyBB < spry - dety))
              continue;
            // position of the detection area in the frame
            int offsx = frax - sprx + detx;
            int offsy = fray - spry + dety;
            // if the detection area extends beyond the bounding box (right or
            // bottom), continue:
            if ((offsx + detw > (int)maxxBB + 1) ||
                (offsy + deth > (int)maxyBB + 1))
              continue;
            // we can now check if the full detection area is around the found
            // detection dword
            bool notthere = false;
            for (uint16_t tk = 0; tk < deth; tk++) {
              for (uint16_t tl = 0; tl < detw; tl++) {
                uint8_t val =
                    spriteDesc[(tk + dety) * MAX_SPRITE_SIZE + tl + detx];
                if (val == 255) continue;
                if (val !=
                    Frame[(tk + offsy) * g_serumData.frameWidth + tl + offsx]) {
                  notthere = true;
                  break;
                }
              }
              if (notthere == true) break;
            }
            if (!notthere) {
              pquelsprites[*nspr] = qspr;
              if (frax - minxBB < sprx) {
                pspx[*nspr] =
                    (uint16_t)(sprx -
                               (frax - minxBB));  // display sprite from point
                pfrx[*nspr] = (uint16_t)minxBB;
                pwid[*nspr] = std::min((uint16_t)(spw - pspx[*nspr]),
                                       (uint16_t)(maxxBB - minxBB + 1));
              } else {
                pspx[*nspr] = 0;
                pfrx[*nspr] = (uint16_t)(frax - sprx);
                pwid[*nspr] = std::min((uint16_t)(maxxBB - pfrx[*nspr] + 1),
                                       (uint16_t)spw);
              }
              if (fray - minyBB < spry) {
                pspy[*nspr] = (uint16_t)(spry - (fray - minyBB));
                pfry[*nspr] = (uint16_t)minyBB;
                phei[*nspr] = std::min((uint16_t)(sph - pspy[*nspr]),
                                       (uint16_t)(maxyBB - minyBB + 1));
              } else {
                pspy[*nspr] = 0;
                pfry[*nspr] = (uint16_t)(fray - spry);
                phei[*nspr] = std::min((uint16_t)(maxyBB - pfry[*nspr] + 1),
                                       (uint16_t)sph);
              }
              // we check the identical sprites as there may be duplicate due to
              // the multi detection zones
              bool identicalfound = false;
              for (uint8_t tk = 0; tk < *nspr; tk++) {
                if ((pquelsprites[*nspr] == pquelsprites[tk]) &&
                    (pfrx[*nspr] == pfrx[tk]) && (pfry[*nspr] == pfry[tk]) &&
                    (pwid[*nspr] == pwid[tk]) && (phei[*nspr] == phei[tk]))
                  identicalfound = true;
              }
              if (!identicalfound) {
                (*nspr)++;
                if (*nspr == MAX_SPRITES_PER_FRAME) return true;
              }
            }
          }
        }
      }
    }
    ti++;
  }
  if (*nspr > 0) return true;
  return false;
}

bool Check_Spritesv2(uint8_t* recframe, uint32_t quelleframe,
                     uint8_t* pquelsprites, uint8_t* nspr, uint16_t* pfrx,
                     uint16_t* pfry, uint16_t* pspx, uint16_t* pspy,
                     uint16_t* pwid, uint16_t* phei) {
  uint8_t* frameSprites = g_serumData.framesprites[quelleframe];
  uint16_t* frameSpriteBB = g_serumData.framespriteBB[quelleframe];
  const std::vector<SerumData::SpriteDetectionPlanEntryV2>* spritePlan =
      nullptr;
  if (quelleframe < g_serumData.spriteDetectionPlanV2.size()) {
    spritePlan = &g_serumData.spriteDetectionPlanV2[quelleframe];
  }
  // TODO(v6-cleanup): remove legacy sparse-vector fallback once we decide to
  // enforce presence of prebuilt spriteDetectionPlanV2 for all supported load
  // paths. The fallback currently exists for:
  // 1) backward compatibility with v5 cROMc (no persisted plan),
  // 2) defensive behavior when v6 runtime state is incomplete/corrupt.
  const bool usePlan = spritePlan && !spritePlan->empty();
  size_t planIndex = 0;
  uint8_t ti = 0;
  uint32_t mdword;
  *nspr = 0;
  bool isshapedframe = false;
  while (
      (usePlan && planIndex < spritePlan->size()) ||
      (!usePlan && (ti < MAX_SPRITES_PER_FRAME) && (frameSprites[ti] < 255))) {
    uint8_t qspr;
    short minxBB;
    short minyBB;
    short maxxBB;
    short maxyBB;
    int spw = 0;
    int sph = 0;
    bool isshapecheck = false;

    if (usePlan) {
      const SerumData::SpriteDetectionPlanEntryV2& planEntry =
          (*spritePlan)[planIndex++];
      qspr = planEntry.spriteId;
      minxBB = static_cast<short>(planEntry.minX);
      minyBB = static_cast<short>(planEntry.minY);
      maxxBB = static_cast<short>(planEntry.maxX);
      maxyBB = static_cast<short>(planEntry.maxY);
      spw = static_cast<int>(planEntry.spriteWidth);
      sph = static_cast<int>(planEntry.spriteHeight);
      isshapecheck = planEntry.shapeCheck;
    } else {
      qspr = frameSprites[ti];
      minxBB = static_cast<short>(frameSpriteBB[ti * 4]);
      minyBB = static_cast<short>(frameSpriteBB[ti * 4 + 1]);
      maxxBB = static_cast<short>(frameSpriteBB[ti * 4 + 2]);
      maxyBB = static_cast<short>(frameSpriteBB[ti * 4 + 3]);
      isshapecheck = g_serumData.sprshapemode[qspr][0] > 0;
      spw = (qspr < g_serumData.spriteWidthV2.size())
                ? g_serumData.spriteWidthV2[qspr]
                : 0;
      sph = (qspr < g_serumData.spriteHeightV2.size())
                ? g_serumData.spriteHeightV2[qspr]
                : 0;
    }

    uint8_t* Frame = recframe;
    if (isshapecheck) {
      if (!isshapedframe) {
        for (int i = 0; i < g_serumData.frameWidth * g_serumData.frameHeight;
             i++) {
          if (Frame[i] > 0)
            frameshape[i] = 1;
          else
            frameshape[i] = 0;
        }
        isshapedframe = true;
      }
      Frame = frameshape;
    }
    uint8_t* spriteOriginal = g_serumData.spriteoriginal[qspr];
    uint16_t* spriteDetAreas = g_serumData.spritedetareas[qspr];
    uint32_t* spriteDetDwords = g_serumData.spritedetdwords[qspr];
    uint16_t* spriteDetDwordPos = g_serumData.spritedetdwordpos[qspr];
    if (spw <= 0 || sph <= 0) {
      GetSpriteSize(qspr, &spw, &sph, spriteOriginal, MAX_SPRITE_WIDTH,
                    MAX_SPRITE_HEIGHT);
    }
    for (uint32_t tm = 0; tm < MAX_SPRITE_DETECT_AREAS; tm++) {
      if (spriteDetAreas[tm * 4] == 0xffff) continue;
      // we look for the sprite in the frame sent
      for (short ty = minyBB; ty <= maxyBB; ty++) {
        mdword =
            (uint32_t)(Frame[ty * g_serumData.frameWidth + minxBB] << 8) |
            (uint32_t)(Frame[ty * g_serumData.frameWidth + minxBB + 1] << 16) |
            (uint32_t)(Frame[ty * g_serumData.frameWidth + minxBB + 2] << 24);
        for (short tx = minxBB; tx <= maxxBB - 3; tx++) {
          uint32_t tj = ty * g_serumData.frameWidth + tx;
          mdword = (mdword >> 8) | (uint32_t)(Frame[tj + 3] << 24);
          // we look for the magic dword first:
          uint16_t sddp = spriteDetDwordPos[tm];
          if (mdword == spriteDetDwords[tm]) {
            short frax =
                (short)tx;  // position in the frame of the detection dword
            short fray = (short)ty;
            short sprx =
                (short)(sddp % MAX_SPRITE_WIDTH);  // position in the sprite of
                                                   // the detection dword
            short spry = (short)(sddp / MAX_SPRITE_WIDTH);
            // details of the det area:
            short detx =
                (short)spriteDetAreas[tm * 4];  // position of the detection
                                                // area in the sprite
            short dety = (short)spriteDetAreas[tm * 4 + 1];
            short detw = (short)
                spriteDetAreas[tm * 4 + 2];  // size of the detection area
            short deth = (short)spriteDetAreas[tm * 4 + 3];
            // if the detection area starts before the frame (left or top),
            // continue:
            if ((frax - minxBB < sprx - detx) || (fray - minyBB < spry - dety))
              continue;
            // position of the detection area in the frame
            int offsx = frax - sprx + detx;
            int offsy = fray - spry + dety;
            // if the detection area extends beyond the bounding box (right or
            // bottom), continue:
            if ((offsx + detw > (int)maxxBB + 1) ||
                (offsy + deth > (int)maxyBB + 1))
              continue;
            // we can now check if the full detection area is around the found
            // detection dword
            bool notthere = false;
            for (uint16_t tk = 0; tk < deth; tk++) {
              for (uint16_t tl = 0; tl < detw; tl++) {
                uint8_t val =
                    spriteOriginal[(tk + dety) * MAX_SPRITE_WIDTH + tl + detx];
                if (val == 255) continue;
                if (val !=
                    Frame[(tk + offsy) * g_serumData.frameWidth + tl + offsx]) {
                  notthere = true;
                  break;
                }
              }
              if (notthere == true) break;
            }
            if (!notthere) {
              pquelsprites[*nspr] = qspr;
              if (frax - minxBB < sprx) {
                pspx[*nspr] =
                    (uint16_t)(sprx -
                               (frax - minxBB));  // display sprite from point
                pfrx[*nspr] = (uint16_t)minxBB;
                pwid[*nspr] = std::min((uint16_t)(spw - pspx[*nspr]),
                                       (uint16_t)(maxxBB - minxBB + 1));
              } else {
                pspx[*nspr] = 0;
                pfrx[*nspr] = (uint16_t)(frax - sprx);
                pwid[*nspr] = std::min((uint16_t)(maxxBB - pfrx[*nspr] + 1),
                                       (uint16_t)spw);
              }
              if (fray - minyBB < spry) {
                pspy[*nspr] = (uint16_t)(spry - (fray - minyBB));
                pfry[*nspr] = (uint16_t)minyBB;
                phei[*nspr] = std::min((uint16_t)(sph - pspy[*nspr]),
                                       (uint16_t)(maxyBB - minyBB + 1));
              } else {
                pspy[*nspr] = 0;
                pfry[*nspr] = (uint16_t)(fray - spry);
                phei[*nspr] = std::min((uint16_t)(maxyBB - pfry[*nspr] + 1),
                                       (uint16_t)sph);
              }
              // we check the identical sprites as there may be duplicate due to
              // the multi detection zones
              bool identicalfound = false;
              for (uint8_t tk = 0; tk < *nspr; tk++) {
                if ((pquelsprites[*nspr] == pquelsprites[tk]) &&
                    (pfrx[*nspr] == pfrx[tk]) && (pfry[*nspr] == pfry[tk]) &&
                    (pwid[*nspr] == pwid[tk]) && (phei[*nspr] == phei[tk]))
                  identicalfound = true;
              }
              if (!identicalfound) {
                (*nspr)++;
                if (*nspr == MAX_SPRITES_PER_FRAME) return true;
              }
            }
          }
        }
      }
    }
    if (!usePlan) ti++;
  }
  if (*nspr > 0) return true;
  return false;
}

void Colorize_Framev1(uint8_t* frame, uint32_t IDfound) {
  uint16_t tj, ti;
  // Generate the colorized version of a frame once identified in the crom
  // frames
  for (tj = 0; tj < g_serumData.frameHeight; tj++) {
    for (ti = 0; ti < g_serumData.frameWidth; ti++) {
      uint16_t tk = tj * g_serumData.frameWidth + ti;

      if ((g_serumData.backgroundIDs[IDfound][0] <
           g_serumData.backgroundCount) &&
          (frame[tk] == 0) && (ti >= g_serumData.backgroundBB[IDfound][0]) &&
          (tj >= g_serumData.backgroundBB[IDfound][1]) &&
          (ti <= g_serumData.backgroundBB[IDfound][2]) &&
          (tj <= g_serumData.backgroundBB[IDfound][3]))
        mySerum.frame[tk] =
            g_serumData
                .backgroundframes[g_serumData.backgroundIDs[IDfound][0]][tk];
      else {
        uint8_t dynamicLayerIndex = g_serumData.dynamasks[IDfound][tk];
        if (dynamicLayerIndex == 255)
          mySerum.frame[tk] = g_serumData.cframes[IDfound][tk];
        else
          mySerum.frame[tk] =
              g_serumData.dyna4cols[IDfound]
                                   [dynamicLayerIndex * g_serumData.colorCount +
                                    frame[tk]];
      }
    }
  }
}

bool CheckExtraFrameAvailable(uint32_t frID) {
  // Check if there is an extra frame for this frame
  // (and if all the sprites and background involved are available)
  if (g_serumData.isextraframe[frID][0] == 0) return false;
  if (g_serumData.backgroundIDs[frID][0] < 0xffff &&
      g_serumData.isextrabackground[g_serumData.backgroundIDs[frID][0]][0] == 0)
    return false;
  for (uint32_t ti = 0; ti < MAX_SPRITES_PER_FRAME; ti++) {
    if (g_serumData.framesprites[frID][ti] < 255 &&
        g_serumData.isextrasprite[g_serumData.framesprites[frID][ti]][0] == 0)
      return false;
  }
  return true;
}

bool ColorInRotation(uint32_t IDfound, uint16_t col, uint16_t* norot,
                     uint16_t* posinrot, bool isextra) {
  *norot = 0xffff;
  if (IDfound < g_serumData.frameCount) {
    const std::vector<std::vector<uint16_t>>& lookupColors =
        isextra ? g_serumData.rotationLookupColorsV2Extra
                : g_serumData.rotationLookupColorsV2;
    const std::vector<std::vector<uint16_t>>& lookupMeta =
        isextra ? g_serumData.rotationLookupMetaV2Extra
                : g_serumData.rotationLookupMetaV2;
    if (IDfound < lookupColors.size() && IDfound < lookupMeta.size()) {
      const std::vector<uint16_t>& frameColors = lookupColors[IDfound];
      const std::vector<uint16_t>& frameMeta = lookupMeta[IDfound];
      for (size_t i = 0; i < frameColors.size() && i < frameMeta.size(); ++i) {
        if (frameColors[i] == col) {
          *norot = static_cast<uint16_t>((frameMeta[i] >> 8) & 0xff);
          *posinrot = static_cast<uint16_t>(frameMeta[i] & 0xff);
          return true;
        }
      }
    }
  }

  // TODO(v6-cleanup): remove this legacy scan once we decide to require
  // persisted/rebuilt rotation lookup vectors for all supported load paths.
  // The fallback is currently kept for:
  // 1) backward compatibility with v5 cROMc (no persisted lookup tables),
  // 2) defensive behavior when v6 lookup initialization is missing.
  uint16_t* pcol = isextra ? g_serumData.colorrotations_v2_extra[IDfound]
                           : g_serumData.colorrotations_v2[IDfound];
  for (uint32_t ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
    for (uint32_t tj = 2; tj < 2u + pcol[ti * MAX_LENGTH_COLOR_ROTATION];
         tj++)  // val [0] is for length and val [1] is for duration in ms
    {
      if (col == pcol[ti * MAX_LENGTH_COLOR_ROTATION + tj]) {
        *norot = ti;
        *posinrot =
            tj - 2;  // val [0] is for length and val [1] is for duration in ms
        return true;
      }
    }
  }
  return false;
}

void CheckDynaShadow(uint16_t* pfr, uint32_t nofr, uint8_t dynamicLayerIndex,
                     uint8_t* isdynapix, uint16_t fx, uint16_t fy, uint32_t fw,
                     uint32_t fh, bool isextra) {
  uint8_t dsdir;
  if (isextra)
    dsdir = g_serumData.dynashadowsdir_extra[nofr][dynamicLayerIndex];
  else
    dsdir = g_serumData.dynashadowsdir[nofr][dynamicLayerIndex];
  if (dsdir == 0) return;
  uint16_t tcol;
  if (isextra)
    tcol = g_serumData.dynashadowscol_extra[nofr][dynamicLayerIndex];
  else
    tcol = g_serumData.dynashadowscol[nofr][dynamicLayerIndex];
  if ((dsdir & 0b1) > 0 && fx > 0 && fy > 0 &&
      isdynapix[(fy - 1) * fw + fx - 1] == 0)  // dyna shadow top left
  {
    isdynapix[(fy - 1) * fw + fx - 1] = 1;
    pfr[(fy - 1) * fw + fx - 1] = tcol;
  }
  if ((dsdir & 0b10) > 0 && fy > 0 &&
      isdynapix[(fy - 1) * fw + fx] == 0)  // dyna shadow top
  {
    isdynapix[(fy - 1) * fw + fx] = 1;
    pfr[(fy - 1) * fw + fx] = tcol;
  }
  if ((dsdir & 0b100) > 0 && fx < fw - 1 && fy > 0 &&
      isdynapix[(fy - 1) * fw + fx + 1] == 0)  // dyna shadow top right
  {
    isdynapix[(fy - 1) * fw + fx + 1] = 1;
    pfr[(fy - 1) * fw + fx + 1] = tcol;
  }
  if ((dsdir & 0b1000) > 0 && fx < fw - 1 &&
      isdynapix[fy * fw + fx + 1] == 0)  // dyna shadow right
  {
    isdynapix[fy * fw + fx + 1] = 1;
    pfr[fy * fw + fx + 1] = tcol;
  }
  if ((dsdir & 0b10000) > 0 && fx < fw - 1 && fy < fh - 1 &&
      isdynapix[(fy + 1) * fw + fx + 1] == 0)  // dyna shadow bottom right
  {
    isdynapix[(fy + 1) * fw + fx + 1] = 1;
    pfr[(fy + 1) * fw + fx + 1] = tcol;
  }
  if ((dsdir & 0b100000) > 0 && fy < fh - 1 &&
      isdynapix[(fy + 1) * fw + fx] == 0)  // dyna shadow bottom
  {
    isdynapix[(fy + 1) * fw + fx] = 1;
    pfr[(fy + 1) * fw + fx] = tcol;
  }
  if ((dsdir & 0b1000000) > 0 && fx > 0 && fy < fh - 1 &&
      isdynapix[(fy + 1) * fw + fx - 1] == 0)  // dyna shadow bottom left
  {
    isdynapix[(fy + 1) * fw + fx - 1] = 1;
    pfr[(fy + 1) * fw + fx - 1] = tcol;
  }
  if ((dsdir & 0b10000000) > 0 && fx > 0 &&
      isdynapix[fy * fw + fx - 1] == 0)  // dyna shadow left
  {
    isdynapix[fy * fw + fx - 1] = 1;
    pfr[fy * fw + fx - 1] = tcol;
  }
}

void Colorize_Framev2(uint8_t* frame, uint32_t IDfound,
                      bool applySceneBackground = false,
                      bool blackOutStaticContent = false,
                      bool suppressFrameBackgroundImage = false) {
  uint16_t tj, ti;
  // TODO(v6-perf-step4): optional next optimization is a precompiled per-frame
  // render plan for this function (segment/static/dynamic/background passes)
  // stored in cROMc v6 and rebuilt for v5. Keep current logic unchanged until
  // users confirm scene lag is gone and no regressions from recent changes.
  // Generate the colorized version of a frame once identified in the crom
  // frames
  bool isextra = CheckExtraFrameAvailable(IDfound);
  mySerum.flags &= 0b11111100;
  uint16_t* pfr;
  uint16_t* prot;
  uint16_t* prt;
  uint32_t* cshft;
  uint16_t* pSceneBackgroundFrame;
  if (mySerum.frame32) mySerum.width32 = 0;
  if (mySerum.frame64) mySerum.width64 = 0;
  uint8_t isdynapix[256 * 64];
  if (((mySerum.frame32 && g_serumData.frameHeight == 32) ||
       (mySerum.frame64 && g_serumData.frameHeight == 64)) &&
      isoriginalrequested) {
    const uint16_t bgId = g_serumData.backgroundIDs[IDfound][0];
    const bool hasBackground = bgId < g_serumData.backgroundCount;
    uint8_t* backgroundMask = g_serumData.backgroundmask[IDfound];
    uint8_t* dynamasks = g_serumData.dynamasks[IDfound];
    uint16_t* cframesV2 = g_serumData.cframes_v2[IDfound];
    uint16_t* dyna4colsV2 = g_serumData.dyna4cols_v2[IDfound];
    uint16_t* backgroundFrame =
        hasBackground ? g_serumData.backgroundframes_v2[bgId] : nullptr;

    // create the original res frame
    if (g_serumData.frameHeight == 32) {
      pfr = mySerum.frame32;
      mySerum.flags |= FLAG_RETURNED_32P_FRAME_OK;
      prot = mySerum.rotationsinframe32;
      mySerum.width32 = g_serumData.frameWidth;
      prt = g_serumData.colorrotations_v2[IDfound];
      cshft = colorshifts32;
      pSceneBackgroundFrame = mySerum.frame32;
    } else {
      pfr = mySerum.frame64;
      mySerum.flags |= FLAG_RETURNED_64P_FRAME_OK;
      prot = mySerum.rotationsinframe64;
      mySerum.width64 = g_serumData.frameWidth;
      prt = g_serumData.colorrotations_v2[IDfound];
      cshft = colorshifts64;
      pSceneBackgroundFrame = mySerum.frame64;
    }
    if (applySceneBackground)
      memcpy(
          sceneBackgroundFrame, pSceneBackgroundFrame,
          g_serumData.frameWidth * g_serumData.frameHeight * sizeof(uint16_t));
    memset(isdynapix, 0, g_serumData.frameHeight * g_serumData.frameWidth);
    for (tj = 0; tj < g_serumData.frameHeight; tj++) {
      for (ti = 0; ti < g_serumData.frameWidth; ti++) {
        uint16_t tk = tj * g_serumData.frameWidth + ti;
        if (hasBackground && (frame[tk] == 0) && (backgroundMask[tk] > 0)) {
          if (isdynapix[tk] == 0) {
            if (applySceneBackground) {
              pfr[tk] = sceneBackgroundFrame[tk];
            } else if (!suppressFrameBackgroundImage) {
              pfr[tk] = backgroundFrame[tk];
              if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                  &prot[tk * 2 + 1], false))
                pfr[tk] =
                    prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                        (cshft[prot[tk * 2]] + prot[tk * 2 + 1]) %
                            prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
            } else {
              // Keep current output pixel: background image is a placeholder.
            }
          }
        } else {
          uint8_t dynamicLayerIndex = dynamasks[tk];
          if (dynamicLayerIndex == 255) {
            if (isdynapix[tk] == 0) {
              if (blackOutStaticContent && hasBackground && (frame[tk] > 0) &&
                  (backgroundMask[tk] > 0)) {
                pfr[tk] = sceneBackgroundFrame[tk];
              } else {
                pfr[tk] = cframesV2[tk];
                if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                    &prot[tk * 2 + 1], false))
                  pfr[tk] =
                      prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                          (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                              prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
              }
            }
          } else {
            if (frame[tk] > 0) {
              CheckDynaShadow(pfr, IDfound, dynamicLayerIndex, isdynapix, ti,
                              tj, g_serumData.frameWidth,
                              g_serumData.frameHeight, false);
              isdynapix[tk] = 1;
              pfr[tk] = dyna4colsV2[dynamicLayerIndex * g_serumData.colorCount +
                                    frame[tk]];
            } else if (isdynapix[tk] == 0)
              pfr[tk] = dyna4colsV2[dynamicLayerIndex * g_serumData.colorCount +
                                    frame[tk]];
            prot[tk * 2] = prot[tk * 2 + 1] = 0xffff;
          }
        }
      }
    }
  }
  if (isextra &&
      ((mySerum.frame32 && g_serumData.extraFrameHeight == 32) ||
       (mySerum.frame64 && g_serumData.extraFrameHeight == 64)) &&
      isextrarequested) {
    const uint16_t bgId = g_serumData.backgroundIDs[IDfound][0];
    const bool hasBackground = bgId < g_serumData.backgroundCount;
    uint8_t* backgroundMaskExtra = g_serumData.backgroundmask_extra[IDfound];
    uint8_t* dynamasksExtra = g_serumData.dynamasks_extra[IDfound];
    uint16_t* cframesV2Extra = g_serumData.cframes_v2_extra[IDfound];
    uint16_t* dyna4colsV2Extra = g_serumData.dyna4cols_v2_extra[IDfound];
    uint16_t* backgroundFrameExtra =
        hasBackground ? g_serumData.backgroundframes_v2_extra[bgId] : nullptr;

    // create the extra res frame
    if (g_serumData.extraFrameHeight == 32) {
      pfr = mySerum.frame32;
      mySerum.flags |= FLAG_RETURNED_32P_FRAME_OK;
      prot = mySerum.rotationsinframe32;
      mySerum.width32 = g_serumData.extraFrameWidth;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts32;
      pSceneBackgroundFrame = mySerum.frame32;
    } else {
      pfr = mySerum.frame64;
      mySerum.flags |= FLAG_RETURNED_64P_FRAME_OK;
      prot = mySerum.rotationsinframe64;
      mySerum.width64 = g_serumData.extraFrameWidth;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts64;
      pSceneBackgroundFrame = mySerum.frame64;
    }
    if (applySceneBackground)
      memcpy(sceneBackgroundFrame, pSceneBackgroundFrame,
             g_serumData.extraFrameWidth * g_serumData.extraFrameHeight *
                 sizeof(uint16_t));
    memset(isdynapix, 0,
           g_serumData.extraFrameHeight * g_serumData.extraFrameWidth);
    for (tj = 0; tj < g_serumData.extraFrameHeight; tj++) {
      for (ti = 0; ti < g_serumData.extraFrameWidth; ti++) {
        uint16_t tk = tj * g_serumData.extraFrameWidth + ti;
        uint16_t tl;
        if (g_serumData.extraFrameHeight == 64)
          tl = tj / 2 * g_serumData.frameWidth + ti / 2;
        else
          tl = tj * 2 * g_serumData.frameWidth + ti * 2;

        if (hasBackground && (frame[tl] == 0) &&
            (backgroundMaskExtra[tk] > 0)) {
          if (isdynapix[tk] == 0) {
            if (applySceneBackground) {
              pfr[tk] = sceneBackgroundFrame[tk];
            } else if (!suppressFrameBackgroundImage) {
              pfr[tk] = backgroundFrameExtra[tk];
              if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                  &prot[tk * 2 + 1], true)) {
                pfr[tk] =
                    prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                        (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                            prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
              }
            } else {
              // Keep current output pixel: background image is a placeholder.
            }
          }
        } else {
          uint8_t dynamicLayerIndex = dynamasksExtra[tk];
          if (dynamicLayerIndex == 255) {
            if (isdynapix[tk] == 0) {
              if (blackOutStaticContent && hasBackground && (frame[tl] > 0) &&
                  (backgroundMaskExtra[tk] > 0)) {
                pfr[tk] = sceneBackgroundFrame[tk];
              } else {
                pfr[tk] = cframesV2Extra[tk];
                if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                    &prot[tk * 2 + 1], true)) {
                  pfr[tk] =
                      prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                          (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                              prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
                }
              }
            }
          } else {
            if (frame[tl] > 0) {
              CheckDynaShadow(pfr, IDfound, dynamicLayerIndex, isdynapix, ti,
                              tj, g_serumData.extraFrameWidth,
                              g_serumData.extraFrameHeight, true);
              isdynapix[tk] = 1;
              pfr[tk] =
                  dyna4colsV2Extra[dynamicLayerIndex * g_serumData.colorCount +
                                   frame[tl]];
            } else if (isdynapix[tk] == 0)
              pfr[tk] =
                  dyna4colsV2Extra[dynamicLayerIndex * g_serumData.colorCount +
                                   frame[tl]];
            prot[tk * 2] = prot[tk * 2 + 1] = 0xffff;
          }
        }
      }
    }
  }
}

void Colorize_Spritev1(uint8_t spriteIndex, uint16_t frameX, uint16_t frameY,
                       uint16_t spriteX, uint16_t spriteY, uint16_t width,
                       uint16_t height) {
  for (uint16_t tj = 0; tj < height; tj++) {
    for (uint16_t ti = 0; ti < width; ti++) {
      if (g_serumData.spritedescriptionso[spriteIndex]
                                         [(tj + spriteY) * MAX_SPRITE_SIZE +
                                          ti + spriteX] < 255) {
        mySerum.frame[(frameY + tj) * g_serumData.frameWidth + frameX + ti] =
            g_serumData.spritedescriptionsc[spriteIndex]
                                           [(tj + spriteY) * MAX_SPRITE_SIZE +
                                            ti + spriteX];
      }
    }
  }
}

void Colorize_Spritev2(uint8_t* oframe, uint8_t spriteIndex, uint16_t frameX,
                       uint16_t frameY, uint16_t spriteX, uint16_t spriteY,
                       uint16_t width, uint16_t height, uint32_t IDfound) {
  uint16_t *pfr, *prot;
  uint16_t* prt;
  uint32_t* cshft;
  if (((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) &&
       g_serumData.frameHeight == 32) ||
      ((mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) &&
       g_serumData.frameHeight == 64)) {
    if (g_serumData.frameHeight == 32) {
      pfr = mySerum.frame32;
      prot = mySerum.rotationsinframe32;
      prt = g_serumData.colorrotations_v2[IDfound];
      cshft = colorshifts32;
    } else {
      pfr = mySerum.frame64;
      prot = mySerum.rotationsinframe64;
      prt = g_serumData.colorrotations_v2[IDfound];
      cshft = colorshifts64;
    }
    for (uint16_t tj = 0; tj < height; tj++) {
      for (uint16_t ti = 0; ti < width; ti++) {
        uint16_t tk = (frameY + tj) * g_serumData.frameWidth + frameX + ti;
        uint32_t tl = (tj + spriteY) * MAX_SPRITE_WIDTH + ti + spriteX;
        uint8_t spriteref = g_serumData.spriteoriginal[spriteIndex][tl];
        if (spriteref < 255) {
          uint8_t dynamicLayerIndex =
              g_serumData.dynaspritemasks[spriteIndex][tl];
          if (dynamicLayerIndex == 255) {
            pfr[tk] = g_serumData.spritecolored[spriteIndex][tl];
            if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                &prot[tk * 2 + 1], false))
              pfr[tk] = prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                            (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                                prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
          } else {
            pfr[tk] =
                g_serumData
                    .dynasprite4cols[spriteIndex][dynamicLayerIndex *
                                                      g_serumData.colorCount +
                                                  oframe[tk]];
            if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                &prot[tk * 2 + 1], false))
              pfr[tk] = prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                            (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                                prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
          }
        }
      }
    }
  }
  if (((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) &&
       g_serumData.extraFrameHeight == 32) ||
      ((mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) &&
       g_serumData.extraFrameHeight == 64)) {
    uint16_t scaledHeight, scaledWidth, scaledFrameX, scaledFrameY,
        scaledSpriteY, scaledSpriteX;
    if (g_serumData.extraFrameHeight == 32) {
      pfr = mySerum.frame32;
      prot = mySerum.rotationsinframe32;
      scaledHeight = height / 2;
      scaledWidth = width / 2;
      scaledFrameX = frameX / 2;
      scaledFrameY = frameY / 2;
      scaledSpriteX = spriteX / 2;
      scaledSpriteY = spriteY / 2;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts32;
    } else {
      pfr = mySerum.frame64;
      prot = mySerum.rotationsinframe64;
      scaledHeight = height * 2;
      scaledWidth = width * 2;
      scaledFrameX = frameX * 2;
      scaledFrameY = frameY * 2;
      scaledSpriteX = spriteX * 2;
      scaledSpriteY = spriteY * 2;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts64;
    }
    for (uint16_t tj = 0; tj < scaledHeight; tj++) {
      for (uint16_t ti = 0; ti < scaledWidth; ti++) {
        uint16_t tk = (scaledFrameY + tj) * g_serumData.extraFrameWidth +
                      scaledFrameX + ti;
        if (g_serumData.spritemask_extra[spriteIndex][(tj + scaledSpriteY) *
                                                          MAX_SPRITE_WIDTH +
                                                      ti + scaledSpriteX] <
            255) {
          uint8_t dynamicLayerIndex =
              g_serumData
                  .dynaspritemasks_extra[spriteIndex][(tj + scaledSpriteY) *
                                                          MAX_SPRITE_WIDTH +
                                                      ti + scaledSpriteX];
          if (dynamicLayerIndex == 255) {
            pfr[tk] =
                g_serumData
                    .spritecolored_extra[spriteIndex][(tj + scaledSpriteY) *
                                                          MAX_SPRITE_WIDTH +
                                                      ti + scaledSpriteX];
            if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                &prot[tk * 2 + 1], true))
              pfr[tk] = prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                            (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                                prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
          } else {
            uint16_t tl;
            if (g_serumData.extraFrameHeight == 64)
              tl = (tj / 2 + frameY) * g_serumData.frameWidth + ti / 2 + frameX;
            else
              tl = (tj * 2 + frameY) * g_serumData.frameWidth + ti * 2 + frameX;
            pfr[tk] =
                g_serumData.dynasprite4cols_extra[spriteIndex]
                                                 [dynamicLayerIndex *
                                                      g_serumData.colorCount +
                                                  oframe[tl]];
            if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                &prot[tk * 2 + 1], true))
              pfr[tk] = prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                            (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                                prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
          }
        }
      }
    }
  }
}

void Copy_Frame_Palette(uint32_t nofr) {
  memcpy(mySerum.palette, g_serumData.cpal[nofr],
         g_serumData.classicColorCount * 3);
}

SERUM_API void Serum_SetIgnoreUnknownFramesTimeout(uint16_t milliseconds) {
  ignoreUnknownFramesTimeout = milliseconds;
}

SERUM_API void Serum_SetMaximumUnknownFramesToSkip(uint8_t maximum) {
  maxFramesToSkip = maximum;
}

SERUM_API void Serum_SetGenerateCRomC(bool generate) {
  generateCRomC = generate;
}

SERUM_API void Serum_SetStandardPalette(const uint8_t* palette,
                                        const int bitDepth) {
  int palette_length = (1 << bitDepth) * 3;
  assert(palette_length < PALETTE_SIZE);

  if (palette_length <= PALETTE_SIZE) {
    memcpy(standardPalette, palette, palette_length);
    standardPaletteLength = palette_length;
  }
}

uint32_t Calc_Next_Rotationv1(uint32_t now) {
  uint32_t nextrot = 0xffffffff;
  for (int ti = 0; ti < MAX_COLOR_ROTATIONS; ti++) {
    if (mySerum.rotations[ti * 3] == 255) continue;
    if (colorrotnexttime[ti] < nextrot) nextrot = colorrotnexttime[ti];
  }
  if (nextrot == 0xffffffff) return 0;
  return nextrot - now;
}

uint32_t Serum_ColorizeWithMetadatav1(uint8_t* frame) {
  // return IDENTIFY_NO_FRAME if no new frame detected
  // return 0 if new frame with no rotation detected
  // return > 0 if new frame with rotations detected, the value is the delay
  // before the first rotation in ms
  mySerum.triggerID = 0xffffffff;

  if (!enabled) {
    // apply standard palette
    memcpy(mySerum.palette, standardPalette, standardPaletteLength);
    return 0;
  }

  // Let's first identify the incoming frame among the ones we have in the crom
  uint32_t frameID = Identify_Frame(frame, false);
  mySerum.frameID = IDENTIFY_NO_FRAME;
  uint32_t now = GetMonotonicTimeMs();
  if (is_real_machine() && !showStatusMessages) {
    showStatusMessages = (g_serumData.triggerIDs[lastfound][0] > 0xff98 &&
                          g_serumData.triggerIDs[lastfound][0] < 0xffffffff);
    if (showStatusMessages) ignoreUnknownFramesTimeout = 0x2000;
  }
  if (frameID != IDENTIFY_NO_FRAME && !showStatusMessages) {
    if ((monochromeMode || monochromePaletteMode) &&
        IsFullBlackFrame(frame,
                         g_serumData.frameWidth * g_serumData.frameHeight)) {
      frameID = IDENTIFY_NO_FRAME;
    }
    if (frameID != IDENTIFY_NO_FRAME) {
      uint32_t triggerId = g_serumData.triggerIDs[lastfound][0];
      monochromeMode = (triggerId == MONOCHROME_TRIGGER_ID);
      monochromePaletteMode = false;
      if (g_serumData.triggerIDs[lastfound][0] > 0xff98)
        g_serumData.triggerIDs[lastfound][0] = 0xffffffff;

      lastframe_found = now;
      if (maxFramesToSkip) {
        framesSkippedCounter = 0;
      }

      if (frameID == IDENTIFY_SAME_FRAME) {
        if (keepTriggersInternal ||
            mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
          mySerum.triggerID = 0xffffffff;
        return IDENTIFY_SAME_FRAME;
      }

      mySerum.frameID = frameID;
      mySerum.rotationtimer = 0;

      uint8_t spriteIds[MAX_SPRITES_PER_FRAME], spriteCount;
      uint16_t frameX[MAX_SPRITES_PER_FRAME], frameY[MAX_SPRITES_PER_FRAME],
          spriteX[MAX_SPRITES_PER_FRAME], spriteY[MAX_SPRITES_PER_FRAME],
          spriteWidth[MAX_SPRITES_PER_FRAME],
          spriteHeight[MAX_SPRITES_PER_FRAME];
      memset(spriteIds, 255, MAX_SPRITES_PER_FRAME);

      bool isspr = Check_Spritesv1(frame, (uint32_t)lastfound, spriteIds,
                                   &spriteCount, frameX, frameY, spriteX,
                                   spriteY, spriteWidth, spriteHeight);
      if (((frameID < MAX_NUMBER_FRAMES) || isspr) &&
          g_serumData.activeframes[lastfound][0] != 0) {
        Colorize_Framev1(frame, lastfound);
        Copy_Frame_Palette(lastfound);
        {
          uint32_t ti = 0;
          while (ti < spriteCount) {
            Colorize_Spritev1(spriteIds[ti], frameX[ti], frameY[ti],
                              spriteX[ti], spriteY[ti], spriteWidth[ti],
                              spriteHeight[ti]);
            ti++;
          }
        }
        memcpy(mySerum.rotations, g_serumData.colorrotations[lastfound],
               MAX_COLOR_ROTATIONS * 3);
        for (uint32_t ti = 0; ti < MAX_COLOR_ROTATIONS; ti++) {
          if (mySerum.rotations[ti * 3] == 255) {
            colorrotnexttime[ti] = 0;
            continue;
          }
          // Reset the timer if the previous frame had this rotation inactive or
          // if the last init time is more than a new rotation away. Otherwise,
          // we keep the already running timings for subsequent frames like
          // blinking PUSH START or GAME OVER.
          if ((colorshiftinittime[ti] + mySerum.rotations[ti * 3 + 2] * 10) <=
              now) {
            colorshiftinittime[ti] = now;
            colorrotnexttime[ti] =
                colorshiftinittime[ti] + mySerum.rotations[ti * 3 + 2] * 10;
          }

          if (colorrotnexttime[ti] <= now)
            colorrotnexttime[ti] =
                colorshiftinittime[ti] + mySerum.rotations[ti * 3 + 2] * 10;
        }

        mySerum.rotationtimer = Calc_Next_Rotationv1(now);

        if (g_serumData.triggerIDs[lastfound][0] != lastTriggerID ||
            lasttriggerTimestamp < (now - PUP_TRIGGER_REPEAT_TIMEOUT)) {
          lastTriggerID = mySerum.triggerID =
              g_serumData.triggerIDs[lastfound][0];
          lasttriggerTimestamp = now;
        }

        if (keepTriggersInternal ||
            mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
          mySerum.triggerID = 0xffffffff;

        return mySerum.rotationtimer;
      }
    }
  }

  mySerum.triggerID = 0xffffffff;

  if (monochromeMode ||
      (ignoreUnknownFramesTimeout &&
       (now - lastframe_found) >= ignoreUnknownFramesTimeout) ||
      (maxFramesToSkip && (frameID == IDENTIFY_NO_FRAME) &&
       (++framesSkippedCounter >= maxFramesToSkip))) {
    // apply standard palette
    memcpy(mySerum.palette, standardPalette, standardPaletteLength);
    // disable render features like rotations
    for (uint32_t ti = 0; ti < MAX_COLOR_ROTATIONS * 3; ti++) {
      mySerum.rotations[ti] = 255;
    }
    mySerum.rotationtimer = 0;
    return 0;  // new but not colorized frame, return true
  }

  return IDENTIFY_NO_FRAME;  // no new frame, return false, client has to update
                             // rotations!
}

uint32_t Calc_Next_Rotationv2(uint32_t now) {
  uint32_t nextrot = 0xffffffff;
  for (int ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
    if (mySerum.frame32 &&
        mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION] > 0 &&
        mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1] > 0) {
      if (colorrotnexttime32[ti] < nextrot) nextrot = colorrotnexttime32[ti];
    }
    if (mySerum.frame64 &&
        mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION] > 0 &&
        mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1] > 0) {
      if (colorrotnexttime64[ti] < nextrot) nextrot = colorrotnexttime64[ti];
    }
  }
  if (nextrot == 0xffffffff) return 0;
  return nextrot - now;
}

static void StopV2ColorRotations(void) {
  if (mySerum.rotations32) {
    std::memset(
        mySerum.rotations32, 0,
        MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * sizeof(uint16_t));
  }
  if (mySerum.rotations64) {
    std::memset(
        mySerum.rotations64, 0,
        MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * sizeof(uint16_t));
  }
  for (uint8_t ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
    colorrotnexttime32[ti] = 0;
    colorrotnexttime64[ti] = 0;
    colorshifts32[ti] = 0;
    colorshifts64[ti] = 0;
  }
}

static bool CaptureMonochromePaletteFromFrameV2(uint32_t frameId) {
  if (g_serumData.colorCount == 0 || g_serumData.colorCount > 16) {
    monochromePaletteV2Length = 0;
    return false;
  }
  const uint16_t* dyna = (g_serumData.frameHeight == 32)
                             ? g_serumData.dyna4cols_v2[frameId]
                             : g_serumData.dyna4cols_v2_extra[frameId];
  if (!dyna) {
    monochromePaletteV2Length = 0;
    return false;
  }
  const uint8_t ncolors = (uint8_t)g_serumData.colorCount;
  for (uint8_t i = 0; i < ncolors; i++) {
    monochromePaletteV2[i] = dyna[i];
  }
  monochromePaletteV2Length = ncolors;
  return true;
}

static bool IsFullBlackFrame(const uint8_t* frame, uint32_t size) {
  if (!frame || size == 0) return false;
  for (uint32_t i = 0; i < size; i++) {
    if (frame[i] != 0) return false;
  }
  return true;
}

static void ConfigureSceneEndHold(uint16_t sceneId, bool interruptable,
                                  uint8_t sceneOptions) {
  sceneEndHoldUntilMs = 0;
  sceneEndHoldDurationMs = 0;
  if (sceneOptions != 0 || interruptable || !g_serumData.sceneGenerator) {
    return;
  }

  uint32_t holdMs = 0;
  if (g_serumData.sceneGenerator->getSceneEndHoldDurationMs(sceneId, holdMs) &&
      holdMs > 0) {
    sceneEndHoldDurationMs = holdMs;
  }
}

static void ForceNormalFrameRefreshAfterSceneEnd(void) {
  // Force Identify_Frame(normal) to emit a concrete frame ID once after
  // scene teardown, even when the underlying DMD frame did not change.
  lastframe_full_crc_normal = 0xffffffff;
}

static void EnsureValidOutputDimensions(void) {
  if ((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) && mySerum.width32 == 0) {
    if (g_serumData.frameHeight == 32) {
      mySerum.width32 = g_serumData.frameWidth;
    } else if (g_serumData.extraFrameHeight == 32) {
      mySerum.width32 = g_serumData.extraFrameWidth;
    }
    if (mySerum.width32 == 0) {
      Log("Invalid 32P frame width=0, clearing 32P output flag");
      mySerum.flags &= ~FLAG_RETURNED_32P_FRAME_OK;
    }
  }

  if ((mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) && mySerum.width64 == 0) {
    if (g_serumData.frameHeight == 64) {
      mySerum.width64 = g_serumData.frameWidth;
    } else if (g_serumData.extraFrameHeight == 64) {
      mySerum.width64 = g_serumData.extraFrameWidth;
    }
    if (mySerum.width64 == 0) {
      Log("Invalid 64P frame width=0, clearing 64P output flag");
      mySerum.flags &= ~FLAG_RETURNED_64P_FRAME_OK;
    }
  }
}

SERUM_API uint32_t
Serum_ColorizeWithMetadatav2(uint8_t* frame, bool sceneFrameRequested = false,
                             uint32_t forcedFrameId = IDENTIFY_NO_FRAME) {
  // return IDENTIFY_NO_FRAME if no new frame detected
  // return 0 if new frame with no rotation detected
  // return > 0 if new frame with rotations detected, the value is the delay
  // before the first rotation in ms
  mySerum.triggerID = 0xffffffff;
  mySerum.frameID = IDENTIFY_NO_FRAME;

  // Let's first identify the incoming frame among the ones we have in the crom
  uint32_t frameID = IDENTIFY_NO_FRAME;
  if (forcedFrameId < MAX_NUMBER_FRAMES) {
    frameID = forcedFrameId;
    if (sceneFrameRequested) {
      lastfound_scene = forcedFrameId;
    } else {
      lastfound_normal = forcedFrameId;
    }
    lastfound = forcedFrameId;
  } else {
    frameID = Identify_Frame(frame, sceneFrameRequested);
  }
  uint32_t now = GetMonotonicTimeMs();
  bool rotationIsScene = false;
  if (is_real_machine() && !showStatusMessages) {
    showStatusMessages = (g_serumData.triggerIDs[lastfound][0] > 0xff98 &&
                          g_serumData.triggerIDs[lastfound][0] < 0xffffffff);
    if (showStatusMessages) ignoreUnknownFramesTimeout = 0x2000;
  }
  if (frameID != IDENTIFY_NO_FRAME && !showStatusMessages) {
    if ((monochromeMode || monochromePaletteMode) &&
        IsFullBlackFrame(frame,
                         g_serumData.frameWidth * g_serumData.frameHeight)) {
      frameID = IDENTIFY_NO_FRAME;
    }
    if (frameID != IDENTIFY_NO_FRAME) {
      uint32_t triggerId = g_serumData.triggerIDs[lastfound][0];
      monochromeMode = (triggerId == MONOCHROME_TRIGGER_ID);
      monochromePaletteMode = false;
      if (triggerId == MONOCHROME_PALETTE_TRIGGER_ID) {
        monochromePaletteMode = CaptureMonochromePaletteFromFrameV2(lastfound);
        monochromeMode = false;
      }
      if (g_serumData.triggerIDs[lastfound][0] > 0xff98)
        g_serumData.triggerIDs[lastfound][0] = 0xffffffff;

      if (!monochromeMode && g_serumData.sceneGenerator->isActive() &&
          !sceneFrameRequested &&
          (sceneCurrentFrame < sceneFrameCount || sceneEndHoldUntilMs > 0) &&
          !sceneInterruptable) {
        if (keepTriggersInternal ||
            mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
          mySerum.triggerID = 0xffffffff;
        // Scene is active and not interruptable
        return IDENTIFY_NO_FRAME;
      }

      // frame identified
      lastframe_found = now;
      if (maxFramesToSkip) {
        framesSkippedCounter = 0;
      }

      if (frameID == IDENTIFY_SAME_FRAME) {
        if (keepTriggersInternal ||
            mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
          mySerum.triggerID = 0xffffffff;
        return IDENTIFY_SAME_FRAME;
      }

      mySerum.frameID = frameID;
      if (!sceneFrameRequested) {
        memcpy(lastFrame, frame,
               g_serumData.frameWidth * g_serumData.frameHeight);
        lastFrameId = frameID;
        lastFrameCrcForSpriteCache = calc_crc32(
            lastFrame, 255,
            g_serumData.frameWidth * g_serumData.frameHeight, 0);
        lastFrameSpriteCacheValid = false;

        if (sceneFrameCount > 0 &&
            (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
                FLAG_SCENE_AS_BACKGROUND &&
            lastTriggerID < MONOCHROME_TRIGGER_ID &&
            g_serumData.triggerIDs[lastfound][0] == lastTriggerID) {
          // New frame has the same Trigger ID, continuing an already running
          // seamless looped scene.
          // Wait for the next rotation to have a smooth transition.
          return IDENTIFY_SAME_FRAME;
        } else if (sceneIsLastBackgroundFrame &&
                   (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
                       FLAG_SCENE_AS_BACKGROUND &&
                   lastTriggerID < MONOCHROME_TRIGGER_ID &&
                   g_serumData.triggerIDs[lastfound][0] == lastTriggerID) {
          // New frame has the same Trigger ID, continuing an already running.
        } else {
          if (sceneFrameCount > 0 &&
              (sceneOptionFlags & FLAG_SCENE_RESUME_IF_RETRIGGERED) ==
                  FLAG_SCENE_RESUME_IF_RETRIGGERED &&
              lastTriggerID < 0xffffffff &&
              sceneCurrentFrame < sceneFrameCount) {
            g_sceneResumeState[lastTriggerID] = {sceneCurrentFrame, now};
          }

          // stop any scene
          sceneFrameCount = 0;
          sceneIsLastBackgroundFrame = false;
          sceneEndHoldUntilMs = 0;
          sceneEndHoldDurationMs = 0;
          mySerum.rotationtimer = 0;

          // lastfound is set by Identify_Frame, check if we have a new PUP
          // trigger
          if (!monochromeMode &&
              (g_serumData.triggerIDs[lastfound][0] != lastTriggerID ||
               lasttriggerTimestamp < (now - PUP_TRIGGER_REPEAT_TIMEOUT))) {
            lastTriggerID = mySerum.triggerID =
                g_serumData.triggerIDs[lastfound][0];
            lasttriggerTimestamp = now;

            if (g_serumData.sceneGenerator->isActive() &&
                lastTriggerID < 0xffffffff) {
              if (g_serumData.sceneGenerator->getSceneInfo(
                      lastTriggerID, sceneFrameCount, sceneDurationPerFrame,
                      sceneInterruptable, sceneStartImmediately,
                      sceneRepeatCount, sceneOptionFlags)) {
                const bool sceneIsBackground =
                    (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
                    FLAG_SCENE_AS_BACKGROUND;
                if (sceneIsBackground) {
                  // Background scenes should not preempt rendering immediately.
                  sceneStartImmediately = false;
                } else {
                  // Foreground scenes and color rotations are mutually
                  // exclusive.
                  StopV2ColorRotations();
                }
                ConfigureSceneEndHold(lastTriggerID, sceneInterruptable,
                                      sceneOptionFlags);
                // Log(DMDUtil_LogLevel_DEBUG, "Serum: trigger ID %lu found in
                // scenes, frame count=%d, duration=%dms",
                //     m_pSerum->triggerID, sceneFrameCount,
                //     sceneDurationPerFrame);
                sceneCurrentFrame = 0;
                if ((sceneOptionFlags & FLAG_SCENE_RESUME_IF_RETRIGGERED) ==
                    FLAG_SCENE_RESUME_IF_RETRIGGERED) {
                  auto it = g_sceneResumeState.find(lastTriggerID);
                  if (it != g_sceneResumeState.end()) {
                    if ((now - it->second.timestampMs) <=
                            SCENE_RESUME_WINDOW_MS &&
                        it->second.nextFrame < sceneFrameCount) {
                      sceneCurrentFrame = it->second.nextFrame;
                    }
                    g_sceneResumeState.erase(it);
                  }
                } else {
                  g_sceneResumeState.erase(lastTriggerID);
                }
                if (sceneStartImmediately) {
                  uint32_t sceneRotationResult = Serum_RenderScene();
                  if (sceneRotationResult & FLAG_RETURNED_V2_SCENE)
                    return sceneRotationResult;
                }
                mySerum.rotationtimer = sceneDurationPerFrame;
                rotationIsScene = true;
              }
            }
          }
        }
      }

      bool isBackgroundScene = (sceneFrameCount > 0 &&
                                (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
                                    FLAG_SCENE_AS_BACKGROUND);
      bool suppressPlaceholderBackground =
          isBackgroundScene && !sceneFrameRequested;
      bool isBackgroundSceneRequested =
          isBackgroundScene && sceneFrameRequested;
      uint8_t spriteIds[MAX_SPRITES_PER_FRAME], spriteCount;
      uint16_t frameX[MAX_SPRITES_PER_FRAME], frameY[MAX_SPRITES_PER_FRAME],
          spriteX[MAX_SPRITES_PER_FRAME], spriteY[MAX_SPRITES_PER_FRAME],
          spriteWidth[MAX_SPRITES_PER_FRAME],
          spriteHeight[MAX_SPRITES_PER_FRAME];
      memset(spriteIds, 255, MAX_SPRITES_PER_FRAME);

      bool isspr = false;
      if (sceneFrameRequested && !isBackgroundSceneRequested) {
        isspr = false;
      } else if (isBackgroundSceneRequested) {
        // Background scene path reuses the same `lastFrame` while the scene is
        // advancing. Cache sprite detection result for that stable source frame
        // to avoid paying full sprite matching cost on every scene tick.
        if (lastFrameSpriteCacheValid &&
            lastFrameSpriteCacheFrameId == lastFrameId &&
            lastFrameSpriteCacheCrc == lastFrameCrcForSpriteCache) {
          spriteCount = lastFrameSpriteCacheCount;
          memcpy(spriteIds, lastFrameSpriteCacheIds, sizeof(spriteIds));
          memcpy(frameX, lastFrameSpriteCacheFrameX, sizeof(frameX));
          memcpy(frameY, lastFrameSpriteCacheFrameY, sizeof(frameY));
          memcpy(spriteX, lastFrameSpriteCacheSpriteX, sizeof(spriteX));
          memcpy(spriteY, lastFrameSpriteCacheSpriteY, sizeof(spriteY));
          memcpy(spriteWidth, lastFrameSpriteCacheWidth, sizeof(spriteWidth));
          memcpy(spriteHeight, lastFrameSpriteCacheHeight, sizeof(spriteHeight));
          isspr = spriteCount > 0;
        } else {
          isspr = Check_Spritesv2(lastFrame, lastFrameId, spriteIds, &spriteCount,
                                  frameX, frameY, spriteX, spriteY, spriteWidth,
                                  spriteHeight);
          lastFrameSpriteCacheValid = true;
          lastFrameSpriteCacheFrameId = lastFrameId;
          lastFrameSpriteCacheCrc = lastFrameCrcForSpriteCache;
          lastFrameSpriteCacheCount = spriteCount;
          memcpy(lastFrameSpriteCacheIds, spriteIds, sizeof(spriteIds));
          memcpy(lastFrameSpriteCacheFrameX, frameX, sizeof(frameX));
          memcpy(lastFrameSpriteCacheFrameY, frameY, sizeof(frameY));
          memcpy(lastFrameSpriteCacheSpriteX, spriteX, sizeof(spriteX));
          memcpy(lastFrameSpriteCacheSpriteY, spriteY, sizeof(spriteY));
          memcpy(lastFrameSpriteCacheWidth, spriteWidth, sizeof(spriteWidth));
          memcpy(lastFrameSpriteCacheHeight, spriteHeight,
                 sizeof(spriteHeight));
        }
      } else {
        isspr = Check_Spritesv2(frame, lastfound, spriteIds, &spriteCount,
                                frameX, frameY, spriteX, spriteY, spriteWidth,
                                spriteHeight);
      }
      if (((frameID < MAX_NUMBER_FRAMES) || isspr) &&
          g_serumData.activeframes[lastfound][0] != 0) {
        if (!sceneIsLastBackgroundFrame) {
          Colorize_Framev2(frame, lastfound, false, false,
                           suppressPlaceholderBackground);
        }
        if ((isBackgroundSceneRequested) || sceneIsLastBackgroundFrame) {
          Colorize_Framev2(
              lastFrame, lastFrameId, true,
              (sceneOptionFlags & FLAG_SCENE_ONLY_DYNAMIC_CONTENT) ==
                  FLAG_SCENE_ONLY_DYNAMIC_CONTENT);
        }
        if (isspr) {
          uint8_t ti = 0;
          while (ti < spriteCount) {
            Colorize_Spritev2(
                isBackgroundSceneRequested ? lastFrame : frame, spriteIds[ti],
                frameX[ti], frameY[ti], spriteX[ti], spriteY[ti],
                spriteWidth[ti], spriteHeight[ti],
                isBackgroundSceneRequested ? lastFrameId : lastfound);
            ti++;
          }
        }

        bool allowParallelRotations =
            (sceneFrameCount == 0) ||
            ((sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
             FLAG_SCENE_AS_BACKGROUND);
        if (!sceneFrameRequested && allowParallelRotations) {
          uint16_t *pcr32, *pcr64;
          if (g_serumData.frameHeight == 32) {
            pcr32 = g_serumData.colorrotations_v2[lastfound];
            pcr64 = g_serumData.colorrotations_v2_extra[lastfound];
          } else {
            pcr32 = g_serumData.colorrotations_v2_extra[lastfound];
            pcr64 = g_serumData.colorrotations_v2[lastfound];
          }

          bool isRotation = false;

          if (mySerum.frame32) {
            memcpy(mySerum.rotations32, pcr32,
                   MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * 2);
            for (uint8_t ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
              if (mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION] == 0 ||
                  mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1] ==
                      0) {
                colorrotnexttime32[ti] = 0;
                continue;
              }
              // Reset the timer if the previous frame had this rotation
              // inactive or if the last init time is more than a new rotation
              // away. Otherwise, we keep the already running timings for
              // subsequent frames like blinking PUSH START or GAME OVER.
              if (colorshiftinittime32[ti] +
                      mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1] <=
                  now) {
                colorshiftinittime32[ti] = now;
                colorrotnexttime32[ti] =
                    colorshiftinittime32[ti] +
                    mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1];
              }

              if (colorrotnexttime32[ti] <= now)
                colorrotnexttime32[ti] =
                    colorshiftinittime32[ti] +
                    mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1];

              isRotation = true;
            }
          }
          if (mySerum.frame64) {
            memcpy(mySerum.rotations64, pcr64,
                   MAX_COLOR_ROTATION_V2 * MAX_LENGTH_COLOR_ROTATION * 2);
            for (uint8_t ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
              if (mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION] == 0 ||
                  mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1] ==
                      0) {
                colorrotnexttime64[ti] = 0;
                continue;
              }
              // Reset the timer if the previous frame had this rotation
              // inactive or if the last init time is more than a new rotation
              // away. Otherwise, we keep the already running timings for
              // subsequent frames like blinking PUSH START or GAME OVER.
              if (colorshiftinittime64[ti] +
                      mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1] <=
                  now) {
                colorshiftinittime64[ti] = now;
                colorrotnexttime64[ti] =
                    colorshiftinittime64[ti] +
                    mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1];
              }

              if (colorrotnexttime64[ti] <= now)
                colorrotnexttime64[ti] =
                    colorshiftinittime64[ti] +
                    mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1];

              isRotation = true;
            }
          }

          uint32_t rotationTimer = isRotation ? Calc_Next_Rotationv2(now) : 0;
          if (rotationIsScene && mySerum.rotationtimer > 0) {
            if (rotationTimer == 0) {
              // Scene timer only.
            } else {
              mySerum.rotationtimer =
                  std::min(mySerum.rotationtimer, rotationTimer);
            }
          } else {
            mySerum.rotationtimer = rotationTimer;
          }
        }

        if (0 == mySerum.rotationtimer &&
            g_serumData.sceneGenerator->isActive() && !sceneFrameRequested &&
            sceneEndHoldUntilMs == 0 && sceneCurrentFrame >= sceneFrameCount &&
            g_serumData.sceneGenerator->getAutoStartSceneInfo(
                sceneFrameCount, sceneDurationPerFrame, sceneInterruptable,
                sceneStartImmediately, sceneRepeatCount, sceneOptionFlags)) {
          ConfigureSceneEndHold(
              g_serumData.sceneGenerator->getAutoStartSceneId(),
              sceneInterruptable, sceneOptionFlags);
          mySerum.rotationtimer =
              g_serumData.sceneGenerator->getAutoStartTimer();
          rotationIsScene = true;
        }

        if (keepTriggersInternal ||
            mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
          mySerum.triggerID = 0xffffffff;

        EnsureValidOutputDimensions();
        return (uint32_t)mySerum.rotationtimer |
               (rotationIsScene ? FLAG_RETURNED_V2_SCENE : 0);
      }
    }
  }

  mySerum.triggerID = 0xffffffff;

  if (monochromeMode || monochromePaletteMode ||
      (ignoreUnknownFramesTimeout &&
       (now - lastframe_found) >= ignoreUnknownFramesTimeout) ||
      (maxFramesToSkip && (frameID == IDENTIFY_NO_FRAME) &&
       (++framesSkippedCounter >= maxFramesToSkip))) {
    // apply monochrome frame colors
    for (uint16_t y = 0; y < g_serumData.frameHeight; y++) {
      for (uint16_t x = 0; x < g_serumData.frameWidth; x++) {
        uint8_t src = frame[y * g_serumData.frameWidth + x];
        if (monochromePaletteMode && monochromePaletteV2Length > 0 &&
            src < monochromePaletteV2Length) {
          mySerum.frame32[y * g_serumData.frameWidth + x] =
              monochromePaletteV2[src];
        } else if (g_serumData.colorCount < 16) {
          mySerum.frame32[y * g_serumData.frameWidth + x] = greyscale_4[src];
        } else {
          mySerum.frame32[y * g_serumData.frameWidth + x] = greyscale_16[src];
        }
      }
    }

    mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
    mySerum.width32 = g_serumData.frameWidth;
    mySerum.width64 = 0;
    mySerum.frameID = 0xfffffffd;  // monochrome frame ID

    // disable render features like rotations
    for (uint8_t ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
      colorrotnexttime32[ti] = 0;
      colorrotnexttime64[ti] = 0;
    }
    mySerum.rotationtimer = 0;

    EnsureValidOutputDimensions();
    return 0;  // "colorized" frame with no rotations
  }

  return IDENTIFY_NO_FRAME;  // no new frame, client has to update rotations!
}

SERUM_API uint32_t Serum_Colorize(uint8_t* frame) {
  // return IDENTIFY_NO_FRAME if no new frame detected
  // return 0 if new frame with no rotation detected
  // return > 0 if new frame with rotations detected, the value is the delay
  // before the first rotation in ms
  if (g_serumData.SerumVersion == SERUM_V2)
    return Serum_ColorizeWithMetadatav2(frame);
  else
    return Serum_ColorizeWithMetadatav1(frame);
}

uint32_t Serum_ApplyRotationsv1(void) {
  uint32_t isrotation = 0;
  uint32_t now = GetMonotonicTimeMs();
  for (int ti = 0; ti < MAX_COLOR_ROTATIONS; ti++) {
    if (mySerum.rotations[ti * 3] == 255) continue;
    uint32_t elapsed = now - colorshiftinittime[ti];
    if (elapsed >= (uint32_t)(mySerum.rotations[ti * 3 + 2] * 10)) {
      colorshifts[ti]++;
      colorshifts[ti] %= mySerum.rotations[ti * 3 + 1];
      colorshiftinittime[ti] = now;
      colorrotnexttime[ti] = now + mySerum.rotations[ti * 3 + 2] * 10;
      isrotation = FLAG_RETURNED_V1_ROTATED;
      uint8_t palsave[3 * 64];
      memcpy(palsave, &mySerum.palette[mySerum.rotations[ti * 3] * 3],
             (size_t)mySerum.rotations[ti * 3 + 1] * 3);
      for (int tj = 0; tj < mySerum.rotations[ti * 3 + 1]; tj++) {
        uint32_t shift = (tj + 1) % mySerum.rotations[ti * 3 + 1];
        mySerum.palette[(mySerum.rotations[ti * 3] + tj) * 3] =
            palsave[shift * 3];
        mySerum.palette[(mySerum.rotations[ti * 3] + tj) * 3 + 1] =
            palsave[shift * 3 + 1];
        mySerum.palette[(mySerum.rotations[ti * 3] + tj) * 3 + 2] =
            palsave[shift * 3 + 2];
      }
    }
  }
  mySerum.rotationtimer = (uint16_t)Calc_Next_Rotationv1(
      now);  // can't be more than 65s, so val is contained in the lower word of
             // val
  return ((uint32_t)mySerum.rotationtimer |
          isrotation);  // if there was a rotation, returns the next time in ms
                        // to the next one and set high dword to 1
                        // if not, just the delay to the next rotation
}

uint32_t Serum_RenderScene(void) {
  if (g_serumData.sceneGenerator->isActive() &&
      (sceneCurrentFrame < sceneFrameCount || sceneEndHoldUntilMs > 0)) {
    const uint32_t now = GetMonotonicTimeMs();
    if (sceneEndHoldUntilMs > 0) {
      if (now < sceneEndHoldUntilMs) {
        mySerum.rotationtimer = sceneEndHoldUntilMs - now;
        return (mySerum.rotationtimer & 0xffff) | FLAG_RETURNED_V2_SCENE;
      }

      // End hold elapsed: finish scene now.
      sceneEndHoldUntilMs = 0;
      sceneFrameCount = 0;
      mySerum.rotationtimer = 0;
      ForceNormalFrameRefreshAfterSceneEnd();

      switch (sceneOptionFlags) {
        case FLAG_SCENE_BLACK_WHEN_FINISHED:
          if (mySerum.frame32) memset(mySerum.frame32, 0, 32 * mySerum.width32);
          if (mySerum.frame64) memset(mySerum.frame64, 0, 64 * mySerum.width64);
          break;

        case FLAG_SCENE_SHOW_PREVIOUS_FRAME_WHEN_FINISHED:
          if (lastfound < MAX_NUMBER_FRAMES &&
              g_serumData.activeframes[lastfound][0] != 0) {
            Serum_ColorizeWithMetadatav2(lastFrame);
          } else {
            if (mySerum.frame32)
              memset(mySerum.frame32, 0, 32 * mySerum.width32);
            if (mySerum.frame64)
              memset(mySerum.frame64, 0, 64 * mySerum.width64);
          }
          break;

        case 0:  // keep the last frame of the scene
        default:
          if (sceneEndHoldDurationMs > 0 && !sceneInterruptable) {
            // autoStart+flag0 for non-interruptable scene means timed end-hold.
            break;
          }
          if (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) {
            sceneIsLastBackgroundFrame = true;
          }
          break;
      }

      return FLAG_RETURNED_V2_SCENE;
    }

    uint16_t result = g_serumData.sceneGenerator->generateFrame(
        lastTriggerID, sceneCurrentFrame, sceneFrame);
    // if result is 0xffff, the frame was generated and we can go
    if (0xffff == result) {
      mySerum.rotationtimer = sceneDurationPerFrame;
      uint32_t directSceneFrameId = IDENTIFY_NO_FRAME;
      uint8_t currentGroup = 0;
      if (g_serumData.sceneGenerator->getCurrentGroup(lastTriggerID,
                                                      currentGroup)) {
        const uint32_t sceneGroupKey =
            MakeSceneGroupKey(lastTriggerID, currentGroup);
        auto offsetIt =
            g_serumData.sceneGroupFrameTableOffset.find(sceneGroupKey);
        auto lengthIt =
            g_serumData.sceneGroupFrameTableLength.find(sceneGroupKey);
        if (offsetIt != g_serumData.sceneGroupFrameTableOffset.end() &&
            lengthIt != g_serumData.sceneGroupFrameTableLength.end() &&
            sceneCurrentFrame < lengthIt->second) {
          const uint32_t flatIndex = offsetIt->second + sceneCurrentFrame;
          if (flatIndex < g_serumData.sceneGroupFrameIdsFlat.size()) {
            directSceneFrameId = g_serumData.sceneGroupFrameIdsFlat[flatIndex];
          }
        }
      }
      if (directSceneFrameId >= MAX_NUMBER_FRAMES) {
        Log("Missing direct scene frame lookup for sceneId=%u group=%u "
            "frameIndex=%u",
            lastTriggerID, currentGroup, sceneCurrentFrame);
        sceneFrameCount = 0;
        mySerum.rotationtimer = 0;
        ForceNormalFrameRefreshAfterSceneEnd();
        return 0;
      }
      Serum_ColorizeWithMetadatav2(sceneFrame, true, directSceneFrameId);
      sceneCurrentFrame++;
      if (sceneCurrentFrame >= sceneFrameCount && sceneRepeatCount > 0) {
        if (sceneRepeatCount == 1) {
          sceneCurrentFrame = 0;  // loop
        } else {
          sceneCurrentFrame = 0;  // repeat the scene
          if (--sceneRepeatCount <= 1) {
            sceneRepeatCount = 0;  // no more repeat
          }
        }
      }

      if (sceneCurrentFrame >= sceneFrameCount) {
        if (sceneEndHoldDurationMs > 0) {
          sceneEndHoldUntilMs = now + sceneEndHoldDurationMs;
          mySerum.rotationtimer = sceneEndHoldDurationMs;
          return (mySerum.rotationtimer & 0xffff) | FLAG_RETURNED_V2_SCENE;
        }

        sceneFrameCount = 0;  // scene ended
        mySerum.rotationtimer = 0;
        ForceNormalFrameRefreshAfterSceneEnd();

        switch (sceneOptionFlags) {
          case FLAG_SCENE_BLACK_WHEN_FINISHED:
            if (mySerum.frame32)
              memset(mySerum.frame32, 0, 32 * mySerum.width32);
            if (mySerum.frame64)
              memset(mySerum.frame64, 0, 64 * mySerum.width64);
            break;

          case FLAG_SCENE_SHOW_PREVIOUS_FRAME_WHEN_FINISHED:
            if (lastfound < MAX_NUMBER_FRAMES &&
                g_serumData.activeframes[lastfound][0] != 0) {
              Serum_ColorizeWithMetadatav2(lastFrame);
            } else {
              if (mySerum.frame32)
                memset(mySerum.frame32, 0, 32 * mySerum.width32);
              if (mySerum.frame64)
                memset(mySerum.frame64, 0, 64 * mySerum.width64);
            }
            break;

          case 0:  // keep the last frame of the scene
          default:
            if (sceneEndHoldDurationMs > 0 && !sceneInterruptable) {
              // autoStart+flag0 for non-interruptable scene means timed
              // end-hold.
              break;
            }
            if (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) {
              sceneIsLastBackgroundFrame = true;
            }
            break;
        }
      }
    } else if (result > 0) {
      // frame not ready yet, return the time to wait
      mySerum.rotationtimer = result;
      return mySerum.rotationtimer | FLAG_RETURNED_V2_SCENE;
    } else {
      sceneFrameCount = 0;  // error generating scene frame, stop the scene
      mySerum.rotationtimer = 0;
      ForceNormalFrameRefreshAfterSceneEnd();
    }
    return (mySerum.rotationtimer & 0xffff) | FLAG_RETURNED_V2_ROTATED32 |
           FLAG_RETURNED_V2_ROTATED64 |
           FLAG_RETURNED_V2_SCENE;  // scene frame, so we consider both frames
                                    // changed
  }

  return 0;
}

uint32_t Serum_ApplyRotationsv2(void) {
  uint32_t sceneRotationResult = Serum_RenderScene();
  bool sceneIsActive = (sceneRotationResult & FLAG_RETURNED_V2_SCENE) != 0;
  bool sceneIsBackground =
      (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) == FLAG_SCENE_AS_BACKGROUND;
  if (sceneIsActive && !sceneIsBackground) {
    // Foreground scenes own the output; no parallel color rotations.
    return sceneRotationResult;
  }

  uint32_t sceneTimer = sceneRotationResult & 0xffff;
  uint32_t isrotation = sceneRotationResult & (FLAG_RETURNED_V2_ROTATED32 |
                                               FLAG_RETURNED_V2_ROTATED64);

  // rotation[0] = number of colors in rotation
  // rotation[1] = delay in ms between each color change
  // rotation[2..n] = color indexes

  uint32_t sizeframe;
  uint32_t now = GetMonotonicTimeMs();
  if (mySerum.frame32) {
    sizeframe = 32 * mySerum.width32;
    if (mySerum.modifiedelements32)
      memset(mySerum.modifiedelements32, 0, sizeframe);
    for (int ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
      if (mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION] == 0 ||
          mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1] == 0)
        continue;
      uint32_t elapsed = now - colorshiftinittime32[ti];
      if (elapsed >=
          (uint32_t)(mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1])) {
        colorshifts32[ti]++;
        colorshifts32[ti] %=
            mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION];
        colorshiftinittime32[ti] = now;
        colorrotnexttime32[ti] =
            now + mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1];
        isrotation |= FLAG_RETURNED_V2_ROTATED32;
        for (uint32_t tj = 0; tj < sizeframe; tj++) {
          if (mySerum.rotationsinframe32[tj * 2] == ti) {
            // if we have a pixel which is part of this rotation, we modify it
            mySerum.frame32[tj] =
                mySerum.rotations32
                    [ti * MAX_LENGTH_COLOR_ROTATION + 2 +
                     (mySerum.rotationsinframe32[tj * 2 + 1] +
                      colorshifts32[ti]) %
                         mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION]];
            if (mySerum.modifiedelements32) mySerum.modifiedelements32[tj] = 1;
          }
        }
      }
    }
  }
  if (mySerum.frame64) {
    sizeframe = 64 * mySerum.width64;
    if (mySerum.modifiedelements64)
      memset(mySerum.modifiedelements64, 0, sizeframe);
    for (int ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
      if (mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION] == 0 ||
          mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1] == 0)
        continue;
      uint32_t elapsed = now - colorshiftinittime64[ti];
      if (elapsed >=
          (uint32_t)(mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1])) {
        colorshifts64[ti]++;
        colorshifts64[ti] %=
            mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION];
        colorshiftinittime64[ti] = now;
        colorrotnexttime64[ti] =
            now + mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1];
        isrotation |= FLAG_RETURNED_V2_ROTATED64;
        for (uint32_t tj = 0; tj < sizeframe; tj++) {
          if (mySerum.rotationsinframe64[tj * 2] == ti) {
            // if we have a pixel which is part of this rotation, we modify it
            mySerum.frame64[tj] =
                mySerum.rotations64
                    [ti * MAX_LENGTH_COLOR_ROTATION + 2 +
                     (mySerum.rotationsinframe64[tj * 2 + 1] +
                      colorshifts64[ti]) %
                         mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION]];
            if (mySerum.modifiedelements64) mySerum.modifiedelements64[tj] = 1;
          }
        }
      }
    }
  }
  uint32_t rotationTimer = Calc_Next_Rotationv2(now) &
                           0xffff;  // can't be more than 2048ms, so val is
                                    // contained in the lower word of val
  if (rotationTimer > 2048)
    rotationTimer = 0;  // more than 2048ms is not possible, stop the rotation

  uint32_t nextTimer = 0;
  if (sceneTimer == 0)
    nextTimer = rotationTimer;
  else if (rotationTimer == 0)
    nextTimer = sceneTimer;
  else
    nextTimer = std::min(sceneTimer, rotationTimer);

  mySerum.rotationtimer = nextTimer;
  return mySerum.rotationtimer | isrotation |
         (sceneIsActive ? FLAG_RETURNED_V2_SCENE : 0);
}

SERUM_API uint32_t Serum_Rotate(void) {
  if (g_serumData.SerumVersion == SERUM_V2) {
    return Serum_ApplyRotationsv2();
  } else {
    return Serum_ApplyRotationsv1();
  }
  return 0;
}

SERUM_API void Serum_DisableColorization() { enabled = false; }

SERUM_API void Serum_EnableColorization() { enabled = true; }

SERUM_API void Serum_DisablePupTriggers(void) { keepTriggersInternal = true; }

SERUM_API void Serum_EnablePupTrigers(void) { keepTriggersInternal = false; }

SERUM_API bool Serum_Scene_ParseCSV(const char* const csv_filename) {
  if (!g_serumData.sceneGenerator) return false;
  return g_serumData.sceneGenerator->parseCSV(csv_filename);
}

SERUM_API bool Serum_Scene_GenerateDump(const char* const dump_filename,
                                        int id) {
  if (!g_serumData.sceneGenerator) return false;
  return g_serumData.sceneGenerator->generateDump(dump_filename, id);
}

SERUM_API bool Serum_Scene_GetInfo(uint16_t sceneId, uint16_t* frameCount,
                                   uint16_t* durationPerFrame,
                                   bool* interruptable, bool* startImmediately,
                                   uint8_t* repeat, uint8_t* sceneOptions) {
  if (!g_serumData.sceneGenerator) return false;
  return g_serumData.sceneGenerator->getSceneInfo(
      sceneId, *frameCount, *durationPerFrame, *interruptable,
      *startImmediately, *repeat, *sceneOptions);
}

SERUM_API bool Serum_Scene_GenerateFrame(uint16_t sceneId, uint16_t frameIndex,
                                         uint8_t* buffer, int group) {
  if (!g_serumData.sceneGenerator) return false;
  return (0xffff == g_serumData.sceneGenerator->generateFrame(
                        sceneId, frameIndex, buffer, group, true));
}

SERUM_API uint32_t Serum_Scene_Trigger(uint16_t sceneId) {
  if (!g_serumData.sceneGenerator || g_serumData.SerumVersion != SERUM_V2) {
    return 0;
  }

  // Do not interrupt an already running non-interruptable scene, unless it's
  // the same scene being retriggered and we are allowed to resume it.
  if (sceneFrameCount > 0 &&
      (sceneCurrentFrame < sceneFrameCount || sceneEndHoldUntilMs > 0) &&
      !sceneInterruptable && sceneId != lastTriggerID) {
    uint32_t wait =
        mySerum.rotationtimer ? mySerum.rotationtimer : sceneDurationPerFrame;
    return (wait & 0xffff) | FLAG_RETURNED_V2_SCENE;
  }

  uint16_t frameCount = 0;
  uint16_t durationPerFrame = 0;
  bool interruptable = false;
  bool startImmediately = false;
  uint8_t repeat = 0;
  uint8_t options = 0;

  if (!g_serumData.sceneGenerator->getSceneInfo(
          sceneId, frameCount, durationPerFrame, interruptable,
          startImmediately, repeat, options)) {
    return 0;
  }

  uint32_t now = GetMonotonicTimeMs();

  if (sceneFrameCount > 0 &&
      (sceneOptionFlags & FLAG_SCENE_RESUME_IF_RETRIGGERED) ==
          FLAG_SCENE_RESUME_IF_RETRIGGERED &&
      lastTriggerID < 0xffffffff && sceneCurrentFrame < sceneFrameCount) {
    g_sceneResumeState[lastTriggerID] = {sceneCurrentFrame, now};
  }

  sceneFrameCount = frameCount;
  sceneDurationPerFrame = durationPerFrame;
  sceneInterruptable = interruptable;
  sceneStartImmediately = startImmediately;
  sceneRepeatCount = repeat;
  sceneOptionFlags = options;
  if ((sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
      FLAG_SCENE_AS_BACKGROUND) {
    sceneStartImmediately = false;
  } else {
    StopV2ColorRotations();
  }
  ConfigureSceneEndHold(sceneId, sceneInterruptable, sceneOptionFlags);
  sceneIsLastBackgroundFrame = false;
  sceneCurrentFrame = 0;

  if ((sceneOptionFlags & FLAG_SCENE_RESUME_IF_RETRIGGERED) ==
      FLAG_SCENE_RESUME_IF_RETRIGGERED) {
    auto it = g_sceneResumeState.find(sceneId);
    if (it != g_sceneResumeState.end()) {
      if ((now - it->second.timestampMs) <= SCENE_RESUME_WINDOW_MS &&
          it->second.nextFrame < sceneFrameCount) {
        sceneCurrentFrame = it->second.nextFrame;
      }
      g_sceneResumeState.erase(it);
    }
  } else {
    g_sceneResumeState.erase(sceneId);
  }

  lastTriggerID = sceneId;
  lasttriggerTimestamp = now;
  mySerum.triggerID = sceneId;
  if (keepTriggersInternal || mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD) {
    mySerum.triggerID = 0xffffffff;
  }

  if (sceneStartImmediately) {
    return Serum_RenderScene();
  }

  mySerum.rotationtimer = sceneDurationPerFrame;
  return (mySerum.rotationtimer & 0xffff) | FLAG_RETURNED_V2_SCENE;
}

SERUM_API void Serum_Scene_SetDepth(uint8_t depth) {
  if (g_serumData.sceneGenerator) g_serumData.sceneGenerator->setDepth(depth);
}

SERUM_API int Serum_Scene_GetDepth(void) {
  if (!g_serumData.sceneGenerator) return 0;
  return g_serumData.sceneGenerator->getDepth();
}

SERUM_API bool Serum_Scene_IsActive(void) {
  if (!g_serumData.sceneGenerator) return false;
  return g_serumData.sceneGenerator->isActive();
}

SERUM_API void Serum_Scene_Reset(void) {
  if (g_serumData.sceneGenerator) g_serumData.sceneGenerator->Reset();
}
