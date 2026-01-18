#pragma once

#ifdef _MSC_VER
#define SERUM_EDITOR_API extern "C" __declspec(dllexport)
#else
#define SERUM_EDITOR_API extern "C" __attribute__((visibility("default")))
#endif

#include <cstdint>

#ifdef MAX_DYNA_SETS_PER_SPRITE
#undef MAX_DYNA_SETS_PER_SPRITE
#endif
#ifdef MAX_SPRITE_WIDTH
#undef MAX_SPRITE_WIDTH
#endif
#ifdef MAX_SPRITE_HEIGHT
#undef MAX_SPRITE_HEIGHT
#endif
#ifdef MAX_SPRITES_PER_FRAME
#undef MAX_SPRITES_PER_FRAME
#endif
#ifdef MAX_LENGTH_COLOR_ROTATION
#undef MAX_LENGTH_COLOR_ROTATION
#endif
#ifdef MAX_SPRITE_DETECT_AREAS
#undef MAX_SPRITE_DETECT_AREAS
#endif

#include "serum.h"

struct SerumEditorSpriteView {
  const uint8_t* original = nullptr;
  const uint16_t* colored = nullptr;
  const uint8_t* mask_extra = nullptr;
  const uint16_t* colored_extra = nullptr;
  const uint8_t* dynasprite_mask = nullptr;
  const uint8_t* dynasprite_mask_extra = nullptr;
  const uint16_t* dynasprite_cols = nullptr;
  const uint16_t* dynasprite_cols_extra = nullptr;
  const uint16_t* det_areas = nullptr;
  const uint32_t* det_dwords = nullptr;
  const uint16_t* det_dword_pos = nullptr;
  uint8_t shape_mode = 0;
};

struct SerumEditorFrameView {
  const uint8_t* original = nullptr;
  const uint16_t* colorized = nullptr;
  const uint16_t* colorized_extra = nullptr;
  const uint8_t* dynamask = nullptr;
  const uint8_t* dynamask_extra = nullptr;
  const uint16_t* dyna4cols = nullptr;
  const uint16_t* dyna4cols_extra = nullptr;
  uint16_t background_id = 0xffff;
  const uint8_t* background_mask = nullptr;
  const uint8_t* background_mask_extra = nullptr;
  const uint16_t* background_frame = nullptr;
  const uint16_t* background_frame_extra = nullptr;
  const uint8_t* frame_sprites = nullptr;
  const uint16_t* frame_sprite_bboxes = nullptr;
};

struct SerumEditorDataView {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t width_extra = 0;
  uint32_t height_extra = 0;
  uint32_t nocolors = 0;
  uint32_t nsprites = 0;
  const SerumEditorSpriteView* sprites = nullptr;
};

struct SerumEditorSpriteMatch {
  uint8_t sprite_index = 255;
  uint16_t frx = 0;
  uint16_t fry = 0;
  uint16_t spx = 0;
  uint16_t spy = 0;
  uint16_t wid = 0;
  uint16_t hei = 0;
};

struct SerumEditorRotationState {
  uint32_t next_time_ms[MAX_COLOR_ROTATION_V2] = {};
  uint16_t shift[MAX_COLOR_ROTATION_V2] = {};
  uint8_t active[MAX_COLOR_ROTATION_V2] = {};
};

SERUM_EDITOR_API uint8_t SerumEditor_MatchSprites(
    const SerumEditorDataView* data, const SerumEditorFrameView* frame,
    SerumEditorSpriteMatch* matches, uint8_t max_matches);

SERUM_EDITOR_API bool SerumEditor_RenderFrame(
    const SerumEditorDataView* data, const SerumEditorFrameView* frame,
    const SerumEditorSpriteMatch* matches, uint8_t match_count, bool use_extra,
    uint16_t* out_frame);

SERUM_EDITOR_API bool SerumEditor_RenderFrameWithRotations(
    const SerumEditorDataView* data, const SerumEditorFrameView* frame,
    const SerumEditorSpriteMatch* matches, uint8_t match_count, bool use_extra,
    const uint16_t* rotations, const uint32_t* shifts, uint16_t* out_frame,
    uint16_t* rotations_in_frame);

SERUM_EDITOR_API void SerumEditor_InitRotationState(
    const uint16_t* rotations, SerumEditorRotationState* state,
    uint32_t now_ms);

SERUM_EDITOR_API uint32_t SerumEditor_ApplyRotations(
    const uint16_t* rotations, const uint16_t* input_frame,
    uint16_t* output_frame, uint32_t width, uint32_t height,
    SerumEditorRotationState* state, uint32_t now_ms);

SERUM_EDITOR_API uint32_t SerumEditor_ApplyRotationsMasked(
    const uint16_t* rotations, const uint16_t* input_frame,
    uint16_t* output_frame, uint16_t* rotations_in_frame, uint32_t width,
    uint32_t height, SerumEditorRotationState* state, uint32_t now_ms);
