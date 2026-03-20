#include "SerumData.h"

#include <unordered_set>

#include "DecompressingIStream.h"
#include "miniz/miniz.h"
#include "serum-version.h"

bool is_real_machine();

namespace {
constexpr uint32_t kMonochromeTriggerId = 65432u;
constexpr uint32_t kMonochromePaletteTriggerId = 65431u;

uint64_t MakeCriticalTriggerSignature(uint8_t mask, uint8_t shape,
                                      uint32_t hash) {
  return (uint64_t(mask) << 40) | (uint64_t(shape) << 32) | hash;
}
}  // namespace

static uint32_t GetDebugSpriteIdFromEnv() {
  const char *value = std::getenv("SERUM_DEBUG_SPRITE_ID");
  if (!value || value[0] == '\0') {
    return 0xffffffffu;
  }
  char *endPtr = nullptr;
  unsigned long parsed = std::strtoul(value, &endPtr, 0);
  if (endPtr == value || *endPtr != '\0') {
    return 0xffffffffu;
  }
  return static_cast<uint32_t>(parsed);
}

SerumData::SerumData()
    : SerumVersion(0),
      concentrateFileVersion(SERUM_CONCENTRATE_VERSION),
      is256x64(false),
      hashcodes(0, true),
      shapecompmode(0),
      compmaskID(255),
      movrctID(0),
      compmasks(0),
      movrcts(0),
      cpal(0),
      isextraframe(0, true),
      cframes(0, false, true),
      cframes_v2(0, false, true),
      cframes_v2_extra(0, false, true),
      dynamasks(0, false, true, true, 0, 1),
      dynamasks_active(0, false, true, true, 0, 1),
      dynamasks_extra(0, false, true, true, 0, 1),
      dynamasks_extra_active(0, false, true, true, 0, 1),
      dyna4cols(0),
      dyna4cols_v2(0, false, true),
      dyna4cols_v2_extra(0, false, true),
      framesprites(255),
      spritedescriptionso(0),
      spritedescriptionso_opaque(0, false, true, true, 0, 1),
      spritedescriptionsc(0),
      isextrasprite(0, true),
      spriteoriginal(0, false, true, true, 0, 1),
      spriteoriginal_opaque(0, false, true, true, 0, 1),
      spritemask_extra(0, false, true, true, 0, 1),
      spritemask_extra_opaque(0, false, true, true, 0, 1),
      spritecolored(0, false, true),
      spritecolored_extra(0, false, true),
      activeframes(1),
      colorrotations(0),
      colorrotations_v2(0),
      colorrotations_v2_extra(0),
      spritedetdwords(0),
      spritedetdwordpos(0),
      spritedetareas(0),
      triggerIDs(0xffffffff),
      framespriteBB(0, false, true),
      isextrabackground(0, true),
      backgroundframes(0, false, true),
      backgroundframes_v2(0, false, true),
      backgroundframes_v2_extra(0, false, true),
      backgroundIDs(0xffff),
      backgroundBB(0),
      backgroundmask(0, false, true, true, 0, 1),
      backgroundmask_extra(0, false, true, true, 0, 1),
      dynashadowsdir(0),
      dynashadowscol(0),
      dynashadowsdir_extra(0),
      dynashadowscol_extra(0),
      dynasprite4cols(0),
      dynasprite4cols_extra(0),
      dynaspritemasks(0, false, true, true, 0, 1),
      dynaspritemasks_active(0, false, true, true, 0, 1),
      dynaspritemasks_extra(0, false, true, true, 0, 1),
      dynaspritemasks_extra_active(0, false, true, true, 0, 1),
      sprshapemode(0) {
  cframes_v2.setProfileLabel("cframes_v2");
  cframes_v2_extra.setProfileLabel("cframes_v2_extra");
  dynamasks.setProfileLabel("dynamasks");
  dynamasks_active.setProfileLabel("dynamasks_active");
  dynamasks_extra.setProfileLabel("dynamasks_extra");
  dynamasks_extra_active.setProfileLabel("dynamasks_extra_active");
  backgroundmask.setProfileLabel("backgroundmask");
  backgroundmask_extra.setProfileLabel("backgroundmask_extra");
  dynaspritemasks.setProfileLabel("dynaspritemasks");
  dynaspritemasks_active.setProfileLabel("dynaspritemasks_active");
  dynaspritemasks_extra.setProfileLabel("dynaspritemasks_extra");
  dynaspritemasks_extra_active.setProfileLabel("dynaspritemasks_extra_active");
  sceneGenerator = new SceneGenerator();
  if (is_real_machine()) {
    m_packingSidecarsStorage.emplace_back(256u * 1024u * 1024u, 0xA5);
  }
}

SerumData::~SerumData() {}

