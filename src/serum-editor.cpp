#include "serum-editor.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

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
                 const SerumEditorSpriteView& sprite,
                 uint8_t sprite_index,
                 const uint16_t* sprite_bb,
                 SerumEditorSpriteMatch* matches,
                 uint8_t* match_count,
                 uint8_t max_matches) {
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
    GetSpriteSize(sprite_index, &spw, &sph, sprite.original,
                  MAX_SPRITE_WIDTH, MAX_SPRITE_HEIGHT);

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
                mdword = (mdword >> 8) |
                         (static_cast<uint32_t>(normalizedFrameValue(
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
                        matches[i].frx == match.frx &&
                        matches[i].fry == match.fry &&
                        matches[i].wid == match.wid &&
                        matches[i].hei == match.hei) {
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

void RenderSprite(const SerumEditorDataView* data,
                  const SerumEditorFrameView* frame,
                  const SerumEditorSpriteView& sprite,
                  const SerumEditorSpriteMatch& match,
                  bool use_extra,
                  uint16_t* out_frame) {
    if (!data || !frame || !out_frame || !sprite.original) {
        return;
    }
    if (!frame->original) {
        return;
    }
    const uint32_t width = use_extra ? data->width_extra : data->width;
    const uint32_t height = use_extra ? data->height_extra : data->height;
    if (width == 0 || height == 0) {
        return;
    }

    if (!use_extra) {
        for (uint16_t y = 0; y < match.hei; ++y) {
            for (uint16_t x = 0; x < match.wid; ++x) {
                const uint32_t frame_index = (match.fry + y) * width + match.frx + x;
                const uint32_t sprite_index = (match.spy + y) * MAX_SPRITE_WIDTH + match.spx + x;
                const uint8_t sprite_ref = sprite.original[sprite_index];
                if (sprite_ref >= 255) {
                    continue;
                }
                uint8_t dynacouche = 255;
                if (sprite.dynasprite_mask) {
                    dynacouche = sprite.dynasprite_mask[sprite_index];
                }
                if (dynacouche == 255 || !sprite.dynasprite_cols) {
                    if (sprite.colored) {
                        out_frame[frame_index] = sprite.colored[sprite_index];
                    }
                } else {
                    const uint8_t original = frame->original[frame_index];
                    const uint32_t offset = dynacouche * data->nocolors + original;
                    out_frame[frame_index] = sprite.dynasprite_cols[offset];
                }
            }
        }
        return;
    }

    const bool extra_is_64 = data->height_extra == 64;
    const uint16_t thei = extra_is_64 ? match.hei * 2 : match.hei / 2;
    const uint16_t twid = extra_is_64 ? match.wid * 2 : match.wid / 2;
    const uint16_t tfrx = extra_is_64 ? match.frx * 2 : match.frx / 2;
    const uint16_t tfry = extra_is_64 ? match.fry * 2 : match.fry / 2;
    const uint16_t tspx = extra_is_64 ? match.spx * 2 : match.spx / 2;
    const uint16_t tspy = extra_is_64 ? match.spy * 2 : match.spy / 2;

    for (uint16_t y = 0; y < thei; ++y) {
        for (uint16_t x = 0; x < twid; ++x) {
            const uint32_t frame_index = (tfry + y) * width + tfrx + x;
            const uint32_t sprite_index = (tspy + y) * MAX_SPRITE_WIDTH + tspx + x;
            if (sprite.mask_extra && sprite.mask_extra[sprite_index] >= 255) {
                continue;
            }
            uint8_t dynacouche = 255;
            if (sprite.dynasprite_mask_extra) {
                dynacouche = sprite.dynasprite_mask_extra[sprite_index];
            }
            if (dynacouche == 255 || !sprite.dynasprite_cols_extra) {
                if (sprite.colored_extra) {
                    out_frame[frame_index] = sprite.colored_extra[sprite_index];
                }
            } else {
                uint16_t tl;
                if (extra_is_64) {
                    tl = static_cast<uint16_t>((y / 2 + match.fry) * data->width + x / 2 + match.frx);
                } else {
                    tl = static_cast<uint16_t>((y * 2 + match.fry) * data->width + x * 2 + match.frx);
                }
                const uint8_t original = frame->original[tl];
                const uint32_t offset = dynacouche * data->nocolors + original;
                out_frame[frame_index] = sprite.dynasprite_cols_extra[offset];
            }
        }
    }
}

}  // namespace

SERUM_EDITOR_API uint8_t SerumEditor_MatchSprites(
    const SerumEditorDataView* data,
    const SerumEditorFrameView* frame,
    SerumEditorSpriteMatch* matches,
    uint8_t max_matches) {
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
        const uint16_t* bb = frame->frame_sprite_bboxes + static_cast<size_t>(slot) * 4;
        MatchSprite(data, frame, sprite, sprite_id, bb, matches, &count, max_matches);
        if (count >= max_matches) {
            break;
        }
    }
    return count;
}

SERUM_EDITOR_API bool SerumEditor_RenderFrame(
    const SerumEditorDataView* data,
    const SerumEditorFrameView* frame,
    const SerumEditorSpriteMatch* matches,
    uint8_t match_count,
    bool use_extra,
    uint16_t* out_frame) {
    if (!data || !frame || !out_frame || !frame->original) {
        return false;
    }
    const uint32_t width = use_extra ? data->width_extra : data->width;
    const uint32_t height = use_extra ? data->height_extra : data->height;
    if (width == 0 || height == 0) {
        return false;
    }

    const bool use_background = (frame->background_id != 0xffff) &&
        (use_extra ? frame->background_frame_extra : frame->background_frame) &&
        (use_extra ? frame->background_mask_extra : frame->background_mask);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t idx = y * width + x;
            uint32_t tl = idx;
            if (use_extra) {
                if (data->height_extra == 64) {
                    tl = (y / 2) * data->width + x / 2;
                } else {
                    tl = y * 2 * data->width + x * 2;
                }
            }
            uint16_t value = 0;
            const uint8_t original = frame->original[tl];
            if (use_background && original == 0) {
                const uint8_t* bg_mask = use_extra ? frame->background_mask_extra : frame->background_mask;
                if (bg_mask && bg_mask[idx] > 0) {
                    const uint16_t* bg_frame = use_extra ? frame->background_frame_extra : frame->background_frame;
                    value = bg_frame ? bg_frame[idx] : 0;
                    out_frame[idx] = value;
                    continue;
                }
            }
            const uint8_t* dyn_mask = use_extra ? frame->dynamask_extra : frame->dynamask;
            const uint16_t* dyn_cols = use_extra ? frame->dyna4cols_extra : frame->dyna4cols;
            if (dyn_mask && dyn_cols) {
                const uint8_t dynacouche = dyn_mask[idx];
                if (dynacouche != 255) {
                    const uint32_t offset = dynacouche * data->nocolors + original;
                    out_frame[idx] = dyn_cols[offset];
                    continue;
                }
            }
            const uint16_t* colorized = use_extra ? frame->colorized_extra : frame->colorized;
            value = colorized ? colorized[idx] : 0;
            out_frame[idx] = value;
        }
    }

    if (matches && data->sprites) {
        for (uint8_t i = 0; i < match_count; ++i) {
            const uint8_t sprite_id = matches[i].sprite_index;
            if (sprite_id == 255 || sprite_id >= data->nsprites) {
                continue;
            }
            const SerumEditorSpriteView& sprite = data->sprites[sprite_id];
            RenderSprite(data, frame, sprite, matches[i], use_extra, out_frame);
        }
    }
    return true;
}

