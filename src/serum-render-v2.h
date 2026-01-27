#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "serum.h"

struct SerumV2RenderFrameInput {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t base_width = 0;
  uint32_t base_height = 0;
  uint32_t nocolors = 0;
  const uint8_t* base_frame = nullptr;
  const uint16_t* colorized = nullptr;
  const uint8_t* dyna_mask = nullptr;
  const uint16_t* dyna_colors = nullptr;
  const uint8_t* background_mask = nullptr;
  const uint16_t* background_frame = nullptr;
  bool black_out_static_content = false;
  const uint8_t* dynashadows_dir = nullptr;
  const uint16_t* dynashadows_col = nullptr;
};

struct SerumV2RenderSpriteInput {
  const uint8_t* sprite_original = nullptr;
  const uint16_t* sprite_colored = nullptr;
  const uint8_t* sprite_mask_extra = nullptr;
  const uint16_t* sprite_colored_extra = nullptr;
  const uint8_t* dynasprite_mask = nullptr;
  const uint8_t* dynasprite_mask_extra = nullptr;
  const uint16_t* dynasprite_cols = nullptr;
  const uint16_t* dynasprite_cols_extra = nullptr;
};

struct SerumV2SpritePlacement {
  uint16_t frx = 0;
  uint16_t fry = 0;
  uint16_t spx = 0;
  uint16_t spy = 0;
  uint16_t wid = 0;
  uint16_t hei = 0;
};

struct SerumV2RenderRotationInput {
  const uint16_t* rotations = nullptr;
  const uint32_t* shifts = nullptr;
};

inline bool SerumV2_ColorInRotation(const uint16_t* rotations, uint16_t color,
                                    uint16_t* out_rot, uint16_t* out_pos) {
  if (!out_rot || !out_pos) {
    return false;
  }
  *out_rot = 0xffff;
  *out_pos = 0;
  if (!rotations) {
    return false;
  }
  for (uint32_t rot = 0; rot < MAX_COLOR_ROTATION_V2; ++rot) {
    const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
    const uint16_t length = rotations[base];
    for (uint32_t pos = 0; pos < length; ++pos) {
      if (color == rotations[base + 2 + pos]) {
        *out_rot = static_cast<uint16_t>(rot);
        *out_pos = static_cast<uint16_t>(pos);
        return true;
      }
    }
  }
  return false;
}