void SerumData::Clear() {
  m_packingSidecarsNormalized = false;
  hashcodes.clear();
  shapecompmode.clear();
  compmaskID.clear();
  compmasks.clear();
  cpal.clear();
  isextraframe.clear();
  cframes_v2.clear();
  cframes_v2_extra.clear();
  cframes.clear();
  dynamasks.clear();
  dynamasks_active.clear();
  dynamasks_extra.clear();
  dynamasks_extra_active.clear();
  dyna4cols.clear();
  dyna4cols_v2.clear();
  dyna4cols_v2_extra.clear();
  framesprites.clear();
  spritedescriptionso.clear();
  spritedescriptionso_opaque.clear();
  spritedescriptionsc.clear();
  isextrasprite.clear();
  spriteoriginal.clear();
  spriteoriginal_opaque.clear();
  spritemask_extra.clear();
  spritemask_extra_opaque.clear();
  spritecolored.clear();
  spritecolored_extra.clear();
  activeframes.clear();
  colorrotations.clear();
  colorrotations_v2.clear();
  colorrotations_v2_extra.clear();
  spritedetareas.clear();
  spritedetdwords.clear();
  spritedetdwordpos.clear();
  triggerIDs.clear();
  framespriteBB.clear();
  isextrabackground.clear();
  backgroundframes.clear();
  backgroundframes_v2.clear();
  backgroundframes_v2_extra.clear();
  backgroundIDs.clear();
  backgroundBB.clear();
  backgroundmask.clear();
  backgroundmask_extra.clear();
  dynashadowsdir.clear();
  dynashadowscol.clear();
  dynashadowsdir_extra.clear();
  dynashadowscol_extra.clear();
  dynasprite4cols.clear();
  dynasprite4cols_extra.clear();
  dynaspritemasks.clear();
  dynaspritemasks_active.clear();
  dynaspritemasks_extra.clear();
  dynaspritemasks_extra_active.clear();
  sprshapemode.clear();
  frameHasDynamic.clear();
  frameHasDynamicExtra.clear();
  frameIsScene.clear();
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
  sceneFramesBySignature.clear();
  sceneFrameIdByTriplet.clear();
  colorRotationLookupByFrameAndColor.clear();
  criticalTriggerFramesBySignature.clear();
}

void SerumData::BuildCriticalTriggerLookup() {
  criticalTriggerFramesBySignature.clear();
  if (nframes == 0) {
    return;
  }

  for (uint32_t frameId = 0; frameId < nframes; ++frameId) {
    if (frameId < frameIsScene.size() && frameIsScene[frameId] != 0) {
      continue;
    }

    const uint32_t triggerId = triggerIDs[frameId][0];
    if (triggerId != kMonochromeTriggerId &&
        triggerId != kMonochromePaletteTriggerId) {
      continue;
    }

    const uint8_t mask = compmaskID[frameId][0];
    const uint8_t shape = shapecompmode[frameId][0];
    const uint32_t hash = hashcodes[frameId][0];
    criticalTriggerFramesBySignature[MakeCriticalTriggerSignature(mask, shape,
                                                                  hash)]
        .push_back(frameId);
  }
}

void SerumData::DebugLogSpriteDynamicSidecarState(const char *stage,
                                                  uint32_t spriteId) {
  const uint32_t debugSpriteId = GetDebugSpriteIdFromEnv();
  if (spriteId != debugSpriteId || spriteId >= nsprites) {
    return;
  }

  const bool hasDyna = dynaspritemasks.hasData(spriteId);
  const bool hasActive = dynaspritemasks_active.hasData(spriteId);
  uint32_t dyna255 = 0;
  uint32_t dyna0 = 0;
  uint32_t dynaOther = 0;
  uint32_t active0 = 0;
  uint32_t active1 = 0;
  uint32_t activeOther = 0;

  if (hasDyna) {
    const uint8_t *source = dynaspritemasks[spriteId];
    for (size_t i = 0; i < MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT; ++i) {
      const uint8_t value = source[i];
      if (value == 255) {
        ++dyna255;
      } else if (value == 0) {
        ++dyna0;
      } else {
        ++dynaOther;
      }
    }
  }

  if (hasActive) {
    const uint8_t *active = dynaspritemasks_active[spriteId];
    for (size_t i = 0; i < MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT; ++i) {
      const uint8_t value = active[i];
      if (value == 0) {
        ++active0;
      } else if (value == 1) {
        ++active1;
      } else {
        ++activeOther;
      }
    }
  }

  Log("Serum debug sprite sidecar: stage=%s spriteId=%u hasDyna=%s "
      "hasActive=%s dyna255=%u dyna0=%u dynaOther=%u active0=%u active1=%u "
      "activeOther=%u",
      stage ? stage : "unknown", spriteId, hasDyna ? "true" : "false",
      hasActive ? "true" : "false", dyna255, dyna0, dynaOther, active0, active1,
      activeOther);
}

void SerumData::DebugLogPackingSidecarsStorageSizes() {
  for (size_t i = 1; i < m_packingSidecarsStorage.size(); ++i) {
    Log("Serum debug packing sidecar storage: index=%zu size=%zu", i,
        m_packingSidecarsStorage[i].size());
  }
}

