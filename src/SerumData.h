#pragma once

#include <algorithm>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "SceneGenerator.h"
#include "serum-accel.h"
#include "serum.h"
#include "sparse-vector.h"

inline uint16_t ToLittleEndian16(uint16_t value) {
  uint16_t result;
  uint8_t *data = (uint8_t *)&result;
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
  return result;
}

inline uint16_t FromLittleEndian16(uint16_t value) {
  const uint8_t *data = (uint8_t *)&value;
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

inline uint32_t ToLittleEndian32(uint32_t value) {
  uint32_t result;
  uint8_t *data = (uint8_t *)&result;
  data[0] = value & 0xFF;
  data[1] = (value >> 8) & 0xFF;
  data[2] = (value >> 16) & 0xFF;
  data[3] = (value >> 24) & 0xFF;
  return result;
}

inline uint32_t FromLittleEndian32(uint32_t value) {
  const uint8_t *data = (uint8_t *)&value;
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

// Speaker lights of a colorization (the lights part of the cROM settings
// block). An action is what a frame, or the default, does to one channel.
enum : uint8_t {
  SERUM_LIGHT_UNCHANGED = 0,  // the channel keeps what it plays
  SERUM_LIGHT_EFFECT = 1,     // effect: an index into SerumData::lightEffects
  SERUM_LIGHT_COLOR = 2,      // color: a static color (0xWWRRGGBB)
  SERUM_LIGHT_OFF = 3,
};

struct SerumLightAction {
  uint8_t kind = SERUM_LIGHT_UNCHANGED;
  uint16_t effect = 0;
  uint32_t color = 0;

  template <class Archive>
  void serialize(Archive &ar) {
    ar(kind, effect, color);
  }
};

struct SerumLightFrame {
  uint32_t frameId = 0;
  SerumLightAction channels[SERUM_LIGHT_CHANNELS];

  template <class Archive>
  void serialize(Archive &ar) {
    ar(frameId);
    for (SerumLightAction &action : channels) ar(action);
  }
};

// The dynamic colour sets of a frame that colour only the original plane (bit n
// for set n): in layer mode, on a frame with content of its own on the extra
// plane, their zones are not part of the scaled layer, so the frame's extra
// plane shows there instead. For pixel art recoloured through a dynamic zone
// whose extra plane is an imported picture.
struct SerumOriginalOnlySets {
  uint32_t frameId = 0;
  uint32_t sets = 0;

  template <class Archive>
  void serialize(Archive &ar) {
    ar(frameId, sets);
  }
};

struct SerumLightEffectData {
  Serum_Light_Effect effect = {};

  template <class Archive>
  void serialize(Archive &ar) {
    ar(effect.mode, effect.options, effect.speed, effect.durationMs,
       effect.colors[0], effect.colors[1], effect.colors[2], effect.brightness);
  }
};

class SerumData {
 public:
  struct SpriteDetectMeta {
    uint32_t detectionWord = 0;
    uint16_t detectionWordPos = 0;
    uint16_t detectX = 0;
    uint16_t detectY = 0;
    uint16_t detectWidth = 0;
    uint16_t detectHeight = 0;

    template <class Archive>
    void serialize(Archive &ar) {
      ar(detectionWord, detectionWordPos, detectX, detectY, detectWidth,
         detectHeight);
    }
  };

  struct SceneSignatureLookupEntry {
    uint64_t key = 0;
    std::vector<uint32_t> frameIds;

    template <class Archive>
    void serialize(Archive &ar) {
      ar(key, frameIds);
    }
  };

  struct SceneTripletLookupEntry {
    uint64_t key = 0;
    uint32_t frameId = 0;

    template <class Archive>
    void serialize(Archive &ar) {
      ar(key, frameId);
    }
  };

  struct ColorRotationLookupEntry {
    uint64_t key = 0;
    uint16_t value = 0;

    template <class Archive>
    void serialize(Archive &ar) {
      ar(key, value);
    }
  };

  struct CriticalTriggerLookupEntry {
    uint64_t key = 0;
    std::vector<uint32_t> frameIds;

    template <class Archive>
    void serialize(Archive &ar) {
      ar(key, frameIds);
    }
  };

  struct NormalBucketEntry {
    uint8_t mask = 0;
    uint8_t shape = 0;
    uint16_t reserved = 0;

    template <class Archive>
    void serialize(Archive &ar) {
      ar(mask, shape, reserved);
    }
  };

  SerumData();
  ~SerumData();

  void SetLogCallback(Serum_LogCallback callback, const void *userData) {
    m_logCallback = callback;
    m_logUserData = userData;

    if (sceneGenerator) {
      sceneGenerator->SetLogCallback(callback, userData);
    }
  }

  void Clear();
  bool SaveToFile(const char *filename);
  bool LoadFromFile(const char *filename, const uint8_t flags);
  bool LoadFromBuffer(const uint8_t *data, size_t size, const uint8_t flags);
  void BuildPackingSidecarsAndNormalize();
  void BuildSpriteRuntimeSidecars();

  void BuildCriticalTriggerLookup();
  void DebugLogSpriteDynamicSidecarState(const char *stage, uint32_t spriteId);
  void DebugLogPackingSidecarsStorageSizes();
  bool HasSpriteRuntimeSidecars() const;
  void BuildColorRotationLookup();
  void RefreshPreparedLoadMetadata();
  bool TryGetColorRotation(uint32_t frameId, uint16_t color, bool isextra,
                           uint16_t &rotationIndex,
                           uint16_t &positionInRotation) const;
  void LogSparseVectorProfileSnapshot();
  void DebugLogSceneLookupSummary(const char *stage);

  // Header data
  char rname[64];
  uint8_t SerumVersion;
  uint16_t concentrateFileVersion;
  uint32_t fwidth, fheight;
  uint32_t fwidth_extra, fheight_extra;
  uint32_t nframes;
  uint32_t nocolors, nccolors;
  uint32_t ncompmasks, nmovmasks;
  uint32_t nsprites;
  uint16_t nbackgrounds;
  bool is256x64;
  // SERUM_SCALING_* selector for original -> extra resolution upscaling.
  // Persisted from concentrate version 8 on; older archives default to
  // SERUM_SCALING_SCALE2X, which is the look the stack has always produced.
  uint8_t scalingAlgorithm = SERUM_SCALING_SCALE2X;
  // SERUM_SHADOW_OFFSET_* selector for dynamic shadows on the upscaled extra
  // plane. Persisted from concentrate version 8 on; older archives default to
  // NATIVE, which is what the pre-layer extra-plane renderer produced.
  uint8_t shadowOffsetMode = SERUM_SHADOW_OFFSET_NATIVE;
  // The scenes and the scaling settings came with the colorization (the
  // settings block of the cROM): a pup.csv or scaling.txt beside it does not
  // change them.
  bool scenesFromColorization = false;
  bool scalingFromColorization = false;

  // Speaker lights (persisted from concentrate version 11): the effects, the
  // default of each channel and the frames that change channels, sorted by
  // frame ID. hasLights is false for a colorization without them.
  bool hasLights = false;
  std::vector<SerumLightEffectData> lightEffects;
  SerumLightAction lightDefaults[SERUM_LIGHT_CHANNELS];
  std::vector<SerumLightFrame> lightFrames;
  // The frame's light changes, nullptr when it has none.
  const SerumLightFrame *LightFrame(uint32_t frameId) const {
    const auto it = std::lower_bound(
        lightFrames.begin(), lightFrames.end(), frameId,
        [](const SerumLightFrame &f, uint32_t id) { return f.frameId < id; });
    return it != lightFrames.end() && it->frameId == frameId ? &*it : nullptr;
  }
  // Dynamic sets that colour only the original plane (persisted from
  // concentrate version 12), sorted by frame ID; frames without any are left
  // out.
  std::vector<SerumOriginalOnlySets> originalOnlySets;
  // The frame's original-only sets as a bit mask; 0 when it has none.
  uint32_t OriginalOnlySets(uint32_t frameId) const {
    const auto it = std::lower_bound(
        originalOnlySets.begin(), originalOnlySets.end(), frameId,
        [](const SerumOriginalOnlySets &f, uint32_t id) {
          return f.frameId < id;
        });
    return it != originalOnlySets.end() && it->frameId == frameId ? it->sets
                                                                  : 0;
  }
  // The effect an action starts; false for SERUM_LIGHT_UNCHANGED.
  bool ResolveLightAction(const SerumLightAction &action,
                          Serum_Light_Effect &effect) const;

  // The frame sprites of a cROMc before concentrate version 10 and of a cROM
  // before format version 1: 8-bit sprite IDs, 255 for an empty slot.
  // NarrowFrameSprites() writes them (for tests of the older versions; it
  // throws for a colorization that does not fit: more than
  // LEGACY_SPRITES_PER_FRAME slots, a sprite ID from 255 on).
  SparseVector<uint8_t> NarrowFrameSprites() const;
  void WidenFrameSprites(SparseVector<uint8_t> &legacy);

  // The sprite slots of a frame in the loaded colorization: those its file
  // has (LEGACY_SPRITES_PER_FRAME before cROM format version 1), at most
  // MAX_SPRITES_PER_FRAME.
  uint32_t SpritesPerFrame() const {
    const size_t slots = framesprites.valuesPerElement();
    return slots == 0 ? LEGACY_SPRITES_PER_FRAME
                      : static_cast<uint32_t>(
                            std::min<size_t>(slots, MAX_SPRITES_PER_FRAME));
  }

  // Vector data
  SparseVector<uint32_t> hashcodes;
  SparseVector<uint8_t> shapecompmode;
  SparseVector<uint8_t> compmaskID;
  SparseVector<uint8_t> movrctID;
  SparseVector<uint8_t> compmasks;
  SparseVector<uint8_t> movrcts;  // Currently unused
  SparseVector<uint8_t> cpal;
  SparseVector<uint8_t> isextraframe;
  SparseVector<uint8_t> cframes;
  SparseVector<uint16_t> cframes_v2;
  SparseVector<uint16_t> cframes_v2_extra;
  SparseVector<uint8_t> dynamasks;
  SparseVector<uint8_t> dynamasks_active;
  SparseVector<uint8_t> dynamasks_extra;
  SparseVector<uint8_t> dynamasks_extra_active;
  SparseVector<uint8_t> dyna4cols;
  SparseVector<uint16_t> dyna4cols_v2;
  SparseVector<uint16_t> dyna4cols_v2_extra;
  // The sprite IDs of each frame's slots, SERUM_NO_SPRITE for an empty one;
  // SpritesPerFrame() slots per frame. 8 bit with 255 for an empty slot in a
  // cROM before format version 1 and a cROMc before concentrate version 10,
  // widened on load.
  SparseVector<uint16_t> framesprites;
  SparseVector<uint8_t> spritedescriptionso;
  SparseVector<uint8_t> spritedescriptionso_opaque;
  SparseVector<uint8_t> spritedescriptionsc;
  SparseVector<uint8_t> isextrasprite;
  SparseVector<uint8_t> spriteoriginal;
  SparseVector<uint8_t> spriteoriginal_opaque;
  SparseVector<uint8_t> spritemask_extra;
  SparseVector<uint8_t> spritemask_extra_opaque;
  SparseVector<uint16_t> spritecolored;
  SparseVector<uint16_t> spritecolored_extra;
  SparseVector<uint8_t> activeframes;
  SparseVector<uint8_t> colorrotations;
  SparseVector<uint16_t> colorrotations_v2;
  SparseVector<uint16_t> colorrotations_v2_extra;
  SparseVector<uint32_t> spritedetdwords;
  SparseVector<uint16_t> spritedetdwordpos;
  SparseVector<uint16_t> spritedetareas;
  SparseVector<uint32_t> triggerIDs;
  SparseVector<uint16_t> framespriteBB;
  SparseVector<uint8_t> isextrabackground;
  SparseVector<uint8_t> backgroundframes;
  SparseVector<uint16_t> backgroundframes_v2;
  SparseVector<uint16_t> backgroundframes_v2_extra;
  SparseVector<uint16_t> backgroundIDs;
  SparseVector<uint16_t> backgroundBB;
  SparseVector<uint8_t> backgroundmask;
  SparseVector<uint8_t> backgroundmask_extra;
  SparseVector<uint8_t> dynashadowsdir;
  SparseVector<uint16_t> dynashadowscol;
  SparseVector<uint8_t> dynashadowsdir_extra;
  SparseVector<uint16_t> dynashadowscol_extra;
  SparseVector<uint16_t> dynasprite4cols;
  SparseVector<uint16_t> dynasprite4cols_extra;
  SparseVector<uint8_t> dynaspritemasks;
  SparseVector<uint8_t> dynaspritemasks_active;
  SparseVector<uint8_t> dynaspritemasks_extra;
  SparseVector<uint8_t> dynaspritemasks_extra_active;
  SparseVector<uint8_t> sprshapemode;
  std::vector<uint8_t> frameHasDynamic;
  std::vector<uint8_t> frameHasDynamicExtra;
  std::vector<uint8_t> frameIsScene;
  std::vector<uint32_t> spriteCandidateOffsets;
  std::vector<uint16_t> spriteCandidateIds;
  std::vector<uint8_t> spriteCandidateSlots;
  std::vector<uint8_t> frameHasShapeSprite;
  std::vector<uint16_t> spriteWidth;
  std::vector<uint16_t> spriteHeight;
  std::vector<uint8_t> spriteUsesShape;
  std::vector<uint32_t> spriteDetectOffsets;
  std::vector<SpriteDetectMeta> spriteDetectMeta;
  std::vector<uint32_t> spriteOpaqueRowSegmentStart;
  std::vector<uint16_t> spriteOpaqueRowSegmentCount;
  std::vector<uint16_t> spriteOpaqueSegments;
  std::unordered_map<uint64_t, std::vector<uint32_t>> sceneFramesBySignature;
  std::unordered_map<uint64_t, std::vector<uint32_t>> normalFramesBySignature;
  std::vector<NormalBucketEntry> normalIdentifyBuckets;
  std::vector<uint32_t> frameToNormalBucket;
  std::unordered_map<uint64_t, uint32_t> sceneFrameIdByTriplet;
  std::unordered_map<uint64_t, uint16_t> colorRotationLookupByFrameAndColor;
  std::unordered_map<uint64_t, std::vector<uint32_t>>
      criticalTriggerFramesBySignature;
  uint8_t hasAnyExtraFrame = 0;
  uint32_t publicTriggerCount = 0;

  SceneGenerator *sceneGenerator;

 private:
  void Log(const char *format, ...);

  Serum_LogCallback m_logCallback = nullptr;
  const void *m_logUserData = nullptr;

  uint8_t m_loadFlags = 0;
  bool m_packingSidecarsNormalized = false;
  std::vector<std::vector<uint8_t>> m_packingSidecarsStorage;

  friend class cereal::access;

  template <class Archive>
  void SerializeSpriteCandidateIds(Archive &ar) {
    if (concentrateFileVersion >= 10) {
      ar(spriteCandidateIds);
    } else if constexpr (Archive::is_saving::value) {
      std::vector<uint8_t> legacy(spriteCandidateIds.size());
      for (size_t i = 0; i < legacy.size(); ++i) {
        if (spriteCandidateIds[i] >= 255) {
          throw std::runtime_error(
              "sprite IDs from 255 on need concentrate version 10");
        }
        legacy[i] = static_cast<uint8_t>(spriteCandidateIds[i]);
      }
      ar(legacy);
    } else {
      std::vector<uint8_t> legacy;
      ar(legacy);
      spriteCandidateIds.assign(legacy.begin(), legacy.end());
    }
  }

  template <class Archive>
  void serialize(Archive &ar) {
    ar(rname, SerumVersion, fwidth, fheight, fwidth_extra, fheight_extra,
       nframes, nocolors, nccolors, ncompmasks, nmovmasks, nsprites,
       nbackgrounds, is256x64, hashcodes, shapecompmode, compmaskID, movrctID,
       compmasks, movrcts, cpal, isextraframe, cframes, cframes_v2,
       cframes_v2_extra, dynamasks, dynamasks_extra, dyna4cols, dyna4cols_v2,
       dyna4cols_v2_extra);
    // 16-bit sprite IDs from concentrate version 10 on, 8 bit before.
    if (concentrateFileVersion >= 10) {
      ar(framesprites);
    } else if constexpr (Archive::is_saving::value) {
      SparseVector<uint8_t> legacy = NarrowFrameSprites();
      ar(legacy);
    } else {
      SparseVector<uint8_t> legacy(255);
      ar(legacy);
      WidenFrameSprites(legacy);
    }
    ar(spritedescriptionso, spritedescriptionsc, isextrasprite, spriteoriginal,
       spritemask_extra, spritecolored, spritecolored_extra, activeframes,
       colorrotations, colorrotations_v2, colorrotations_v2_extra,
       spritedetdwords, spritedetdwordpos, spritedetareas, triggerIDs,
       framespriteBB, isextrabackground, backgroundframes, backgroundframes_v2,
       backgroundframes_v2_extra, backgroundIDs, backgroundBB, backgroundmask,
       backgroundmask_extra, dynashadowsdir, dynashadowscol,
       dynashadowsdir_extra, dynashadowscol_extra, dynasprite4cols,
       dynasprite4cols_extra, dynaspritemasks, dynaspritemasks_extra,
       sprshapemode);

    if constexpr (Archive::is_saving::value) {
      if (concentrateFileVersion >= 6) {
        std::vector<SceneSignatureLookupEntry> sceneSignatureEntries;
        sceneSignatureEntries.reserve(sceneFramesBySignature.size());
        for (const auto &entry : sceneFramesBySignature) {
          SceneSignatureLookupEntry serialized;
          serialized.key = entry.first;
          serialized.frameIds = entry.second;
          std::sort(serialized.frameIds.begin(), serialized.frameIds.end());
          sceneSignatureEntries.push_back(std::move(serialized));
        }
        std::sort(
            sceneSignatureEntries.begin(), sceneSignatureEntries.end(),
            [](const SceneSignatureLookupEntry &a,
               const SceneSignatureLookupEntry &b) { return a.key < b.key; });

        std::vector<SceneSignatureLookupEntry> normalSignatureEntries;
        normalSignatureEntries.reserve(normalFramesBySignature.size());
        for (const auto &entry : normalFramesBySignature) {
          SceneSignatureLookupEntry serialized;
          serialized.key = entry.first;
          serialized.frameIds = entry.second;
          std::sort(serialized.frameIds.begin(), serialized.frameIds.end());
          normalSignatureEntries.push_back(std::move(serialized));
        }
        std::sort(
            normalSignatureEntries.begin(), normalSignatureEntries.end(),
            [](const SceneSignatureLookupEntry &a,
               const SceneSignatureLookupEntry &b) { return a.key < b.key; });

        std::vector<SceneTripletLookupEntry> sceneTripletEntries;
        sceneTripletEntries.reserve(sceneFrameIdByTriplet.size());
        for (const auto &entry : sceneFrameIdByTriplet) {
          sceneTripletEntries.push_back({entry.first, entry.second});
        }
        std::sort(
            sceneTripletEntries.begin(), sceneTripletEntries.end(),
            [](const SceneTripletLookupEntry &a,
               const SceneTripletLookupEntry &b) { return a.key < b.key; });

        std::vector<ColorRotationLookupEntry> colorRotationEntries;
        colorRotationEntries.reserve(colorRotationLookupByFrameAndColor.size());
        for (const auto &entry : colorRotationLookupByFrameAndColor) {
          colorRotationEntries.push_back({entry.first, entry.second});
        }
        std::sort(
            colorRotationEntries.begin(), colorRotationEntries.end(),
            [](const ColorRotationLookupEntry &a,
               const ColorRotationLookupEntry &b) { return a.key < b.key; });

        std::vector<CriticalTriggerLookupEntry> criticalTriggerEntries;
        criticalTriggerEntries.reserve(criticalTriggerFramesBySignature.size());
        for (const auto &entry : criticalTriggerFramesBySignature) {
          CriticalTriggerLookupEntry serialized;
          serialized.key = entry.first;
          serialized.frameIds = entry.second;
          std::sort(serialized.frameIds.begin(), serialized.frameIds.end());
          criticalTriggerEntries.push_back(std::move(serialized));
        }
        std::sort(
            criticalTriggerEntries.begin(), criticalTriggerEntries.end(),
            [](const CriticalTriggerLookupEntry &a,
               const CriticalTriggerLookupEntry &b) { return a.key < b.key; });

        ar(frameIsScene, sceneSignatureEntries, normalSignatureEntries,
           normalIdentifyBuckets, frameToNormalBucket, spriteoriginal_opaque,
           spritemask_extra_opaque, spritedescriptionso_opaque,
           dynamasks_active, dynamasks_extra_active, dynaspritemasks_active,
           dynaspritemasks_extra_active, frameHasDynamic, frameHasDynamicExtra,
           sceneTripletEntries, colorRotationEntries, criticalTriggerEntries,
           spriteCandidateOffsets);
        SerializeSpriteCandidateIds(ar);
        ar(spriteCandidateSlots, frameHasShapeSprite, spriteWidth, spriteHeight,
           spriteUsesShape, spriteDetectOffsets, spriteDetectMeta,
           spriteOpaqueRowSegmentStart, spriteOpaqueRowSegmentCount,
           spriteOpaqueSegments, hasAnyExtraFrame, publicTriggerCount);
      }
    } else {
      if (concentrateFileVersion >= 6) {
        std::vector<SceneSignatureLookupEntry> sceneSignatureEntries;
        std::vector<SceneSignatureLookupEntry> normalSignatureEntries;
        std::vector<SceneTripletLookupEntry> sceneTripletEntries;
        std::vector<ColorRotationLookupEntry> colorRotationEntries;
        std::vector<CriticalTriggerLookupEntry> criticalTriggerEntries;
        ar(frameIsScene, sceneSignatureEntries, normalSignatureEntries,
           normalIdentifyBuckets, frameToNormalBucket, spriteoriginal_opaque,
           spritemask_extra_opaque, spritedescriptionso_opaque,
           dynamasks_active, dynamasks_extra_active, dynaspritemasks_active,
           dynaspritemasks_extra_active, frameHasDynamic, frameHasDynamicExtra,
           sceneTripletEntries, colorRotationEntries, criticalTriggerEntries,
           spriteCandidateOffsets);
        SerializeSpriteCandidateIds(ar);
        ar(spriteCandidateSlots, frameHasShapeSprite, spriteWidth, spriteHeight,
           spriteUsesShape, spriteDetectOffsets, spriteDetectMeta,
           spriteOpaqueRowSegmentStart, spriteOpaqueRowSegmentCount,
           spriteOpaqueSegments, hasAnyExtraFrame, publicTriggerCount);

        sceneFramesBySignature.clear();
        sceneFramesBySignature.reserve(sceneSignatureEntries.size());
        for (const auto &entry : sceneSignatureEntries) {
          sceneFramesBySignature[entry.key] = entry.frameIds;
        }

        normalFramesBySignature.clear();
        normalFramesBySignature.reserve(normalSignatureEntries.size());
        for (const auto &entry : normalSignatureEntries) {
          normalFramesBySignature[entry.key] = entry.frameIds;
        }

        sceneFrameIdByTriplet.clear();
        sceneFrameIdByTriplet.reserve(sceneTripletEntries.size());
        for (const auto &entry : sceneTripletEntries) {
          sceneFrameIdByTriplet[entry.key] = entry.frameId;
        }

        colorRotationLookupByFrameAndColor.clear();
        colorRotationLookupByFrameAndColor.reserve(colorRotationEntries.size());
        for (const auto &entry : colorRotationEntries) {
          colorRotationLookupByFrameAndColor[entry.key] = entry.value;
        }
        serum_accel::RotationLookupChanged(*this);

        criticalTriggerFramesBySignature.clear();
        criticalTriggerFramesBySignature.reserve(criticalTriggerEntries.size());
        for (const auto &entry : criticalTriggerEntries) {
          criticalTriggerFramesBySignature[entry.key] = entry.frameIds;
        }
      } else {
        frameIsScene.clear();
        sceneFramesBySignature.clear();
        normalFramesBySignature.clear();
        normalIdentifyBuckets.clear();
        frameToNormalBucket.clear();
        sceneFrameIdByTriplet.clear();
        colorRotationLookupByFrameAndColor.clear();
        criticalTriggerFramesBySignature.clear();
        spriteoriginal_opaque.clear();
        spritemask_extra_opaque.clear();
        spritedescriptionso_opaque.clear();
        dynamasks_active.clear();
        dynamasks_extra_active.clear();
        dynaspritemasks_active.clear();
        dynaspritemasks_extra_active.clear();
        frameHasDynamic.clear();
        frameHasDynamicExtra.clear();
        spriteCandidateOffsets.clear();
        spriteCandidateIds.clear();
        spriteCandidateSlots.clear();
        frameHasShapeSprite.clear();
        spriteWidth.clear();
        spriteHeight.clear();
        spriteUsesShape.clear();
        spriteDetectOffsets.clear();
        spriteDetectMeta.clear();
        spriteOpaqueRowSegmentStart.clear();
        spriteOpaqueRowSegmentCount.clear();
        spriteOpaqueSegments.clear();
        hasAnyExtraFrame = 0;
        publicTriggerCount = 0;
      }
    }

    if constexpr (Archive::is_saving::value) {
      if (concentrateFileVersion >= 6) {
        constexpr uint32_t kSceneDataMagic = 0x53434431;  // "SCD1"
        constexpr uint32_t kMaxSceneDataEntries = 100000;
        uint32_t magic = kSceneDataMagic;
        const std::vector<SceneData> scenes =
            sceneGenerator ? sceneGenerator->getSceneData()
                           : std::vector<SceneData>{};
        uint32_t count = static_cast<uint32_t>(scenes.size());
        if (count > kMaxSceneDataEntries) {
          count = kMaxSceneDataEntries;
        }
        ar(magic, count);
        for (uint32_t i = 0; i < count; ++i) {
          ar(scenes[i]);
        }
      } else {
        ar(sceneGenerator ? sceneGenerator->getSceneData()
                          : std::vector<SceneData>{});
      }
    } else {
      // An is256x64 project is rendered from its extra-plane data only, and a
      // 32p-only request with FLAG_REQUEST_FALLBACK gets that plane too, so
      // the data has to stay for it.
      const bool nativePlaneFallback =
          is256x64 && (m_loadFlags & FLAG_REQUEST_32P_FRAMES) &&
          (m_loadFlags & FLAG_REQUEST_FALLBACK);
      if (SERUM_V2 == SerumVersion &&
          ((fheight == 32 && !(m_loadFlags & FLAG_REQUEST_64P_FRAMES) &&
            !nativePlaneFallback) ||
           (fheight == 64 && !(m_loadFlags & FLAG_REQUEST_32P_FRAMES)))) {
        isextraframe.clearIndex();
        isextrabackground.clearIndex();
        isextrasprite.clearIndex();
      }

      cframes_v2_extra.setParent(&isextraframe);
      dynamasks_extra.setParent(&isextraframe);
      dynamasks_extra_active.setParent(&isextraframe);
      dyna4cols_v2_extra.setParent(&isextraframe);
      spritemask_extra.setParent(&isextrasprite);
      spritemask_extra_opaque.setParent(&isextrasprite);
      spritecolored_extra.setParent(&isextrasprite);
      colorrotations_v2_extra.setParent(&isextraframe);
      framespriteBB.setParent(&framesprites);
      backgroundframes_v2_extra.setParent(&isextrabackground);
      backgroundmask.setParent(&backgroundIDs);
      backgroundmask_extra.setParent(&backgroundIDs);
      dynashadowsdir_extra.setParent(&isextraframe);
      dynashadowscol_extra.setParent(&isextraframe);
      dynasprite4cols_extra.setParent(&isextraframe);
      dynaspritemasks_extra.setParent(&isextraframe);
      dynaspritemasks_extra_active.setParent(&isextrasprite);
      backgroundBB.setParent(&backgroundIDs);

      std::vector<SceneData> loadedScenes;
      if (concentrateFileVersion >= 6) {
        constexpr uint32_t kSceneDataMagic = 0x53434431;  // "SCD1"
        constexpr uint32_t kMaxSceneDataEntries = 100000;
        uint32_t magic = 0;
        uint32_t count = 0;
        ar(magic, count);
        if (magic != kSceneDataMagic) {
          throw std::runtime_error("Invalid scene data block in cROMc");
        }
        if (count > kMaxSceneDataEntries) {
          throw std::runtime_error("Scene data count exceeds hard limit");
        }
        loadedScenes.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
          SceneData scene;
          ar(scene);
          loadedScenes.push_back(scene);
        }
      } else {
        ar(loadedScenes);
      }
      if (sceneGenerator) {
        sceneGenerator->setSceneData(std::move(loadedScenes));
        sceneGenerator->setDepth(nocolors == 16 ? 4 : 2);
      }
    }

    if constexpr (Archive::is_saving::value) {
      if (concentrateFileVersion >= 8) {
        ar(scalingAlgorithm, shadowOffsetMode);
      }
      if (concentrateFileVersion >= 9) {
        ar(scenesFromColorization, scalingFromColorization);
      }
      if (concentrateFileVersion >= 11) {
        ar(hasLights, lightEffects, lightFrames);
        for (SerumLightAction &action : lightDefaults) ar(action);
      }
      if (concentrateFileVersion >= 12) {
        ar(originalOnlySets);
      }
    } else {
      if (concentrateFileVersion >= 8) {
        ar(scalingAlgorithm, shadowOffsetMode);
        // Two algorithms; anything else stored is out of range and reads as
        // the default.
        if (scalingAlgorithm != SERUM_SCALING_LINE_DOUBLING) {
          scalingAlgorithm = SERUM_SCALING_SCALE2X;
        }
        if (shadowOffsetMode > SERUM_SHADOW_OFFSET_PROPORTIONAL) {
          shadowOffsetMode = SERUM_SHADOW_OFFSET_NATIVE;
        }
      } else {
        scalingAlgorithm = SERUM_SCALING_SCALE2X;
        shadowOffsetMode = SERUM_SHADOW_OFFSET_NATIVE;
      }
      if (concentrateFileVersion >= 9) {
        ar(scenesFromColorization, scalingFromColorization);
      } else {
        scenesFromColorization = false;
        scalingFromColorization = false;
      }
      if (concentrateFileVersion >= 11) {
        ar(hasLights, lightEffects, lightFrames);
        for (SerumLightAction &action : lightDefaults) ar(action);
      } else {
        hasLights = false;
        lightEffects.clear();
        lightFrames.clear();
        for (SerumLightAction &action : lightDefaults) action = {};
      }
      if (concentrateFileVersion >= 12) {
        ar(originalOnlySets);
      } else {
        originalOnlySets.clear();
      }
    }
  }
};