inline void SerumV2_RenderFrame(const SerumV2RenderFrameInput* input,
                                const SerumV2RenderRotationInput* rotation,
                                uint16_t* out_frame,
                                uint16_t* rotations_in_frame) {
  if (!input || !out_frame || !input->base_frame || input->width == 0 ||
      input->height == 0) {
    return;
  }

  const bool use_rotations =
      rotation && rotation->rotations && rotations_in_frame;
  const bool use_dynashadows = input->dynashadows_dir && input->dynashadows_col;
  std::vector<uint8_t> dynapix_storage;
  uint8_t* isdynapix = nullptr;
  if (use_dynashadows) {
    dynapix_storage.assign(input->width * input->height, 0);
    isdynapix = dynapix_storage.data();
  }

  auto map_base_index = [&](uint32_t x, uint32_t y) -> uint32_t {
    if (input->base_width == 0 || input->base_height == 0) {
      return 0;
    }
    if (input->width == input->base_width &&
        input->height == input->base_height) {
      return y * input->base_width + x;
    }
    if (input->height == 64) {
      return (y / 2) * input->base_width + x / 2;
    }
    return y * 2 * input->base_width + x * 2;
  };

  auto check_dyna_shadow = [&](uint8_t dynacouche, uint16_t fx, uint16_t fy,
                               uint32_t fw, uint32_t fh) {
    if (!isdynapix || !input->dynashadows_dir || !input->dynashadows_col) {
      return;
    }
    const uint8_t dsdir = input->dynashadows_dir[dynacouche];
    if (dsdir == 0) {
      return;
    }
    const uint16_t tcol = input->dynashadows_col[dynacouche];
    if ((dsdir & 0b1) > 0 && fx > 0 && fy > 0 &&
        isdynapix[(fy - 1) * fw + fx - 1] == 0) {
      isdynapix[(fy - 1) * fw + fx - 1] = 1;
      out_frame[(fy - 1) * fw + fx - 1] = tcol;
    }
    if ((dsdir & 0b10) > 0 && fy > 0 && isdynapix[(fy - 1) * fw + fx] == 0) {
      isdynapix[(fy - 1) * fw + fx] = 1;
      out_frame[(fy - 1) * fw + fx] = tcol;
    }
    if ((dsdir & 0b100) > 0 && fx < fw - 1 && fy > 0 &&
        isdynapix[(fy - 1) * fw + fx + 1] == 0) {
      isdynapix[(fy - 1) * fw + fx + 1] = 1;
      out_frame[(fy - 1) * fw + fx + 1] = tcol;
    }
    if ((dsdir & 0b1000) > 0 && fx < fw - 1 &&
        isdynapix[fy * fw + fx + 1] == 0) {
      isdynapix[fy * fw + fx + 1] = 1;
      out_frame[fy * fw + fx + 1] = tcol;
    }
    if ((dsdir & 0b10000) > 0 && fx < fw - 1 && fy < fh - 1 &&
        isdynapix[(fy + 1) * fw + fx + 1] == 0) {
      isdynapix[(fy + 1) * fw + fx + 1] = 1;
      out_frame[(fy + 1) * fw + fx + 1] = tcol;
    }
    if ((dsdir & 0b100000) > 0 && fy < fh - 1 &&
        isdynapix[(fy + 1) * fw + fx] == 0) {
      isdynapix[(fy + 1) * fw + fx] = 1;
      out_frame[(fy + 1) * fw + fx] = tcol;
    }
    if ((dsdir & 0b1000000) > 0 && fx > 0 && fy < fh - 1 &&
        isdynapix[(fy + 1) * fw + fx - 1] == 0) {
      isdynapix[(fy + 1) * fw + fx - 1] = 1;
      out_frame[(fy + 1) * fw + fx - 1] = tcol;
    }
    if ((dsdir & 0b10000000) > 0 && fx > 0 &&
        isdynapix[fy * fw + fx - 1] == 0) {
      isdynapix[fy * fw + fx - 1] = 1;
      out_frame[fy * fw + fx - 1] = tcol;
    }
  };

  for (uint32_t y = 0; y < input->height; ++y) {
    for (uint32_t x = 0; x < input->width; ++x) {
      const uint32_t idx = y * input->width + x;
      const uint32_t base_idx = map_base_index(x, y);
      const uint8_t original = input->base_frame[base_idx];

      const bool use_background =
          input->background_frame && input->background_mask &&
          (input->background_mask[idx] > 0);
      if (use_background && (original == 0 || input->black_out_static_content)) {
        if (!isdynapix || isdynapix[idx] == 0) {
          out_frame[idx] = input->background_frame[idx];
          if (use_rotations) {
            uint16_t rot = 0xffff;
            uint16_t pos = 0;
            if (SerumV2_ColorInRotation(rotation->rotations, out_frame[idx],
                                        &rot, &pos) &&
                rotation->shifts) {
              const uint16_t length =
                  rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION];
              out_frame[idx] =
                  rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION + 2 +
                                      (rotation->shifts[rot] + pos) % length];
            }
            rotations_in_frame[idx * 2] = rot;
            rotations_in_frame[idx * 2 + 1] = pos;
          }
        }
        continue;
      }

      if (input->dyna_mask && input->dyna_colors) {
        const uint8_t dynacouche = input->dyna_mask[idx];
        if (dynacouche != 255) {
          if (original > 0 && isdynapix) {
            check_dyna_shadow(dynacouche, static_cast<uint16_t>(x),
                              static_cast<uint16_t>(y), input->width,
                              input->height);
            isdynapix[idx] = 1;
          }
          out_frame[idx] =
              input->dyna_colors[dynacouche * input->nocolors + original];
          if (use_rotations) {
            rotations_in_frame[idx * 2] = 0xffff;
            rotations_in_frame[idx * 2 + 1] = 0;
          }
          continue;
        }
      }

      if (!isdynapix || isdynapix[idx] == 0) {
        out_frame[idx] = input->colorized ? input->colorized[idx] : 0;
        if (use_rotations) {
          uint16_t rot = 0xffff;
          uint16_t pos = 0;
          if (SerumV2_ColorInRotation(rotation->rotations, out_frame[idx], &rot,
                                      &pos) &&
              rotation->shifts) {
            const uint16_t length =
                rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION];
            out_frame[idx] =
                rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION + 2 +
                                    (rotation->shifts[rot] + pos) % length];
          }
          rotations_in_frame[idx * 2] = rot;
          rotations_in_frame[idx * 2 + 1] = pos;
        }
      }
    }
  }
}