void SerumData::BuildPackingSidecarsAndNormalize() {
  if (m_packingSidecarsNormalized) {
    return;
  }

  const size_t spritePixels = MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT;
  const size_t spritePixelsV1 = MAX_SPRITE_SIZE * MAX_SPRITE_SIZE;
  const size_t framePixels = static_cast<size_t>(fwidth) * fheight;
  const size_t extraFramePixels =
      static_cast<size_t>(fwidth_extra) * fheight_extra;

  std::vector<uint8_t> normalized;
  std::vector<uint8_t> flags;
  if (!m_packingSidecarsStorage.empty()) {
    m_packingSidecarsStorage.reserve(static_cast<size_t>(nsprites) * 5 +
                                     static_cast<size_t>(nframes) * 2);
  }
  auto storeSidecarCopy = [this](const uint8_t *data, size_t size) {
    if (!data || size == 0 || m_packingSidecarsStorage.empty()) {
      return;
    }
    m_packingSidecarsStorage.emplace_back(data, data + size);
  };

  normalized.resize(spritePixels);
  flags.resize(spritePixels);
  for (uint32_t spriteId = 0; spriteId < nsprites; ++spriteId) {
    const bool hasSourceVector = spriteoriginal.hasData(spriteId);
    const bool hasOpaqueVector = spriteoriginal_opaque.hasData(spriteId);
    if (!hasSourceVector && !hasOpaqueVector) {
      continue;
    }
    const uint8_t *source = spriteoriginal[spriteId];
    const uint8_t *opaqueSource = spriteoriginal_opaque[spriteId];
    for (size_t i = 0; i < spritePixels; ++i) {
      const uint8_t value = hasSourceVector ? source[i] : 0;
      const bool opaque =
          hasOpaqueVector ? (opaqueSource[i] > 0) : (value != 255);
      flags[i] = opaque ? 1 : 0;
      normalized[i] = opaque ? value : 0;
    }
    spriteoriginal_opaque.set(spriteId, flags.data(), spritePixels);
    storeSidecarCopy(flags.data(), spritePixels);
    spriteoriginal.set(spriteId, normalized.data(), spritePixels);
  }

  for (uint32_t spriteId = 0; spriteId < nsprites; ++spriteId) {
    if (isextrasprite[spriteId][0] == 0) {
      continue;
    }
    const bool hasSourceVector = spritemask_extra.hasData(spriteId);
    const bool hasOpaqueVector = spritemask_extra_opaque.hasData(spriteId);
    if (!hasSourceVector && !hasOpaqueVector) {
      continue;
    }
    const uint8_t *source = spritemask_extra[spriteId];
    const uint8_t *opaqueSource = spritemask_extra_opaque[spriteId];
    for (size_t i = 0; i < spritePixels; ++i) {
      const uint8_t value = hasSourceVector ? source[i] : 0;
      const bool opaque =
          hasOpaqueVector ? (opaqueSource[i] > 0) : (value != 255);
      flags[i] = opaque ? 1 : 0;
      normalized[i] = opaque ? value : 0;
    }
    spritemask_extra_opaque.set(spriteId, flags.data(), spritePixels,
                                &isextrasprite);
    storeSidecarCopy(flags.data(), spritePixels);
    spritemask_extra.set(spriteId, normalized.data(), spritePixels,
                         &isextrasprite);
  }

  normalized.resize(spritePixelsV1);
  flags.resize(spritePixelsV1);
  for (uint32_t spriteId = 0; spriteId < nsprites; ++spriteId) {
    const bool hasSourceVector = spritedescriptionso.hasData(spriteId);
    const bool hasOpaqueVector = spritedescriptionso_opaque.hasData(spriteId);
    if (!hasSourceVector && !hasOpaqueVector) {
      continue;
    }
    const uint8_t *source = spritedescriptionso[spriteId];
    const uint8_t *opaqueSource = spritedescriptionso_opaque[spriteId];
    for (size_t i = 0; i < spritePixelsV1; ++i) {
      const uint8_t value = hasSourceVector ? source[i] : 0;
      const bool opaque =
          hasOpaqueVector ? (opaqueSource[i] > 0) : (value != 255);
      flags[i] = opaque ? 1 : 0;
      normalized[i] = opaque ? value : 0;
    }
    spritedescriptionso_opaque.set(spriteId, flags.data(), spritePixelsV1);
    storeSidecarCopy(flags.data(), spritePixelsV1);
    spritedescriptionso.set(spriteId, normalized.data(), spritePixelsV1);
  }

  normalized.resize(framePixels);
  flags.resize(framePixels);
  frameHasDynamic.assign(nframes, 0);
  for (uint32_t frameId = 0; frameId < nframes; ++frameId) {
    const bool hasSourceVector = dynamasks.hasData(frameId);
    const bool hasActiveVector = dynamasks_active.hasData(frameId);
    if (!hasSourceVector && !hasActiveVector) {
      continue;
    }
    const uint8_t *source = dynamasks[frameId];
    const uint8_t *activeSource = dynamasks_active[frameId];
    bool anyActive = false;
    for (size_t i = 0; i < framePixels; ++i) {
      const uint8_t value = hasSourceVector ? source[i] : 0;
      const bool active =
          hasActiveVector ? (activeSource[i] > 0) : (value != 255);
      flags[i] = active ? 1 : 0;
      normalized[i] = active ? value : 0;
      anyActive = anyActive || active;
    }
    dynamasks_active.set(frameId, flags.data(), framePixels);
    storeSidecarCopy(flags.data(), framePixels);
    dynamasks.set(frameId, normalized.data(), framePixels,
                  static_cast<SparseVector<uint8_t> *>(nullptr), anyActive);
    frameHasDynamic[frameId] = anyActive ? 1 : 0;
  }

  if (extraFramePixels > 0) {
    normalized.resize(extraFramePixels);
    flags.resize(extraFramePixels);
    frameHasDynamicExtra.assign(nframes, 0);
    for (uint32_t frameId = 0; frameId < nframes; ++frameId) {
      if (isextraframe[frameId][0] == 0) {
        continue;
      }
      const bool hasSourceVector = dynamasks_extra.hasData(frameId);
      const bool hasActiveVector = dynamasks_extra_active.hasData(frameId);
      if (!hasSourceVector && !hasActiveVector) {
        continue;
      }
      const uint8_t *source = dynamasks_extra[frameId];
      const uint8_t *activeSource = dynamasks_extra_active[frameId];
      bool anyActive = false;
      for (size_t i = 0; i < extraFramePixels; ++i) {
        const uint8_t value = hasSourceVector ? source[i] : 0;
        const bool active =
            hasActiveVector ? (activeSource[i] > 0) : (value != 255);
        flags[i] = active ? 1 : 0;
        normalized[i] = active ? value : 0;
        anyActive = anyActive || active;
      }
      dynamasks_extra_active.set(frameId, flags.data(), extraFramePixels,
                                 &isextraframe);
      storeSidecarCopy(flags.data(), extraFramePixels);
      dynamasks_extra.set(frameId, normalized.data(), extraFramePixels,
                          &isextraframe, anyActive);
      frameHasDynamicExtra[frameId] = anyActive ? 1 : 0;
    }
  } else {
    frameHasDynamicExtra.assign(nframes, 0);
  }

  normalized.resize(spritePixels);
  flags.resize(spritePixels);
  for (uint32_t spriteId = 0; spriteId < nsprites; ++spriteId) {
    DebugLogSpriteDynamicSidecarState("normalize-before", spriteId);
    const bool hasSourceVector = dynaspritemasks.hasData(spriteId);
    const bool hasActiveVector = dynaspritemasks_active.hasData(spriteId);
    if (!hasSourceVector && !hasActiveVector) {
      continue;
    }
    const uint8_t *source = dynaspritemasks[spriteId];
    const uint8_t *activeSource = dynaspritemasks_active[spriteId];
    bool anyActive = false;
    for (size_t i = 0; i < spritePixels; ++i) {
      const uint8_t value = hasSourceVector ? source[i] : 0;
      const bool active =
          hasActiveVector ? (activeSource[i] > 0) : (value != 255);
      flags[i] = active ? 1 : 0;
      normalized[i] = active ? value : 0;
      anyActive = anyActive || active;
    }
    dynaspritemasks_active.set(spriteId, flags.data(), spritePixels);
    storeSidecarCopy(flags.data(), spritePixels);
    dynaspritemasks.set(spriteId, normalized.data(), spritePixels,
                        static_cast<SparseVector<uint8_t> *>(nullptr),
                        anyActive);
    DebugLogSpriteDynamicSidecarState("normalize-after", spriteId);
  }

  for (uint32_t spriteId = 0; spriteId < nsprites; ++spriteId) {
    if (isextrasprite[spriteId][0] == 0) {
      continue;
    }
    const bool hasSourceVector = dynaspritemasks_extra.hasData(spriteId);
    const bool hasActiveVector = dynaspritemasks_extra_active.hasData(spriteId);
    if (!hasSourceVector && !hasActiveVector) {
      continue;
    }
    const uint8_t *source = dynaspritemasks_extra[spriteId];
    const uint8_t *activeSource = dynaspritemasks_extra_active[spriteId];
    bool anyActive = false;
    for (size_t i = 0; i < spritePixels; ++i) {
      const uint8_t value = hasSourceVector ? source[i] : 0;
      const bool active =
          hasActiveVector ? (activeSource[i] > 0) : (value != 255);
      flags[i] = active ? 1 : 0;
      normalized[i] = active ? value : 0;
      anyActive = anyActive || active;
    }
    dynaspritemasks_extra_active.set(spriteId, flags.data(), spritePixels,
                                     &isextrasprite);
    storeSidecarCopy(flags.data(), spritePixels);
    dynaspritemasks_extra.set(spriteId, normalized.data(), spritePixels,
                              &isextrasprite, anyActive);
  }

  m_packingSidecarsNormalized = true;
}