SERUM_EDITOR_API void SerumEditor_InitRotationState(
    const uint16_t* rotations,
    SerumEditorRotationState* state,
    uint32_t now_ms) {
    if (!state) {
        return;
    }
    std::memset(state, 0, sizeof(*state));
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
        state->active[rot] = 1;
        state->next_time_ms[rot] = now_ms + delay;
        state->shift[rot] = 0;
    }
}

SERUM_EDITOR_API uint32_t SerumEditor_ApplyRotations(
    const uint16_t* rotations,
    const uint16_t* input_frame,
    uint16_t* output_frame,
    uint32_t width,
    uint32_t height,
    SerumEditorRotationState* state,
    uint32_t now_ms) {
    if (!rotations || !input_frame || !output_frame || width == 0 || height == 0 || !state) {
        return 0;
    }
    std::memcpy(output_frame, input_frame, static_cast<std::size_t>(width) * height * sizeof(uint16_t));

    bool has_active = false;
    uint32_t next_delay = 0xffffffffu;
    for (uint32_t rot = 0; rot < MAX_COLOR_ROTATION_V2; ++rot) {
        const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
        const uint16_t length = rotations[base];
        const uint16_t delay = rotations[base + 1];
        if (length == 0 || delay == 0) {
            state->active[rot] = 0;
            state->shift[rot] = 0;
            continue;
        }
        has_active = true;
        state->active[rot] = 1;
        if (state->next_time_ms[rot] == 0) {
            state->next_time_ms[rot] = now_ms + delay;
        }
        if (now_ms >= state->next_time_ms[rot]) {
            state->shift[rot] = static_cast<uint16_t>((state->shift[rot] + 1) % length);
            state->next_time_ms[rot] = now_ms + delay;
        }
        const uint32_t remaining = (state->next_time_ms[rot] > now_ms)
            ? (state->next_time_ms[rot] - now_ms)
            : 0;
        next_delay = std::min(next_delay, remaining);
    }

    if (!has_active) {
        return 0;
    }

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * width + x;
            const uint16_t value = output_frame[idx];
            for (uint32_t rot = 0; rot < MAX_COLOR_ROTATION_V2; ++rot) {
                if (!state->active[rot]) {
                    continue;
                }
                const uint32_t base = rot * MAX_LENGTH_COLOR_ROTATION;
                const uint16_t length = rotations[base];
                const uint16_t shift = state->shift[rot];
                const uint16_t* colors = rotations + base + 2;
                for (uint16_t pos = 0; pos < length; ++pos) {
                    if (value == colors[pos]) {
                        const uint16_t next = colors[(pos + shift) % length];
                        output_frame[idx] = next;
                        rot = MAX_COLOR_ROTATION_V2;
                        break;
                    }
                }
            }
        }
    }

    return next_delay == 0xffffffffu ? 0 : next_delay;
}