inline void SerumV2_RenderSprite(const SerumV2RenderFrameInput* frame_input,
                                 const SerumV2RenderSpriteInput* sprite_input,
                                 const SerumV2SpritePlacement* placement,
                                 const SerumV2RenderRotationInput* rotation,
                                 uint16_t* out_frame,
                                 uint16_t* rotations_in_frame) {
  if (!frame_input || !sprite_input || !placement || !out_frame ||
      !frame_input->base_frame) {
    return;
  }

  const bool use_extra = (frame_input->width != frame_input->base_width) ||
                         (frame_input->height != frame_input->base_height);
  const bool use_rotations =
      rotation && rotation->rotations && rotations_in_frame;

  if (!use_extra) {
    if (!sprite_input->sprite_original) {
      return;
    }
    for (uint16_t y = 0; y < placement->hei; ++y) {
      for (uint16_t x = 0; x < placement->wid; ++x) {
        const uint32_t frame_index =
            (placement->fry + y) * frame_input->width + placement->frx + x;
        const uint32_t sprite_index =
            (placement->spy + y) * MAX_SPRITE_WIDTH + placement->spx + x;
        const uint8_t sprite_ref = sprite_input->sprite_original[sprite_index];
        if (sprite_ref >= 255) {
          continue;
        }
        uint8_t dynacouche = 255;
        if (sprite_input->dynasprite_mask) {
          dynacouche = sprite_input->dynasprite_mask[sprite_index];
        }
        if (dynacouche == 255 || !sprite_input->dynasprite_cols) {
          if (sprite_input->sprite_colored) {
            out_frame[frame_index] = sprite_input->sprite_colored[sprite_index];
          }
        } else {
          const uint8_t original = frame_input->base_frame[frame_index];
          const uint32_t offset = dynacouche * frame_input->nocolors + original;
          out_frame[frame_index] = sprite_input->dynasprite_cols[offset];
        }
        if (use_rotations) {
          uint16_t rot = 0xffff;
          uint16_t pos = 0;
          if (SerumV2_ColorInRotation(rotation->rotations,
                                      out_frame[frame_index], &rot, &pos) &&
              rotation->shifts) {
            const uint16_t length =
                rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION];
            out_frame[frame_index] =
                rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION + 2 +
                                    (rotation->shifts[rot] + pos) % length];
          }
          rotations_in_frame[frame_index * 2] = rot;
          rotations_in_frame[frame_index * 2 + 1] = pos;
        }
      }
    }
    return;
  }

  const bool extra_is_64 = frame_input->height == 64;
  const uint16_t thei = extra_is_64 ? placement->hei * 2 : placement->hei / 2;
  const uint16_t twid = extra_is_64 ? placement->wid * 2 : placement->wid / 2;
  const uint16_t tfrx = extra_is_64 ? placement->frx * 2 : placement->frx / 2;
  const uint16_t tfry = extra_is_64 ? placement->fry * 2 : placement->fry / 2;
  const uint16_t tspx = extra_is_64 ? placement->spx * 2 : placement->spx / 2;
  const uint16_t tspy = extra_is_64 ? placement->spy * 2 : placement->spy / 2;

  for (uint16_t y = 0; y < thei; ++y) {
    for (uint16_t x = 0; x < twid; ++x) {
      const uint32_t frame_index = (tfry + y) * frame_input->width + tfrx + x;
      const uint32_t sprite_index = (tspy + y) * MAX_SPRITE_WIDTH + tspx + x;
      if (sprite_input->sprite_mask_extra &&
          sprite_input->sprite_mask_extra[sprite_index] >= 255) {
        continue;
      }
      uint8_t dynacouche = 255;
      if (sprite_input->dynasprite_mask_extra) {
        dynacouche = sprite_input->dynasprite_mask_extra[sprite_index];
      }
      if (dynacouche == 255 || !sprite_input->dynasprite_cols_extra) {
        if (sprite_input->sprite_colored_extra) {
          out_frame[frame_index] =
              sprite_input->sprite_colored_extra[sprite_index];
        }
      } else {
        uint16_t tl;
        if (extra_is_64) {
          tl = static_cast<uint16_t>((y / 2 + placement->fry) *
                                         frame_input->base_width +
                                     x / 2 + placement->frx);
        } else {
          tl = static_cast<uint16_t>((y * 2 + placement->fry) *
                                         frame_input->base_width +
                                     x * 2 + placement->frx);
        }
        const uint8_t original = frame_input->base_frame[tl];
        const uint32_t offset = dynacouche * frame_input->nocolors + original;
        out_frame[frame_index] = sprite_input->dynasprite_cols_extra[offset];
      }
      if (use_rotations) {
        uint16_t rot = 0xffff;
        uint16_t pos = 0;
        if (SerumV2_ColorInRotation(rotation->rotations, out_frame[frame_index],
                                    &rot, &pos) &&
            rotation->shifts) {
          const uint16_t length =
              rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION];
          out_frame[frame_index] =
              rotation->rotations[rot * MAX_LENGTH_COLOR_ROTATION + 2 +
                                  (rotation->shifts[rot] + pos) % length];
        }
        rotations_in_frame[frame_index * 2] = rot;
        rotations_in_frame[frame_index * 2 + 1] = pos;
      }
    }
  }
}

