#include "serum-editor.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "serum-render-v2.h"
namespace {

void GetSpriteSize(uint8_t sprite, int* out_width, int* out_height,
                   const uint8_t* spriteData, int max_width, int max_height) {
  *out_width = 0;
  *out_height = 0;
  if (!spriteData) {
    return;
  }
  for (int y = 0; y < max_height; ++y) {
    for (int x = 0; x < max_width; ++x) {
      if (spriteData[y * max_width + x] < 255) {
        if (y > *out_height) *out_height = y;
        if (x > *out_width) *out_width = x;
      }
    }
  }
  (*out_height)++;
  (*out_width)++;
}

uint8_t normalizedSpriteValue(uint8_t value, bool shape_mode) {
  if (!shape_mode) {
    return value;
  }
  if (value == 255) {
    return 255;
  }
  return value > 0 ? 1 : 0;
}

uint8_t normalizedFrameValue(uint8_t value, bool shape_mode) {
  if (!shape_mode) {
    return value;
  }
  return value > 0 ? 1 : 0;
}

bool MatchSprite(const SerumEditorDataView* data,
                 const SerumEditorFrameView* frame,
                 const SerumEditorSpriteView& sprite, uint8_t sprite_index,
                 const uint16_t* sprite_bb, SerumEditorSpriteMatch* matches,
                 uint8_t* match_count, uint8_t max_matches) {
  if (!data || !frame || !matches || !match_count) {
    return false;
  }
  if (!frame->original || !sprite.original || !sprite.det_areas ||
      !sprite.det_dwords || !sprite.det_dword_pos) {
    return false;
  }
  const bool shape_mode = sprite.shape_mode > 0;

  int spw = 0;
  int sph = 0;
  GetSpriteSize(sprite_index, &spw, &sph, sprite.original, MAX_SPRITE_WIDTH,
                MAX_SPRITE_HEIGHT);

  const short minxBB = static_cast<short>(sprite_bb[0]);
  const short minyBB = static_cast<short>(sprite_bb[1]);
  const short maxxBB = static_cast<short>(sprite_bb[2]);
  const short maxyBB = static_cast<short>(sprite_bb[3]);

  for (uint32_t area = 0; area < MAX_SPRITE_DETECT_AREAS; ++area) {
    if (sprite.det_areas[area * 4] == 0xffff) {
      continue;
    }
    for (short ty = minyBB; ty <= maxyBB; ++ty) {
      uint32_t mdword =
          (static_cast<uint32_t>(normalizedFrameValue(
               frame->original[ty * data->width + minxBB], shape_mode))
           << 8) |
          (static_cast<uint32_t>(normalizedFrameValue(
               frame->original[ty * data->width + minxBB + 1], shape_mode))
           << 16) |
          (static_cast<uint32_t>(normalizedFrameValue(
               frame->original[ty * data->width + minxBB + 2], shape_mode))
           << 24);
      for (short tx = minxBB; tx <= maxxBB - 3; ++tx) {
        uint32_t tj = static_cast<uint32_t>(ty) * data->width + tx;
        mdword = (mdword >> 8) | (static_cast<uint32_t>(normalizedFrameValue(
                                      frame->original[tj + 3], shape_mode))
                                  << 24);
        const uint16_t sddp = sprite.det_dword_pos[area];
        if (mdword != sprite.det_dwords[area]) {
          continue;
        }
        short frax = tx;
        short fray = ty;
        short sprx = static_cast<short>(sddp % MAX_SPRITE_WIDTH);
        short spry = static_cast<short>(sddp / MAX_SPRITE_WIDTH);
        short detx = static_cast<short>(sprite.det_areas[area * 4]);
        short dety = static_cast<short>(sprite.det_areas[area * 4 + 1]);
        short detw = static_cast<short>(sprite.det_areas[area * 4 + 2]);
        short deth = static_cast<short>(sprite.det_areas[area * 4 + 3]);

        if ((frax - minxBB < sprx - detx) || (fray - minyBB < spry - dety)) {
          continue;
        }
        const int offsx = frax - sprx + detx;
        const int offsy = fray - spry + dety;
        if ((offsx + detw > static_cast<int>(maxxBB) + 1) ||
            (offsy + deth > static_cast<int>(maxyBB) + 1)) {
          continue;
        }
        bool notthere = false;
        for (uint16_t y = 0; y < deth; ++y) {
          for (uint16_t x = 0; x < detw; ++x) {
            const uint8_t val = normalizedSpriteValue(
                sprite.original[(y + dety) * MAX_SPRITE_WIDTH + x + detx],
                shape_mode);
            if (val == 255) {
              continue;
            }
            const uint8_t frame_val = normalizedFrameValue(
                frame->original[(y + offsy) * data->width + x + offsx],
                shape_mode);
            if (val != frame_val) {
              notthere = true;
              break;
            }
          }
          if (notthere) {
            break;
          }
        }
        if (notthere) {
          continue;
        }

        SerumEditorSpriteMatch match;
        match.sprite_index = sprite_index;
        if (frax - minxBB < sprx) {
          match.spx = static_cast<uint16_t>(sprx - (frax - minxBB));
          match.frx = static_cast<uint16_t>(minxBB);
          match.wid = static_cast<uint16_t>(
              std::min(spw - match.spx, (maxxBB - minxBB + 1)));
        } else {
          match.spx = 0;
          match.frx = static_cast<uint16_t>(frax - sprx);
          match.wid = static_cast<uint16_t>(
              std::min(spw, static_cast<int>(maxxBB - match.frx + 1)));
        }
        if (fray - minyBB < spry) {
          match.spy = static_cast<uint16_t>(spry - (fray - minyBB));
          match.fry = static_cast<uint16_t>(minyBB);
          match.hei = static_cast<uint16_t>(
              std::min(sph - match.spy, (maxyBB - minyBB + 1)));
        } else {
          match.spy = 0;
          match.fry = static_cast<uint16_t>(fray - spry);
          match.hei = static_cast<uint16_t>(
              std::min(sph, static_cast<int>(maxyBB - match.fry + 1)));
        }

        bool identical = false;
        for (uint8_t i = 0; i < *match_count; ++i) {
          if (matches[i].sprite_index == match.sprite_index &&
              matches[i].frx == match.frx && matches[i].fry == match.fry &&
              matches[i].wid == match.wid && matches[i].hei == match.hei) {
            identical = true;
            break;
          }
        }
        if (!identical) {
          matches[*match_count] = match;
          (*match_count)++;
          if (*match_count >= max_matches) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

}  // namespace

SERUM_EDITOR_API uint8_t SerumEditor_MatchSprites(
    const SerumEditorDataView* data, const SerumEditorFrameView* frame,
    SerumEditorSpriteMatch* matches, uint8_t max_matches) {
  if (!data || !frame || !matches || !frame->frame_sprites ||
      !frame->frame_sprite_bboxes || !data->sprites) {
    return 0;
  }
  uint8_t count = 0;
  for (uint8_t slot = 0; slot < MAX_SPRITES_PER_FRAME; ++slot) {
    const uint8_t sprite_id = frame->frame_sprites[slot];
    if (sprite_id == 255 || sprite_id >= data->nsprites) {
      continue;
    }
    const SerumEditorSpriteView& sprite = data->sprites[sprite_id];
    if (!sprite.original) {
      continue;
    }
    const uint16_t* bb =
        frame->frame_sprite_bboxes + static_cast<size_t>(slot) * 4;
    MatchSprite(data, frame, sprite, sprite_id, bb, matches, &count,
                max_matches);
    if (count >= max_matches) {
      break;
    }
  }
  return count;
}

SERUM_EDITOR_API bool SerumEditor_RenderFrame(
    const SerumEditorDataView* data, const SerumEditorFrameView* frame,
    const SerumEditorSpriteMatch* matches, uint8_t match_count, bool use_extra,
    uint16_t* out_frame) {
  if (!data || !frame || !out_frame || !frame->original) {
    return false;
  }
  const uint32_t width = use_extra ? data->width_extra : data->width;
  const uint32_t height = use_extra ? data->height_extra : data->height;
  if (width == 0 || height == 0) {
    return false;
  }

  SerumV2RenderFrameInput input{};
  input.width = width;
  input.height = height;
  input.base_width = data->width;
  input.base_height = data->height;
  input.nocolors = data->nocolors;
  input.base_frame = frame->original;
  input.colorized = use_extra ? frame->colorized_extra : frame->colorized;
  input.dyna_mask = use_extra ? frame->dynamask_extra : frame->dynamask;
  input.dyna_colors = use_extra ? frame->dyna4cols_extra : frame->dyna4cols;
  if (frame->background_id != 0xffff) {
    input.background_mask =
        use_extra ? frame->background_mask_extra : frame->background_mask;
    input.background_frame =
        use_extra ? frame->background_frame_extra : frame->background_frame;
  }
  SerumV2_RenderFrame(&input, nullptr, out_frame, nullptr);

  if (matches && data->sprites) {
    for (uint8_t i = 0; i < match_count; ++i) {
      const uint8_t sprite_id = matches[i].sprite_index;
      if (sprite_id == 255 || sprite_id >= data->nsprites) {
        continue;
      }
      const SerumEditorSpriteView& sprite = data->sprites[sprite_id];
      SerumV2RenderSpriteInput sprite_input;
      sprite_input.sprite_original = sprite.original;
      sprite_input.sprite_colored = sprite.colored;
      sprite_input.sprite_mask_extra = sprite.mask_extra;
      sprite_input.sprite_colored_extra = sprite.colored_extra;
      sprite_input.dynasprite_mask = sprite.dynasprite_mask;
      sprite_input.dynasprite_mask_extra = sprite.dynasprite_mask_extra;
      sprite_input.dynasprite_cols = sprite.dynasprite_cols;
      sprite_input.dynasprite_cols_extra = sprite.dynasprite_cols_extra;
      SerumV2SpritePlacement placement;
      placement.frx = matches[i].frx;
      placement.fry = matches[i].fry;
      placement.spx = matches[i].spx;
      placement.spy = matches[i].spy;
      placement.wid = matches[i].wid;
      placement.hei = matches[i].hei;
      SerumV2_RenderSprite(&input, &sprite_input, &placement, nullptr,
                           out_frame, nullptr);
    }
  }
  return true;
}

SERUM_EDITOR_API bool SerumEditor_RenderFrameWithRotations(
    const SerumEditorDataView* data, const SerumEditorFrameView* frame,
    const SerumEditorSpriteMatch* matches, uint8_t match_count, bool use_extra,
    const uint16_t* rotations, const uint32_t* shifts, uint16_t* out_frame,
    uint16_t* rotations_in_frame) {
  if (!data || !frame || !out_frame || !frame->original) {
    return false;
  }
  const uint32_t width = use_extra ? data->width_extra : data->width;
  const uint32_t height = use_extra ? data->height_extra : data->height;
  if (width == 0 || height == 0) {
    return false;
  }

  SerumV2RenderFrameInput input{};
  input.width = width;
  input.height = height;
  input.base_width = data->width;
  input.base_height = data->height;
  input.nocolors = data->nocolors;
  input.base_frame = frame->original;
  input.colorized = use_extra ? frame->colorized_extra : frame->colorized;
  input.dyna_mask = use_extra ? frame->dynamask_extra : frame->dynamask;
  input.dyna_colors = use_extra ? frame->dyna4cols_extra : frame->dyna4cols;
  if (frame->background_id != 0xffff) {
    input.background_mask =
        use_extra ? frame->background_mask_extra : frame->background_mask;
    input.background_frame =
        use_extra ? frame->background_frame_extra : frame->background_frame;
  }

  SerumV2RenderRotationInput rotation{};
  rotation.rotations = rotations;
  rotation.shifts = shifts;
  SerumV2_RenderFrame(&input, rotations ? &rotation : nullptr, out_frame,
                      rotations_in_frame);

  if (matches && data->sprites) {
    for (uint8_t i = 0; i < match_count; ++i) {
      const uint8_t sprite_id = matches[i].sprite_index;
      if (sprite_id == 255 || sprite_id >= data->nsprites) {
        continue;
      }
      const SerumEditorSpriteView& sprite = data->sprites[sprite_id];
      SerumV2RenderSpriteInput sprite_input;
      sprite_input.sprite_original = sprite.original;
      sprite_input.sprite_colored = sprite.colored;
      sprite_input.sprite_mask_extra = sprite.mask_extra;
      sprite_input.sprite_colored_extra = sprite.colored_extra;
      sprite_input.dynasprite_mask = sprite.dynasprite_mask;
      sprite_input.dynasprite_mask_extra = sprite.dynasprite_mask_extra;
      sprite_input.dynasprite_cols = sprite.dynasprite_cols;
      sprite_input.dynasprite_cols_extra = sprite.dynasprite_cols_extra;
      SerumV2SpritePlacement placement;
      placement.frx = matches[i].frx;
      placement.fry = matches[i].fry;
      placement.spx = matches[i].spx;
      placement.spy = matches[i].spy;
      placement.wid = matches[i].wid;
      placement.hei = matches[i].hei;
      SerumV2_RenderSprite(&input, &sprite_input, &placement,
                           rotations ? &rotation : nullptr, out_frame,
                           rotations_in_frame);
    }
  }
  return true;
}

SERUM_EDITOR_API void SerumEditor_InitRotationState(
    const uint16_t* rotations, SerumEditorRotationState* state,
    uint32_t now_ms) {
  if (!state) {
    return;
  }
  SerumV2_InitRotationState(rotations, state->next_time_ms, state->shift,
                            state->active, now_ms);
}

SERUM_EDITOR_API uint32_t SerumEditor_ApplyRotations(
    const uint16_t* rotations, const uint16_t* input_frame,
    uint16_t* output_frame, uint32_t width, uint32_t height,
    SerumEditorRotationState* state, uint32_t now_ms) {
  if (!rotations || !input_frame || !output_frame || width == 0 ||
      height == 0 || !state) {
    return 0;
  }
  std::memcpy(output_frame, input_frame,
              static_cast<std::size_t>(width) * height * sizeof(uint16_t));

  return SerumV2_ApplyRotations(
      rotations, output_frame, nullptr, static_cast<uint32_t>(width) * height,
      state->next_time_ms, state->shift, state->active, now_ms, nullptr,
      nullptr, true, nullptr);
}

SERUM_EDITOR_API uint32_t SerumEditor_ApplyRotationsMasked(
    const uint16_t* rotations, const uint16_t* input_frame,
    uint16_t* output_frame, uint16_t* rotations_in_frame, uint32_t width,
    uint32_t height, SerumEditorRotationState* state, uint32_t now_ms) {
  if (!rotations || !input_frame || !output_frame || width == 0 ||
      height == 0 || !state || !rotations_in_frame) {
    return 0;
  }
  std::memcpy(output_frame, input_frame,
              static_cast<std::size_t>(width) * height * sizeof(uint16_t));

  return SerumV2_ApplyRotations(
      rotations, output_frame, rotations_in_frame,
      static_cast<uint32_t>(width) * height, state->next_time_ms, state->shift,
      state->active, now_ms, nullptr, nullptr, true, nullptr);
}