bool SerumData::HasSpriteRuntimeSidecars() const {
  if (nframes == 0 || nsprites == 0) {
    return false;
  }
  if (spriteCandidateOffsets.size() != static_cast<size_t>(nframes) + 1) {
    return false;
  }
  if (spriteCandidateIds.size() != spriteCandidateSlots.size()) {
    return false;
  }
  if (frameHasShapeSprite.size() != nframes) {
    return false;
  }
  if (spriteWidth.size() != nsprites || spriteHeight.size() != nsprites ||
      spriteUsesShape.size() != nsprites) {
    return false;
  }
  if (spriteDetectOffsets.size() != static_cast<size_t>(nsprites) + 1) {
    return false;
  }
  if (spriteOpaqueRowSegmentStart.size() !=
          static_cast<size_t>(nsprites) * MAX_SPRITE_HEIGHT ||
      spriteOpaqueRowSegmentCount.size() !=
          static_cast<size_t>(nsprites) * MAX_SPRITE_HEIGHT) {
    return false;
  }
  return true;
}

void SerumData::BuildSpriteRuntimeSidecars() {
  auto storeRuntimeSidecarCopy = [this](const void *data, size_t size) {
    if (!data || size == 0 || m_packingSidecarsStorage.empty()) {
      return;
    }
    const auto *bytes = static_cast<const uint8_t *>(data);
    m_packingSidecarsStorage.emplace_back(bytes, bytes + size);
  };

  spriteCandidateOffsets.assign(static_cast<size_t>(nframes) + 1, 0);
  spriteCandidateIds.clear();
  spriteCandidateSlots.clear();
  frameHasShapeSprite.assign(nframes, 0);

  spriteWidth.assign(nsprites, 0);
  spriteHeight.assign(nsprites, 0);
  spriteUsesShape.assign(nsprites, 0);
  spriteDetectOffsets.assign(static_cast<size_t>(nsprites) + 1, 0);
  spriteDetectMeta.clear();
  spriteOpaqueRowSegmentStart.assign(
      static_cast<size_t>(nsprites) * MAX_SPRITE_HEIGHT, 0);
  spriteOpaqueRowSegmentCount.assign(
      static_cast<size_t>(nsprites) * MAX_SPRITE_HEIGHT, 0);
  spriteOpaqueSegments.clear();

  const size_t spritePixels = MAX_SPRITE_WIDTH * MAX_SPRITE_HEIGHT;
  for (uint32_t spriteId = 0; spriteId < nsprites; ++spriteId) {
    if (sprshapemode.hasData(spriteId) && sprshapemode[spriteId][0] > 0) {
      spriteUsesShape[spriteId] = 1;
    }

    if (!spriteoriginal_opaque.hasData(spriteId) ||
        !spriteoriginal.hasData(spriteId)) {
      spriteDetectOffsets[spriteId + 1] = spriteDetectOffsets[spriteId];
      continue;
    }

    const uint8_t *spriteOpaque = spriteoriginal_opaque[spriteId];
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < MAX_SPRITE_HEIGHT; ++y) {
      const uint32_t rowIndex =
          static_cast<uint32_t>(spriteId) * MAX_SPRITE_HEIGHT + y;
      spriteOpaqueRowSegmentStart[rowIndex] =
          static_cast<uint32_t>(spriteOpaqueSegments.size());
      uint16_t rowSegmentCount = 0;

      int x = 0;
      while (x < MAX_SPRITE_WIDTH) {
        const uint32_t pixelIndex = y * MAX_SPRITE_WIDTH + x;
        if (spriteOpaque[pixelIndex] == 0) {
          ++x;
          continue;
        }
        const int segmentStart = x;
        while (x < MAX_SPRITE_WIDTH &&
               spriteOpaque[y * MAX_SPRITE_WIDTH + x] > 0) {
          if (x > maxX) {
            maxX = x;
          }
          if (y > maxY) {
            maxY = y;
          }
          ++x;
        }
        const int segmentLength = x - segmentStart;
        spriteOpaqueSegments.push_back(static_cast<uint16_t>(segmentStart));
        spriteOpaqueSegments.push_back(static_cast<uint16_t>(segmentLength));
        ++rowSegmentCount;
      }
      spriteOpaqueRowSegmentCount[rowIndex] = rowSegmentCount;
    }

    spriteWidth[spriteId] = static_cast<uint16_t>(maxX >= 0 ? maxX + 1 : 0);
    spriteHeight[spriteId] = static_cast<uint16_t>(maxY >= 0 ? maxY + 1 : 0);

    const uint32_t detectStart = static_cast<uint32_t>(spriteDetectMeta.size());
    for (uint32_t detectIndex = 0; detectIndex < MAX_SPRITE_DETECT_AREAS;
         ++detectIndex) {
      const uint16_t *areas = spritedetareas[spriteId];
      if (areas[detectIndex * 4] == 0xffff) {
        continue;
      }
      SpriteDetectMeta meta;
      meta.detectionWord = spritedetdwords[spriteId][detectIndex];
      meta.detectionWordPos = spritedetdwordpos[spriteId][detectIndex];
      meta.detectX = areas[detectIndex * 4];
      meta.detectY = areas[detectIndex * 4 + 1];
      meta.detectWidth = areas[detectIndex * 4 + 2];
      meta.detectHeight = areas[detectIndex * 4 + 3];

      if (meta.detectWidth == 0 || meta.detectHeight == 0) {
        continue;
      }

      // Guard malformed data that would read beyond sprite storage.
      const uint32_t maxDetectX = static_cast<uint32_t>(meta.detectX) +
                                  static_cast<uint32_t>(meta.detectWidth);
      const uint32_t maxDetectY = static_cast<uint32_t>(meta.detectY) +
                                  static_cast<uint32_t>(meta.detectHeight);
      if (maxDetectX > MAX_SPRITE_WIDTH || maxDetectY > MAX_SPRITE_HEIGHT) {
        continue;
      }

      // Skip detect zones with no opaque pixels after normalization.
      bool hasOpaque = false;
      for (uint16_t dy = 0; dy < meta.detectHeight && !hasOpaque; ++dy) {
        const uint32_t row = static_cast<uint32_t>(meta.detectY + dy);
        for (uint16_t dx = 0; dx < meta.detectWidth; ++dx) {
          const uint32_t x = static_cast<uint32_t>(meta.detectX + dx);
          const uint32_t idx = row * MAX_SPRITE_WIDTH + x;
          if (idx < spritePixels && spriteOpaque[idx] > 0) {
            hasOpaque = true;
            break;
          }
        }
      }
      if (hasOpaque) {
        spriteDetectMeta.push_back(meta);
      }
    }
    spriteDetectOffsets[spriteId + 1] =
        static_cast<uint32_t>(spriteDetectMeta.size());
    if (spriteDetectOffsets[spriteId + 1] < detectStart) {
      spriteDetectOffsets[spriteId + 1] = detectStart;
    }
  }

  spriteCandidateIds.reserve(static_cast<size_t>(nframes) *
                             MAX_SPRITES_PER_FRAME / 2);
  spriteCandidateSlots.reserve(static_cast<size_t>(nframes) *
                               MAX_SPRITES_PER_FRAME / 2);
  for (uint32_t frameId = 0; frameId < nframes; ++frameId) {
    spriteCandidateOffsets[frameId] =
        static_cast<uint32_t>(spriteCandidateIds.size());
    std::unordered_set<uint8_t> dedupe;
    dedupe.reserve(MAX_SPRITES_PER_FRAME);
    for (uint32_t i = 0; i < MAX_SPRITES_PER_FRAME; ++i) {
      const uint8_t spriteId = framesprites[frameId][i];
      if (spriteId >= 255 || spriteId >= nsprites) {
        break;
      }
      if (!spriteoriginal.hasData(spriteId) ||
          !spriteoriginal_opaque.hasData(spriteId)) {
        continue;
      }
      if (spriteDetectOffsets[spriteId] == spriteDetectOffsets[spriteId + 1]) {
        continue;
      }
      if (!dedupe.insert(spriteId).second) {
        continue;
      }
      spriteCandidateIds.push_back(spriteId);
      spriteCandidateSlots.push_back(static_cast<uint8_t>(i));
      frameHasShapeSprite[frameId] =
          frameHasShapeSprite[frameId] || spriteUsesShape[spriteId];
    }
  }
  spriteCandidateOffsets[nframes] =
      static_cast<uint32_t>(spriteCandidateIds.size());

  storeRuntimeSidecarCopy(
      spriteCandidateOffsets.data(),
      spriteCandidateOffsets.size() * sizeof(spriteCandidateOffsets[0]));
  storeRuntimeSidecarCopy(
      spriteCandidateIds.data(),
      spriteCandidateIds.size() * sizeof(spriteCandidateIds[0]));
  storeRuntimeSidecarCopy(
      spriteCandidateSlots.data(),
      spriteCandidateSlots.size() * sizeof(spriteCandidateSlots[0]));
  storeRuntimeSidecarCopy(
      frameHasShapeSprite.data(),
      frameHasShapeSprite.size() * sizeof(frameHasShapeSprite[0]));
  storeRuntimeSidecarCopy(spriteWidth.data(),
                          spriteWidth.size() * sizeof(spriteWidth[0]));
  storeRuntimeSidecarCopy(spriteHeight.data(),
                          spriteHeight.size() * sizeof(spriteHeight[0]));
  storeRuntimeSidecarCopy(spriteUsesShape.data(),
                          spriteUsesShape.size() * sizeof(spriteUsesShape[0]));
  storeRuntimeSidecarCopy(
      spriteDetectOffsets.data(),
      spriteDetectOffsets.size() * sizeof(spriteDetectOffsets[0]));
  storeRuntimeSidecarCopy(
      spriteDetectMeta.data(),
      spriteDetectMeta.size() * sizeof(spriteDetectMeta[0]));
  storeRuntimeSidecarCopy(spriteOpaqueRowSegmentStart.data(),
                          spriteOpaqueRowSegmentStart.size() *
                              sizeof(spriteOpaqueRowSegmentStart[0]));
  storeRuntimeSidecarCopy(spriteOpaqueRowSegmentCount.data(),
                          spriteOpaqueRowSegmentCount.size() *
                              sizeof(spriteOpaqueRowSegmentCount[0]));
  storeRuntimeSidecarCopy(
      spriteOpaqueSegments.data(),
      spriteOpaqueSegments.size() * sizeof(spriteOpaqueSegments[0]));
}

