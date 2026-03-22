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
#include <mach/mach.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
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

static bool IsEnvFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return false;
  }
  return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
         strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0;
}

static uint32_t GetEnvUintClamped(const char* name, uint32_t maxValue) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return 0;
  }
  char* endPtr = nullptr;
  unsigned long parsed = std::strtoul(value, &endPtr, 10);
  if (endPtr == value || *endPtr != '\0') {
    return 0;
  }
  if (parsed > maxValue) {
    parsed = maxValue;
  }
  return static_cast<uint32_t>(parsed);
}

static bool g_profileDynamicHotPaths = false;
static bool g_profileDynamicHotPathsWindowed = false;
static bool g_profileSparseVectors = false;
static uint64_t g_profileRoundTripNs = 0;
static uint64_t g_profileColorizeFrameV2Ns = 0;
static uint64_t g_profileColorizeSpriteV2Ns = 0;
static uint64_t g_profileColorizeCalls = 0;
static uint64_t g_profileIdentifyTotalNs = 0;
static uint64_t g_profileIdentifyNormalNs = 0;
static uint64_t g_profileIdentifySceneNs = 0;
static uint64_t g_profileIdentifyCriticalNs = 0;
static uint64_t g_profileIdentifyNormalCalls = 0;
static uint64_t g_profileIdentifySceneCalls = 0;
static uint64_t g_profileIdentifyCriticalCalls = 0;
static uint64_t g_profileIncomingFrameCalls = 0;
static uint64_t g_profileNoFrameReturns = 0;
static uint64_t g_profileSameFrameReturns = 0;
static uint64_t g_profileLastLoggedInputCount = 0;
static uint64_t g_profilePeakRssBytes = 0;
static uint64_t g_profileStartupStartRssBytes = 0;
static uint64_t g_profileStartupPeakRssBytes = 0;
static const char* g_profileStartupPeakStage = "startup-begin";
static uint32_t g_profileFrameOperationDepth = 0;
static bool g_profileFrameOperationFinished = false;
static std::chrono::steady_clock::time_point g_profileFrameOperationStart;
static bool g_debugFrameTracingInitialized = false;
static uint32_t g_debugTargetInputCrc = 0;
static uint32_t g_debugTargetFrameId = 0xffffffffu;
static bool g_debugStageHashes = false;
static uint32_t g_debugCurrentInputCrc = 0;
static bool g_debugTraceAllInputs = false;
static uint32_t g_debugFrameMetaLoggedFor = 0xffffffffu;
static bool g_debugBypassSceneGate = false;
static bool g_debugVerboseIdentify = false;
static bool g_debugVerboseSprites = false;
static bool g_debugVerboseScenes = false;
static std::vector<std::pair<uint8_t, uint8_t>> g_criticalTriggerMaskShapes;

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
uint32_t sceneNextFrameAtMs = 0;
uint8_t sceneFrame[256 * 64] = {0};
uint8_t lastFrame[256 * 64] = {0};
uint32_t lastFrameId = 0;  // last frame ID identified
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

extern bool cromloaded;
extern uint32_t lastfound;
uint32_t calc_crc32(uint8_t* source, uint8_t mask, uint32_t n, uint8_t Shape);
uint32_t crc32_fast(uint8_t* s, uint32_t n);
static uint64_t MakeFrameSignature(uint8_t mask, uint8_t shape, uint32_t hash);
static bool DebugTraceMatches(uint32_t inputCrc, uint32_t frameId);
static bool DebugIdentifyVerboseEnabled();

static void BeginProfileFrameOperation(void) {
  if (!g_profileDynamicHotPaths) {
    return;
  }
  if (g_profileFrameOperationDepth++ == 0) {
    g_profileFrameOperationStart = std::chrono::steady_clock::now();
    g_profileFrameOperationFinished = false;
  }
}

static void FinishProfileRenderedFrameOperationMaybe(void) {
  if (!g_profileDynamicHotPaths || g_profileFrameOperationDepth == 0 ||
      g_profileFrameOperationFinished) {
    return;
  }
  g_profileRoundTripNs +=
      (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - g_profileFrameOperationStart)
          .count();
  ++g_profileColorizeCalls;
  g_profileFrameOperationFinished = true;
}

static void EndProfileFrameOperation(void) {
  if (!g_profileDynamicHotPaths || g_profileFrameOperationDepth == 0) {
    return;
  }
  --g_profileFrameOperationDepth;
  if (g_profileFrameOperationDepth == 0) {
    g_profileFrameOperationFinished = false;
  }
}

static void InitCriticalTriggerLookupRuntimeState(void) {
  g_criticalTriggerMaskShapes.clear();
  if (g_serumData.criticalTriggerFramesBySignature.empty()) {
    return;
  }

  std::unordered_set<uint16_t> uniqueMaskShapeKeys;
  uniqueMaskShapeKeys.reserve(
      g_serumData.criticalTriggerFramesBySignature.size());
  for (const auto& entry : g_serumData.criticalTriggerFramesBySignature) {
    const uint8_t mask = static_cast<uint8_t>((entry.first >> 40) & 0xffu);
    const uint8_t shape = static_cast<uint8_t>((entry.first >> 32) & 0xffu);
    const uint16_t key = (uint16_t(mask) << 8) | shape;
    if (uniqueMaskShapeKeys.insert(key).second) {
      g_criticalTriggerMaskShapes.emplace_back(mask, shape);
    }
  }
}

static uint32_t IdentifyCriticalTriggerFrame(uint8_t* frame) {
  const auto profileStart = g_profileDynamicHotPaths
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
  if (!cromloaded || g_criticalTriggerMaskShapes.empty() ||
      g_serumData.criticalTriggerFramesBySignature.empty()) {
    if (g_profileDynamicHotPaths) {
      g_profileIdentifyCriticalNs +=
          (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - profileStart)
              .count();
      ++g_profileIdentifyCriticalCalls;
    }
    return IDENTIFY_NO_FRAME;
  }

  const uint32_t pixels = g_serumData.is256x64
                              ? (256 * 64)
                              : (g_serumData.fwidth * g_serumData.fheight);
  for (const auto& maskShape : g_criticalTriggerMaskShapes) {
    const uint32_t hash =
        calc_crc32(frame, maskShape.first, pixels, maskShape.second);
    auto it = g_serumData.criticalTriggerFramesBySignature.find(
        MakeFrameSignature(maskShape.first, maskShape.second, hash));
    if (it != g_serumData.criticalTriggerFramesBySignature.end() &&
        !it->second.empty()) {
      if (g_profileDynamicHotPaths) {
        g_profileIdentifyCriticalNs +=
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - profileStart)
                .count();
        ++g_profileIdentifyCriticalCalls;
      }
      return it->second.front();
    }
  }

  if (g_profileDynamicHotPaths) {
    g_profileIdentifyCriticalNs +=
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - profileStart)
            .count();
    ++g_profileIdentifyCriticalCalls;
  }
  return IDENTIFY_NO_FRAME;
}

static uint32_t SelectFrameIdInWrapOrder(const std::vector<uint32_t>& frameIds,
                                         uint32_t startFrameId) {
  if (frameIds.empty() || g_serumData.nframes == 0) {
    return IDENTIFY_NO_FRAME;
  }

  uint32_t bestFrameId = frameIds.front();
  uint32_t bestDistance =
      (bestFrameId >= startFrameId)
          ? (bestFrameId - startFrameId)
          : (g_serumData.nframes - startFrameId + bestFrameId);
  for (size_t i = 1; i < frameIds.size(); ++i) {
    const uint32_t frameId = frameIds[i];
    const uint32_t distance =
        (frameId >= startFrameId)
            ? (frameId - startFrameId)
            : (g_serumData.nframes - startFrameId + frameId);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestFrameId = frameId;
    }
  }
  return bestFrameId;
}

static uint32_t ResolveIdentifiedFrameMatch(uint8_t* frame, uint32_t inputCrc,
                                            uint32_t candidateFrameId,
                                            uint8_t mask, bool& first_match,
                                            uint32_t& lastfound_stream,
                                            uint32_t& lastframe_full_crc) {
  if (candidateFrameId >= g_serumData.nframes) {
    return IDENTIFY_NO_FRAME;
  }
  if (DebugIdentifyVerboseEnabled() &&
      DebugTraceMatches(inputCrc, candidateFrameId)) {
    Log("Serum debug identify candidate: inputCrc=%u frameId=%u "
        "mask=%u storedHash=%u lastfound=%u",
        inputCrc, candidateFrameId, mask,
        g_serumData.hashcodes[candidateFrameId][0], lastfound_stream);
  }
  if (first_match || candidateFrameId != lastfound_stream || mask < 255) {
    if (DebugIdentifyVerboseEnabled() &&
        DebugTraceMatches(inputCrc, candidateFrameId)) {
      Log("Serum debug identify decision: inputCrc=%u frameId=%u "
          "reason=%s firstMatch=%s lastfoundStream=%u mask=%u "
          "fullCrcBefore=%u",
          inputCrc, candidateFrameId,
          first_match ? "first-match"
                      : (candidateFrameId != lastfound_stream ? "new-frame-id"
                                                              : "mask-lt-255"),
          first_match ? "true" : "false", lastfound_stream, mask,
          lastframe_full_crc);
    }
    lastfound_stream = candidateFrameId;
    lastfound = candidateFrameId;
    lastframe_full_crc =
        crc32_fast(frame, g_serumData.is256x64
                              ? (256 * 64)
                              : (g_serumData.fwidth * g_serumData.fheight));
    first_match = false;
    return candidateFrameId;
  }

  const uint32_t full_crc = crc32_fast(
      frame, g_serumData.is256x64 ? (256 * 64)
                                  : (g_serumData.fwidth * g_serumData.fheight));
  if (full_crc != lastframe_full_crc) {
    if (DebugIdentifyVerboseEnabled() &&
        DebugTraceMatches(inputCrc, candidateFrameId)) {
      Log("Serum debug identify decision: inputCrc=%u frameId=%u "
          "reason=full-crc-diff firstMatch=%s lastfoundStream=%u "
          "mask=%u fullCrcBefore=%u fullCrcNow=%u",
          inputCrc, candidateFrameId, first_match ? "true" : "false",
          lastfound_stream, mask, lastframe_full_crc, full_crc);
    }
    lastframe_full_crc = full_crc;
    lastfound = candidateFrameId;
    return candidateFrameId;
  }
  if (DebugIdentifyVerboseEnabled() &&
      DebugTraceMatches(inputCrc, candidateFrameId)) {
    Log("Serum debug identify decision: inputCrc=%u frameId=%u "
        "reason=same-frame firstMatch=%s lastfoundStream=%u mask=%u "
        "fullCrc=%u",
        inputCrc, candidateFrameId, first_match ? "true" : "false",
        lastfound_stream, mask, full_crc);
  }
  lastfound = candidateFrameId;
  return IDENTIFY_SAME_FRAME;
}

static bool IsCriticalMonochromeTriggerFrame(uint32_t frameId) {
  if (frameId >= g_serumData.nframes) {
    return false;
  }
  const uint32_t triggerId = g_serumData.triggerIDs[frameId][0];
  return triggerId == MONOCHROME_TRIGGER_ID ||
         triggerId == MONOCHROME_PALETTE_TRIGGER_ID;
}
uint16_t monochromePaletteV2[16] = {0};
uint8_t monochromePaletteV2Length = 0;

uint32_t Serum_RenderScene(void);
static void BuildFrameLookupVectors(void);
static uint64_t MakeFrameSignature(uint8_t mask, uint8_t shape, uint32_t hash);
static uint64_t MakeSceneTripletKey(uint16_t sceneId, uint8_t group,
                                    uint16_t frameIndex);
static void InitFrameLookupRuntimeStateFromStoredData(void);
static void StopV2ColorRotations(void);
static bool CaptureMonochromePaletteFromFrameV2(uint32_t frameId);
static bool IsFullBlackFrame(const uint8_t* frame, uint32_t size);
static void ConfigureSceneEndHold(uint16_t sceneId, bool interruptable,
                                  uint8_t sceneOptions);
static void ForceNormalFrameRefreshAfterSceneEnd(void);
static bool ValidateLoadedGeometry(bool isV2, const char* sourceTag);
uint32_t Identify_Frame(uint8_t* frame, bool sceneFrameRequested);

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
uint32_t lastframe_full_crc_scene = 0;
bool first_match_normal = true;
bool first_match_scene = true;
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

static uint32_t GetEnvUint32Auto(const char* name, uint32_t defaultValue) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return defaultValue;
  }
  char* endPtr = nullptr;
  unsigned long parsed = std::strtoul(value, &endPtr, 0);
  if (endPtr == value || *endPtr != '\0') {
    return defaultValue;
  }
  return static_cast<uint32_t>(parsed);
}

static uint64_t GetProcessResidentMemoryBytes() {
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return static_cast<uint64_t>(info.resident_size);
  }
#elif defined(__unix__)
  long rssPages = 0;
  FILE* statm = std::fopen("/proc/self/statm", "r");
  if (statm != nullptr) {
    if (std::fscanf(statm, "%*s %ld", &rssPages) == 1 && rssPages > 0) {
      std::fclose(statm);
      const long pageSize = sysconf(_SC_PAGESIZE);
      if (pageSize > 0) {
        return static_cast<uint64_t>(rssPages) *
               static_cast<uint64_t>(pageSize);
      }
    } else {
      std::fclose(statm);
    }
  }
#endif

#if defined(__unix__) || defined(__APPLE__)
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024ull;
#endif
  }
#endif

  return 0;
}

static void ResetStartupRssProfile() {
  if (!g_profileDynamicHotPaths) {
    g_profileStartupStartRssBytes = 0;
    g_profileStartupPeakRssBytes = 0;
    g_profileStartupPeakStage = "startup-begin";
    return;
  }
  g_profileStartupStartRssBytes = GetProcessResidentMemoryBytes();
  g_profileStartupPeakRssBytes = g_profileStartupStartRssBytes;
  g_profileStartupPeakStage = "startup-begin";
}

static void NoteStartupRssSample(const char* stage) {
  if (!g_profileDynamicHotPaths) {
    return;
  }
  const uint64_t rssBytes = GetProcessResidentMemoryBytes();
  if (rssBytes >= g_profileStartupPeakRssBytes) {
    g_profileStartupPeakRssBytes = rssBytes;
    g_profileStartupPeakStage = stage;
  }
}

static void LogStartupRssSummary() {
  if (!g_profileDynamicHotPaths) {
    return;
  }
  const uint64_t currentBytes = GetProcessResidentMemoryBytes();
  const double startMiB =
      (double)g_profileStartupStartRssBytes / (1024.0 * 1024.0);
  const double currentMiB = (double)currentBytes / (1024.0 * 1024.0);
  const double peakMiB =
      (double)g_profileStartupPeakRssBytes / (1024.0 * 1024.0);
  Log("Perf startup peak: start=%.1fMiB current=%.1fMiB peak=%.1fMiB stage=%s",
      startMiB, currentMiB, peakMiB, g_profileStartupPeakStage);
}

static void ResetDynamicHotPathProfile() {
  g_profileRoundTripNs = 0;
  g_profileColorizeFrameV2Ns = 0;
  g_profileColorizeSpriteV2Ns = 0;
  g_profileColorizeCalls = 0;
  g_profileIdentifyTotalNs = 0;
  g_profileIdentifyNormalNs = 0;
  g_profileIdentifySceneNs = 0;
  g_profileIdentifyCriticalNs = 0;
  g_profileIdentifyNormalCalls = 0;
  g_profileIdentifySceneCalls = 0;
  g_profileIdentifyCriticalCalls = 0;
  g_profileIncomingFrameCalls = 0;
  g_profileNoFrameReturns = 0;
  g_profileSameFrameReturns = 0;
  g_profileLastLoggedInputCount = 0;
  g_profilePeakRssBytes = GetProcessResidentMemoryBytes();
  g_profileFrameOperationDepth = 0;
  g_profileFrameOperationFinished = false;
}

