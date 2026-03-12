#pragma once

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

  // Header data
  char romName[64];
  uint8_t SerumVersion;
  uint16_t concentrateFileVersion;
  uint32_t frameWidth, frameHeight;
  uint32_t extraFrameWidth, extraFrameHeight;
  uint32_t frameCount;
  uint32_t colorCount, classicColorCount;
  uint32_t comparisonMaskCount, movementMaskCount;
  uint32_t spriteCount;
  uint16_t backgroundCount;
  bool isNative256x64;
  std::vector<uint8_t> storage;

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
  SparseVector<uint8_t> dynamasks_extra;
  SparseVector<uint8_t> dyna4cols;
  SparseVector<uint16_t> dyna4cols_v2;
  SparseVector<uint16_t> dyna4cols_v2_extra;
  SparseVector<uint8_t> framesprites;
  SparseVector<uint8_t> spritedescriptionso;
  SparseVector<uint8_t> spritedescriptionsc;
  SparseVector<uint8_t> isextrasprite;
  SparseVector<uint8_t> spriteoriginal;
  SparseVector<uint8_t> spritemask_extra;
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
  SparseVector<uint8_t> dynaspritemasks_extra;
  SparseVector<uint8_t> sprshapemode;
  std::vector<uint8_t> frameIsScene;
  std::unordered_map<uint64_t, std::vector<uint32_t>> sceneFramesBySignature;
  std::unordered_map<uint64_t, uint32_t> sceneFrameIdsBySceneKey;
  std::unordered_map<uint32_t, uint32_t> sceneGroupFrameTableOffset;
  std::unordered_map<uint32_t, uint16_t> sceneGroupFrameTableLength;
  std::vector<uint32_t> sceneGroupFrameIdsFlat;
  std::vector<uint16_t> spriteWidthV1;
  std::vector<uint16_t> spriteHeightV1;
  std::vector<uint16_t> spriteWidthV2;
  std::vector<uint16_t> spriteHeightV2;

  SceneGenerator *sceneGenerator;

 private:
  void Log(const char *format, ...);

  Serum_LogCallback m_logCallback = nullptr;
  const void *m_logUserData = nullptr;

  uint8_t m_loadFlags = 0;

  friend class cereal::access;

  template <class Archive>
  void serialize(Archive &ar) {
    ar(romName, SerumVersion, frameWidth, frameHeight, extraFrameWidth,
       extraFrameHeight, frameCount, colorCount, classicColorCount,
       comparisonMaskCount, movementMaskCount, spriteCount, backgroundCount,
       isNative256x64, hashcodes, shapecompmode, compmaskID, movrctID,
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
        ar(frameIsScene, sceneFramesBySignature, sceneFrameIdsBySceneKey,
           sceneGroupFrameTableOffset, sceneGroupFrameTableLength,
           sceneGroupFrameIdsFlat, spriteWidthV1, spriteHeightV1,
           spriteWidthV2, spriteHeightV2);
      }
    } else {
      if (concentrateFileVersion >= 6) {
        ar(frameIsScene, sceneFramesBySignature, sceneFrameIdsBySceneKey,
           sceneGroupFrameTableOffset, sceneGroupFrameTableLength,
           sceneGroupFrameIdsFlat, spriteWidthV1, spriteHeightV1,
           spriteWidthV2, spriteHeightV2);
      } else {
        frameIsScene.clear();
        sceneFramesBySignature.clear();
        sceneFrameIdsBySceneKey.clear();
        sceneGroupFrameTableOffset.clear();
        sceneGroupFrameTableLength.clear();
        sceneGroupFrameIdsFlat.clear();
        spriteWidthV1.clear();
        spriteHeightV1.clear();
        spriteWidthV2.clear();
        spriteHeightV2.clear();
      }
    }

    if constexpr (Archive::is_saving::value) {
      ar(sceneGenerator ? sceneGenerator->getSceneData()
                        : std::vector<SceneData>{});
    } else {
      if (SERUM_V2 == SerumVersion &&
          ((frameHeight == 32 && !(m_loadFlags & FLAG_REQUEST_64P_FRAMES)) ||
           (frameHeight == 64 && !(m_loadFlags & FLAG_REQUEST_32P_FRAMES)))) {
        isextraframe.clearIndex();
        isextrabackground.clearIndex();
        isextrasprite.clearIndex();
      }

      cframes_v2_extra.setParent(&isextraframe);
      dynamasks_extra.setParent(&isextraframe);
      dyna4cols_v2_extra.setParent(&isextraframe);
      spritemask_extra.setParent(&isextrasprite);
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
      backgroundBB.setParent(&backgroundIDs);

      std::vector<SceneData> loadedScenes;
      ar(loadedScenes);
      if (sceneGenerator) {
        sceneGenerator->setSceneData(std::move(loadedScenes));
        sceneGenerator->setDepth(colorCount == 16 ? 4 : 2);
      }
    }
  }
};