void SerumData::LogSparseVectorProfileSnapshot() {
  auto logCounters = [&](auto &vec) {
    uint64_t accesses = 0;
    uint64_t decodes = 0;
    uint64_t cacheHits = 0;
    uint64_t directHits = 0;
    vec.consumeProfileCounters(accesses, decodes, cacheHits, directHits);
    const char *label = vec.getProfileLabel();
    if (!label || accesses == 0) {
      return;
    }
    Log("SparseProfile %s: accesses=%llu decodes=%llu cacheHits=%llu "
        "direct=%llu",
        label, (unsigned long long)accesses, (unsigned long long)decodes,
        (unsigned long long)cacheHits, (unsigned long long)directHits);
  };

  logCounters(cframes_v2);
  logCounters(cframes_v2_extra);
  logCounters(backgroundmask);
  logCounters(backgroundmask_extra);
  logCounters(dynamasks);
  logCounters(dynamasks_active);
  logCounters(dynamasks_extra);
  logCounters(dynamasks_extra_active);
  logCounters(dynaspritemasks);
  logCounters(dynaspritemasks_active);
  logCounters(dynaspritemasks_extra);
  logCounters(dynaspritemasks_extra_active);
}

void SerumData::BuildColorRotationLookup() {
  colorRotationLookupByFrameAndColor.clear();
  if (SerumVersion != SERUM_V2 || nframes == 0) {
    return;
  }

  colorRotationLookupByFrameAndColor.reserve(nframes * 8);
  auto buildPlane = [&](bool isextra) {
    for (uint32_t frameId = 0; frameId < nframes; ++frameId) {
      uint16_t *pcol = isextra ? colorrotations_v2_extra[frameId]
                               : colorrotations_v2[frameId];
      for (uint32_t rot = 0; rot < MAX_COLOR_ROTATION_V2; ++rot) {
        const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
        const uint16_t length = pcol[base];
        for (uint16_t pos = 0; pos < length; ++pos) {
          const uint16_t color = pcol[base + 2 + pos];
          const uint64_t key = (uint64_t(frameId) << 17) |
                               (uint64_t(isextra ? 1 : 0) << 16) | color;
          // Keep first assignment in case of duplicates.
          if (colorRotationLookupByFrameAndColor.find(key) ==
              colorRotationLookupByFrameAndColor.end()) {
            colorRotationLookupByFrameAndColor[key] =
                static_cast<uint16_t>((rot << 8) | (pos & 0xff));
          }
        }
      }
    }
  };

  buildPlane(false);
  buildPlane(true);
}