static void MaybeLogDynamicHotPathProfileWindow(bool sceneFrameRequested) {
  if (!g_profileDynamicHotPaths || sceneFrameRequested ||
      g_profileIncomingFrameCalls == 0 ||
      (g_profileIncomingFrameCalls % 240u) != 0u ||
      g_profileIncomingFrameCalls == g_profileLastLoggedInputCount) {
    return;
  }

  const double roundTripMs = g_profileColorizeCalls == 0
                                 ? 0.0
                                 : (double)g_profileRoundTripNs /
                                       (double)g_profileColorizeCalls /
                                       1000000.0;
  const double frameMs = g_profileColorizeCalls == 0
                             ? 0.0
                             : (double)g_profileColorizeFrameV2Ns /
                                   (double)g_profileColorizeCalls / 1000000.0;
  const double spriteMs = g_profileColorizeCalls == 0
                              ? 0.0
                              : (double)g_profileColorizeSpriteV2Ns /
                                    (double)g_profileColorizeCalls / 1000000.0;
  const double identifyMs = g_profileColorizeCalls == 0
                                ? 0.0
                                : (double)g_profileIdentifyTotalNs /
                                      (double)g_profileColorizeCalls /
                                      1000000.0;
  const double identifyNormalMs =
      g_profileIdentifyNormalCalls == 0
          ? 0.0
          : (double)g_profileIdentifyNormalNs /
                (double)g_profileIdentifyNormalCalls / 1000000.0;
  const double identifySceneMs = g_profileIdentifySceneCalls == 0
                                     ? 0.0
                                     : (double)g_profileIdentifySceneNs /
                                           (double)g_profileIdentifySceneCalls /
                                           1000000.0;
  const double identifyCriticalMs =
      g_profileIdentifyCriticalCalls == 0
          ? 0.0
          : (double)g_profileIdentifyCriticalNs /
                (double)g_profileIdentifyCriticalCalls / 1000000.0;
  const uint64_t rssBytes = GetProcessResidentMemoryBytes();
  if (rssBytes > g_profilePeakRssBytes) {
    g_profilePeakRssBytes = rssBytes;
  }
  const double rssMiB = (double)rssBytes / (1024.0 * 1024.0);
  const double peakRssMiB = (double)g_profilePeakRssBytes / (1024.0 * 1024.0);
  Log("Perf dynamic avg: frame=%.3fms Colorize_Framev2=%.3fms "
      "Colorize_Spritev2=%.3fms Identify=%.3fms "
      "IdentifyNormal=%.3fms IdentifyScene=%.3fms "
      "IdentifyCritical=%.3fms inputs=%llu rendered=%llu "
      "same=%llu noFrame=%llu rss=%.1fMiB peak=%.1fMiB",
      roundTripMs, frameMs, spriteMs, identifyMs, identifyNormalMs,
      identifySceneMs, identifyCriticalMs,
      static_cast<unsigned long long>(g_profileIncomingFrameCalls),
      static_cast<unsigned long long>(g_profileColorizeCalls),
      static_cast<unsigned long long>(g_profileSameFrameReturns),
      static_cast<unsigned long long>(g_profileNoFrameReturns), rssMiB,
      peakRssMiB);
  if (g_profileSparseVectors) {
    g_serumData.LogSparseVectorProfileSnapshot();
  }
  g_profileLastLoggedInputCount = g_profileIncomingFrameCalls;
  if (g_profileDynamicHotPathsWindowed) {
    ResetDynamicHotPathProfile();
  }
}

static void InitDebugFrameTracingFromEnv(void) {
  if (g_debugFrameTracingInitialized) {
    return;
  }
  g_debugFrameTracingInitialized = true;
  g_debugTargetInputCrc = GetEnvUint32Auto("SERUM_DEBUG_INPUT_CRC", 0);
  g_debugTargetFrameId = GetEnvUint32Auto("SERUM_DEBUG_FRAME_ID", 0xffffffffu);
  g_debugStageHashes = IsEnvFlagEnabled("SERUM_DEBUG_STAGE_HASHES");
  g_debugTraceAllInputs = IsEnvFlagEnabled("SERUM_DEBUG_TRACE_INPUTS");
  g_debugBypassSceneGate = IsEnvFlagEnabled("SERUM_DEBUG_BYPASS_SCENE_GATE");
  g_debugVerboseIdentify = IsEnvFlagEnabled("SERUM_DEBUG_IDENTIFY_VERBOSE");
  g_debugVerboseSprites = IsEnvFlagEnabled("SERUM_DEBUG_SPRITE_VERBOSE");
  g_debugVerboseScenes = IsEnvFlagEnabled("SERUM_DEBUG_SCENE_VERBOSE");
  if (g_debugTargetInputCrc != 0 || g_debugTargetFrameId != 0xffffffffu ||
      g_debugStageHashes || g_debugTraceAllInputs || g_debugBypassSceneGate ||
      g_debugVerboseIdentify || g_debugVerboseSprites || g_debugVerboseScenes) {
    Log("Serum debug tracing enabled: inputCrc=%u frameId=%u stageHashes=%s "
        "traceAllInputs=%s bypassSceneGate=%s identifyVerbose=%s "
        "spriteVerbose=%s sceneVerbose=%s",
        g_debugTargetInputCrc, g_debugTargetFrameId,
        g_debugStageHashes ? "on" : "off", g_debugTraceAllInputs ? "on" : "off",
        g_debugBypassSceneGate ? "on" : "off",
        g_debugVerboseIdentify ? "on" : "off",
        g_debugVerboseSprites ? "on" : "off",
        g_debugVerboseScenes ? "on" : "off");
  }
}

static bool DebugTraceMatches(uint32_t inputCrc, uint32_t frameId) {
  InitDebugFrameTracingFromEnv();
  const bool crcMatches =
      (g_debugTargetInputCrc == 0) || (inputCrc == g_debugTargetInputCrc);
  const bool frameMatches = (g_debugTargetFrameId == 0xffffffffu) ||
                            (frameId == g_debugTargetFrameId);
  return crcMatches && frameMatches;
}

static bool DebugTraceMatchesInputCrc(uint32_t inputCrc) {
  InitDebugFrameTracingFromEnv();
  return (g_debugTargetInputCrc == 0) || (inputCrc == g_debugTargetInputCrc);
}

static bool DebugTraceAllInputsEnabled() {
  InitDebugFrameTracingFromEnv();
  return g_debugTraceAllInputs;
}

static bool DebugIdentifyVerboseEnabled() {
  InitDebugFrameTracingFromEnv();
  return g_debugVerboseIdentify;
}

static bool DebugSpriteVerboseEnabled() {
  InitDebugFrameTracingFromEnv();
  return g_debugVerboseSprites;
}

static bool DebugSceneVerboseEnabled() {
  InitDebugFrameTracingFromEnv();
  return g_debugVerboseScenes;
}

static void DebugLogSceneEvent(const char* event, uint16_t sceneId,
                               uint16_t frameIndex, uint16_t frameCount,
                               uint16_t durationPerFrame, uint8_t options,
                               bool interruptable, bool startImmediately,
                               uint8_t repeatCount, uint8_t group = 0,
                               int result = -1) {
  if (!DebugSceneVerboseEnabled()) {
    return;
  }
  Log("Serum debug scene event: event=%s sceneId=%u frameIndex=%u "
      "frameCount=%u duration=%u options=%u interruptable=%s "
      "startImmediately=%s repeat=%u group=%u result=%d",
      event ? event : "unknown", sceneId, frameIndex, frameCount,
      durationPerFrame, options, interruptable ? "true" : "false",
      startImmediately ? "true" : "false", repeatCount, group, result);
}

static void DebugLogFrameMetadataIfRequested(uint32_t frameId) {
  InitDebugFrameTracingFromEnv();
  if (g_debugTargetFrameId == 0xffffffffu || frameId != g_debugTargetFrameId ||
      frameId >= g_serumData.nframes || g_debugFrameMetaLoggedFor == frameId) {
    return;
  }
  g_debugFrameMetaLoggedFor = frameId;

  const uint8_t mask = g_serumData.compmaskID[frameId][0];
  const uint8_t shape = g_serumData.shapecompmode[frameId][0];
  const uint32_t hash = g_serumData.hashcodes[frameId][0];
  const uint8_t active = g_serumData.activeframes[frameId][0];
  const uint32_t triggerId = g_serumData.triggerIDs[frameId][0];
  const uint16_t backgroundId = g_serumData.backgroundIDs[frameId][0];
  const uint8_t isExtra = g_serumData.isextraframe[frameId][0];
  const uint8_t hasDynamic = (frameId < g_serumData.frameHasDynamic.size())
                                 ? g_serumData.frameHasDynamic[frameId]
                                 : 0;
  const uint8_t hasDynamicExtra =
      (frameId < g_serumData.frameHasDynamicExtra.size())
          ? g_serumData.frameHasDynamicExtra[frameId]
          : 0;
  const uint8_t isScene = (frameId < g_serumData.frameIsScene.size())
                              ? g_serumData.frameIsScene[frameId]
                              : 0;

  Log("Serum debug frame meta: frameId=%u mask=%u shape=%u hash=%u active=%u "
      "triggerId=%u backgroundId=%u isExtra=%u hasDynamic=%u "
      "hasDynamicExtra=%u isScene=%u",
      frameId, mask, shape, hash, active, triggerId, backgroundId, isExtra,
      hasDynamic, hasDynamicExtra, isScene);

  const uint8_t* spriteSlots = g_serumData.framesprites[frameId];
  const uint16_t* spriteBB = g_serumData.framespriteBB[frameId];
  uint32_t spriteCount = 0;
  for (uint32_t i = 0; i < MAX_SPRITES_PER_FRAME; ++i) {
    if (spriteSlots[i] >= 255) {
      break;
    }
    ++spriteCount;
  }
  if (spriteCount == 0) {
    Log("Serum debug frame sprites: frameId=%u count=0", frameId);
    return;
  }

  for (uint32_t i = 0; i < spriteCount; ++i) {
    const uint8_t spriteId = spriteSlots[i];
    const uint8_t usesShape = (spriteId < g_serumData.spriteUsesShape.size())
                                  ? g_serumData.spriteUsesShape[spriteId]
                                  : g_serumData.sprshapemode[spriteId][0];
    Log("Serum debug frame sprite-slot: frameId=%u slot=%u spriteId=%u "
        "bbox=[%u,%u..%u,%u] usesShape=%u",
        frameId, i, spriteId, spriteBB[i * 4], spriteBB[i * 4 + 1],
        spriteBB[i * 4 + 2], spriteBB[i * 4 + 3], usesShape);
  }

  g_serumData.DebugLogPackingSidecarsStorageSizes();
}

static bool FrameHasRenderableContent(uint32_t frameId) {
  if (frameId >= g_serumData.nframes) {
    return false;
  }
  if (g_serumData.activeframes[frameId][0] != 0) {
    return true;
  }
  if (g_serumData.backgroundIDs[frameId][0] < g_serumData.nbackgrounds) {
    return true;
  }
  if (frameId < g_serumData.frameHasDynamic.size() &&
      g_serumData.frameHasDynamic[frameId] > 0) {
    return true;
  }
  if (frameId < g_serumData.frameHasDynamicExtra.size() &&
      g_serumData.frameHasDynamicExtra[frameId] > 0) {
    return true;
  }
  return false;
}