inline void SerumV2_InitRotationState(const uint16_t* rotations,
                                      uint32_t* next_time_ms, uint16_t* shifts,
                                      uint8_t* active, uint32_t now_ms) {
  if (!next_time_ms || !shifts || !active) {
    return;
  }
  std::memset(next_time_ms, 0, sizeof(uint32_t) * MAX_COLOR_ROTATION_V2);
  std::memset(shifts, 0, sizeof(uint16_t) * MAX_COLOR_ROTATION_V2);
  std::memset(active, 0, sizeof(uint8_t) * MAX_COLOR_ROTATION_V2);
  if (!rotations) {
    return;
  }
  for (uint32_t rot = 0; rot < MAX_COLOR_ROTATION_V2; ++rot) {
    const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
    const uint16_t length = rotations[base];
    const uint16_t delay = rotations[base + 1];
    if (length == 0 || delay == 0) {
      continue;
    }
    active[rot] = 1;
    next_time_ms[rot] = now_ms + delay;
    shifts[rot] = 0;
  }
}

template <typename ShiftT>
inline uint32_t SerumV2_ApplyRotationsImpl(
    const uint16_t* rotations, uint16_t* frame, uint16_t* rotations_in_frame,
    uint32_t pixel_count, uint32_t* next_time_ms, ShiftT* shifts,
    uint8_t* active, uint32_t now_ms, uint8_t* modified_elements,
    uint32_t* last_time_ms, bool init_if_unset, bool* out_rotated) {
  if (!rotations || !frame || !next_time_ms || !shifts || !active) {
    return 0;
  }

  if (modified_elements) {
    std::memset(modified_elements, 0, pixel_count);
  }

  bool has_active = false;
  bool rotated = false;
  uint32_t next_delay = 0xffffffffu;

  for (uint32_t rot = 0; rot < MAX_COLOR_ROTATION_V2; ++rot) {
    const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
    const uint16_t length = rotations[base];
    const uint16_t delay = rotations[base + 1];
    if (length == 0 || delay == 0) {
      active[rot] = 0;
      shifts[rot] = 0;
      continue;
    }
    has_active = true;
    active[rot] = 1;
    if (init_if_unset && next_time_ms[rot] == 0) {
      next_time_ms[rot] = now_ms + delay;
      if (last_time_ms) {
        last_time_ms[rot] = now_ms;
      }
    }
    if (next_time_ms[rot] != 0 && now_ms >= next_time_ms[rot]) {
      shifts[rot] = static_cast<ShiftT>((shifts[rot] + 1) % length);
      next_time_ms[rot] = now_ms + delay;
      if (last_time_ms) {
        last_time_ms[rot] = now_ms;
      }
      rotated = true;
    }
    const uint32_t remaining =
        (next_time_ms[rot] > now_ms) ? (next_time_ms[rot] - now_ms) : 0;
    next_delay = std::min(next_delay, remaining);
  }

  if (!has_active) {
    if (out_rotated) {
      *out_rotated = false;
    }
    return 0;
  }
  if (!rotated) {
    if (out_rotated) {
      *out_rotated = false;
    }
    return next_delay == 0xffffffffu ? 0 : next_delay;
  }

  if (rotations_in_frame) {
    for (uint32_t idx = 0; idx < pixel_count; ++idx) {
      const uint16_t rot = rotations_in_frame[idx * 2];
      if (rot == 0xffff || !active[rot]) {
        continue;
      }
      const uint16_t pos = rotations_in_frame[idx * 2 + 1];
      const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
      const uint16_t length = rotations[base];
      const uint16_t next = rotations[base + 2 + (pos + shifts[rot]) % length];
      if (frame[idx] != next) {
        frame[idx] = next;
        if (modified_elements) {
          modified_elements[idx] = 1;
        }
      }
    }
  } else {
    for (uint32_t idx = 0; idx < pixel_count; ++idx) {
      const uint16_t value = frame[idx];
      for (uint32_t rot = 0; rot < MAX_COLOR_ROTATION_V2; ++rot) {
        if (!active[rot]) {
          continue;
        }
        const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
        const uint16_t length = rotations[base];
        for (uint16_t pos = 0; pos < length; ++pos) {
          if (value == rotations[base + 2 + pos]) {
            frame[idx] = rotations[base + 2 + (pos + shifts[rot]) % length];
            if (modified_elements) {
              modified_elements[idx] = 1;
            }
            rot = MAX_COLOR_ROTATION_V2;
            break;
          }
        }
      }
    }
  }

  if (out_rotated) {
    *out_rotated = true;
  }
  return next_delay == 0xffffffffu ? 0 : next_delay;
}