bool SerumData::TryGetColorRotation(uint32_t frameId, uint16_t color,
                                    bool isextra, uint16_t &rotationIndex,
                                    uint16_t &positionInRotation) const {
  const uint64_t key =
      (uint64_t(frameId) << 17) | (uint64_t(isextra ? 1 : 0) << 16) | color;
  auto it = colorRotationLookupByFrameAndColor.find(key);
  if (it == colorRotationLookupByFrameAndColor.end()) {
    return false;
  }
  rotationIndex = static_cast<uint16_t>((it->second >> 8) & 0xff);
  positionInRotation = static_cast<uint16_t>(it->second & 0xff);
  return true;
}

bool SerumData::SaveToFile(const char *filename) {
  try {
    BuildPackingSidecarsAndNormalize();
    if (!HasSpriteRuntimeSidecars()) {
      BuildSpriteRuntimeSidecars();
    }
    Log("Writing %s", filename);
    // Serialize to memory buffer first
    std::ostringstream ss(std::ios::binary);
    {
      cereal::PortableBinaryOutputArchive archive(ss);
      archive(*this);
    }
    std::string data = ss.str();

    // Compress data - use uint32_t for consistent sizes
    uint32_t srcLen = (uint32_t)data.size();
    mz_ulong dstLen = compressBound(srcLen);
    std::vector<unsigned char> compressedData(dstLen);

    int status = compress2(compressedData.data(), &dstLen,
                           (const unsigned char *)data.data(), srcLen,
                           MZ_BEST_COMPRESSION);

    if (status != MZ_OK) {
      Log("Compression error: %d", status);
      return false;
    }

    // Write compressed data to file
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
      Log("Failed to open %s for writing", filename);
      return false;
    }

    // Write magic string first
    const char magic[] = "CROM";
    fwrite(magic, 1, 4, fp);

    // Write version
    uint16_t littleVersion = ToLittleEndian16(concentrateFileVersion);
    fwrite(&littleVersion, sizeof(uint16_t), 1, fp);

    // Write original size
    uint32_t littleEndianSize =
        ToLittleEndian32((uint32_t)srcLen);  // Use srcLen directly
    fwrite(&littleEndianSize, sizeof(uint32_t), 1, fp);

    // Write compressed data
    fwrite(compressedData.data(), 1, dstLen, fp);
    fclose(fp);

    Log("Writing %s finished", filename);
    return true;
  } catch (const std::exception &e) {
    Log("Exception when writing %s: %s", filename, e.what());
    return false;
  } catch (...) {
    Log("Failed to write %s", filename);
    return false;
  }
}