static uint64_t DebugHashBytesFNV1a64(const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static uint64_t DebugHashFrameRegionFNV1a64(const uint16_t* frame,
                                            uint32_t stride, uint16_t x,
                                            uint16_t y, uint16_t width,
                                            uint16_t height) {
  if (!frame || width == 0 || height == 0) {
    return 1469598103934665603ull;
  }
  uint64_t hash = 1469598103934665603ull;
  for (uint16_t row = 0; row < height; ++row) {
    const uint16_t* src = frame + static_cast<size_t>(y + row) * stride + x;
    for (uint16_t col = 0; col < width; ++col) {
      const uint16_t value = src[col];
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
      hash ^= bytes[0];
      hash *= 1099511628211ull;
      hash ^= bytes[1];
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

static uint64_t DebugHashCurrentOutputFrame(uint32_t frameId, bool isExtra) {
  uint16_t* output = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  if ((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) && mySerum.frame32) {
    output = mySerum.frame32;
    width = mySerum.width32;
    height = 32;
  } else if ((mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) && mySerum.frame64) {
    output = mySerum.frame64;
    width = mySerum.width64;
    height = 64;
  }
  if (!output || width == 0 || height == 0) {
    return 0;
  }
  const uint64_t hash = DebugHashBytesFNV1a64(
      output, static_cast<size_t>(width) * height * sizeof(uint16_t));
  if (g_debugStageHashes &&
      DebugTraceMatches(g_debugCurrentInputCrc, frameId)) {
    Log("Serum debug stage hash: frameId=%u inputCrc=%u stage=%s hash=%llu "
        "size=%ux%u",
        frameId, g_debugCurrentInputCrc, isExtra ? "base-extra" : "base",
        static_cast<unsigned long long>(hash), width, height);
  }
  return hash;
}

static void DebugLogColorizeFrameV2Assets(
    uint32_t frameId, uint32_t inputCrc, bool isExtra, uint32_t width,
    uint32_t height, const uint16_t* frameColors,
    const uint8_t* frameBackgroundMask, const uint16_t* frameBackground,
    bool frameHasDynamic, const uint8_t* frameDyna,
    const uint8_t* frameDynaActive, const uint16_t* frameDynaColors,
    const uint16_t* colorRotations, uint16_t backgroundId) {
  if (!g_debugStageHashes || !DebugTraceMatches(inputCrc, frameId)) {
    return;
  }

  const uint32_t pixelCount = width * height;
  uint32_t backgroundMaskPixels = 0;
  uint32_t dynamicActivePixels = 0;
  uint32_t dynamicNonZeroPixels = 0;
  if (frameBackgroundMask) {
    for (uint32_t i = 0; i < pixelCount; ++i) {
      if (frameBackgroundMask[i] > 0) {
        ++backgroundMaskPixels;
      }
    }
  }
  if (frameHasDynamic && frameDynaActive) {
    for (uint32_t i = 0; i < pixelCount; ++i) {
      if (frameDynaActive[i] > 0) {
        ++dynamicActivePixels;
        if (frameDyna && frameDyna[i] > 0) {
          ++dynamicNonZeroPixels;
        }
      }
    }
  }

  const uint64_t colorsHash =
      frameColors ? DebugHashBytesFNV1a64(frameColors,
                                          (size_t)pixelCount * sizeof(uint16_t))
                  : 0;
  const uint64_t backgroundMaskHash =
      frameBackgroundMask
          ? DebugHashBytesFNV1a64(frameBackgroundMask, (size_t)pixelCount)
          : 0;
  const uint64_t backgroundHash =
      frameBackground
          ? DebugHashBytesFNV1a64(frameBackground,
                                  (size_t)pixelCount * sizeof(uint16_t))
          : 0;
  const uint64_t dynaHash =
      (frameHasDynamic && frameDyna)
          ? DebugHashBytesFNV1a64(frameDyna, (size_t)pixelCount)
          : 0;
  const uint64_t dynaActiveHash =
      (frameHasDynamic && frameDynaActive)
          ? DebugHashBytesFNV1a64(frameDynaActive, (size_t)pixelCount)
          : 0;
  const uint64_t dynaColorsHash =
      (frameHasDynamic && frameDynaColors)
          ? DebugHashBytesFNV1a64(frameDynaColors,
                                  (size_t)MAX_DYNA_SETS_PER_FRAME_V2 *
                                      g_serumData.nocolors * sizeof(uint16_t))
          : 0;
  const uint64_t rotationHash =
      colorRotations ? DebugHashBytesFNV1a64(colorRotations,
                                             (size_t)MAX_COLOR_ROTATION_V2 *
                                                 MAX_LENGTH_COLOR_ROTATION *
                                                 sizeof(uint16_t))
                     : 0;

  Log("Serum debug stage assets: frameId=%u inputCrc=%u stage=%s "
      "backgroundId=%u colorsHash=%llu backgroundMaskHash=%llu "
      "backgroundHash=%llu backgroundPixels=%u dynamic=%s "
      "dynaHash=%llu dynaActiveHash=%llu dynaColorsHash=%llu "
      "dynamicPixels=%u dynamicNonZero=%u rotationHash=%llu",
      frameId, inputCrc, isExtra ? "assets-extra" : "assets", backgroundId,
      static_cast<unsigned long long>(colorsHash),
      static_cast<unsigned long long>(backgroundMaskHash),
      static_cast<unsigned long long>(backgroundHash), backgroundMaskPixels,
      frameHasDynamic ? "true" : "false",
      static_cast<unsigned long long>(dynaHash),
      static_cast<unsigned long long>(dynaActiveHash),
      static_cast<unsigned long long>(dynaColorsHash), dynamicActivePixels,
      dynamicNonZeroPixels, static_cast<unsigned long long>(rotationHash));
}

static bool DebugTraceSpritesForCurrentInput() {
  return DebugTraceMatchesInputCrc(g_debugCurrentInputCrc);
}

static void DebugLogSpriteCheckStart(uint32_t frameId, uint32_t candidateCount,
                                     bool hasCandidateSidecars,
                                     bool frameHasShapeCandidates) {
  if (!DebugSpriteVerboseEnabled() || !DebugTraceSpritesForCurrentInput()) {
    return;
  }
  Log("Serum debug sprites start: frameId=%u inputCrc=%u candidates=%u "
      "sidecars=%s shapeCandidates=%s",
      frameId, g_debugCurrentInputCrc, candidateCount,
      hasCandidateSidecars ? "true" : "false",
      frameHasShapeCandidates ? "true" : "false");
}

static void DebugLogSpriteCandidate(uint32_t frameId, uint8_t spriteId,
                                    uint8_t spriteSlot, bool usesShape,
                                    uint32_t detectCount, short minxBB,
                                    short minyBB, short maxxBB, short maxyBB,
                                    int spriteWidth, int spriteHeight) {
  if (!DebugSpriteVerboseEnabled() || !DebugTraceSpritesForCurrentInput()) {
    return;
  }
  Log("Serum debug sprite candidate: frameId=%u inputCrc=%u spriteId=%u "
      "slot=%u shape=%s detectCount=%u bbox=[%d,%d..%d,%d] size=%dx%d",
      frameId, g_debugCurrentInputCrc, spriteId, spriteSlot,
      usesShape ? "true" : "false", detectCount, minxBB, minyBB, maxxBB, maxyBB,
      spriteWidth, spriteHeight);
}

static void DebugLogSpriteDetectionWord(uint32_t frameId, uint8_t spriteId,
                                        uint32_t detectionIndex,
                                        uint32_t detectionWord, short frax,
                                        short fray, short offsx, short offsy,
                                        short detw, short deth) {
  if (!DebugSpriteVerboseEnabled() || !DebugTraceSpritesForCurrentInput()) {
    return;
  }
  Log("Serum debug sprite detection: frameId=%u inputCrc=%u spriteId=%u "
      "detectIndex=%u word=%u framePos=(%d,%d) area=(%d,%d %dx%d)",
      frameId, g_debugCurrentInputCrc, spriteId, detectionIndex, detectionWord,
      frax, fray, offsx, offsy, detw, deth);
}

static void DebugLogSpriteRejected(uint32_t frameId, uint8_t spriteId,
                                   uint8_t spriteSlot, const char* reason,
                                   uint32_t detectionIndex, short frax,
                                   short fray, short offsx, short offsy,
                                   uint32_t detailA = 0, uint32_t detailB = 0,
                                   uint32_t detailC = 0, uint32_t detailD = 0) {
  if (!DebugSpriteVerboseEnabled() || !DebugTraceSpritesForCurrentInput()) {
    return;
  }
  Log("Serum debug sprite rejected: frameId=%u inputCrc=%u spriteId=%u "
      "slot=%u reason=%s detectIndex=%u framePos=(%d,%d) area=(%d,%d) "
      "detailA=%u detailB=%u detailC=%u detailD=%u",
      frameId, g_debugCurrentInputCrc, spriteId, spriteSlot, reason,
      detectionIndex, frax, fray, offsx, offsy, detailA, detailB, detailC,
      detailD);
}

static void DebugLogSpriteAccepted(uint32_t frameId, uint8_t spriteId,
                                   uint8_t spriteSlot, uint16_t frameX,
                                   uint16_t frameY, uint16_t spriteX,
                                   uint16_t spriteY, uint16_t width,
                                   uint16_t height, bool duplicate) {
  if (!DebugSpriteVerboseEnabled() || !DebugTraceSpritesForCurrentInput()) {
    return;
  }
  Log("Serum debug sprite accepted: frameId=%u inputCrc=%u spriteId=%u "
      "slot=%u frame=(%u,%u) sprite=(%u,%u) size=%ux%u duplicate=%s",
      frameId, g_debugCurrentInputCrc, spriteId, spriteSlot, frameX, frameY,
      spriteX, spriteY, width, height, duplicate ? "true" : "false");
}

static void DebugLogSpriteCheckResult(uint32_t frameId, uint8_t nspr) {
  if (!DebugSpriteVerboseEnabled() || !DebugTraceSpritesForCurrentInput()) {
    return;
  }
  Log("Serum debug sprites result: frameId=%u inputCrc=%u matches=%u", frameId,
      g_debugCurrentInputCrc, nspr);
}

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

  if (!is_valid_frame(g_serumData.fwidth, g_serumData.fheight)) {
    Log("Invalid frame size in %s: %ux%u", sourceTag, g_serumData.fwidth,
        g_serumData.fheight);
    return false;
  }

  if (isV2) {
    if (g_serumData.fheight != 32 && g_serumData.fheight != 64) {
      Log("Invalid base frame height in %s: %u (expected 32 or 64)", sourceTag,
          g_serumData.fheight);
      return false;
    }

    const bool hasExtra =
        (g_serumData.fwidth_extra > 0 || g_serumData.fheight_extra > 0);
    if (hasExtra) {
      if (!is_valid_frame(g_serumData.fwidth_extra,
                          g_serumData.fheight_extra)) {
        Log("Invalid extra frame size in %s: %ux%u", sourceTag,
            g_serumData.fwidth_extra, g_serumData.fheight_extra);
        return false;
      }
      if (g_serumData.fheight_extra != 32 && g_serumData.fheight_extra != 64) {
        Log("Invalid extra frame height in %s: %u (expected 32 or 64)",
            sourceTag, g_serumData.fheight_extra);
        return false;
      }
    }
  } else {
    if (g_serumData.nccolors == 0 || g_serumData.nccolors > 64) {
      Log("Invalid palette size in %s: nccolors=%u", sourceTag,
          g_serumData.nccolors);
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
  first_match_normal = true;
  first_match_scene = true;
  sceneEndHoldUntilMs = 0;
  sceneEndHoldDurationMs = 0;
  sceneNextFrameAtMs = 0;
  monochromeMode = false;
  monochromePaletteMode = false;
  monochromePaletteV2Length = 0;
  g_sceneResumeState.clear();
  g_criticalTriggerMaskShapes.clear();

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
  if (g_serumData.sceneGenerator && g_serumData.sceneGenerator->isActive()) {
    g_serumData.sceneGenerator->setDepth(g_serumData.nocolors == 16 ? 4 : 2);
  }
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
  mySerum.nocolors = g_serumData.nocolors;

  if (!ValidateLoadedGeometry(g_serumData.SerumVersion == SERUM_V2, "cROMc")) {
    Log("Failed to vaildate cROMc geometry.");
    enabled = false;
    return NULL;
  }

  {
    const char* debugSpriteId = std::getenv("SERUM_DEBUG_SPRITE_ID");
    if (debugSpriteId && debugSpriteId[0] != '\0') {
      char* endPtr = nullptr;
      unsigned long parsed = std::strtoul(debugSpriteId, &endPtr, 0);
      if (endPtr != debugSpriteId && *endPtr == '\0') {
        g_serumData.DebugLogSpriteDynamicSidecarState(
            "post-load-prepared", static_cast<uint32_t>(parsed));
      }
    }
  }

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

static void LogLoadedColorizationSource(const std::string& path,
                                        bool loadedFromConcentrate) {
  if (loadedFromConcentrate) {
    Log("Loaded %s (Serum v%d, concentrate v%d)", path.c_str(),
        g_serumData.SerumVersion, g_serumData.concentrateFileVersion);
  } else {
    Log("Loaded %s (Serum v%d)", path.c_str(), g_serumData.SerumVersion);
  }
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
  if ((g_serumData.nframes == 0) || (g_serumData.nocolors == 0) ||
      !ValidateLoadedGeometry(true, "cROM/v2")) {
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

  g_serumData.BuildPackingSidecarsAndNormalize();

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
  if (!ValidateLoadedGeometry(false, "cROM/v1")) {
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

  g_serumData.BuildPackingSidecarsAndNormalize();
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
  g_profileDynamicHotPaths = IsEnvFlagEnabled("SERUM_PROFILE_DYNAMIC_HOTPATHS");
  g_profileDynamicHotPathsWindowed =
      IsEnvFlagEnabled("SERUM_PROFILE_DYNAMIC_HOTPATHS_WINDOWED");
  g_profileSparseVectors = IsEnvFlagEnabled("SERUM_PROFILE_SPARSE_VECTORS");
  g_profileRoundTripNs = 0;
  g_profileColorizeFrameV2Ns = 0;
  g_profileColorizeSpriteV2Ns = 0;
  g_profileColorizeCalls = 0;
  g_profileIdentifyTotalNs = 0;
  g_profileIdentifyNormalNs = 0;
  g_profileIdentifySceneNs = 0;
  g_profileIdentifyCriticalNs = 0;
  g_profileIdentifyNormalCalls = 0;
  g_profileIdentifySceneCalls = 0;
  g_profileIdentifyCriticalCalls = 0;
  g_profilePeakRssBytes = 0;
  g_profileFrameOperationDepth = 0;
  g_profileFrameOperationFinished = false;
  ResetStartupRssProfile();

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
  NoteStartupRssSample("after-file-scan");
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
      NoteStartupRssSample("before-cromc-load");
      result = Serum_LoadConcentrate(pFoundFile->c_str(), flags);
      loadedFromConcentrate = (result != NULL);
      if (result) {
        NoteStartupRssSample("after-cromc-load");
        LogLoadedColorizationSource(*pFoundFile, true);
        if (csvFoundFile && g_serumData.SerumVersion == SERUM_V2 &&
            g_serumData.sceneGenerator->parseCSV(csvFoundFile->c_str())) {
          sceneDataUpdatedFromCsv = true;
          NoteStartupRssSample("after-csv-update");
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
    NoteStartupRssSample("before-crom-load");
    result = Serum_LoadFilev1(pFoundFile->c_str(), flags);
    if (result) {
      NoteStartupRssSample("after-crom-load");
      LogLoadedColorizationSource(*pFoundFile, false);
      if (csvFoundFile && g_serumData.SerumVersion == SERUM_V2) {
        sceneDataUpdatedFromCsv =
            g_serumData.sceneGenerator->parseCSV(csvFoundFile->c_str());
        if (sceneDataUpdatedFromCsv) {
          NoteStartupRssSample("after-csv-update");
        }
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
    const bool rebuildDerivedLookups = !loadedFromConcentrate ||
                                       g_serumData.concentrateFileVersion < 6 ||
                                       sceneDataUpdatedFromCsv;
    if (loadedFromConcentrate && g_serumData.concentrateFileVersion < 6) {
      g_serumData.BuildPackingSidecarsAndNormalize();
      NoteStartupRssSample("after-packing-sidecar-normalize");
    }
    if (rebuildDerivedLookups) {
      BuildFrameLookupVectors();
      NoteStartupRssSample("after-frame-lookup-build");
    } else {
      InitFrameLookupRuntimeStateFromStoredData();
      NoteStartupRssSample("after-frame-lookup-restore");
    }
    if (rebuildDerivedLookups ||
        g_serumData.colorRotationLookupByFrameAndColor.empty()) {
      g_serumData.BuildColorRotationLookup();
      NoteStartupRssSample("after-color-rotation-build");
    }
    if (!g_serumData.HasSpriteRuntimeSidecars() &&
        (!loadedFromConcentrate || g_serumData.concentrateFileVersion < 6)) {
      g_serumData.BuildSpriteRuntimeSidecars();
      NoteStartupRssSample("after-sprite-sidecar-build");
    }
    InitCriticalTriggerLookupRuntimeState();
    NoteStartupRssSample("before-runtime");
    LogStartupRssSummary();
    ResetDynamicHotPathProfile();
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
  g_serumData.normalFramesBySignature.clear();
  g_serumData.normalIdentifyBuckets.clear();
  g_serumData.frameToNormalBucket.clear();
  g_serumData.sceneFrameIdByTriplet.clear();

  if (g_serumData.nframes == 0) return;
  g_serumData.frameIsScene.resize(g_serumData.nframes, 0);
  g_serumData.frameToNormalBucket.assign(g_serumData.nframes, 0xffffffffu);
  const uint32_t pixels = g_serumData.is256x64
                              ? (256 * 64)
                              : (g_serumData.fwidth * g_serumData.fheight);

  // Build scene signatures in the same domain used by Identify_Frame:
  // (mask, shape, crc32 over original frame pixels).
  if (g_serumData.SerumVersion == SERUM_V2 && g_serumData.fwidth == 128 &&
      g_serumData.fheight == 32 && g_serumData.sceneGenerator &&
      g_serumData.sceneGenerator->isActive()) {
    std::unordered_set<uint16_t> uniqueMaskShapeKeys;
    std::vector<std::pair<uint8_t, uint8_t>> uniqueMaskShapes;
    uniqueMaskShapes.reserve(g_serumData.nframes);

    for (uint32_t frameId = 0; frameId < g_serumData.nframes; ++frameId) {
      const uint8_t mask = g_serumData.compmaskID[frameId][0];
      const uint8_t shape = g_serumData.shapecompmode[frameId][0];
      const uint16_t key = (uint16_t(mask) << 8) | shape;
      if (uniqueMaskShapeKeys.insert(key).second) {
        uniqueMaskShapes.emplace_back(mask, shape);
      }
    }

    std::unordered_set<uint64_t> sceneSignatures;
    sceneSignatures.reserve(uniqueMaskShapes.size() * 64);

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
          for (const auto& maskShape : uniqueMaskShapes) {
            uint32_t hash = calc_crc32(generatedSceneFrame, maskShape.first,
                                       pixels, maskShape.second);
            sceneSignatures.insert(
                MakeFrameSignature(maskShape.first, maskShape.second, hash));
          }
        }
      }
    }

    for (uint32_t frameId = 0; frameId < g_serumData.nframes; ++frameId) {
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

    g_serumData.BuildCriticalTriggerLookup();

    if (g_serumData.concentrateFileVersion >= 6) {
      // Build direct lookup table: (sceneId, group, frameIndex) -> frameId.
      // Keep this as a preprocessing step only; runtime scene rendering can
      // use it to bypass generic scene identification.
      const uint32_t saved_lastfound = lastfound;
      const uint32_t saved_lastfound_scene = lastfound_scene;
      const uint32_t saved_lastframe_full_crc_scene = lastframe_full_crc_scene;
      const bool saved_first_match_scene = first_match_scene;

      first_match_scene = true;
      lastfound_scene = 0;
      lastframe_full_crc_scene = 0;

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
            const uint32_t identified =
                Identify_Frame(generatedSceneFrame, true);
            if (identified == IDENTIFY_NO_FRAME) {
              continue;
            }
            const uint32_t frameId = (identified == IDENTIFY_SAME_FRAME)
                                         ? lastfound_scene
                                         : identified;
            if (frameId >= g_serumData.nframes) {
              continue;
            }
            g_serumData.sceneFrameIdByTriplet[MakeSceneTripletKey(
                scene.sceneId, static_cast<uint8_t>(group), frameIndex)] =
                frameId;
          }
        }
      }

      lastfound = saved_lastfound;
      lastfound_scene = saved_lastfound_scene;
      lastframe_full_crc_scene = saved_lastframe_full_crc_scene;
      first_match_scene = saved_first_match_scene;
    }
  }

  for (uint32_t frameId = 0; frameId < g_serumData.nframes; ++frameId) {
    if (g_serumData.frameIsScene[frameId] != 0) {
      continue;
    }
    const uint8_t mask = g_serumData.compmaskID[frameId][0];
    const uint8_t shape = g_serumData.shapecompmode[frameId][0];
    const uint32_t hash = g_serumData.hashcodes[frameId][0];
    uint32_t bucketIndex = 0xffffffffu;
    for (uint32_t i = 0; i < g_serumData.normalIdentifyBuckets.size(); ++i) {
      const auto& bucket = g_serumData.normalIdentifyBuckets[i];
      if (bucket.mask == mask && bucket.shape == shape) {
        bucketIndex = i;
        break;
      }
    }
    if (bucketIndex == 0xffffffffu) {
      bucketIndex =
          static_cast<uint32_t>(g_serumData.normalIdentifyBuckets.size());
      g_serumData.normalIdentifyBuckets.push_back({mask, shape, 0});
    }
    g_serumData.frameToNormalBucket[frameId] = bucketIndex;
    g_serumData.normalFramesBySignature[MakeFrameSignature(mask, shape, hash)]
        .push_back(frameId);
  }

  Log("Loaded %d frames and %d rotation scene frames",
      g_serumData.nframes - numSceneFrames, numSceneFrames);

  lastfound_scene = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.nframes; ++frameId) {
    if (g_serumData.frameIsScene[frameId]) {
      lastfound_scene = frameId;
      break;
    }
  }

  lastfound_normal = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.nframes; ++frameId) {
    if (!g_serumData.frameIsScene[frameId]) {
      lastfound_normal = frameId;
      break;
    }
  }
}

static uint64_t MakeFrameSignature(uint8_t mask, uint8_t shape, uint32_t hash) {
  return (uint64_t(mask) << 40) | (uint64_t(shape) << 32) | hash;
}

static uint64_t MakeSceneTripletKey(uint16_t sceneId, uint8_t group,
                                    uint16_t frameIndex) {
  return (uint64_t(sceneId) << 24) | (uint64_t(group) << 16) |
         uint64_t(frameIndex);
}

static void InitFrameLookupRuntimeStateFromStoredData(void) {
  if (g_serumData.frameIsScene.size() != g_serumData.nframes) {
    BuildFrameLookupVectors();
    return;
  }

  uint32_t numSceneFrames = 0;
  for (uint8_t isScene : g_serumData.frameIsScene) {
    if (isScene) numSceneFrames++;
  }
  Log("Loaded %d frames and %d rotation scene frames",
      g_serumData.nframes - numSceneFrames, numSceneFrames);

  lastfound_scene = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.nframes; ++frameId) {
    if (g_serumData.frameIsScene[frameId]) {
      lastfound_scene = frameId;
      break;
    }
  }

  lastfound_normal = 0;
  for (uint32_t frameId = 0; frameId < g_serumData.nframes; ++frameId) {
    if (!g_serumData.frameIsScene[frameId]) {
      lastfound_normal = frameId;
      break;
    }
  }
}

uint32_t Identify_Frame(uint8_t* frame, bool sceneFrameRequested) {
  const auto profileStart = g_profileDynamicHotPaths
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
  auto finishProfile = [&](uint32_t result) -> uint32_t {
    if (g_profileDynamicHotPaths) {
      const uint64_t elapsedNs =
          (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - profileStart)
              .count();
      g_profileIdentifyTotalNs += elapsedNs;
      if (sceneFrameRequested) {
        g_profileIdentifySceneNs += elapsedNs;
        ++g_profileIdentifySceneCalls;
      } else {
        g_profileIdentifyNormalNs += elapsedNs;
        ++g_profileIdentifyNormalCalls;
      }
    }
    return result;
  };
  if (!cromloaded) return finishProfile(IDENTIFY_NO_FRAME);
  DebugLogFrameMetadataIfRequested(g_debugTargetFrameId);
  uint32_t tj = sceneFrameRequested
                    ? lastfound_scene
                    : lastfound_normal;  // stream-local search start
  const uint32_t pixels = g_serumData.is256x64
                              ? (256 * 64)
                              : (g_serumData.fwidth * g_serumData.fheight);
  const uint32_t inputCrc = crc32_fast(frame, pixels);
  uint32_t& lastfound_stream =
      sceneFrameRequested ? lastfound_scene : lastfound_normal;
  bool& first_match =
      sceneFrameRequested ? first_match_scene : first_match_normal;
  uint32_t& lastframe_full_crc = sceneFrameRequested
                                     ? lastframe_full_crc_scene
                                     : lastframe_full_crc_normal;
  if (!sceneFrameRequested) {
    const uint32_t bucketCount =
        static_cast<uint32_t>(g_serumData.normalIdentifyBuckets.size());
    if (bucketCount == 0 ||
        g_serumData.frameToNormalBucket.size() != g_serumData.nframes) {
      if (DebugIdentifyVerboseEnabled() &&
          DebugTraceMatchesInputCrc(inputCrc)) {
        Log("Serum debug identify miss: inputCrc=%u sceneRequested=false",
            inputCrc);
      }
      return finishProfile(IDENTIFY_NO_FRAME);
    }
    std::vector<uint8_t> bucketVisited(bucketCount, 0);
    do {
      if (g_serumData.frameIsScene[tj] != 0) {
        if (++tj >= g_serumData.nframes) tj = 0;
        continue;
      }

      const uint32_t bucketIndex = g_serumData.frameToNormalBucket[tj];
      if (bucketIndex >= bucketCount || bucketVisited[bucketIndex]) {
        if (++tj >= g_serumData.nframes) tj = 0;
        continue;
      }
      bucketVisited[bucketIndex] = 1;

      const auto& bucket = g_serumData.normalIdentifyBuckets[bucketIndex];
      const uint8_t mask = bucket.mask;
      const uint8_t Shape = bucket.shape;

      const uint32_t Hashc = calc_crc32(frame, mask, pixels, Shape);
      if (DebugIdentifyVerboseEnabled() && DebugTraceMatches(inputCrc, tj)) {
        Log("Serum debug identify seed: inputCrc=%u startFrame=%u "
            "sceneRequested=false mask=%u shape=%u hash=%u",
            inputCrc, tj, mask, Shape, Hashc);
      }

      auto normalSigIt = g_serumData.normalFramesBySignature.find(
          MakeFrameSignature(mask, Shape, Hashc));
      if (normalSigIt != g_serumData.normalFramesBySignature.end() &&
          !normalSigIt->second.empty()) {
        const uint32_t candidateFrameId =
            SelectFrameIdInWrapOrder(normalSigIt->second, tj);
        const uint32_t resolved = ResolveIdentifiedFrameMatch(
            frame, inputCrc, candidateFrameId, mask, first_match,
            lastfound_stream, lastframe_full_crc);
        if (resolved != IDENTIFY_NO_FRAME) {
          return finishProfile(resolved);
        }
      }

      if (++tj >= g_serumData.nframes) tj = 0;
    } while (tj != lastfound_stream);

    if (DebugIdentifyVerboseEnabled() && DebugTraceMatchesInputCrc(inputCrc)) {
      Log("Serum debug identify miss: inputCrc=%u sceneRequested=false",
          inputCrc);
    }
    return finishProfile(IDENTIFY_NO_FRAME);
  }

  memset(framechecked, false, g_serumData.nframes);
  do {
    if (g_serumData.frameIsScene[tj] != (sceneFrameRequested ? 1 : 0)) {
      if (++tj >= g_serumData.nframes) tj = 0;
      continue;
    }
    if (!framechecked[tj]) {
      // calculate the hashcode for the generated frame with the mask and
      // shapemode of the current crom frame
      uint8_t mask = g_serumData.compmaskID[tj][0];
      uint8_t Shape = g_serumData.shapecompmode[tj][0];
      uint32_t Hashc = calc_crc32(frame, mask, pixels, Shape);
      if (DebugIdentifyVerboseEnabled() && DebugTraceMatches(inputCrc, tj)) {
        Log("Serum debug identify seed: inputCrc=%u startFrame=%u "
            "sceneRequested=%s mask=%u shape=%u hash=%u",
            inputCrc, tj, sceneFrameRequested ? "true" : "false", mask, Shape,
            Hashc);
      }
      if (sceneFrameRequested) {
        auto sigIt = g_serumData.sceneFramesBySignature.find(
            MakeFrameSignature(mask, Shape, Hashc));
        if (sigIt == g_serumData.sceneFramesBySignature.end()) {
          framechecked[tj] = true;
          if (++tj >= g_serumData.nframes) tj = 0;
          continue;
        }
        for (uint32_t ti : sigIt->second) {
          if (DebugIdentifyVerboseEnabled() &&
              DebugTraceMatches(inputCrc, ti)) {
            Log("Serum debug identify scene candidate: inputCrc=%u frameId=%u "
                "mask=%u shape=%u hash=%u storedHash=%u lastfound=%u",
                inputCrc, ti, mask, Shape, Hashc, g_serumData.hashcodes[ti][0],
                lastfound_stream);
          }
          if (first_match || ti != lastfound_stream || mask < 255) {
            if (DebugIdentifyVerboseEnabled() &&
                DebugTraceMatches(inputCrc, ti)) {
              Log("Serum debug identify decision: inputCrc=%u frameId=%u "
                  "reason=%s firstMatch=%s lastfoundStream=%u mask=%u "
                  "fullCrcBefore=%u",
                  inputCrc, ti,
                  first_match ? "first-match"
                              : (ti != lastfound_stream ? "new-frame-id"
                                                        : "mask-lt-255"),
                  first_match ? "true" : "false", lastfound_stream, mask,
                  lastframe_full_crc);
            }
            lastfound_stream = ti;
            lastfound = ti;
            lastframe_full_crc = crc32_fast(frame, pixels);
            first_match = false;
            return finishProfile(ti);
          }

          uint32_t full_crc = crc32_fast(frame, pixels);
          if (full_crc != lastframe_full_crc) {
            if (DebugIdentifyVerboseEnabled() &&
                DebugTraceMatches(inputCrc, ti)) {
              Log("Serum debug identify decision: inputCrc=%u frameId=%u "
                  "reason=full-crc-diff firstMatch=%s lastfoundStream=%u "
                  "mask=%u fullCrcBefore=%u fullCrcNow=%u",
                  inputCrc, ti, first_match ? "true" : "false",
                  lastfound_stream, mask, lastframe_full_crc, full_crc);
            }
            lastframe_full_crc = full_crc;
            lastfound = ti;
            return finishProfile(ti);
          }
          if (DebugIdentifyVerboseEnabled() &&
              DebugTraceMatches(inputCrc, ti)) {
            Log("Serum debug identify decision: inputCrc=%u frameId=%u "
                "reason=same-frame firstMatch=%s lastfoundStream=%u mask=%u "
                "fullCrc=%u",
                inputCrc, ti, first_match ? "true" : "false", lastfound_stream,
                mask, full_crc);
          }
          lastfound = ti;
          return finishProfile(IDENTIFY_SAME_FRAME);
        }
        framechecked[tj] = true;
        if (++tj >= g_serumData.nframes) tj = 0;
        continue;
      }
      framechecked[tj] = true;
    }
    if (++tj >= g_serumData.nframes) tj = 0;
  } while (tj != lastfound_stream);

  if (DebugIdentifyVerboseEnabled() && DebugTraceMatchesInputCrc(inputCrc)) {
    Log("Serum debug identify miss: inputCrc=%u sceneRequested=%s", inputCrc,
        sceneFrameRequested ? "true" : "false");
  }
  return finishProfile(IDENTIFY_NO_FRAME);  // we found no corresponding frame
}

static uint32_t BuildRuntimeFeatureFlags(uint32_t frameId) {
  uint32_t featureFlags = 0;

  if (frameId == IDENTIFY_NO_FRAME) {
    return featureFlags;
  }

  if (frameId == 0xfffffffd) {
    return SERUM_RUNTIME_FEATURE_MONOCHROME_FALLBACK;
  }

  if (frameId >= g_serumData.nframes) {
    return featureFlags;
  }

  featureFlags |= SERUM_RUNTIME_FEATURE_MATCHED;

  if (g_serumData.backgroundIDs[frameId][0] < g_serumData.nbackgrounds) {
    featureFlags |= SERUM_RUNTIME_FEATURE_BACKGROUND;
  }

  if (frameId < g_serumData.frameHasDynamic.size() &&
      g_serumData.frameHasDynamic[frameId] > 0) {
    featureFlags |= SERUM_RUNTIME_FEATURE_DYNAMIC;
  }

  if (frameId < g_serumData.frameHasDynamicExtra.size() &&
      g_serumData.frameHasDynamicExtra[frameId] > 0) {
    featureFlags |= SERUM_RUNTIME_FEATURE_DYNAMIC_EXTRA;
  }

  for (uint8_t spriteIndex = 0; spriteIndex < MAX_SPRITES_PER_FRAME;
       ++spriteIndex) {
    if (g_serumData.framesprites[frameId][spriteIndex] < 255) {
      featureFlags |= SERUM_RUNTIME_FEATURE_SPRITES;
      break;
    }
  }

  if (frameId < g_serumData.frameHasShapeSprite.size() &&
      g_serumData.frameHasShapeSprite[frameId] > 0) {
    featureFlags |= SERUM_RUNTIME_FEATURE_SHAPE_SPRITES;
  }

  const uint16_t* rotations = g_serumData.colorrotations_v2[frameId];
  for (uint8_t rotationIndex = 0; rotationIndex < MAX_COLOR_ROTATION_V2;
       ++rotationIndex) {
    if (rotations[rotationIndex * MAX_LENGTH_COLOR_ROTATION] > 0) {
      featureFlags |= SERUM_RUNTIME_FEATURE_COLOR_ROTATION;
      break;
    }
  }

  if (frameId < g_serumData.frameIsScene.size() &&
      g_serumData.frameIsScene[frameId] > 0) {
    featureFlags |= SERUM_RUNTIME_FEATURE_SCENE;
  }

  if (g_serumData.triggerIDs[frameId][0] < 0xffffffff) {
    featureFlags |= SERUM_RUNTIME_FEATURE_TRIGGER;
  }

  return featureFlags;
}

void GetSpriteSize(uint8_t nospr, int* pswid, int* pshei,
                   const uint8_t* spriteData, int sswid, int sshei,
                   const uint8_t* spriteOpaque) {
  *pswid = *pshei = 0;
  if (nospr >= g_serumData.nsprites) return;
  if (!spriteData) return;
  for (int tj = 0; tj < sshei; tj++) {
    for (int ti = 0; ti < sswid; ti++) {
      if (spriteOpaque[tj * sswid + ti] > 0) {
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
    if (!g_serumData.spritedescriptionso.hasData(qspr) ||
        !g_serumData.spritedescriptionso_opaque.hasData(qspr)) {
      ti++;
      continue;
    }
    const uint8_t* spriteDescription = g_serumData.spritedescriptionso[qspr];
    const uint8_t* spriteOpaque = g_serumData.spritedescriptionso_opaque[qspr];
    int spw, sph;
    GetSpriteSize(qspr, &spw, &sph, spriteDescription, MAX_SPRITE_SIZE,
                  MAX_SPRITE_SIZE, spriteOpaque);
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
                const uint32_t spritePixelIndex =
                    (tk + dety) * MAX_SPRITE_SIZE + tl + detx;
                if (spriteOpaque[spritePixelIndex] == 0) continue;
                uint8_t val = spriteDescription[spritePixelIndex];
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
  *nspr = 0;
  if (g_serumData.fwidth < 4 || quelleframe >= g_serumData.nframes) {
    return false;
  }

  // Exact dword index for this frame (replaces Bloom false-positive path).
  std::unordered_set<uint32_t> frameDwords;
  frameDwords.reserve(static_cast<size_t>(g_serumData.fheight) *
                      std::max(1u, g_serumData.fwidth - 3));
  for (uint32_t y = 0; y < g_serumData.fheight; ++y) {
    const uint32_t rowBase = y * g_serumData.fwidth;
    uint32_t dword = (uint32_t)(recframe[rowBase] << 8) |
                     (uint32_t)(recframe[rowBase + 1] << 16) |
                     (uint32_t)(recframe[rowBase + 2] << 24);
    for (uint32_t x = 0; x <= g_serumData.fwidth - 4; ++x) {
      dword = (dword >> 8) | (uint32_t)(recframe[rowBase + x + 3] << 24);
      frameDwords.insert(dword);
    }
  }
  std::unordered_set<uint32_t> frameShapeDwords;
  bool frameShapeDwordsBuilt = false;

  const uint16_t* frameSpriteBoundingBoxes =
      g_serumData.framespriteBB[quelleframe];
  uint32_t candidateStart = 0;
  uint32_t candidateEnd = 0;
  const bool hasCandidateSidecars =
      g_serumData.spriteCandidateOffsets.size() ==
          static_cast<size_t>(g_serumData.nframes) + 1 &&
      g_serumData.spriteCandidateIds.size() ==
          g_serumData.spriteCandidateSlots.size();
  if (hasCandidateSidecars) {
    candidateStart = g_serumData.spriteCandidateOffsets[quelleframe];
    candidateEnd = g_serumData.spriteCandidateOffsets[quelleframe + 1];
    if (candidateEnd > g_serumData.spriteCandidateIds.size()) {
      candidateEnd =
          static_cast<uint32_t>(g_serumData.spriteCandidateIds.size());
    }
  }

  uint32_t mdword;
  bool hasShapeFrameBuffer = false;
  const bool frameHasShapeCandidates =
      hasCandidateSidecars &&
      quelleframe < g_serumData.frameHasShapeSprite.size() &&
      g_serumData.frameHasShapeSprite[quelleframe] > 0;
  const uint32_t candidateCount = hasCandidateSidecars
                                      ? (candidateEnd - candidateStart)
                                      : MAX_SPRITES_PER_FRAME;
  DebugLogSpriteCheckStart(quelleframe, candidateCount, hasCandidateSidecars,
                           frameHasShapeCandidates);
  for (uint32_t candidateIndex = 0; candidateIndex < candidateCount;
       ++candidateIndex) {
    uint8_t qspr = 255;
    uint8_t spriteSlot = 0;
    if (hasCandidateSidecars) {
      qspr = g_serumData.spriteCandidateIds[candidateStart + candidateIndex];
      spriteSlot =
          g_serumData.spriteCandidateSlots[candidateStart + candidateIndex];
    } else {
      qspr = g_serumData.framesprites[quelleframe][candidateIndex];
      if (qspr >= 255) {
        break;
      }
      spriteSlot = static_cast<uint8_t>(candidateIndex);
    }
    if (qspr >= g_serumData.nsprites || spriteSlot >= MAX_SPRITES_PER_FRAME) {
      continue;
    }

    if (!g_serumData.spriteoriginal.hasData(qspr) ||
        !g_serumData.spriteoriginal_opaque.hasData(qspr)) {
      continue;
    }
    const uint8_t* spriteOriginal = g_serumData.spriteoriginal[qspr];
    const uint8_t* spriteOpaque = g_serumData.spriteoriginal_opaque[qspr];
    uint8_t* Frame = recframe;
    const bool isshapecheck = qspr < g_serumData.spriteUsesShape.size()
                                  ? (g_serumData.spriteUsesShape[qspr] > 0)
                                  : (g_serumData.sprshapemode[qspr][0] > 0);
    if (isshapecheck && frameHasShapeCandidates) {
      if (!hasShapeFrameBuffer) {
        for (int i = 0; i < g_serumData.fwidth * g_serumData.fheight; i++) {
          frameshape[i] = (Frame[i] > 0) ? 1 : 0;
        }
        hasShapeFrameBuffer = true;
      }
      Frame = frameshape;
      if (!frameShapeDwordsBuilt) {
        frameShapeDwords.clear();
        frameShapeDwords.reserve(static_cast<size_t>(g_serumData.fheight) *
                                 std::max(1u, g_serumData.fwidth - 3));
        for (uint32_t y = 0; y < g_serumData.fheight; ++y) {
          const uint32_t rowBase = y * g_serumData.fwidth;
          uint32_t dword = (uint32_t)(frameshape[rowBase] << 8) |
                           (uint32_t)(frameshape[rowBase + 1] << 16) |
                           (uint32_t)(frameshape[rowBase + 2] << 24);
          for (uint32_t x = 0; x <= g_serumData.fwidth - 4; ++x) {
            dword =
                (dword >> 8) | (uint32_t)(frameshape[rowBase + x + 3] << 24);
            frameShapeDwords.insert(dword);
          }
        }
        frameShapeDwordsBuilt = true;
      }
    }

    const int spw = (qspr < g_serumData.spriteWidth.size())
                        ? g_serumData.spriteWidth[qspr]
                        : MAX_SPRITE_WIDTH;
    const int sph = (qspr < g_serumData.spriteHeight.size())
                        ? g_serumData.spriteHeight[qspr]
                        : MAX_SPRITE_HEIGHT;

    short minxBB = (short)(frameSpriteBoundingBoxes[spriteSlot * 4]);
    short minyBB = (short)(frameSpriteBoundingBoxes[spriteSlot * 4 + 1]);
    short maxxBB = (short)(frameSpriteBoundingBoxes[spriteSlot * 4 + 2]);
    short maxyBB = (short)(frameSpriteBoundingBoxes[spriteSlot * 4 + 3]);
    if (minxBB > maxxBB || minyBB > maxyBB || maxxBB - minxBB < 3) {
      continue;
    }

    const uint32_t detectStart = qspr < g_serumData.spriteDetectOffsets.size()
                                     ? g_serumData.spriteDetectOffsets[qspr]
                                     : 0;
    const uint32_t detectEnd =
        (qspr + 1) < g_serumData.spriteDetectOffsets.size()
            ? g_serumData.spriteDetectOffsets[qspr + 1]
            : detectStart;
    DebugLogSpriteCandidate(quelleframe, qspr, spriteSlot, isshapecheck,
                            detectEnd - detectStart, minxBB, minyBB, maxxBB,
                            maxyBB, spw, sph);
    for (uint32_t tm = detectStart; tm < detectEnd; tm++) {
      const auto& detMeta = g_serumData.spriteDetectMeta[tm];
      const bool hasDetectionWord =
          isshapecheck
              ? (frameShapeDwords.find(detMeta.detectionWord) !=
                 frameShapeDwords.end())
              : (frameDwords.find(detMeta.detectionWord) != frameDwords.end());
      if (!hasDetectionWord) {
        continue;
      }

      // we look for the sprite in the frame sent
      for (short ty = minyBB; ty <= maxyBB; ty++) {
        mdword = (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB] << 8) |
                 (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB + 1] << 16) |
                 (uint32_t)(Frame[ty * g_serumData.fwidth + minxBB + 2] << 24);
        for (short tx = minxBB; tx <= maxxBB - 3; tx++) {
          uint32_t tj = ty * g_serumData.fwidth + tx;
          mdword = (mdword >> 8) | (uint32_t)(Frame[tj + 3] << 24);
          // we look for the magic dword first:
          const uint16_t sddp = detMeta.detectionWordPos;
          if (mdword == detMeta.detectionWord) {
            short frax =
                (short)tx;  // position in the frame of the detection dword
            short fray = (short)ty;
            short sprx =
                (short)(sddp % MAX_SPRITE_WIDTH);  // position in the sprite of
                                                   // the detection dword
            short spry = (short)(sddp / MAX_SPRITE_WIDTH);
            // details of the det area:
            const short detx = static_cast<short>(detMeta.detectX);
            const short dety = static_cast<short>(detMeta.detectY);
            const short detw = static_cast<short>(detMeta.detectWidth);
            const short deth = static_cast<short>(detMeta.detectHeight);
            // if the detection area starts before the frame (left or top),
            // continue:
            if ((frax - minxBB < sprx - detx) ||
                (fray - minyBB < spry - dety)) {
              DebugLogSpriteRejected(quelleframe, qspr, spriteSlot,
                                     "bbox-start", tm - detectStart, frax, fray,
                                     0, 0, static_cast<uint32_t>(frax - minxBB),
                                     static_cast<uint32_t>(sprx - detx),
                                     static_cast<uint32_t>(fray - minyBB),
                                     static_cast<uint32_t>(spry - dety));
              continue;
            }
            // position of the detection area in the frame
            int offsx = frax - sprx + detx;
            int offsy = fray - spry + dety;
            // if the detection area extends beyond the bounding box (right or
            // bottom), continue:
            if ((offsx + detw > (int)maxxBB + 1) ||
                (offsy + deth > (int)maxyBB + 1)) {
              DebugLogSpriteRejected(quelleframe, qspr, spriteSlot, "bbox-end",
                                     tm - detectStart, frax, fray,
                                     static_cast<short>(offsx),
                                     static_cast<short>(offsy),
                                     static_cast<uint32_t>(offsx + detw),
                                     static_cast<uint32_t>((int)maxxBB + 1),
                                     static_cast<uint32_t>(offsy + deth),
                                     static_cast<uint32_t>((int)maxyBB + 1));
              continue;
            }
            DebugLogSpriteDetectionWord(quelleframe, qspr, tm - detectStart,
                                        detMeta.detectionWord, frax, fray,
                                        static_cast<short>(offsx),
                                        static_cast<short>(offsy), detw, deth);
            // we can now check if the full detection area is around the found
            // detection dword
            bool notthere = false;
            for (uint16_t tk = 0; tk < deth && !notthere; tk++) {
              const uint32_t spriteRow = static_cast<uint32_t>(dety + tk);
              const uint32_t rowIndex =
                  static_cast<uint32_t>(qspr) * MAX_SPRITE_HEIGHT + spriteRow;
              if (rowIndex >= g_serumData.spriteOpaqueRowSegmentStart.size()) {
                DebugLogSpriteRejected(
                    quelleframe, qspr, spriteSlot, "row-sidecar-oob",
                    tm - detectStart, frax, fray, static_cast<short>(offsx),
                    static_cast<short>(offsy), rowIndex,
                    static_cast<uint32_t>(
                        g_serumData.spriteOpaqueRowSegmentStart.size()),
                    spriteRow, static_cast<uint32_t>(tk));
                notthere = true;
                break;
              }
              const uint32_t segStartIndex =
                  g_serumData.spriteOpaqueRowSegmentStart[rowIndex];
              const uint16_t segCount =
                  g_serumData.spriteOpaqueRowSegmentCount[rowIndex];
              for (uint16_t seg = 0; seg < segCount && !notthere; ++seg) {
                const uint32_t segIndex = segStartIndex + seg * 2;
                if (segIndex + 1 >= g_serumData.spriteOpaqueSegments.size()) {
                  DebugLogSpriteRejected(
                      quelleframe, qspr, spriteSlot, "segment-sidecar-oob",
                      tm - detectStart, frax, fray, static_cast<short>(offsx),
                      static_cast<short>(offsy), segIndex,
                      static_cast<uint32_t>(
                          g_serumData.spriteOpaqueSegments.size()),
                      segStartIndex, segCount);
                  notthere = true;
                  break;
                }
                const uint16_t segmentX =
                    g_serumData.spriteOpaqueSegments[segIndex];
                const uint16_t segmentLen =
                    g_serumData.spriteOpaqueSegments[segIndex + 1];
                const uint16_t segFrom =
                    std::max<uint16_t>(segmentX, static_cast<uint16_t>(detx));
                const uint16_t segTo = std::min<uint16_t>(
                    static_cast<uint16_t>(segmentX + segmentLen),
                    static_cast<uint16_t>(detx + detw));
                if (segFrom >= segTo) {
                  continue;
                }

                const uint32_t spriteBase =
                    spriteRow * MAX_SPRITE_WIDTH + segFrom;
                const uint32_t frameBase =
                    static_cast<uint32_t>(tk + offsy) * g_serumData.fwidth +
                    static_cast<uint32_t>(segFrom - detx + offsx);
                for (uint16_t x = segFrom; x < segTo; ++x) {
                  const uint32_t spriteOffset = spriteBase + (x - segFrom);
                  const uint32_t frameOffset = frameBase + (x - segFrom);
                  if (spriteOpaque[spriteOffset] == 0) {
                    continue;
                  }
                  const uint8_t expectedValue =
                      isshapecheck ? static_cast<uint8_t>(
                                         spriteOriginal[spriteOffset] > 0)
                                   : spriteOriginal[spriteOffset];
                  if (expectedValue != Frame[frameOffset]) {
                    DebugLogSpriteRejected(
                        quelleframe, qspr, spriteSlot, "opaque-run-mismatch",
                        tm - detectStart, frax, fray, static_cast<short>(offsx),
                        static_cast<short>(offsy), spriteOffset, frameOffset,
                        expectedValue, Frame[frameOffset]);
                    notthere = true;
                    break;
                  }
                }
              }
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
              DebugLogSpriteAccepted(quelleframe, qspr, spriteSlot, pfrx[*nspr],
                                     pfry[*nspr], pspx[*nspr], pspy[*nspr],
                                     pwid[*nspr], phei[*nspr], identicalfound);
              if (identicalfound) {
                DebugLogSpriteRejected(
                    quelleframe, qspr, spriteSlot, "duplicate",
                    tm - detectStart, frax, fray, pfrx[*nspr], pfry[*nspr],
                    pspx[*nspr], pspy[*nspr], pwid[*nspr], phei[*nspr]);
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
  }
  DebugLogSpriteCheckResult(quelleframe, *nspr);
  if (*nspr > 0) return true;
  return false;
}

void Colorize_Framev1(uint8_t* frame, uint32_t IDfound) {
  uint16_t tj, ti;
  // Generate the colorized version of a frame once identified in the crom
  // frames
  const bool frameHasDynamic = IDfound < g_serumData.frameHasDynamic.size() &&
                               g_serumData.frameHasDynamic[IDfound] > 0;
  const uint8_t* frameDyna =
      frameHasDynamic ? g_serumData.dynamasks[IDfound] : nullptr;
  const uint8_t* frameDynaActive =
      frameHasDynamic ? g_serumData.dynamasks_active[IDfound] : nullptr;
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
        if (!frameHasDynamic || frameDynaActive[tk] == 0)
          mySerum.frame[tk] = g_serumData.cframes[IDfound][tk];
        else {
          const uint8_t dynacouche = frameDyna[tk];
          mySerum.frame[tk] =
              g_serumData.dyna4cols[IDfound][dynacouche * g_serumData.nocolors +
                                             frame[tk]];
        }
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
  // Fast path: precomputed O(1) lookup built at load time.
  if (g_serumData.TryGetColorRotation(IDfound, col, isextra, *norot,
                                      *posinrot)) {
    return true;
  }
  *norot = 0xffff;
  return false;
}

void CheckDynaShadow(uint16_t* pfr, const uint8_t* shadowDirByLayer,
                     const uint16_t* shadowColorByLayer, uint8_t dynacouche,
                     uint8_t* isdynapix, uint16_t fx, uint16_t fy, uint32_t fw,
                     uint32_t fh) {
  if (!shadowDirByLayer || !shadowColorByLayer) return;
  const uint8_t dsdir = shadowDirByLayer[dynacouche];
  if (dsdir == 0) return;
  const uint16_t tcol = shadowColorByLayer[dynacouche];

  static const int8_t kNeighborDx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
  static const int8_t kNeighborDy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
  for (uint8_t bit = 0; bit < 8; ++bit) {
    if ((dsdir & (1u << bit)) == 0) continue;
    const int32_t nx = (int32_t)fx + kNeighborDx[bit];
    const int32_t ny = (int32_t)fy + kNeighborDy[bit];
    if (nx < 0 || ny < 0 || nx >= (int32_t)fw || ny >= (int32_t)fh) continue;
    const uint32_t neighborIndex = (uint32_t)ny * fw + (uint32_t)nx;
    if (isdynapix[neighborIndex] != 0) continue;
    isdynapix[neighborIndex] = 1;
    pfr[neighborIndex] = tcol;
  }
}

void Colorize_Framev2(uint8_t* frame, uint32_t IDfound,
                      bool applySceneBackground = false,
                      bool blackOutStaticContent = false,
                      bool suppressFrameBackgroundImage = false) {
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
    const uint16_t backgroundId = g_serumData.backgroundIDs[IDfound][0];
    const bool hasBackground = backgroundId < g_serumData.nbackgrounds;
    const uint8_t* frameBackgroundMask = g_serumData.backgroundmask[IDfound];
    const uint16_t* frameBackground =
        hasBackground ? g_serumData.backgroundframes_v2[backgroundId] : nullptr;
    const uint16_t* frameColors = g_serumData.cframes_v2[IDfound];
    const bool frameHasDynamic = IDfound < g_serumData.frameHasDynamic.size() &&
                                 g_serumData.frameHasDynamic[IDfound] > 0;
    const uint8_t* frameDyna =
        frameHasDynamic ? g_serumData.dynamasks[IDfound] : nullptr;
    const uint8_t* frameDynaActive =
        frameHasDynamic ? g_serumData.dynamasks_active[IDfound] : nullptr;
    const uint16_t* frameDynaColors =
        frameHasDynamic ? g_serumData.dyna4cols_v2[IDfound] : nullptr;
    const uint8_t* frameShadowDir =
        frameHasDynamic ? g_serumData.dynashadowsdir[IDfound] : nullptr;
    const uint16_t* frameShadowColor =
        frameHasDynamic ? g_serumData.dynashadowscol[IDfound] : nullptr;
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
    DebugLogColorizeFrameV2Assets(
        IDfound, g_debugCurrentInputCrc, false, g_serumData.fwidth,
        g_serumData.fheight, frameColors, frameBackgroundMask, frameBackground,
        frameHasDynamic, frameDyna, frameDynaActive, frameDynaColors, prt,
        backgroundId);
    if (applySceneBackground)
      memcpy(sceneBackgroundFrame, pSceneBackgroundFrame,
             g_serumData.fwidth * g_serumData.fheight * sizeof(uint16_t));
    memset(isdynapix, 0, g_serumData.fheight * g_serumData.fwidth);
    for (tj = 0; tj < g_serumData.fheight; tj++) {
      for (ti = 0; ti < g_serumData.fwidth; ti++) {
        uint16_t tk = tj * g_serumData.fwidth + ti;
        if (hasBackground && (frame[tk] == 0) &&
            (frameBackgroundMask[tk] > 0)) {
          if (isdynapix[tk] == 0) {
            if (applySceneBackground) {
              pfr[tk] = sceneBackgroundFrame[tk];
            } else if (!suppressFrameBackgroundImage) {
              pfr[tk] = frameBackground[tk];
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
          if (!frameHasDynamic || frameDynaActive[tk] == 0) {
            if (isdynapix[tk] == 0) {
              if (blackOutStaticContent && hasBackground && (frame[tk] > 0) &&
                  (frameBackgroundMask[tk] > 0)) {
                pfr[tk] = sceneBackgroundFrame[tk];
              } else {
                pfr[tk] = frameColors[tk];
                if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                    &prot[tk * 2 + 1], false))
                  pfr[tk] =
                      prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                          (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                              prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
              }
            }
          } else {
            const uint8_t dynacouche = frameDyna[tk];
            if (frame[tk] > 0) {
              CheckDynaShadow(pfr, frameShadowDir, frameShadowColor, dynacouche,
                              isdynapix, ti, tj, g_serumData.fwidth,
                              g_serumData.fheight);
              isdynapix[tk] = 1;
              pfr[tk] = frameDynaColors[dynacouche * g_serumData.nocolors +
                                        frame[tk]];
            } else if (isdynapix[tk] == 0)
              pfr[tk] = frameDynaColors[dynacouche * g_serumData.nocolors +
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
    const uint16_t backgroundId = g_serumData.backgroundIDs[IDfound][0];
    const bool hasBackground = backgroundId < g_serumData.nbackgrounds;
    const uint8_t* frameBackgroundMaskExtra =
        g_serumData.backgroundmask_extra[IDfound];
    const uint16_t* frameBackgroundExtra =
        hasBackground ? g_serumData.backgroundframes_v2_extra[backgroundId]
                      : nullptr;
    const uint16_t* frameColorsExtra = g_serumData.cframes_v2_extra[IDfound];
    const bool frameHasDynamicExtra =
        IDfound < g_serumData.frameHasDynamicExtra.size() &&
        g_serumData.frameHasDynamicExtra[IDfound] > 0;
    const uint8_t* frameDynaExtra =
        frameHasDynamicExtra ? g_serumData.dynamasks_extra[IDfound] : nullptr;
    const uint8_t* frameDynaExtraActive =
        frameHasDynamicExtra ? g_serumData.dynamasks_extra_active[IDfound]
                             : nullptr;
    const uint16_t* frameDynaColorsExtra =
        frameHasDynamicExtra ? g_serumData.dyna4cols_v2_extra[IDfound]
                             : nullptr;
    const uint8_t* frameShadowDirExtra =
        frameHasDynamicExtra ? g_serumData.dynashadowsdir_extra[IDfound]
                             : nullptr;
    const uint16_t* frameShadowColorExtra =
        frameHasDynamicExtra ? g_serumData.dynashadowscol_extra[IDfound]
                             : nullptr;
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
    DebugLogColorizeFrameV2Assets(
        IDfound, g_debugCurrentInputCrc, true, g_serumData.fwidth_extra,
        g_serumData.fheight_extra, frameColorsExtra, frameBackgroundMaskExtra,
        frameBackgroundExtra, frameHasDynamicExtra, frameDynaExtra,
        frameDynaExtraActive, frameDynaColorsExtra, prt, backgroundId);
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

        if (hasBackground && (frame[tl] == 0) &&
            (frameBackgroundMaskExtra[tk] > 0)) {
          if (isdynapix[tk] == 0) {
            if (applySceneBackground) {
              pfr[tk] = sceneBackgroundFrame[tk];
            } else if (!suppressFrameBackgroundImage) {
              pfr[tk] = frameBackgroundExtra[tk];
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
          if (!frameHasDynamicExtra || frameDynaExtraActive[tk] == 0) {
            if (isdynapix[tk] == 0) {
              if (blackOutStaticContent && hasBackground && (frame[tl] > 0) &&
                  (frameBackgroundMaskExtra[tk] > 0)) {
                pfr[tk] = sceneBackgroundFrame[tk];
              } else {
                pfr[tk] = frameColorsExtra[tk];
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
            const uint8_t dynacouche = frameDynaExtra[tk];
            if (frame[tl] > 0) {
              CheckDynaShadow(pfr, frameShadowDirExtra, frameShadowColorExtra,
                              dynacouche, isdynapix, ti, tj,
                              g_serumData.fwidth_extra,
                              g_serumData.fheight_extra);
              isdynapix[tk] = 1;
              pfr[tk] = frameDynaColorsExtra[dynacouche * g_serumData.nocolors +
                                             frame[tl]];
            } else if (isdynapix[tk] == 0)
              pfr[tk] = frameDynaColorsExtra[dynacouche * g_serumData.nocolors +
                                             frame[tl]];
            prot[tk * 2] = prot[tk * 2 + 1] = 0xffff;
          }
        }
      }
    }
  }
}

void Colorize_Spritev1(uint8_t nosprite, uint16_t frx, uint16_t fry,
                       uint16_t spx, uint16_t spy, uint16_t wid, uint16_t hei) {
  if (!g_serumData.spritedescriptionso_opaque.hasData(nosprite)) return;
  const uint8_t* spriteOpaque =
      g_serumData.spritedescriptionso_opaque[nosprite];
  for (uint16_t tj = 0; tj < hei; tj++) {
    for (uint16_t ti = 0; ti < wid; ti++) {
      if (spriteOpaque[(tj + spy) * MAX_SPRITE_SIZE + ti + spx] > 0) {
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
  const bool traceSprite = DebugSpriteVerboseEnabled() &&
                           DebugTraceMatches(g_debugCurrentInputCrc, IDfound);
  const bool hasOpaque = g_serumData.spriteoriginal_opaque.hasData(nosprite);
  const bool hasDynaActive =
      g_serumData.dynaspritemasks_active.hasData(nosprite);
  const bool hasDyna = g_serumData.dynaspritemasks.hasData(nosprite);
  const bool hasColor = g_serumData.spritecolored.hasData(nosprite);
  const bool hasColorExtra = g_serumData.spritecolored_extra.hasData(nosprite);
  if (!hasOpaque) {
    if (traceSprite) {
      Log("Serum debug sprite render skip: frameId=%u inputCrc=%u spriteId=%u "
          "reason=missing-base-opaque-sidecar",
          IDfound, g_debugCurrentInputCrc, nosprite);
    }
    return;
  }
  const uint8_t* spriteOpaque = g_serumData.spriteoriginal_opaque[nosprite];
  const uint8_t* spriteDyna =
      hasDyna ? g_serumData.dynaspritemasks[nosprite] : nullptr;
  const uint8_t* spriteDynaActive =
      hasDynaActive ? g_serumData.dynaspritemasks_active[nosprite] : nullptr;
  if (hasDyna != hasDynaActive || (hasDyna && spriteDyna == nullptr) ||
      (hasDynaActive && spriteDynaActive == nullptr)) {
    if (traceSprite) {
      Log("Serum debug sprite render skip: frameId=%u inputCrc=%u spriteId=%u "
          "reason=inconsistent-base-dynamic-sidecars hasDyna=%s "
          "hasDynaActive=%s ptrDyna=%s ptrDynaActive=%s",
          IDfound, g_debugCurrentInputCrc, nosprite, hasDyna ? "true" : "false",
          hasDynaActive ? "true" : "false", spriteDyna ? "true" : "false",
          spriteDynaActive ? "true" : "false");
    }
    return;
  }
  if (traceSprite) {
    Log("Serum debug sprite render source: frameId=%u inputCrc=%u spriteId=%u "
        "frame=(%u,%u) sprite=(%u,%u) size=%ux%u hasOpaque=%s hasColor=%s "
        "hasDyna=%s hasDynaActive=%s hasExtraColor=%s",
        IDfound, g_debugCurrentInputCrc, nosprite, frx, fry, spx, spy, wid, hei,
        hasOpaque ? "true" : "false", hasColor ? "true" : "false",
        hasDyna ? "true" : "false", hasDynaActive ? "true" : "false",
        hasColorExtra ? "true" : "false");
  }
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
        if (spriteOpaque[tl] > 0) {
          if (!hasColor) {
            if (traceSprite) {
              Log("Serum debug sprite render skip: frameId=%u inputCrc=%u "
                  "spriteId=%u reason=missing-base-color",
                  IDfound, g_debugCurrentInputCrc, nosprite);
            }
            return;
          }
          if (!hasDynaActive || spriteDynaActive[tl] == 0) {
            pfr[tk] = g_serumData.spritecolored[nosprite][tl];
            if (ColorInRotation(IDfound, pfr[tk], &prot[tk * 2],
                                &prot[tk * 2 + 1], false))
              pfr[tk] = prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION + 2 +
                            (prot[tk * 2 + 1] + cshft[prot[tk * 2]]) %
                                prt[prot[tk * 2] * MAX_LENGTH_COLOR_ROTATION]];
          } else {
            const uint8_t dynacouche = spriteDyna[tl];
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
    if (traceSprite) {
      Log("Serum debug sprite render result: frameId=%u inputCrc=%u "
          "spriteId=%u plane=base rendered=true",
          IDfound, g_debugCurrentInputCrc, nosprite);
    }
  }
  if (((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) &&
       g_serumData.fheight_extra == 32) ||
      ((mySerum.flags & FLAG_RETURNED_64P_FRAME_OK) &&
       g_serumData.fheight_extra == 64)) {
    const bool hasExtraOpaque =
        g_serumData.spritemask_extra_opaque.hasData(nosprite);
    const bool hasExtraDynaActive =
        g_serumData.dynaspritemasks_extra_active.hasData(nosprite);
    const bool hasExtraDyna =
        g_serumData.dynaspritemasks_extra.hasData(nosprite);
    if (!hasExtraOpaque) {
      if (traceSprite) {
        Log("Serum debug sprite render skip: frameId=%u inputCrc=%u "
            "spriteId=%u reason=missing-extra-opaque-sidecar",
            IDfound, g_debugCurrentInputCrc, nosprite);
      }
      return;
    }
    const uint8_t* spriteExtraOpaque =
        g_serumData.spritemask_extra_opaque[nosprite];
    const uint8_t* spriteExtraDyna =
        hasExtraDyna ? g_serumData.dynaspritemasks_extra[nosprite] : nullptr;
    const uint8_t* spriteExtraDynaActive =
        hasExtraDynaActive ? g_serumData.dynaspritemasks_extra_active[nosprite]
                           : nullptr;
    if (hasExtraDyna != hasExtraDynaActive ||
        (hasExtraDyna && spriteExtraDyna == nullptr) ||
        (hasExtraDynaActive && spriteExtraDynaActive == nullptr)) {
      if (traceSprite) {
        Log("Serum debug sprite render skip: frameId=%u inputCrc=%u "
            "spriteId=%u reason=inconsistent-extra-dynamic-sidecars "
            "hasDyna=%s hasDynaActive=%s ptrDyna=%s ptrDynaActive=%s",
            IDfound, g_debugCurrentInputCrc, nosprite,
            hasExtraDyna ? "true" : "false",
            hasExtraDynaActive ? "true" : "false",
            spriteExtraDyna ? "true" : "false",
            spriteExtraDynaActive ? "true" : "false");
      }
      return;
    }
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
        const uint32_t spritePixel = (tj + tspy) * MAX_SPRITE_WIDTH + ti + tspx;
        if (spriteExtraOpaque[spritePixel] > 0) {
          if (!hasColorExtra) {
            if (traceSprite) {
              Log("Serum debug sprite render skip: frameId=%u inputCrc=%u "
                  "spriteId=%u reason=missing-extra-color",
                  IDfound, g_debugCurrentInputCrc, nosprite);
            }
            return;
          }
          if (!hasExtraDynaActive || spriteExtraDynaActive[spritePixel] == 0) {
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
            const uint8_t dynacouche = spriteExtraDyna[spritePixel];
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
    if (traceSprite) {
      Log("Serum debug sprite render result: frameId=%u inputCrc=%u "
          "spriteId=%u plane=extra rendered=true",
          IDfound, g_debugCurrentInputCrc, nosprite);
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
  const uint32_t inputCrc =
      (frame && g_serumData.fwidth > 0 && g_serumData.fheight > 0)
          ? crc32_fast(frame, g_serumData.fwidth * g_serumData.fheight)
          : 0;
  g_debugCurrentInputCrc = inputCrc;
  if (DebugTraceAllInputsEnabled()) {
    Log("Serum debug input: api=v1 inputCrc=%u", inputCrc);
  }
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
        IsFullBlackFrame(frame, g_serumData.fwidth * g_serumData.fheight)) {
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
        if (DebugTraceMatchesInputCrc(g_debugCurrentInputCrc)) {
          Log("Serum debug identify same-frame: inputCrc=%u lastfound=%u "
              "sceneRequested=%s triggerId=%u",
              g_debugCurrentInputCrc, lastfound, "false",
              g_serumData.triggerIDs[lastfound][0]);
        }
        if (DebugTraceAllInputsEnabled()) {
          Log("Serum debug input result: api=v1 inputCrc=%u result=same-frame "
              "lastfound=%u",
              g_debugCurrentInputCrc, lastfound);
        }
        if (keepTriggersInternal ||
            mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
          mySerum.triggerID = 0xffffffff;
        return IDENTIFY_SAME_FRAME;
      }

      mySerum.frameID = frameID;
      mySerum.rotationtimer = 0;
      if (DebugTraceAllInputsEnabled()) {
        Log("Serum debug input result: api=v1 inputCrc=%u result=frame "
            "frameId=%u",
            g_debugCurrentInputCrc, frameID);
      }

      uint8_t nosprite[MAX_SPRITES_PER_FRAME], nspr;
      uint16_t frx[MAX_SPRITES_PER_FRAME], fry[MAX_SPRITES_PER_FRAME],
          spx[MAX_SPRITES_PER_FRAME], spy[MAX_SPRITES_PER_FRAME],
          wid[MAX_SPRITES_PER_FRAME], hei[MAX_SPRITES_PER_FRAME];
      memset(nosprite, 255, MAX_SPRITES_PER_FRAME);

      bool isspr = Check_Spritesv1(frame, (uint32_t)lastfound, nosprite, &nspr,
                                   frx, fry, spx, spy, wid, hei);
      if (((frameID < MAX_NUMBER_FRAMES) || isspr) &&
          FrameHasRenderableContent(lastfound)) {
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
  if (g_serumData.nocolors == 0 || g_serumData.nocolors > 16) {
    monochromePaletteV2Length = 0;
    return false;
  }
  const uint16_t* dyna = (g_serumData.fheight == 32)
                             ? g_serumData.dyna4cols_v2[frameId]
                             : g_serumData.dyna4cols_v2_extra[frameId];
  if (!dyna) {
    monochromePaletteV2Length = 0;
    return false;
  }
  const uint8_t ncolors = (uint8_t)g_serumData.nocolors;
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

static uint32_t Serum_ColorizeWithMetadatav2Internal(uint8_t* frame,
                                                     bool sceneFrameRequested,
                                                     uint32_t knownFrameId) {
  // return IDENTIFY_NO_FRAME if no new frame detected
  // return 0 if new frame with no rotation detected
  // return > 0 if new frame with rotations detected, the value is the delay
  // before the first rotation in ms
  mySerum.triggerID = 0xffffffff;
  mySerum.frameID = IDENTIFY_NO_FRAME;
  g_debugCurrentInputCrc = 0;
  bool backgroundScenePrimedThisCall = false;
  if (g_profileDynamicHotPaths && !sceneFrameRequested &&
      knownFrameId >= g_serumData.nframes) {
    ++g_profileIncomingFrameCalls;
  }

  // Identify frame unless caller already resolved a concrete frame ID.
  uint32_t frameID = IDENTIFY_NO_FRAME;
  const bool fastRejectNonInterruptableScene =
      !sceneFrameRequested && knownFrameId >= g_serumData.nframes &&
      !monochromeMode && g_serumData.sceneGenerator->isActive() &&
      (sceneCurrentFrame < sceneFrameCount || sceneEndHoldUntilMs > 0) &&
      !sceneInterruptable;
  if (fastRejectNonInterruptableScene) {
    frameID = IdentifyCriticalTriggerFrame(frame);
    if (frameID == IDENTIFY_NO_FRAME) {
      if (g_profileDynamicHotPaths && !sceneFrameRequested) {
        ++g_profileNoFrameReturns;
      }
      MaybeLogDynamicHotPathProfileWindow(sceneFrameRequested);
      return IDENTIFY_NO_FRAME;
    }
  }
  if (knownFrameId < g_serumData.nframes) {
    frameID = knownFrameId;
    lastfound = knownFrameId;
    if (sceneFrameRequested) {
      lastfound_scene = knownFrameId;
      first_match_scene = false;
      lastframe_full_crc_scene = 0;
    } else {
      lastfound_normal = knownFrameId;
      first_match_normal = false;
      lastframe_full_crc_normal = 0;
    }
  } else if (frameID != IDENTIFY_NO_FRAME) {
    lastfound = frameID;
    lastfound_normal = frameID;
    first_match_normal = false;
    lastframe_full_crc_normal = 0;
  } else {
    frameID = Identify_Frame(frame, sceneFrameRequested);
  }
  if (frame && g_serumData.fwidth > 0 && g_serumData.fheight > 0) {
    g_debugCurrentInputCrc =
        crc32_fast(frame, g_serumData.fwidth * g_serumData.fheight);
  }
  if (DebugTraceAllInputsEnabled()) {
    Log("Serum debug input: api=v2 inputCrc=%u sceneRequested=%s "
        "knownFrameId=%u",
        g_debugCurrentInputCrc, sceneFrameRequested ? "true" : "false",
        knownFrameId);
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
        IsFullBlackFrame(frame, g_serumData.fwidth * g_serumData.fheight)) {
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
        if (DebugTraceMatches(g_debugCurrentInputCrc, lastfound)) {
          Log("Serum debug v2 gate: inputCrc=%u frameId=%u "
              "gate=scene-noninterruptable currentFrame=%u sceneFrameCount=%u "
              "endHoldUntil=%u bypass=%s",
              g_debugCurrentInputCrc, lastfound, sceneCurrentFrame,
              sceneFrameCount, sceneEndHoldUntilMs,
              g_debugBypassSceneGate ? "true" : "false");
        }
        if (!g_debugBypassSceneGate &&
            !IsCriticalMonochromeTriggerFrame(lastfound)) {
          if (keepTriggersInternal ||
              mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
            mySerum.triggerID = 0xffffffff;
          // Scene is active and not interruptable
          if (g_profileDynamicHotPaths && !sceneFrameRequested) {
            ++g_profileNoFrameReturns;
          }
          MaybeLogDynamicHotPathProfileWindow(sceneFrameRequested);
          return IDENTIFY_NO_FRAME;
        }
        if (IsCriticalMonochromeTriggerFrame(lastfound)) {
          DebugLogSceneEvent(
              "stop-critical-monochrome-trigger",
              static_cast<uint16_t>(lastTriggerID), sceneCurrentFrame,
              sceneFrameCount, sceneDurationPerFrame, sceneOptionFlags,
              sceneInterruptable, sceneStartImmediately, sceneRepeatCount);
          sceneFrameCount = 0;
          sceneIsLastBackgroundFrame = false;
          sceneEndHoldUntilMs = 0;
          sceneEndHoldDurationMs = 0;
          sceneNextFrameAtMs = 0;
          mySerum.rotationtimer = 0;
          ForceNormalFrameRefreshAfterSceneEnd();
        }
      }

      // frame identified
      lastframe_found = now;
      if (maxFramesToSkip) {
        framesSkippedCounter = 0;
      }

      if (frameID == IDENTIFY_SAME_FRAME) {
        if (DebugTraceMatchesInputCrc(g_debugCurrentInputCrc)) {
          Log("Serum debug identify same-frame: inputCrc=%u lastfound=%u "
              "sceneRequested=%s triggerId=%u",
              g_debugCurrentInputCrc, lastfound,
              sceneFrameRequested ? "true" : "false",
              g_serumData.triggerIDs[lastfound][0]);
        }
        if (DebugTraceAllInputsEnabled()) {
          Log("Serum debug input result: api=v2 inputCrc=%u result=same-frame "
              "lastfound=%u sceneRequested=%s",
              g_debugCurrentInputCrc, lastfound,
              sceneFrameRequested ? "true" : "false");
        }
        if (keepTriggersInternal ||
            mySerum.triggerID >= PUP_TRIGGER_MAX_THRESHOLD)
          mySerum.triggerID = 0xffffffff;
        if (g_profileDynamicHotPaths && !sceneFrameRequested) {
          ++g_profileSameFrameReturns;
        }
        MaybeLogDynamicHotPathProfileWindow(sceneFrameRequested);
        return IDENTIFY_SAME_FRAME;
      }

      mySerum.frameID = frameID;
      if (DebugTraceAllInputsEnabled()) {
        Log("Serum debug input result: api=v2 inputCrc=%u result=frame "
            "frameId=%u sceneRequested=%s",
            g_debugCurrentInputCrc, frameID,
            sceneFrameRequested ? "true" : "false");
      }
      if (DebugIdentifyVerboseEnabled() &&
          DebugTraceMatches(g_debugCurrentInputCrc, frameID)) {
        Log("Serum debug identify result: inputCrc=%u frameId=%u "
            "sceneRequested=%s triggerId=%u",
            g_debugCurrentInputCrc, frameID,
            sceneFrameRequested ? "true" : "false",
            g_serumData.triggerIDs[lastfound][0]);
      } else if (DebugTraceAllInputsEnabled() && !sceneFrameRequested) {
        Log("Serum debug trigger candidate: inputCrc=%u frameId=%u "
            "triggerId=%u "
            "lastTriggerId=%u",
            g_debugCurrentInputCrc, frameID,
            g_serumData.triggerIDs[lastfound][0], lastTriggerID);
      }
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
          if (g_profileDynamicHotPaths) {
            ++g_profileSameFrameReturns;
          }
          MaybeLogDynamicHotPathProfileWindow(sceneFrameRequested);
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
          if (sceneFrameCount > 0 || sceneEndHoldUntilMs > 0) {
            DebugLogSceneEvent(
                "stop-normal-frame", static_cast<uint16_t>(lastTriggerID),
                sceneCurrentFrame, sceneFrameCount, sceneDurationPerFrame,
                sceneOptionFlags, sceneInterruptable, sceneStartImmediately,
                sceneRepeatCount);
          }
          sceneFrameCount = 0;
          sceneIsLastBackgroundFrame = false;
          sceneEndHoldUntilMs = 0;
          sceneEndHoldDurationMs = 0;
          sceneNextFrameAtMs = 0;
          mySerum.rotationtimer = 0;

          // lastfound is set by Identify_Frame, check if we have a new PUP
          // trigger
          if (!monochromeMode &&
              (g_serumData.triggerIDs[lastfound][0] != lastTriggerID ||
               lasttriggerTimestamp < (now - PUP_TRIGGER_REPEAT_TIMEOUT))) {
            lastTriggerID = mySerum.triggerID =
                g_serumData.triggerIDs[lastfound][0];
            lasttriggerTimestamp = now;
            if (DebugTraceAllInputsEnabled()) {
              Log("Serum debug trigger commit: inputCrc=%u frameId=%u "
                  "triggerId=%u",
                  g_debugCurrentInputCrc, lastfound, lastTriggerID);
            }

            if (DebugTraceAllInputsEnabled()) {
              Log("Serum debug trigger scene-gate: triggerId=%u "
                  "sceneGeneratorActive=%s triggerValid=%s",
                  lastTriggerID,
                  (g_serumData.sceneGenerator &&
                   g_serumData.sceneGenerator->isActive())
                      ? "true"
                      : "false",
                  lastTriggerID < 0xffffffff ? "true" : "false");
            }

            if (g_serumData.sceneGenerator->isActive() &&
                lastTriggerID < 0xffffffff) {
              const bool hasSceneInfo =
                  g_serumData.sceneGenerator->getSceneInfo(
                      lastTriggerID, sceneFrameCount, sceneDurationPerFrame,
                      sceneInterruptable, sceneStartImmediately,
                      sceneRepeatCount, sceneOptionFlags);
              if (DebugTraceAllInputsEnabled()) {
                Log("Serum debug trigger scene-info: triggerId=%u found=%s "
                    "frameCount=%u duration=%u interruptable=%s "
                    "startImmediately=%s repeat=%u options=%u",
                    lastTriggerID, hasSceneInfo ? "true" : "false",
                    sceneFrameCount, sceneDurationPerFrame,
                    sceneInterruptable ? "true" : "false",
                    sceneStartImmediately ? "true" : "false", sceneRepeatCount,
                    sceneOptionFlags);
              }
              if (hasSceneInfo) {
                DebugLogSceneEvent("trigger",
                                   static_cast<uint16_t>(lastTriggerID), 0,
                                   sceneFrameCount, sceneDurationPerFrame,
                                   sceneOptionFlags, sceneInterruptable,
                                   sceneStartImmediately, sceneRepeatCount);
                const bool sceneIsBackground =
                    (sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
                    FLAG_SCENE_AS_BACKGROUND;
                if (sceneIsBackground) {
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
                  DebugLogSceneEvent("start-immediate",
                                     static_cast<uint16_t>(lastTriggerID), 0,
                                     sceneFrameCount, sceneDurationPerFrame,
                                     sceneOptionFlags, sceneInterruptable,
                                     sceneStartImmediately, sceneRepeatCount);
                  uint32_t sceneRotationResult = Serum_RenderScene();
                  if (sceneRotationResult & FLAG_RETURNED_V2_SCENE) {
                    MaybeLogDynamicHotPathProfileWindow(sceneFrameRequested);
                    return sceneRotationResult;
                  }
                } else if (sceneIsBackground) {
                  DebugLogSceneEvent("prime-background",
                                     static_cast<uint16_t>(lastTriggerID), 0,
                                     sceneFrameCount, sceneDurationPerFrame,
                                     sceneOptionFlags, sceneInterruptable,
                                     sceneStartImmediately, sceneRepeatCount);
                  uint32_t sceneRotationResult = Serum_RenderScene();
                  if (sceneRotationResult & FLAG_RETURNED_V2_SCENE) {
                    backgroundScenePrimedThisCall = true;
                  }
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
      bool suppressPlaceholderBackground = isBackgroundScene &&
                                           !sceneFrameRequested &&
                                           !backgroundScenePrimedThisCall;
      bool isBackgroundSceneRequested =
          isBackgroundScene &&
          (sceneFrameRequested || backgroundScenePrimedThisCall);
      uint8_t nosprite[MAX_SPRITES_PER_FRAME], nspr;
      uint16_t frx[MAX_SPRITES_PER_FRAME], fry[MAX_SPRITES_PER_FRAME],
          spx[MAX_SPRITES_PER_FRAME], spy[MAX_SPRITES_PER_FRAME],
          wid[MAX_SPRITES_PER_FRAME], hei[MAX_SPRITES_PER_FRAME];
      memset(nosprite, 255, MAX_SPRITES_PER_FRAME);

      bool isspr =
          (sceneFrameRequested && !isBackgroundSceneRequested)
              ? false
              : Check_Spritesv2(
                    isBackgroundSceneRequested ? lastFrame : frame,
                    isBackgroundSceneRequested ? lastFrameId : lastfound,
                    nosprite, &nspr, frx, fry, spx, spy, wid, hei);
      if (((frameID < MAX_NUMBER_FRAMES) || isspr) &&
          FrameHasRenderableContent(lastfound)) {
        const bool profileNow = g_profileDynamicHotPaths;
        std::chrono::steady_clock::time_point profStart;
        if (profileNow) {
          profStart = std::chrono::steady_clock::now();
        }
        if (!sceneIsLastBackgroundFrame && !backgroundScenePrimedThisCall) {
          Colorize_Framev2(frame, lastfound, false, false,
                           suppressPlaceholderBackground);
          DebugHashCurrentOutputFrame(lastfound, false);
        }
        if ((isBackgroundSceneRequested) || sceneIsLastBackgroundFrame) {
          Colorize_Framev2(
              lastFrame, lastFrameId, true,
              (sceneOptionFlags & FLAG_SCENE_ONLY_DYNAMIC_CONTENT) ==
                  FLAG_SCENE_ONLY_DYNAMIC_CONTENT);
          DebugHashCurrentOutputFrame(lastFrameId, false);
        }
        if (profileNow) {
          g_profileColorizeFrameV2Ns +=
              (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - profStart)
                  .count();
        }
        if (isspr) {
          std::chrono::steady_clock::time_point spriteStart;
          if (profileNow) {
            spriteStart = std::chrono::steady_clock::now();
          }
          uint8_t ti = 0;
          while (ti < nspr) {
            Colorize_Spritev2(
                isBackgroundSceneRequested ? lastFrame : frame, nosprite[ti],
                frx[ti], fry[ti], spx[ti], spy[ti], wid[ti], hei[ti],
                isBackgroundSceneRequested ? lastFrameId : lastfound);
            ti++;
          }
          if (g_debugStageHashes &&
              DebugTraceMatches(
                  g_debugCurrentInputCrc,
                  isBackgroundSceneRequested ? lastFrameId : lastfound)) {
            uint64_t spriteHash = DebugHashBytesFNV1a64(
                ((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) &&
                 mySerum.frame32)
                    ? static_cast<const void*>(mySerum.frame32)
                    : static_cast<const void*>(mySerum.frame64),
                ((mySerum.flags & FLAG_RETURNED_32P_FRAME_OK) &&
                 mySerum.frame32)
                    ? static_cast<size_t>(mySerum.width32) * 32 *
                          sizeof(uint16_t)
                    : static_cast<size_t>(mySerum.width64) * 64 *
                          sizeof(uint16_t));
            Log("Serum debug stage hash: frameId=%u inputCrc=%u "
                "stage=post-sprites "
                "hash=%llu sprites=%u",
                isBackgroundSceneRequested ? lastFrameId : lastfound,
                g_debugCurrentInputCrc,
                static_cast<unsigned long long>(spriteHash), nspr);
          }
          if (profileNow) {
            g_profileColorizeSpriteV2Ns +=
                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - spriteStart)
                    .count();
          }
        }
        FinishProfileRenderedFrameOperationMaybe();

        bool allowParallelRotations =
            (sceneFrameCount == 0) ||
            ((sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
             FLAG_SCENE_AS_BACKGROUND);
        if (!sceneFrameRequested && allowParallelRotations) {
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

        MaybeLogDynamicHotPathProfileWindow(sceneFrameRequested);
        return (uint32_t)mySerum.rotationtimer |
               (rotationIsScene ? FLAG_RETURNED_V2_SCENE : 0);
      }
    }
  }

  if (DebugTraceAllInputsEnabled()) {
    Log("Serum debug input result: api=v2 inputCrc=%u result=no-frame "
        "sceneRequested=%s",
        g_debugCurrentInputCrc, sceneFrameRequested ? "true" : "false");
  }

  mySerum.triggerID = 0xffffffff;

  if (monochromeMode || monochromePaletteMode ||
      (ignoreUnknownFramesTimeout &&
       (now - lastframe_found) >= ignoreUnknownFramesTimeout) ||
      (maxFramesToSkip && (frameID == IDENTIFY_NO_FRAME) &&
       (++framesSkippedCounter >= maxFramesToSkip))) {
    // apply monochrome frame colors
    for (uint16_t y = 0; y < g_serumData.fheight; y++) {
      for (uint16_t x = 0; x < g_serumData.fwidth; x++) {
        uint8_t src = frame[y * g_serumData.fwidth + x];
        if (monochromePaletteMode && monochromePaletteV2Length > 0 &&
            src < monochromePaletteV2Length) {
          mySerum.frame32[y * g_serumData.fwidth + x] =
              monochromePaletteV2[src];
        } else if (g_serumData.nocolors < 16) {
          mySerum.frame32[y * g_serumData.fwidth + x] = greyscale_4[src];
        } else {
          mySerum.frame32[y * g_serumData.fwidth + x] = greyscale_16[src];
        }
      }
    }

    mySerum.flags = FLAG_RETURNED_32P_FRAME_OK;
    mySerum.width32 = g_serumData.fwidth;
    mySerum.width64 = 0;
    mySerum.frameID = 0xfffffffd;  // monochrome frame ID
    if (DebugTraceAllInputsEnabled()) {
      Log("Serum debug input result: api=v2 inputCrc=%u result=monochrome",
          g_debugCurrentInputCrc);
    }

    // disable render features like rotations
    for (uint8_t ti = 0; ti < MAX_COLOR_ROTATION_V2; ti++) {
      colorrotnexttime32[ti] = 0;
      colorrotnexttime64[ti] = 0;
    }
    mySerum.rotationtimer = 0;
    FinishProfileRenderedFrameOperationMaybe();

    return 0;  // "colorized" frame with no rotations
  }

  if (g_profileDynamicHotPaths && !sceneFrameRequested) {
    ++g_profileNoFrameReturns;
  }
  return IDENTIFY_NO_FRAME;  // no new frame, client has to update rotations!
}

SERUM_API uint32_t
Serum_ColorizeWithMetadatav2(uint8_t* frame, bool sceneFrameRequested = false) {
  BeginProfileFrameOperation();
  const uint32_t result = Serum_ColorizeWithMetadatav2Internal(
      frame, sceneFrameRequested, IDENTIFY_NO_FRAME);
  EndProfileFrameOperation();
  MaybeLogDynamicHotPathProfileWindow(sceneFrameRequested);
  return result;
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
  BeginProfileFrameOperation();
  auto finishSceneProfile = [&](uint32_t result) -> uint32_t {
    EndProfileFrameOperation();
    return result;
  };
  if (g_serumData.sceneGenerator->isActive() &&
      (sceneCurrentFrame < sceneFrameCount || sceneEndHoldUntilMs > 0)) {
    const uint32_t now = GetMonotonicTimeMs();
    if (sceneEndHoldUntilMs > 0) {
      if (now < sceneEndHoldUntilMs) {
        DebugLogSceneEvent(
            "end-hold", static_cast<uint16_t>(lastTriggerID), sceneCurrentFrame,
            sceneFrameCount, sceneDurationPerFrame, sceneOptionFlags,
            sceneInterruptable, sceneStartImmediately, sceneRepeatCount);
        mySerum.rotationtimer = sceneEndHoldUntilMs - now;
        return finishSceneProfile((mySerum.rotationtimer & 0xffff) |
                                  FLAG_RETURNED_V2_SCENE);
      }

      // End hold elapsed: finish scene now.
      sceneEndHoldUntilMs = 0;
      sceneNextFrameAtMs = 0;
      DebugLogSceneEvent(
          "end-hold-finished", static_cast<uint16_t>(lastTriggerID),
          sceneCurrentFrame, sceneFrameCount, sceneDurationPerFrame,
          sceneOptionFlags, sceneInterruptable, sceneStartImmediately,
          sceneRepeatCount);
      sceneFrameCount = 0;
      mySerum.rotationtimer = 0;
      ForceNormalFrameRefreshAfterSceneEnd();

      switch (sceneOptionFlags) {
        case FLAG_SCENE_BLACK_WHEN_FINISHED:
          if (mySerum.frame32) memset(mySerum.frame32, 0, 32 * mySerum.width32);
          if (mySerum.frame64) memset(mySerum.frame64, 0, 64 * mySerum.width64);
          FinishProfileRenderedFrameOperationMaybe();
          break;

        case FLAG_SCENE_SHOW_PREVIOUS_FRAME_WHEN_FINISHED:
          if (lastfound < MAX_NUMBER_FRAMES &&
              FrameHasRenderableContent(lastfound)) {
            Serum_ColorizeWithMetadatav2(lastFrame);
          } else {
            if (mySerum.frame32)
              memset(mySerum.frame32, 0, 32 * mySerum.width32);
            if (mySerum.frame64)
              memset(mySerum.frame64, 0, 64 * mySerum.width64);
            FinishProfileRenderedFrameOperationMaybe();
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

      return finishSceneProfile(FLAG_RETURNED_V2_SCENE);
    }

    bool renderedFromDirectTriplet = false;
    uint8_t currentGroup = 1;
    bool hasGroup = g_serumData.sceneGenerator->updateAndGetCurrentGroup(
        static_cast<uint16_t>(lastTriggerID), sceneCurrentFrame, -1,
        currentGroup);
    if (hasGroup && !g_serumData.sceneFrameIdByTriplet.empty()) {
      auto it = g_serumData.sceneFrameIdByTriplet.find(
          MakeSceneTripletKey(static_cast<uint16_t>(lastTriggerID),
                              currentGroup, sceneCurrentFrame));
      if (it != g_serumData.sceneFrameIdByTriplet.end() &&
          it->second < g_serumData.nframes) {
        if (sceneNextFrameAtMs > now) {
          const uint16_t waitMs =
              static_cast<uint16_t>(sceneNextFrameAtMs - now);
          DebugLogSceneEvent(
              "triplet-wait", static_cast<uint16_t>(lastTriggerID),
              sceneCurrentFrame, sceneFrameCount, sceneDurationPerFrame,
              sceneOptionFlags, sceneInterruptable, sceneStartImmediately,
              sceneRepeatCount, currentGroup, waitMs);
          mySerum.rotationtimer = waitMs;
          return finishSceneProfile(mySerum.rotationtimer |
                                    FLAG_RETURNED_V2_SCENE);
        }
        mySerum.rotationtimer = sceneDurationPerFrame;
        sceneNextFrameAtMs = now + sceneDurationPerFrame;
        Serum_ColorizeWithMetadatav2Internal(sceneFrame, true, it->second);
        renderedFromDirectTriplet = true;
      }
    }
    if (DebugSceneVerboseEnabled()) {
      Log("Serum debug scene path: sceneId=%u frameIndex=%u group=%u "
          "usedTriplet=%s tripletCount=%u",
          static_cast<uint16_t>(lastTriggerID), sceneCurrentFrame, currentGroup,
          renderedFromDirectTriplet ? "true" : "false",
          static_cast<uint32_t>(g_serumData.sceneFrameIdByTriplet.size()));
    }
    if (!renderedFromDirectTriplet) {
      uint16_t result = g_serumData.sceneGenerator->generateFrame(
          lastTriggerID, sceneCurrentFrame, sceneFrame,
          hasGroup ? currentGroup : -1);
      DebugLogSceneEvent("generate", static_cast<uint16_t>(lastTriggerID),
                         sceneCurrentFrame, sceneFrameCount,
                         sceneDurationPerFrame, sceneOptionFlags,
                         sceneInterruptable, sceneStartImmediately,
                         sceneRepeatCount, currentGroup, result);
      if (result > 0 && result < 0xffff) {
        // frame not ready yet, return the time to wait
        mySerum.rotationtimer = result;
        return finishSceneProfile(mySerum.rotationtimer |
                                  FLAG_RETURNED_V2_SCENE);
      }
      if (result != 0xffff) {
        DebugLogSceneEvent(
            "generate-error", static_cast<uint16_t>(lastTriggerID),
            sceneCurrentFrame, sceneFrameCount, sceneDurationPerFrame,
            sceneOptionFlags, sceneInterruptable, sceneStartImmediately,
            sceneRepeatCount, currentGroup, result);
        sceneFrameCount = 0;  // error generating scene frame, stop the scene
        mySerum.rotationtimer = 0;
        sceneNextFrameAtMs = 0;
        ForceNormalFrameRefreshAfterSceneEnd();
        return finishSceneProfile(
            (mySerum.rotationtimer & 0xffff) | FLAG_RETURNED_V2_ROTATED32 |
            FLAG_RETURNED_V2_ROTATED64 | FLAG_RETURNED_V2_SCENE);
      }
      mySerum.rotationtimer = sceneDurationPerFrame;
      sceneNextFrameAtMs = now + sceneDurationPerFrame;
      Serum_ColorizeWithMetadatav2(sceneFrame, true);
    } else {
      DebugLogSceneEvent("triplet-render", static_cast<uint16_t>(lastTriggerID),
                         sceneCurrentFrame, sceneFrameCount,
                         sceneDurationPerFrame, sceneOptionFlags,
                         sceneInterruptable, sceneStartImmediately,
                         sceneRepeatCount, currentGroup, 0xffff);
    }

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
      DebugLogSceneEvent("scene-finished", static_cast<uint16_t>(lastTriggerID),
                         sceneCurrentFrame, sceneFrameCount,
                         sceneDurationPerFrame, sceneOptionFlags,
                         sceneInterruptable, sceneStartImmediately,
                         sceneRepeatCount);
      if (sceneEndHoldDurationMs > 0) {
        sceneEndHoldUntilMs = now + sceneEndHoldDurationMs;
        mySerum.rotationtimer = sceneEndHoldDurationMs;
        return finishSceneProfile((mySerum.rotationtimer & 0xffff) |
                                  FLAG_RETURNED_V2_SCENE);
      }

      sceneFrameCount = 0;  // scene ended
      mySerum.rotationtimer = 0;
      sceneNextFrameAtMs = 0;
      ForceNormalFrameRefreshAfterSceneEnd();

      switch (sceneOptionFlags) {
        case FLAG_SCENE_BLACK_WHEN_FINISHED:
          if (mySerum.frame32) memset(mySerum.frame32, 0, 32 * mySerum.width32);
          if (mySerum.frame64) memset(mySerum.frame64, 0, 64 * mySerum.width64);
          break;

        case FLAG_SCENE_SHOW_PREVIOUS_FRAME_WHEN_FINISHED:
          if (lastfound < MAX_NUMBER_FRAMES &&
              FrameHasRenderableContent(lastfound)) {
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
    }

    return finishSceneProfile(
        (mySerum.rotationtimer & 0xffff) | FLAG_RETURNED_V2_ROTATED32 |
        FLAG_RETURNED_V2_ROTATED64 |
        FLAG_RETURNED_V2_SCENE);  // scene frame, so we consider both frames
                                  // changed
  }

  return finishSceneProfile(0);
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

SERUM_API bool Serum_GetRuntimeMetadata(Serum_Runtime_Metadata* metadata) {
  if (metadata == nullptr) {
    return false;
  }

  if (metadata->size != 0 && metadata->size < sizeof(Serum_Runtime_Metadata)) {
    return false;
  }

  memset(metadata, 0, sizeof(*metadata));
  metadata->size = sizeof(*metadata);
  metadata->serumVersion = mySerum.SerumVersion;
  metadata->frameID = mySerum.frameID;
  metadata->triggerID = mySerum.triggerID;
  metadata->rotationtimer = mySerum.rotationtimer;
  metadata->featureFlags = BuildRuntimeFeatureFlags(mySerum.frameID);
  return true;
}

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
  sceneNextFrameAtMs = 0;

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

  if (sceneStartImmediately || ((sceneOptionFlags & FLAG_SCENE_AS_BACKGROUND) ==
                                FLAG_SCENE_AS_BACKGROUND)) {
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