inline uint32_t SerumV2_ApplyRotations(
    const uint16_t* rotations, uint16_t* frame, uint16_t* rotations_in_frame,
    uint32_t pixel_count, uint32_t* next_time_ms, uint16_t* shifts,
    uint8_t* active, uint32_t now_ms, uint8_t* modified_elements,
    uint32_t* last_time_ms, bool init_if_unset, bool* out_rotated) {
  return SerumV2_ApplyRotationsImpl<uint16_t>(
      rotations, frame, rotations_in_frame, pixel_count, next_time_ms, shifts,
      active, now_ms, modified_elements, last_time_ms, init_if_unset,
      out_rotated);
}

inline uint32_t SerumV2_ApplyRotations(
    const uint16_t* rotations, uint16_t* frame, uint16_t* rotations_in_frame,
    uint32_t pixel_count, uint32_t* next_time_ms, uint32_t* shifts,
    uint8_t* active, uint32_t now_ms, uint8_t* modified_elements,
    uint32_t* last_time_ms, bool init_if_unset, bool* out_rotated) {
  return SerumV2_ApplyRotationsImpl<uint32_t>(
      rotations, frame, rotations_in_frame, pixel_count, next_time_ms, shifts,
      active, now_ms, modified_elements, last_time_ms, init_if_unset,
      out_rotated);
}
