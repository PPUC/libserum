#define __STDC_WANT_LIB_EXT1_ 1

#include "serum-decode.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
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
uint32_t sceneLastBackgroundFrameID = 0;
uint8_t sceneRepeatCount = 0;
uint8_t sceneOptionFlags = 0;
uint8_t sceneFrame[256 * 64] = {0};
uint8_t lastFrame[256 * 64] = {0};
uint32_t lastFrameId = 0;  // last frame ID identified
uint16_t sceneBackgroundFrame[256 * 64] = {0};
bool monochromeMode = false;
bool showStatusMessages = false;
bool keepTriggersInternal = false;

const int pathbuflen = 4096;

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

uint32_t Serum_RenderScene(void);

// variables
bool cromloaded = false;  // is there a crom loaded?
bool generateCRomC = true;
uint32_t lastfound = 0;  // last frame ID identified (current stream)
uint32_t lastfound_normal = 0;  // last frame ID for non-scene frames
uint32_t lastfound_scene = 0;   // last frame ID for scene frames
uint32_t lastframe_full_crc_normal = 0;
uint32_t lastframe_full_crc_scene = 0;
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
      !std::filesystem::is_directory(path_copy))
    return std::nullopt;

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
    return std::nullopt;
  }

  return std::nullopt;
}

