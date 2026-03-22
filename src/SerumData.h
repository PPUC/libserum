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
  SparseVector<uint8_t> framesprites;
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
  std::vector<uint8_t> spriteCandidateIds;
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
  void serialize(Archive &ar) {
    ar(rname, SerumVersion, fwidth, fheight, fwidth_extra, fheight_extra,
       nframes, nocolors, nccolors, ncompmasks, nmovmasks, nsprites,
       nbackgrounds, is256x64, hashcodes, shapecompmode, compmaskID, movrctID,
       compmasks, movrcts, cpal, isextraframe, cframes, cframes_v2,
       cframes_v2_extra, dynamasks, dynamasks_extra, dyna4cols, dyna4cols_v2,
       dyna4cols_v2_extra, framesprites, spritedescriptionso,
       spritedescriptionsc, isextrasprite, spriteoriginal, spritemask_extra,
       spritecolored, spritecolored_extra, activeframes, colorrotations,
       colorrotations_v2, colorrotations_v2_extra, spritedetdwords,
       spritedetdwordpos, spritedetareas, triggerIDs, framespriteBB,
       isextrabackground, backgroundframes, backgroundframes_v2,
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
           spriteCandidateOffsets, spriteCandidateIds, spriteCandidateSlots,
           frameHasShapeSprite, spriteWidth, spriteHeight, spriteUsesShape,
           spriteDetectOffsets, spriteDetectMeta, spriteOpaqueRowSegmentStart,
           spriteOpaqueRowSegmentCount, spriteOpaqueSegments);
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
           spriteCandidateOffsets, spriteCandidateIds, spriteCandidateSlots,
           frameHasShapeSprite, spriteWidth, spriteHeight, spriteUsesShape,
           spriteDetectOffsets, spriteDetectMeta, spriteOpaqueRowSegmentStart,
           spriteOpaqueRowSegmentCount, spriteOpaqueSegments);

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
      if (SERUM_V2 == SerumVersion &&
          ((fheight == 32 && !(m_loadFlags & FLAG_REQUEST_64P_FRAMES)) ||
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
  }
};