bool SerumData::LoadFromFile(const char *filename, const uint8_t flags) {
  m_loadFlags = flags;
  m_packingSidecarsNormalized = false;
  FILE *fp;
  try {
    fp = fopen(filename, "rb");
    if (!fp) {
      Log("Failed to open %s", filename);
      return false;
    }

    // Read and verify magic string
    char magic[5] = {0};
    if (fread(magic, 1, 4, fp) != 4 || strcmp(magic, "CROM") != 0) {
      Log("Wrong header in %s", filename);
      fclose(fp);
      return false;
    }

    uint16_t littleEndianVersion;
    if (fread(&littleEndianVersion, sizeof(uint16_t), 1, fp) != 1) {
      Log("Failed to detect cROMc version of %s", filename);
      fclose(fp);
      return false;
    }
    concentrateFileVersion = FromLittleEndian16(littleEndianVersion);
    Log("cROMc version %d", concentrateFileVersion);

    if (concentrateFileVersion < 5) {
      Log("The cROMc version of %s is too old. Get a newer version. If you "
          "have a cROM or cRZ file next to the cROMc file, delete the cROMc "
          "file and restart. An updated cROMc will be generated.",
          filename);
      fclose(fp);
      return false;
    }

    if (concentrateFileVersion > SERUM_CONCENTRATE_VERSION) {
      Log("The cROMc version of %s is newer than the maximum supported by this "
          "version of libserum. Get a newer version of libserum.",
          filename);
      fclose(fp);
      return false;
    }

    // Read original size
    uint32_t littleEndianSize;
    if (fread(&littleEndianSize, sizeof(uint32_t), 1, fp) != 1) {
      Log("Failed to detect cROMc size of %s", filename);
      fclose(fp);
      return false;
    }
    uint32_t originalSize = FromLittleEndian32(littleEndianSize);
    Log("cROMc size %u", originalSize);

    // Get total file size - use portable types
    fseek(fp, 0, SEEK_END);
    long totalSizeLong = ftell(fp);
    if (totalSizeLong < 0) {
      Log("Failed to get file size for %s", filename);
      fclose(fp);
      return false;
    }
    if (totalSizeLong > UINT32_MAX) {
      Log("File exceeds size limit %s", filename);
      fclose(fp);
      return false;
    }
    uint32_t totalSize = (uint32_t)totalSizeLong;

    // Adjust for magic(4) + version bytes(2) + size bytes(4)
    uint32_t headerSize = 4 + sizeof(uint16_t) + sizeof(uint32_t);
    fseek(fp, headerSize, SEEK_SET);

    // Calculate compressed size
    if (totalSize <= headerSize) {
      Log("File too small in %s", filename);
      fclose(fp);
      return false;
    }
    uint32_t compressedSize = totalSize - headerSize;

    // Validate sizes
    if (compressedSize == 0 || originalSize == 0) {
      Log("Invalid file size detected in %s", filename);
      fclose(fp);
      return false;
    }
    Log("cROMc compressed size %u", compressedSize);

    // Create a custom stream that decompresses on the fly
    DecompressingIStream decompStream(fp, compressedSize, originalSize);
    struct LegacyLoadFlagGuard {
      explicit LegacyLoadFlagGuard(bool legacy) {
        sparse_vector_serialization::SetLegacyLoadExpected(legacy);
      }
      ~LegacyLoadFlagGuard() {
        sparse_vector_serialization::SetLegacyLoadExpected(false);
      }
    } legacyLoadGuard(concentrateFileVersion <= 5);

    // Deserialize directly from the decompressing stream
    {
      cereal::PortableBinaryInputArchive archive(decompStream);
      archive(*this);
    }

    fclose(fp);
    return true;
  } catch (const std::exception &e) {
    Log("Exception when opening %s: %s", filename, e.what());
    if (fp) fclose(fp);
    return false;
  } catch (...) {
    Log("Unknown exception when opening %s", filename);
    if (fp) fclose(fp);
    return false;
  }
}