void Free_element(void** ppElement) {
  // free a malloc block and set its pointer to NULL
  if (ppElement && *ppElement) {
    free(*ppElement);
    *ppElement = NULL;
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
  lastframe_full_crc_scene = 0;

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
  const uint32_t pixels = g_serumData.is256x64
                              ? (256 * 64)
                              : (g_serumData.fwidth * g_serumData.fheight);
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

  std::string concentratePath;

  // Remove extension and add .cROMc
  if (const char* dot = strrchr(filename, '.')) {
    concentratePath = std::string(filename, dot);
  }

  concentratePath += ".cROMc";

  return g_serumData.SaveToFile(concentratePath.c_str());
}

Serum_Frame_Struc* Serum_LoadConcentrate(const char* filename,
                                         const uint8_t flags) {
  if (!crc32_ready) CRC32encode();

  if (!g_serumData.LoadFromFile(filename, flags)) return NULL;

  FILE* pfile = fopen(filename, "rb");
  if (!pfile) return NULL;

  // Update mySerum structure
  mySerum.SerumVersion = g_serumData.SerumVersion;
  mySerum.flags = flags;
  mySerum.nocolors = g_serumData.nocolors;

  // Set requested frame types
  isoriginalrequested = false;
  isextrarequested = false;
  mySerum.width32 = 0;
  mySerum.width64 = 0;

  if (SERUM_V2 == g_serumData.SerumVersion) {
    if (g_serumData.fheight == 32) {
      if (flags & FLAG_REQUEST_32P_FRAMES) {
        isoriginalrequested = true;
        mySerum.width32 = g_serumData.fwidth;
      }
      if (flags & FLAG_REQUEST_64P_FRAMES) {
        isextrarequested = true;
        mySerum.width64 = g_serumData.fwidth_extra;
      }
    } else {
      if (flags & FLAG_REQUEST_64P_FRAMES) {
        isoriginalrequested = true;
        mySerum.width64 = g_serumData.fwidth;
      }
      if (flags & FLAG_REQUEST_32P_FRAMES) {
        isextrarequested = true;
        mySerum.width32 = g_serumData.fwidth_extra;
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
      for (uint32_t ti = 0; ti < g_serumData.nframes; ti++) {
        if (g_serumData.isextraframe[ti][0] > 0) {
          mySerum.flags |= FLAG_RETURNED_EXTRA_AVAILABLE;
          break;
        }
      }
    }

    frameshape = (uint8_t*)malloc(g_serumData.fwidth * g_serumData.fheight);
    if (!frameshape) {
      Serum_free();
      enabled = false;
      return NULL;
    }
  } else if (SERUM_V1 == g_serumData.SerumVersion) {
    if (g_serumData.fheight == 64) {
      mySerum.width64 = g_serumData.fwidth;
      mySerum.width32 = 0;
    } else {
      mySerum.width32 = g_serumData.fwidth;
      mySerum.width64 = 0;
    }

    mySerum.frame = (uint8_t*)malloc(g_serumData.fwidth * g_serumData.fheight);
    mySerum.palette = (uint8_t*)malloc(3 * 64);
    mySerum.rotations = (uint8_t*)malloc(MAX_COLOR_ROTATIONS * 3);
    if (!mySerum.frame || !mySerum.palette || !mySerum.rotations) {
      Serum_free();
      fclose(pfile);
      enabled = false;
      return NULL;
    }
  }

  mySerum.ntriggers = 0;
  for (uint32_t ti = 0; ti < g_serumData.nframes; ti++) {
    // Every trigger ID greater than PUP_TRIGGER_MAX_THRESHOLD is an internal
    // trigger for rotation scenes and must not be communicated to the PUP
    // Player.
    if (g_serumData.triggerIDs[ti][0] < PUP_TRIGGER_MAX_THRESHOLD)
      mySerum.ntriggers++;
  }

  // Allocate framechecked array
  framechecked = (bool*)malloc(sizeof(bool) * g_serumData.nframes);
  if (!framechecked) {
    Serum_free();
    enabled = false;
    return NULL;
  }

  Full_Reset_ColorRotations();
  cromloaded = true;
  enabled = true;

  return &mySerum;
}

Serum_Frame_Struc* Serum_LoadFilev2(FILE* pfile, const uint8_t flags,
                                    bool uncompressedCROM, char* pathbuf,
                                    uint32_t sizeheader) {
  fread(&g_serumData.fwidth, 4, 1, pfile);
  fread(&g_serumData.fheight, 4, 1, pfile);
  fread(&g_serumData.fwidth_extra, 4, 1, pfile);
  fread(&g_serumData.fheight_extra, 4, 1, pfile);
  isoriginalrequested = false;
  isextrarequested = false;
  mySerum.width32 = 0;
  mySerum.width64 = 0;
  if (g_serumData.fheight == 32) {
    if (flags & FLAG_REQUEST_32P_FRAMES) {
      isoriginalrequested = true;
      mySerum.width32 = g_serumData.fwidth;
    }
    if (flags & FLAG_REQUEST_64P_FRAMES) {
      isextrarequested = true;
      mySerum.width64 = g_serumData.fwidth_extra;
    }

  } else {
    if (flags & FLAG_REQUEST_64P_FRAMES) {
      isoriginalrequested = true;
      mySerum.width64 = g_serumData.fwidth;
    }
    if (flags & FLAG_REQUEST_32P_FRAMES) {
      isextrarequested = true;
      mySerum.width32 = g_serumData.fwidth_extra;
    }
  }
  fread(&g_serumData.nframes, 4, 1, pfile);
  fread(&g_serumData.nocolors, 4, 1, pfile);
  mySerum.nocolors = g_serumData.nocolors;
  if ((g_serumData.fwidth == 0) || (g_serumData.fheight == 0) ||
      (g_serumData.nframes == 0) || (g_serumData.nocolors == 0)) {
    // incorrect file format
    fclose(pfile);
    enabled = false;
    return NULL;
  }
  fread(&g_serumData.ncompmasks, 4, 1, pfile);
  fread(&g_serumData.nsprites, 4, 1, pfile);
  fread(&g_serumData.nbackgrounds, 2, 1,
        pfile);  // g_serumData.nbackgrounds is a uint16_t
  if (sizeheader >= 20 * sizeof(uint32_t)) {
    int is256x64;
    fread(&is256x64, sizeof(int), 1, pfile);
    g_serumData.is256x64 = (is256x64 != 0);
  }

  frameshape = (uint8_t*)malloc(g_serumData.fwidth * g_serumData.fheight);

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

  g_serumData.hashcodes.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.shapecompmode.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.compmaskID.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.compmasks.readFromCRomFile(
      g_serumData.is256x64 ? (256 * 64)
                           : (g_serumData.fwidth * g_serumData.fheight),
      g_serumData.ncompmasks, pfile);
  g_serumData.isextraframe.readFromCRomFile(1, g_serumData.nframes, pfile);
  if (isextrarequested) {
    for (uint32_t ti = 0; ti < g_serumData.nframes; ti++) {
      if (g_serumData.isextraframe[ti][0] > 0) {
        mySerum.flags |= FLAG_RETURNED_EXTRA_AVAILABLE;
        break;
      }
    }
  } else
    g_serumData.isextraframe.clearIndex();
  g_serumData.cframes_v2.readFromCRomFile(
      g_serumData.fwidth * g_serumData.fheight, g_serumData.nframes, pfile);
  g_serumData.cframes_v2_extra.readFromCRomFile(
      g_serumData.fwidth_extra * g_serumData.fheight_extra, g_serumData.nframes,
      pfile, &g_serumData.isextraframe);
  g_serumData.dynamasks.readFromCRomFile(
      g_serumData.fwidth * g_serumData.fheight, g_serumData.nframes, pfile);
  g_serumData.dynamasks_extra.readFromCRomFile(
      g_serumData.fwidth_extra * g_serumData.fheight_extra, g_serumData.nframes,
      pfile, &g_serumData.isextraframe);
  g_serumData.dyna4cols_v2.readFromCRomFile(
      MAX_DYNA_SETS_PER_FRAME_V2 * g_serumData.nocolors, g_serumData.nframes,
      pfile);
  g_serumData.dyna4cols_v2_extra.readFromCRomFile(
      MAX_DYNA_SETS_PER_FRAME_V2 * g_serumData.nocolors, g_serumData.nframes,
      pfile, &g_serumData.isextraframe);
  g_serumData.isextrasprite.readFromCRomFile(1, g_serumData.nsprites, pfile);
  if (!isextrarequested) g_serumData.isextrasprite.clearIndex();
  g_serumData.framesprites.readFromCRomFile(MAX_SPRITES_PER_FRAME,
                                            g_serumData.nframes, pfile);
  g_serumData.spriteoriginal.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.nsprites, pfile);
  g_serumData.spritecolored.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.nsprites, pfile);
  g_serumData.spritemask_extra.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.nsprites, pfile,
      &g_serumData.isextrasprite);
  g_serumData.spritecolored_extra.readFromCRomFile(
      MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.nsprites, pfile,
      &g_serumData.isextrasprite);
  g_serumData.activeframes.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.colorrotations_v2.readFromCRomFile(
      MAX_LENGTH_COLOR_ROTATION * MAX_COLOR_ROTATION_V2, g_serumData.nframes,
      pfile);
  g_serumData.colorrotations_v2_extra.readFromCRomFile(
      MAX_LENGTH_COLOR_ROTATION * MAX_COLOR_ROTATION_V2, g_serumData.nframes,
      pfile, &g_serumData.isextraframe);
  g_serumData.spritedetdwords.readFromCRomFile(MAX_SPRITE_DETECT_AREAS,
                                               g_serumData.nsprites, pfile);
  g_serumData.spritedetdwordpos.readFromCRomFile(MAX_SPRITE_DETECT_AREAS,
                                                 g_serumData.nsprites, pfile);
  g_serumData.spritedetareas.readFromCRomFile(4 * MAX_SPRITE_DETECT_AREAS,
                                              g_serumData.nsprites, pfile);
  g_serumData.triggerIDs.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.framespriteBB.readFromCRomFile(MAX_SPRITES_PER_FRAME * 4,
                                             g_serumData.nframes, pfile,
                                             &g_serumData.framesprites);
  g_serumData.isextrabackground.readFromCRomFile(1, g_serumData.nbackgrounds,
                                                 pfile);
  if (!isextrarequested) g_serumData.isextrabackground.clearIndex();
  g_serumData.backgroundframes_v2.readFromCRomFile(
      g_serumData.fwidth * g_serumData.fheight, g_serumData.nbackgrounds,
      pfile);
  g_serumData.backgroundframes_v2_extra.readFromCRomFile(
      g_serumData.fwidth_extra * g_serumData.fheight_extra,
      g_serumData.nbackgrounds, pfile, &g_serumData.isextrabackground);
  g_serumData.backgroundIDs.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.backgroundmask.readFromCRomFile(
      g_serumData.fwidth * g_serumData.fheight, g_serumData.nframes, pfile,
      &g_serumData.backgroundIDs);
  g_serumData.backgroundmask_extra.readFromCRomFile(
      g_serumData.fwidth_extra * g_serumData.fheight_extra, g_serumData.nframes,
      pfile, &g_serumData.backgroundIDs);

  if (sizeheader >= 15 * sizeof(uint32_t)) {
    g_serumData.dynashadowsdir.readFromCRomFile(MAX_DYNA_SETS_PER_FRAME_V2,
                                                g_serumData.nframes, pfile);
    g_serumData.dynashadowscol.readFromCRomFile(MAX_DYNA_SETS_PER_FRAME_V2,
                                                g_serumData.nframes, pfile);
    g_serumData.dynashadowsdir_extra.readFromCRomFile(
        MAX_DYNA_SETS_PER_FRAME_V2, g_serumData.nframes, pfile,
        &g_serumData.isextraframe);
    g_serumData.dynashadowscol_extra.readFromCRomFile(
        MAX_DYNA_SETS_PER_FRAME_V2, g_serumData.nframes, pfile,
        &g_serumData.isextraframe);
  } else {
    g_serumData.dynashadowsdir.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
    g_serumData.dynashadowscol.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
    g_serumData.dynashadowsdir_extra.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
    g_serumData.dynashadowscol_extra.reserve(MAX_DYNA_SETS_PER_FRAME_V2);
  }

  if (sizeheader >= 18 * sizeof(uint32_t)) {
    g_serumData.dynasprite4cols.readFromCRomFile(
        MAX_DYNA_SETS_PER_SPRITE * g_serumData.nocolors, g_serumData.nsprites,
        pfile);
    g_serumData.dynasprite4cols_extra.readFromCRomFile(
        MAX_DYNA_SETS_PER_SPRITE * g_serumData.nocolors, g_serumData.nsprites,
        pfile, &g_serumData.isextraframe);
    g_serumData.dynaspritemasks.readFromCRomFile(
        MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.nsprites, pfile);
    g_serumData.dynaspritemasks_extra.readFromCRomFile(
        MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT, g_serumData.nsprites, pfile,
        &g_serumData.isextraframe);
  } else {
    g_serumData.dynasprite4cols.reserve(MAX_DYNA_SETS_PER_SPRITE *
                                        g_serumData.nocolors);
    g_serumData.dynasprite4cols_extra.reserve(MAX_DYNA_SETS_PER_SPRITE *
                                              g_serumData.nocolors);
    g_serumData.dynaspritemasks.reserve(MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT);
    g_serumData.dynaspritemasks_extra.reserve(MAX_SPRITE_WIDTH *
                                              MAX_SPRITE_HEIGHT);
  }

  if (sizeheader >= 19 * sizeof(uint32_t)) {
    g_serumData.sprshapemode.readFromCRomFile(1, g_serumData.nsprites, pfile);
    for (uint32_t i = 0; i < g_serumData.nsprites; i++) {
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
    g_serumData.sprshapemode.reserve(g_serumData.nsprites);
  }

  fclose(pfile);

  mySerum.ntriggers = 0;
  uint32_t framespos = g_serumData.nframes / 2;
  uint32_t framesspace = g_serumData.nframes - framespos;
  uint32_t framescount = (framesspace + 9) / 10;

  if (framescount > 0) {
    std::vector<uint32_t> candidates;
    candidates.reserve(framesspace);
    for (uint32_t ti = framespos; ti < g_serumData.nframes; ++ti) {
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

      for (uint32_t offset = 0; (framespos + offset) < g_serumData.nframes;
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
  for (uint32_t ti = 0; ti < g_serumData.nframes; ti++) {
    // Every trigger ID greater than PUP_TRIGGER_MAX_THRESHOLD is an internal
    // trigger for rotation scenes and must not be communicated to the PUP
    // Player.
    if (g_serumData.triggerIDs[ti][0] < PUP_TRIGGER_MAX_THRESHOLD)
      mySerum.ntriggers++;
  }
  framechecked = (bool*)malloc(sizeof(bool) * g_serumData.nframes);
  if (!framechecked) {
    Serum_free();
    enabled = false;
    return NULL;
  }
  if (flags & FLAG_REQUEST_32P_FRAMES) {
    if (g_serumData.fheight == 32)
      mySerum.width32 = g_serumData.fwidth;
    else
      mySerum.width32 = g_serumData.fwidth_extra;
  } else
    mySerum.width32 = 0;
  if (flags & FLAG_REQUEST_64P_FRAMES) {
    if (g_serumData.fheight == 32)
      mySerum.width64 = g_serumData.fwidth_extra;
    else
      mySerum.width64 = g_serumData.fwidth;
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
  fread(g_serumData.rname, 1, 64, pfile);
  uint32_t sizeheader;
  fread(&sizeheader, 4, 1, pfile);
  // if this is a new format file, we load with Serum_LoadNewFile()
  if (sizeheader >= 14 * sizeof(uint32_t))
    return Serum_LoadFilev2(pfile, flags, uncompressedCROM, pathbuf,
                            sizeheader);
  mySerum.SerumVersion = g_serumData.SerumVersion = SERUM_V1;
  fread(&g_serumData.fwidth, 4, 1, pfile);
  fread(&g_serumData.fheight, 4, 1, pfile);
  // The serum file stored the number of frames as uint32_t, but in fact, the
  // number of frames will never exceed the size of uint16_t (65535)
  uint32_t nframes32;
  fread(&nframes32, 4, 1, pfile);
  g_serumData.nframes = (uint16_t)nframes32;
  fread(&g_serumData.nocolors, 4, 1, pfile);
  mySerum.nocolors = g_serumData.nocolors;
  fread(&g_serumData.nccolors, 4, 1, pfile);
  if ((g_serumData.fwidth == 0) || (g_serumData.fheight == 0) ||
      (g_serumData.nframes == 0) || (g_serumData.nocolors == 0) ||
      (g_serumData.nccolors == 0)) {
    // incorrect file format
    fclose(pfile);
    enabled = false;
    return NULL;
  }
  fread(&g_serumData.ncompmasks, 4, 1, pfile);
  fread(&g_serumData.nmovmasks, 4, 1, pfile);
  fread(&g_serumData.nsprites, 4, 1, pfile);
  if (sizeheader >= 13 * sizeof(uint32_t))
    fread(&g_serumData.nbackgrounds, 2, 1, pfile);
  else
    g_serumData.nbackgrounds = 0;
  // allocate memory for the serum format
  uint8_t* spritedescriptionso = (uint8_t*)malloc(
      g_serumData.nsprites * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);
  uint8_t* spritedescriptionsc = (uint8_t*)malloc(
      g_serumData.nsprites * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);

  mySerum.frame = (uint8_t*)malloc(g_serumData.fwidth * g_serumData.fheight);
  mySerum.palette = (uint8_t*)malloc(3 * 64);
  mySerum.rotations = (uint8_t*)malloc(MAX_COLOR_ROTATIONS * 3);
  if (((g_serumData.nsprites > 0) &&
       (!spritedescriptionso || !spritedescriptionsc)) ||
      !mySerum.frame || !mySerum.palette || !mySerum.rotations) {
    Serum_free();
    fclose(pfile);
    enabled = false;
    return NULL;
  }
  // read the cRom file
  g_serumData.hashcodes.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.shapecompmode.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.compmaskID.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.movrctID.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.movrctID.clear();  // we don't need this anymore, but we need to
                                 // read it to skip the data in the file
  g_serumData.compmasks.readFromCRomFile(
      g_serumData.fwidth * g_serumData.fheight, g_serumData.ncompmasks, pfile);
  g_serumData.movrcts.readFromCRomFile(g_serumData.fwidth * g_serumData.fheight,
                                       g_serumData.nmovmasks, pfile);
  g_serumData.movrcts.clear();  // we don't need this anymore, but we need to
                                // read it to skip the data in the file
  g_serumData.cpal.readFromCRomFile(3 * g_serumData.nccolors,
                                    g_serumData.nframes, pfile);
  g_serumData.cframes.readFromCRomFile(g_serumData.fwidth * g_serumData.fheight,
                                       g_serumData.nframes, pfile);
  g_serumData.dynamasks.readFromCRomFile(
      g_serumData.fwidth * g_serumData.fheight, g_serumData.nframes, pfile);
  g_serumData.dyna4cols.readFromCRomFile(
      MAX_DYNA_4COLS_PER_FRAME * g_serumData.nocolors, g_serumData.nframes,
      pfile);
  g_serumData.framesprites.readFromCRomFile(MAX_SPRITES_PER_FRAME,
                                            g_serumData.nframes, pfile);

  for (int ti = 0;
       ti < (int)g_serumData.nsprites * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE;
       ti++) {
    fread(&spritedescriptionsc[ti], 1, 1, pfile);
    fread(&spritedescriptionso[ti], 1, 1, pfile);
  }
  for (uint32_t i = 0; i < g_serumData.nsprites; i++) {
    g_serumData.spritedescriptionsc.set(
        i, &spritedescriptionsc[i * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE],
        MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);
    g_serumData.spritedescriptionso.set(
        i, &spritedescriptionso[i * MAX_SPRITE_SIZE * MAX_SPRITE_SIZE],
        MAX_SPRITE_SIZE * MAX_SPRITE_SIZE);
  }
  Free_element((void**)&spritedescriptionso);
  Free_element((void**)&spritedescriptionsc);

  g_serumData.activeframes.readFromCRomFile(1, g_serumData.nframes, pfile);
  g_serumData.colorrotations.readFromCRomFile(3 * MAX_COLOR_ROTATIONS,
                                              g_serumData.nframes, pfile);
  g_serumData.spritedetdwords.readFromCRomFile(MAX_SPRITE_DETECT_AREAS,
                                               g_serumData.nsprites, pfile);
  g_serumData.spritedetdwordpos.readFromCRomFile(MAX_SPRITE_DETECT_AREAS,
                                                 g_serumData.nsprites, pfile);
  g_serumData.spritedetareas.readFromCRomFile(4 * MAX_SPRITE_DETECT_AREAS,
                                              g_serumData.nsprites, pfile);
  mySerum.ntriggers = 0;
  if (sizeheader >= 11 * sizeof(uint32_t)) {
    g_serumData.triggerIDs.readFromCRomFile(1, g_serumData.nframes, pfile);
  }
  uint32_t framespos = g_serumData.nframes / 2;
  uint32_t framesspace = g_serumData.nframes - framespos;
  uint32_t framescount = (framesspace + 9) / 10;

  if (framescount > 0) {
    std::vector<uint32_t> candidates;
    candidates.reserve(framesspace);
    for (uint32_t ti = framespos; ti < g_serumData.nframes; ++ti) {
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

      for (uint32_t offset = 0; (framespos + offset) < g_serumData.nframes;
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
  for (uint32_t ti = 0; ti < g_serumData.nframes; ti++) {
    if (g_serumData.triggerIDs[ti][0] != 0xffffffff) mySerum.ntriggers++;
  }
  if (sizeheader >= 12 * sizeof(uint32_t))
    g_serumData.framespriteBB.readFromCRomFile(MAX_SPRITES_PER_FRAME * 4,
                                               g_serumData.nframes, pfile,
                                               &g_serumData.framesprites);
  else {
    for (uint32_t tj = 0; tj < g_serumData.nframes; tj++) {
      uint16_t tmp_framespriteBB[4 * MAX_SPRITES_PER_FRAME];
      for (uint32_t ti = 0; ti < MAX_SPRITES_PER_FRAME; ti++) {
        tmp_framespriteBB[ti * 4] = 0;
        tmp_framespriteBB[ti * 4 + 1] = 0;
        tmp_framespriteBB[ti * 4 + 2] = g_serumData.fwidth - 1;
        tmp_framespriteBB[ti * 4 + 3] = g_serumData.fheight - 1;
      }
      g_serumData.framespriteBB.set(tj, tmp_framespriteBB,
                                    MAX_SPRITES_PER_FRAME * 4);
    }
  }
  if (sizeheader >= 13 * sizeof(uint32_t)) {
    g_serumData.backgroundframes.readFromCRomFile(
        g_serumData.fwidth * g_serumData.fheight, g_serumData.nbackgrounds,
        pfile);
    g_serumData.backgroundIDs.readFromCRomFile(1, g_serumData.nframes, pfile);
    g_serumData.backgroundBB.readFromCRomFile(4, g_serumData.nframes, pfile,
                                              &g_serumData.backgroundIDs);
  }
  fclose(pfile);

  // allocate memory for previous detected frame
  framechecked = (bool*)malloc(sizeof(bool) * g_serumData.nframes);
  if (!framechecked) {
    Serum_free();
    enabled = false;
    return NULL;
  }
  if (g_serumData.fheight == 64) {
    mySerum.width64 = g_serumData.fwidth;
    mySerum.width32 = 0;
  } else {
    mySerum.width32 = g_serumData.fwidth;
    mySerum.width64 = 0;
  }
  Full_Reset_ColorRotations();
  cromloaded = true;

  if (!uncompressedCROM) {
    // remove temporary file that had been extracted from compressed CRZ file
    remove(pathbuf);
  }

  enabled = true;
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
  std::optional<std::string> pFoundFile;
  std::optional<std::string> skipFoundFile = find_case_insensitive_file(
      pathbuf, "skip-cromc.txt");
  if (skipFoundFile) {
    Log("Skipping .cROMc load due to presence of %s", skipFoundFile->c_str());
  } else {
    pFoundFile =
        find_case_insensitive_file(pathbuf, std::string(romname) + ".cROMc");

    if (pFoundFile) {
      Log("Found %s", pFoundFile->c_str());
      result = Serum_LoadConcentrate(pFoundFile->c_str(), flags);
      if (result) {
        Log("Loaded %s", pFoundFile->c_str());
        if (csvFoundFile && g_serumData.SerumVersion == SERUM_V2 &&
            g_serumData.sceneGenerator->parseCSV(csvFoundFile->c_str())) {
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
      if (csvFoundFile && g_serumData.SerumVersion == SERUM_V2)
        g_serumData.sceneGenerator->parseCSV(csvFoundFile->c_str());
#ifdef WRITE_CROMC
      if (generateCRomC) Serum_SaveConcentrate(pFoundFile->c_str());
#endif
    } else {
      Log("Failed to load %s", pFoundFile->c_str());
    }
  }
  if (result && g_serumData.sceneGenerator->isActive())
    g_serumData.sceneGenerator->setDepth(result->nocolors == 16 ? 4 : 2);
  if (is_real_machine()) {
    monochromeMode = true;
  }

  return result;
}

SERUM_API void Serum_Dispose(void) { Serum_free(); }

uint32_t Identify_Frame(uint8_t* frame, bool sceneFrameRequested) {
  // Usually the first frame has the ID 0, but lastfound is also initialized
  // with 0. So we need a helper to be able to detect frame 0 as new.
  static bool first_match_normal = true;
  static bool first_match_scene = true;

  if (!cromloaded) return IDENTIFY_NO_FRAME;
  memset(framechecked, false, g_serumData.nframes);
  uint32_t& lastfound_stream =
      sceneFrameRequested ? lastfound_scene : lastfound_normal;
  bool& first_match =
      sceneFrameRequested ? first_match_scene : first_match_normal;
  uint32_t& lastframe_full_crc =
      sceneFrameRequested ? lastframe_full_crc_scene : lastframe_full_crc_normal;
  uint16_t tj = lastfound_stream;  // we start from the frame we last found
  const uint32_t pixels = g_serumData.is256x64
                              ? (256 * 64)
                              : (g_serumData.fwidth * g_serumData.fheight);
  do {
    if (!framechecked[tj]) {
      // calculate the hashcode for the generated frame with the mask and
      // shapemode of the current crom frame
      uint8_t mask = g_serumData.compmaskID[tj][0];
      uint8_t Shape = g_serumData.shapecompmode[tj][0];
      uint32_t Hashc = calc_crc32(frame, mask, pixels, Shape);
      // now we can compare with all the crom frames that share these same mask
      // and shapemode
      uint16_t ti = tj;
      do {
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
        if (++ti >= g_serumData.nframes) ti = 0;
      } while (ti != tj);
    }
    if (++tj >= g_serumData.nframes) tj = 0;
  } while (tj != lastfound_stream);

  return IDENTIFY_NO_FRAME;  // we found no corresponding frame
}

void GetSpriteSize(uint8_t nospr, int* pswid, int* pshei, uint8_t* spriteData,
                   int sswid, int sshei) {
  *pswid = *pshei = 0;
  if (nospr >= g_serumData.nsprites) return;
  if (!spriteData) return;
  for (int tj = 0; tj < sshei; tj++) {
    for (int ti = 0; ti < sswid; ti++) {
      if (spriteData[tj * sswid + ti] < 255) {
        if (tj > *pshei) *pshei = tj;
        if (ti > *pswid) *pswid = ti;
      }
    }
  }
  (*pshei)++;
  (*pswid)++;
}

bool Check_Spritesv1(uint8_t* Frame, uint32_t quelleframe,
                     uint8_t* pquelsprites, uint8_t* nspr, uint16_t* pfrx,
                     uint16_t* pfry, uint16_t* pspx, uint16_t* pspy,
                     uint16_t* pwid, uint16_t* phei) {
  uint8_t ti = 0;
  uint32_t mdword;
  *nspr = 0;
  while ((ti < MAX_SPRITES_PER_FRAME) &&
         (g_serumData.framesprites[quelleframe][ti] < 255)) {
    uint8_t qspr = g_serumData.framesprites[quelleframe][ti];
    int spw, sph;
    GetSpriteSize(qspr, &spw, &sph, g_serumData.spritedescriptionso[qspr],
                  MAX_SPRITE_SIZE, MAX_SPRITE_SIZE);
    short minxBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4]);
    short minyBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4 + 1]);
    short maxxBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4 + 2]);
    short maxyBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4 + 3]);
    for (uint32_t tm = 0; tm < MAX_SPRITE_DETECT_AREAS; tm++) {
      if (g_serumData.spritedetareas[qspr][tm * 4] == 0xffff) continue;
      // we look for the sprite in the frame sent
      for (short ty = minyBB; ty <= maxyBB; ty++) {
        mdword = (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB] << 8) |
                 (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB + 1] << 16) |
                 (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB + 2] << 24);
        for (short tx = minxBB; tx <= maxxBB - 3; tx++) {
          uint32_t tj = ty * g_serumData.fwidth + tx;
          mdword = (mdword >> 8) | (uint32_t)(Frame[tj + 3] << 24);
          // we look for the magic dword first:
          uint16_t sddp = g_serumData.spritedetdwordpos[qspr][tm];
          if (mdword == g_serumData.spritedetdwords[qspr][tm]) {
            short frax =
                (short)tx;  // position in the frame of the detection dword
            short fray = (short)ty;
            short sprx =
                (short)(sddp % MAX_SPRITE_SIZE);  // position in the sprite of
                                                  // the detection dword
            short spry = (short)(sddp / MAX_SPRITE_SIZE);
            // details of the det area:
            short detx =
                (short)g_serumData
                    .spritedetareas[qspr][tm * 4];  // position of the detection
                                                    // area in the sprite
            short dety = (short)g_serumData.spritedetareas[qspr][tm * 4 + 1];
            short detw =
                (short)g_serumData
                    .spritedetareas[qspr]
                                   [tm * 4 + 2];  // size of the detection area
            short deth = (short)g_serumData.spritedetareas[qspr][tm * 4 + 3];
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
                    g_serumData.spritedescriptionso[qspr][(tk + dety) *
                                                              MAX_SPRITE_SIZE +
                                                          tl + detx];
                if (val == 255) continue;
                if (val !=
                    Frame[(tk + offsy) * g_serumData.fwidth + tl + offsx]) {
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
                pwid[*nspr] = MIN((uint16_t)(spw - pspx[*nspr]),
                                  (uint16_t)(maxxBB - minxBB + 1));
              } else {
                pspx[*nspr] = 0;
                pfrx[*nspr] = (uint16_t)(frax - sprx);
                pwid[*nspr] =
                    MIN((uint16_t)(maxxBB - pfrx[*nspr] + 1), (uint16_t)spw);
              }
              if (fray - minyBB < spry) {
                pspy[*nspr] = (uint16_t)(spry - (fray - minyBB));
                pfry[*nspr] = (uint16_t)minyBB;
                phei[*nspr] = MIN((uint16_t)(sph - pspy[*nspr]),
                                  (uint16_t)(maxyBB - minyBB + 1));
              } else {
                pspy[*nspr] = 0;
                pfry[*nspr] = (uint16_t)(fray - spry);
                phei[*nspr] =
                    MIN((uint16_t)(maxyBB - pfry[*nspr] + 1), (uint16_t)sph);
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
  uint8_t ti = 0;
  uint32_t mdword;
  *nspr = 0;
  bool isshapedframe = false;
  while ((ti < MAX_SPRITES_PER_FRAME) &&
         (g_serumData.framesprites[quelleframe][ti] < 255)) {
    uint8_t qspr = g_serumData.framesprites[quelleframe][ti];
    uint8_t* Frame = recframe;
    bool isshapecheck = false;
    if (g_serumData.sprshapemode[qspr][0] > 0) {
      isshapecheck = true;
      if (!isshapedframe) {
        for (int i = 0; i < g_serumData.fwidth * g_serumData.fheight; i++) {
          if (Frame[i] > 0)
            frameshape[i] = 1;
          else
            frameshape[i] = 0;
        }
        isshapedframe = true;
      }
      Frame = frameshape;
    }
    int spw, sph;
    GetSpriteSize(qspr, &spw, &sph, g_serumData.spriteoriginal[qspr],
                  MAX_SPRITE_WIDTH, MAX_SPRITE_HEIGHT);
    short minxBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4]);
    short minyBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4 + 1]);
    short maxxBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4 + 2]);
    short maxyBB = (short)(g_serumData.framespriteBB[quelleframe][ti * 4 + 3]);
    for (uint32_t tm = 0; tm < MAX_SPRITE_DETECT_AREAS; tm++) {
      if (g_serumData.spritedetareas[qspr][tm * 4] == 0xffff) continue;
      // we look for the sprite in the frame sent
      for (short ty = minyBB; ty <= maxyBB; ty++) {
        mdword = (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB] << 8) |
                 (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB + 1] << 16) |
                 (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB + 2] << 24);
        for (short tx = minxBB; tx <= maxxBB - 3; tx++) {
          uint32_t tj = ty * g_serumData.fwidth + tx;
          mdword = (mdword >> 8) | (uint32_t)(Frame[tj + 3] << 24);
          // we look for the magic dword first:
          uint16_t sddp = g_serumData.spritedetdwordpos[qspr][tm];
          if (mdword == g_serumData.spritedetdwords[qspr][tm]) {
            short frax =
                (short)tx;  // position in the frame of the detection dword
            short fray = (short)ty;
            short sprx =
                (short)(sddp % MAX_SPRITE_WIDTH);  // position in the sprite of
                                                   // the detection dword
            short spry = (short)(sddp / MAX_SPRITE_WIDTH);
            // details of the det area:
            short detx =
                (short)g_serumData
                    .spritedetareas[qspr][tm * 4];  // position of the detection
                                                    // area in the sprite
            short dety = (short)g_serumData.spritedetareas[qspr][tm * 4 + 1];
            short detw =
                (short)g_serumData
                    .spritedetareas[qspr]
                                   [tm * 4 + 2];  // size of the detection area
            short deth = (short)g_serumData.spritedetareas[qspr][tm * 4 + 3];
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
                    g_serumData
                        .spriteoriginal[qspr][(tk + dety) * MAX_SPRITE_WIDTH +
                                              tl + detx];
                if (val == 255) continue;
                if (val !=
                    Frame[(tk + offsy) * g_serumData.fwidth + tl + offsx]) {
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
                pwid[*nspr] = MIN((uint16_t)(spw - pspx[*nspr]),
                                  (uint16_t)(maxxBB - minxBB + 1));
              } else {
                pspx[*nspr] = 0;
                pfrx[*nspr] = (uint16_t)(frax - sprx);
                pwid[*nspr] =
                    MIN((uint16_t)(maxxBB - pfrx[*nspr] + 1), (uint16_t)spw);
              }
              if (fray - minyBB < spry) {
                pspy[*nspr] = (uint16_t)(spry - (fray - minyBB));
                pfry[*nspr] = (uint16_t)minyBB;
                phei[*nspr] = MIN((uint16_t)(sph - pspy[*nspr]),
                                  (uint16_t)(maxyBB - minyBB + 1));
              } else {
                pspy[*nspr] = 0;
                pfry[*nspr] = (uint16_t)(fray - spry);
                phei[*nspr] =
                    MIN((uint16_t)(maxyBB - pfry[*nspr] + 1), (uint16_t)sph);
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

void Colorize_Framev1(uint8_t* frame, uint32_t IDfound) {
  uint16_t tj, ti;
  // Generate the colorized version of a frame once identified in the crom
  // frames
  for (tj = 0; tj < g_serumData.fheight; tj++) {
    for (ti = 0; ti < g_serumData.fwidth; ti++) {
      uint16_t tk = tj * g_serumData.fwidth + ti;

      if ((g_serumData.backgroundIDs[IDfound][0] < g_serumData.nbackgrounds) &&
          (frame[tk] == 0) && (ti >= g_serumData.backgroundBB[IDfound][0]) &&
          (tj >= g_serumData.backgroundBB[IDfound][1]) &&
          (ti <= g_serumData.backgroundBB[IDfound][2]) &&
          (tj <= g_serumData.backgroundBB[IDfound][3]))
        mySerum.frame[tk] =
            g_serumData
                .backgroundframes[g_serumData.backgroundIDs[IDfound][0]][tk];
      else {
        uint8_t dynacouche = g_serumData.dynamasks[IDfound][tk];
        if (dynacouche == 255)
          mySerum.frame[tk] = g_serumData.cframes[IDfound][tk];
        else
          mySerum.frame[tk] =
              g_serumData.dyna4cols[IDfound][dynacouche * g_serumData.nocolors +
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
  uint16_t* pcol = NULL;
  if (isextra)
    pcol = g_serumData.colorrotations_v2_extra[IDfound];
  else
    pcol = g_serumData.colorrotations_v2[IDfound];
  *norot = 0xffff;
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

void CheckDynaShadow(uint16_t* pfr, uint32_t nofr, uint8_t dynacouche,
                     uint8_t* isdynapix, uint16_t fx, uint16_t fy, uint32_t fw,
                     uint32_t fh, bool isextra) {
  uint8_t dsdir;
  if (isextra)
    dsdir = g_serumData.dynashadowsdir_extra[nofr][dynacouche];
  else
    dsdir = g_serumData.dynashadowsdir[nofr][dynacouche];
  if (dsdir == 0) return;
  uint16_t tcol;
  if (isextra)
    tcol = g_serumData.dynashadowscol_extra[nofr][dynacouche];
  else
    tcol = g_serumData.dynashadowscol[nofr][dynacouche];
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
                      bool blackOutStaticContent = false) {
  uint16_t tj, ti;
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
  if (((mySerum.frame32 && g_serumData.fheight == 32) ||
       (mySerum.frame64 && g_serumData.fheight == 64)) &&
      isoriginalrequested) {
    // create the original res frame
    if (g_serumData.fheight == 32) {
      pfr = mySerum.frame32;
      mySerum.flags |= FLAG_RETURNED_32P_FRAME_OK;
      prot = mySerum.rotationsinframe32;
      mySerum.width32 = g_serumData.fwidth;
      prt = g_serumData.colorrotations_v2[IDfound];
      cshft = colorshifts32;
      pSceneBackgroundFrame = mySerum.frame32;
    } else {
      pfr = mySerum.frame64;
      mySerum.flags |= FLAG_RETURNED_64P_FRAME_OK;
      prot = mySerum.rotationsinframe64;
      mySerum.width64 = g_serumData.fwidth;
      prt = g_serumData.colorrotations_v2[IDfound];
      cshft = colorshifts64;
      pSceneBackgroundFrame = mySerum.frame64;
    }
    if (applySceneBackground)
      memcpy(sceneBackgroundFrame, pSceneBackgroundFrame,
             g_serumData.fwidth * g_serumData.fheight * sizeof(uint16_t));
    memset(isdynapix, 0, g_serumData.fheight * g_serumData.fwidth);
    for (tj = 0; tj < g_serumData.fheight; tj++) {
      for (ti = 0; ti < g_serumData.fwidth; ti++) {
        uint16_t tk = tj * g_serumData.fwidth + ti;
        if ((g_serumData.backgroundIDs[IDfound][0] <
             g_serumData.nbackgrounds) &&
            (frame[tk] == 0) && (g_serumData.backgroundmask[IDfound][tk] > 0)) {
          if (isdynapix[tk] == 0) {
            if (applySceneBackground) {
              pfr[tk] = sceneBackgroundFrame[tk];
            } else {
              pfr[tk] = g_serumData.backgroundframes_v2
                            [g_serumData.backgroundIDs[IDfound][0]][tk];
              if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                  &prot[tk * 2 + 1], false))
                pfr[tk] =
                    prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                        (cshft[prot[tk * 2]] + prot[tk * 2 + 1]) %
                            prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
            }
          }
        } else {
          uint8_t dynacouche = g_serumData.dynamasks[IDfound][tk];
          if (dynacouche == 255) {
            if (isdynapix[tk] == 0) {
              if (blackOutStaticContent &&
                  (g_serumData.backgroundIDs[IDfound][0] <
                   g_serumData.nbackgrounds) &&
                  (frame[tk] > 0) &&
                  (g_serumData.backgroundmask[IDfound][tk] > 0)) {
                pfr[tk] = sceneBackgroundFrame[tk];
              } else {
                pfr[tk] = g_serumData.cframes_v2[IDfound][tk];
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
              CheckDynaShadow(pfr, IDfound, dynacouche, isdynapix, ti, tj,
                              g_serumData.fwidth, g_serumData.fheight, false);
              isdynapix[tk] = 1;
              pfr[tk] =
                  g_serumData
                      .dyna4cols_v2[IDfound][dynacouche * g_serumData.nocolors +
                                             frame[tk]];
            } else if (isdynapix[tk] == 0)
              pfr[tk] =
                  g_serumData
                      .dyna4cols_v2[IDfound][dynacouche * g_serumData.nocolors +
                                             frame[tk]];
            prot[tk * 2] = prot[tk * 2 + 1] = 0xffff;
          }
        }
      }
    }
  }
  if (isextra &&
      ((mySerum.frame32 && g_serumData.fheight_extra == 32) ||
       (mySerum.frame64 && g_serumData.fheight_extra == 64)) &&
      isextrarequested) {
    // create the extra res frame
    if (g_serumData.fheight_extra == 32) {
      pfr = mySerum.frame32;
      mySerum.flags |= FLAG_RETURNED_32P_FRAME_OK;
      prot = mySerum.rotationsinframe32;
      mySerum.width32 = g_serumData.fwidth_extra;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts32;
      pSceneBackgroundFrame = mySerum.frame32;
    } else {
      pfr = mySerum.frame64;
      mySerum.flags |= FLAG_RETURNED_64P_FRAME_OK;
      prot = mySerum.rotationsinframe64;
      mySerum.width64 = g_serumData.fwidth_extra;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts64;
      pSceneBackgroundFrame = mySerum.frame64;
    }
    if (applySceneBackground)
      memcpy(sceneBackgroundFrame, pSceneBackgroundFrame,
             g_serumData.fwidth_extra * g_serumData.fheight_extra *
                 sizeof(uint16_t));
    memset(isdynapix, 0, g_serumData.fheight_extra * g_serumData.fwidth_extra);
    for (tj = 0; tj < g_serumData.fheight_extra; tj++) {
      for (ti = 0; ti < g_serumData.fwidth_extra; ti++) {
        uint16_t tk = tj * g_serumData.fwidth_extra + ti;
        uint16_t tl;
        if (g_serumData.fheight_extra == 64)
          tl = tj / 2 * g_serumData.fwidth + ti / 2;
        else
          tl = tj * 2 * g_serumData.fwidth + ti * 2;

        if ((g_serumData.backgroundIDs[IDfound][0] <
             g_serumData.nbackgrounds) &&
            (frame[tl] == 0) &&
            (g_serumData.backgroundmask_extra[IDfound][tk] > 0)) {
          if (isdynapix[tk] == 0) {
            if (applySceneBackground) {
              pfr[tk] = sceneBackgroundFrame[tk];
            } else {
              pfr[tk] = g_serumData.backgroundframes_v2_extra
                            [g_serumData.backgroundIDs[IDfound][0]][tk];
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
          uint8_t dynacouche = g_serumData.dynamasks_extra[IDfound][tk];
          if (dynacouche == 255) {
            if (isdynapix[tk] == 0) {
              if (blackOutStaticContent &&
                  (g_serumData.backgroundIDs[IDfound][0] <
                   g_serumData.nbackgrounds) &&
                  (frame[tl] > 0) &&
                  (g_serumData.backgroundmask_extra[IDfound][tk] > 0)) {
                pfr[tk] = sceneBackgroundFrame[tk];
              } else {
                pfr[tk] = g_serumData.cframes_v2_extra[IDfound][tk];
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
              CheckDynaShadow(pfr, IDfound, dynacouche, isdynapix, ti, tj,
                              g_serumData.fwidth_extra,
                              g_serumData.fheight_extra, true);
              isdynapix[tk] = 1;
              pfr[tk] =
                  g_serumData.dyna4cols_v2_extra
                      [IDfound][dynacouche * g_serumData.nocolors + frame[tl]];
            } else if (isdynapix[tk] == 0)
              pfr[tk] =
                  g_serumData.dyna4cols_v2_extra
                      [IDfound][dynacouche * g_serumData.nocolors + frame[tl]];
            prot[tk * 2] = prot[tk * 2 + 1] = 0xffff;
          }
        }
      }
    }
  }
}

void Colorize_Spritev1(uint8_t nosprite, uint16_t frx, uint16_t fry,
                       uint16_t spx, uint16_t spy, uint16_t wid, uint16_t hei) {
  for (uint16_t tj = 0; tj < hei; tj++) {
    for (uint16_t ti = 0; ti < wid; ti++) {
      if (g_serumData
              .spritedescriptionso[nosprite][(tj + spy) * MAX_SPRITE_SIZE + ti +
                                             spx] < 255) {
        mySerum.frame[(fry + tj) * g_serumData.fwidth + frx + ti] =
            g_serumData
                .spritedescriptionsc[nosprite]
                                    [(tj + spy) * MAX_SPRITE_SIZE + ti + spx];
      }
    }
  }
}

void Colorize_Spritev2(uint8_t* oframe, uint8_t nosprite, uint16_t frx,
                       uint16_t fry, uint16_t spx, uint16_t spy, uint16_t wid,
                       uint16_t hei, uint32_t IDfound) {
  uint16_t *pfr, *prot;
  uint16_t* prt;
  uint32_t* cshft;
  if (((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) &&
       g_serumData.fheight == 32) ||
      ((mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) &&
       g_serumData.fheight == 64)) {
    if (g_serumData.fheight == 32) {
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
    for (uint16_t tj = 0; tj < hei; tj++) {
      for (uint16_t ti = 0; ti < wid; ti++) {
        uint16_t tk = (fry + tj) * g_serumData.fwidth + frx + ti;
        uint32_t tl = (tj + spy) * MAX_SPRITE_WIDTH + ti + spx;
        uint8_t spriteref = g_serumData.spriteoriginal[nosprite][tl];
        if (spriteref < 255) {
          uint8_t dynacouche = g_serumData.dynaspritemasks[nosprite][tl];
          if (dynacouche == 255) {
            pfr[tk] = g_serumData.spritecolored[nosprite][tl];
            if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                &prot[tk * 2 + 1], false))
              pfr[tk] = prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                            (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                                prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
          } else {
            pfr[tk] =
                g_serumData.dynasprite4cols[nosprite]
                                           [dynacouche * g_serumData.nocolors +
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
       g_serumData.fheight_extra == 32) ||
      ((mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) &&
       g_serumData.fheight_extra == 64)) {
    uint16_t thei, twid, tfrx, tfry, tspy, tspx;
    if (g_serumData.fheight_extra == 32) {
      pfr = mySerum.frame32;
      prot = mySerum.rotationsinframe32;
      thei = hei / 2;
      twid = wid / 2;
      tfrx = frx / 2;
      tfry = fry / 2;
      tspx = spx / 2;
      tspy = spy / 2;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts32;
    } else {
      pfr = mySerum.frame64;
      prot = mySerum.rotationsinframe64;
      thei = hei * 2;
      twid = wid * 2;
      tfrx = frx * 2;
      tfry = fry * 2;
      tspx = spx * 2;
      tspy = spy * 2;
      prt = g_serumData.colorrotations_v2_extra[IDfound];
      cshft = colorshifts64;
    }
    for (uint16_t tj = 0; tj < thei; tj++) {
      for (uint16_t ti = 0; ti < twid; ti++) {
        uint16_t tk = (tfry + tj) * g_serumData.fwidth_extra + tfrx + ti;
        if (g_serumData
                .spritemask_extra[nosprite][(tj + tspy) * MAX_SPRITE_WIDTH +
                                            ti + tspx] < 255) {
          uint8_t dynacouche =
              g_serumData.dynaspritemasks_extra[nosprite]
                                               [(tj + tspy) * MAX_SPRITE_WIDTH +
                                                ti + tspx];
          if (dynacouche == 255) {
            pfr[tk] =
                g_serumData.spritecolored_extra[nosprite]
                                               [(tj + tspy) * MAX_SPRITE_WIDTH +
                                                ti + tspx];
            if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                &prot[tk * 2 + 1], true))
              pfr[tk] = prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                            (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                                prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
          } else {
            uint16_t tl;
            if (g_serumData.fheight_extra == 64)
              tl = (tj / 2 + fry) * g_serumData.fwidth + ti / 2 + frx;
            else
              tl = (tj * 2 + fry) * g_serumData.fwidth + ti * 2 + frx;
            pfr[tk] =
                g_serumData.dynasprite4cols_extra
                    [nosprite][dynacouche * g_serumData.nocolors + oframe[tl]];
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
  memcpy(mySerum.palette, g_serumData.cpal[nofr], g_serumData.nccolors * 3);
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
    monochromeMode =
        (g_serumData.triggerIDs[lastfound][0] == MONOCHROME_TRIGGER_ID);
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

    uint8_t nosprite[MAX_SPRITES_PER_FRAME], nspr;
    uint16_t frx[MAX_SPRITES_PER_FRAME], fry[MAX_SPRITES_PER_FRAME],
        spx[MAX_SPRITES_PER_FRAME], spy[MAX_SPRITES_PER_FRAME],
        wid[MAX_SPRITES_PER_FRAME], hei[MAX_SPRITES_PER_FRAME];
    memset(nosprite, 255, MAX_SPRITES_PER_FRAME);

    bool isspr = Check_Spritesv1(frame, (uint32_t)lastfound, nosprite, &nspr,
                                 frx, fry, spx, spy, wid, hei);
    if (((frameID < MAX_NUMBER_FRAMES) || isspr) &&
        g_serumData.activeframes[lastfound][0] != 0) {
      Colorize_Framev1(frame, lastfound);
      Copy_Frame_Palette(lastfound);
      {
        uint32_t ti = 0;
        while (ti < nspr) {
          Colorize_Spritev1(nosprite[ti], frx[ti], fry[ti], spx[ti], spy[ti],
                            wid[ti], hei[ti]);
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
        // if the last init time is more than a new rotation away. Otherwise, we
        // keep the already running timings for subsequent frames like blinking
        // PUSH START or GAME OVER.
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

SERUM_API uint32_t
Serum_ColorizeWithMetadatav2(uint8_t* frame, bool sceneFrameRequested = false) {
  // return IDENTIFY_NO_FRAME if no new frame detected
  // return 0 if new frame with no rotation detected
  // return > 0 if new frame with rotations detected, the value is the delay
  // before the first rotation in ms
  mySerum.triggerID = 0xffffffff;
  mySerum.frameID = IDENTIFY_NO_FRAME;

  // Let's first identify the incoming frame among the ones we have in the crom
  uint32_t frameID = Identify_Frame(frame, sceneFrameRequested);
  uint32_t now = GetMonotonicTimeMs();
  bool rotationIsScene = false;
  if (is_real_machine() && !showStatusMessages) {
    showStatusMessages = (g_serumData.triggerIDs[lastfound][0] > 0xff98 &&
                          g_serumData.triggerIDs[lastfound][0] < 0xffffffff);
    if (showStatusMessages) ignoreUnknownFramesTimeout = 0x2000;
  }
  if (frameID != IDENTIFY_NO_FRAME && !showStatusMessages) {
    monochromeMode =
        (g_serumData.triggerIDs[lastfound][0] == MONOCHROME_TRIGGER_ID);
    if (g_serumData.triggerIDs[lastfound][0] > 0xff98)
      g_serumData.triggerIDs[lastfound][0] = 0xffffffff;

    if (!monochromeMode && g_serumData.sceneGenerator->isActive() &&
        !sceneFrameRequested && sceneCurrentFrame < sceneFrameCount &&
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
      memcpy(lastFrame, frame, g_serumData.fwidth * g_serumData.fheight);
      lastFrameId = frameID;

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
        // stop any scene
        sceneFrameCount = 0;
        sceneIsLastBackgroundFrame = false;
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
                    sceneInterruptable, sceneStartImmediately, sceneRepeatCount,
                    sceneOptionFlags)) {
              // Log(DMDUtil_LogLevel_DEBUG, "Serum: trigger ID %lu found in
              // scenes, frame count=%d, duration=%dms",
              //     m_pSerum->triggerID, sceneFrameCount,
              //     sceneDurationPerFrame);
              sceneCurrentFrame = 0;
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

    bool isBackgroundScene = (sceneFrameRequested && sceneFrameCount > 0 &&
                              (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
                                  FLAG_SCENE_AS_BACKGROUND);
    uint8_t nosprite[MAX_SPRITES_PER_FRAME], nspr;
    uint16_t frx[MAX_SPRITES_PER_FRAME], fry[MAX_SPRITES_PER_FRAME],
        spx[MAX_SPRITES_PER_FRAME], spy[MAX_SPRITES_PER_FRAME],
        wid[MAX_SPRITES_PER_FRAME], hei[MAX_SPRITES_PER_FRAME];
    memset(nosprite, 255, MAX_SPRITES_PER_FRAME);

    bool isspr =
        (sceneFrameRequested && !isBackgroundScene)
            ? false
            : Check_Spritesv2(isBackgroundScene ? lastFrame : frame,
                              isBackgroundScene ? lastFrameId : lastfound,
                              nosprite, &nspr, frx, fry, spx, spy, wid, hei);
    if (((frameID < MAX_NUMBER_FRAMES) || isspr) &&
        g_serumData.activeframes[lastfound][0] != 0) {
      Colorize_Framev2(
          sceneIsLastBackgroundFrame ? sceneFrame : frame,
          sceneIsLastBackgroundFrame ? sceneLastBackgroundFrameID : lastfound);
      if (isBackgroundScene) {
        sceneLastBackgroundFrameID = mySerum.frameID;
      }
      if (isBackgroundScene || sceneIsLastBackgroundFrame) {
        Colorize_Framev2(lastFrame, lastFrameId, true,
                         (sceneOptionFlags & FLAG_SCENE_ONLY_DYNAMIC_CONTENT) ==
                             FLAG_SCENE_ONLY_DYNAMIC_CONTENT);
      }
      if (isspr) {
        uint8_t ti = 0;
        while (ti < nspr) {
          Colorize_Spritev2(isBackgroundScene ? lastFrame : frame, nosprite[ti],
                            frx[ti], fry[ti], spx[ti], spy[ti], wid[ti],
                            hei[ti],
                            isBackgroundScene ? lastFrameId : lastfound);
          ti++;
        }
      }

      // Skip rotations if the scene is active
      if (sceneCurrentFrame >= sceneFrameCount) {
        uint16_t *pcr32, *pcr64;
        if (g_serumData.fheight == 32) {
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
                mySerum.rotations32[ti * MAX_LENGTH_COLOR_ROTATION + 1] == 0) {
              colorrotnexttime32[ti] = 0;
              continue;
            }
            // Reset the timer if the previous frame had this rotation inactive
            // or if the last init time is more than a new rotation away.
            // Otherwise, we keep the already running timings for subsequent
            // frames like blinking PUSH START or GAME OVER.
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
                mySerum.rotations64[ti * MAX_LENGTH_COLOR_ROTATION + 1] == 0) {
              colorrotnexttime64[ti] = 0;
              continue;
            }
            // Reset the timer if the previous frame had this rotation inactive
            // or if the last init time is more than a new rotation away.
            // Otherwise, we keep the already running timings for subsequent
            // frames like blinking PUSH START or GAME OVER.
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

        if (isRotation) {
          mySerum.rotationtimer = Calc_Next_Rotationv2(now);
        } else {
          mySerum.rotationtimer = 0;
        }
      }

      if (0 == mySerum.rotationtimer &&
          g_serumData.sceneGenerator->isActive() && !sceneFrameRequested &&
          sceneCurrentFrame >= sceneFrameCount &&
          g_serumData.sceneGenerator->getAutoStartSceneInfo(
              sceneFrameCount, sceneDurationPerFrame, sceneInterruptable,
              sceneStartImmediately, sceneRepeatCount, sceneOptionFlags)) {
        mySerum.rotationtimer = g_serumData.sceneGenerator->getAutoStartTimer();
        rotationIsScene = true;
      }

      if (keepTriggersInternal ||
          mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
        mySerum.triggerID = 0xffffffff;

      return (uint32_t)mySerum.rotationtimer |
             (rotationIsScene ? FLAG_RETURNED_V2_SCENE : 0);
    }
  }

  mySerum.triggerID = 0xffffffff;

  if (monochromeMode ||
      (ignoreUnknownFramesTimeout &&
       (now - lastframe_found) >= ignoreUnknownFramesTimeout) ||
      (maxFramesToSkip && (frameID == IDENTIFY_NO_FRAME) &&
       (++framesSkippedCounter >= maxFramesToSkip))) {
    // apply standard palette
    for (uint16_t y = 0; y < g_serumData.fheight; y++) {
      for (uint16_t x = 0; x < g_serumData.fwidth; x++) {
        if (g_serumData.nocolors < 16)
          mySerum.frame32[y * g_serumData.fwidth + x] =
              greyscale_4[frame[y * g_serumData.fwidth + x]];
        else
          mySerum.frame32[y * g_serumData.fwidth + x] =
              greyscale_16[frame[y * g_serumData.fwidth + x]];
      }
    }

    mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
    mySerum.width32 = g_serumData.fwidth;
    mySerum.width64 = 0;
    mySerum.frameID = 0xfffffffd;  // monochrome frame ID

    // disable render features like rotations
    for (uint8_t ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
      colorrotnexttime32[ti] = 0;
      colorrotnexttime64[ti] = 0;
    }
    mySerum.rotationtimer = 0;

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
      sceneCurrentFrame < sceneFrameCount) {
    uint16_t result = g_serumData.sceneGenerator->generateFrame(
        lastTriggerID, sceneCurrentFrame, sceneFrame);
    // if result is 0xffff, the frame was generated and we can go
    if (0xffff == result) {
      mySerum.rotationtimer = sceneDurationPerFrame;
      Serum_ColorizeWithMetadatav2(sceneFrame, true);
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
        sceneFrameCount = 0;  // scene ended
        mySerum.rotationtimer = 0;

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
  if (sceneRotationResult & FLAG_RETURNED_V2_SCENE) return sceneRotationResult;

  // rotation[0] = number of colors in rotation
  // rotation[1] = delay in ms between each color change
  // rotation[2..n] = color indexes

  uint32_t isrotation = 0;
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
  mySerum.rotationtimer = Calc_Next_Rotationv2(now) &
                          0xffff;  // can't be more than 2048ms, so val is
                                   // contained in the lower word of val
  if (mySerum.rotationtimer > 2048)
    mySerum.rotationtimer =
        0;  // more than 2048ms is not possible, stop the rotation
            // if there was a rotation in the 32P frame, the first bit of the
            // high word is set (0x10000) and if there was a rotation in the 64P
            // frame, the second bit of the high word is set (0x20000)
  return mySerum.rotationtimer |
         isrotation;  // returns the time in ms until the next rotation in the
                      // lowest word
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