bool SerumData::LoadFromBuffer(const uint8_t *data, size_t size,
                               const uint8_t flags) {
  m_loadFlags = flags;
  m_packingSidecarsNormalized = false;

  try {
    if (!data || size < (4 + sizeof(uint16_t) + sizeof(uint32_t))) {
      Log("Buffer too small");
      return false;
    }

    // Read and verify magic string
    if (memcmp(data, "CROM", 4) != 0) {
      Log("Wrong header");
      return false;
    }

    uint16_t littleEndianVersion;
    memcpy(&littleEndianVersion, data + 4, sizeof(uint16_t));
    concentrateFileVersion = FromLittleEndian16(littleEndianVersion);
    Log("cROMc version %d", concentrateFileVersion);

    if (concentrateFileVersion < 5) {
      Log("The cROMc version is too old. Get a newer version.");
      return false;
    }

    if (concentrateFileVersion > SERUM_CONCENTRATE_VERSION) {
      Log("The cROMc version is newer than supported by this libserum.");
      return false;
    }

    uint32_t littleEndianSize;
    memcpy(&littleEndianSize, data + 4 + sizeof(uint16_t), sizeof(uint32_t));
    const uint32_t originalSize = FromLittleEndian32(littleEndianSize);
    Log("cROMc size %u", originalSize);

    const size_t headerSize = 4 + sizeof(uint16_t) + sizeof(uint32_t);
    if (size <= headerSize) {
      Log("File too small");
      return false;
    }

    const size_t compressedSize = size - headerSize;
    if (compressedSize == 0 || originalSize == 0) {
      Log("Invalid file size detected");
      return false;
    }
    Log("cROMc compressed size %u", (uint32_t)compressedSize);

    std::string decompressed;
    decompressed.resize(originalSize);
    mz_ulong dstLen = originalSize;
    const int status =
        uncompress(reinterpret_cast<unsigned char *>(decompressed.data()),
                   &dstLen, data + headerSize, (mz_ulong)compressedSize);
    if (status != MZ_OK || dstLen != originalSize) {
      Log("Decompression error: %d", status);
      return false;
    }

    std::istringstream iss(decompressed, std::ios::binary);
    struct LegacyLoadFlagGuard {
      explicit LegacyLoadFlagGuard(bool legacy) {
        sparse_vector_serialization::SetLegacyLoadExpected(legacy);
      }
      ~LegacyLoadFlagGuard() {
        sparse_vector_serialization::SetLegacyLoadExpected(false);
      }
    } legacyLoadGuard(concentrateFileVersion <= 5);

    {
      cereal::PortableBinaryInputArchive archive(iss);
      archive(*this);
    }

    return true;
  } catch (const std::exception &e) {
    Log("Exception when loading: %s", e.what());
    return false;
  } catch (...) {
    Log("Unknown exception when loading");
    return false;
  }
}

void SerumData::Log(const char *format, ...) {
  if (!m_logCallback) {
    return;
  }

  va_list args;
  va_start(args, format);
  (*(m_logCallback))(format, args, m_logUserData);
  va_end(args);
}
