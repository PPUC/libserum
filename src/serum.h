#pragma once

#ifdef _MSC_VER
#define SERUM_CALLBACK __stdcall
#else
#define SERUM_CALLBACK
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>

// Serum_LoadFromCallback(): puts up to `size` bytes of the cROM into `buffer`
// and returns how many (at least 1), or 0 when it cannot deliver more.
typedef size_t(SERUM_CALLBACK* Serum_ReadCallback)(void* userData,
                                                   uint8_t* buffer,
                                                   size_t size);

// Serum_LoadFromSource(): the blocks of a colorization, in the order a v2
// cROM stores them. Each element of a block is one record of its kind (a
// frame, comparison mask, sprite or background); the comment gives the type
// and count of the values of one element. W x H is the original plane,
// WE x HE the extra plane, N the frame of the ROM (W x H, or WE x HE for a
// 256x64 project), C the colors, S the sprites per frame, SP the pixels of a
// sprite (MAX_SPRITE_WIDTH x MAX_SPRITE_HEIGHT).
typedef enum {
  SERUM_BLOCK_FRAME_HASH,                   // frame: uint32
  SERUM_BLOCK_FRAME_SHAPE_MODE,             // frame: uint8
  SERUM_BLOCK_FRAME_MASK_ID,                // frame: uint8
  SERUM_BLOCK_COMPARISON_MASK,              // mask: uint8 x N
  SERUM_BLOCK_FRAME_HAS_EXTRA,              // frame: uint8
  SERUM_BLOCK_FRAME_COLORS,                 // frame: uint16 x W x H
  SERUM_BLOCK_FRAME_COLORS_EXTRA,           // frame: uint16 x WE x HE
  SERUM_BLOCK_FRAME_DYNAMIC_MASK,           // frame: uint8 x W x H
  SERUM_BLOCK_FRAME_DYNAMIC_MASK_EXTRA,     // frame: uint8 x WE x HE
  SERUM_BLOCK_FRAME_DYNAMIC_COLORS,         // frame: uint16 x 32 x C
  SERUM_BLOCK_FRAME_DYNAMIC_COLORS_EXTRA,   // frame: uint16 x 32 x C
  SERUM_BLOCK_SPRITE_HAS_EXTRA,             // sprite: uint8
  SERUM_BLOCK_FRAME_SPRITES,                // frame: uint16 x S (0xffff: none)
  SERUM_BLOCK_SPRITE_ORIGINAL,              // sprite: uint8 x SP
  SERUM_BLOCK_SPRITE_COLORS,                // sprite: uint16 x SP
  SERUM_BLOCK_SPRITE_MASK_EXTRA,            // sprite: uint8 x SP
  SERUM_BLOCK_SPRITE_COLORS_EXTRA,          // sprite: uint16 x SP
  SERUM_BLOCK_FRAME_ACTIVE,                 // frame: uint8
  SERUM_BLOCK_FRAME_ROTATIONS,              // frame: uint16 x 64 x 4
  SERUM_BLOCK_FRAME_ROTATIONS_EXTRA,        // frame: uint16 x 64 x 4
  SERUM_BLOCK_SPRITE_DETECTION_DWORDS,      // sprite: uint32 x 4
  SERUM_BLOCK_SPRITE_DETECTION_POSITIONS,   // sprite: uint16 x 4
  SERUM_BLOCK_SPRITE_DETECTION_AREAS,       // sprite: uint16 x 4 x 4
  SERUM_BLOCK_FRAME_TRIGGER,                // frame: uint32
  SERUM_BLOCK_FRAME_SPRITE_BOUNDS,          // frame: uint16 x S x 4
  SERUM_BLOCK_BACKGROUND_HAS_EXTRA,         // background: uint8
  SERUM_BLOCK_BACKGROUND_COLORS,            // background: uint16 x W x H
  SERUM_BLOCK_BACKGROUND_COLORS_EXTRA,      // background: uint16 x WE x HE
  SERUM_BLOCK_FRAME_BACKGROUND_ID,          // frame: uint16 (0xffff: none)
  SERUM_BLOCK_FRAME_BACKGROUND_MASK,        // frame: uint8 x W x H
  SERUM_BLOCK_FRAME_BACKGROUND_MASK_EXTRA,  // frame: uint8 x WE x HE
  SERUM_BLOCK_FRAME_SHADOW_DIRECTIONS,      // frame: uint8 x 32
  SERUM_BLOCK_FRAME_SHADOW_COLORS,          // frame: uint16 x 32
  SERUM_BLOCK_FRAME_SHADOW_DIRECTIONS_EXTRA,  // frame: uint8 x 32
  SERUM_BLOCK_FRAME_SHADOW_COLORS_EXTRA,      // frame: uint16 x 32
  SERUM_BLOCK_SPRITE_DYNAMIC_COLORS,          // sprite: uint16 x 9 x C
  SERUM_BLOCK_SPRITE_DYNAMIC_COLORS_EXTRA,    // sprite: uint16 x 9 x C
  SERUM_BLOCK_SPRITE_DYNAMIC_MASK,            // sprite: uint8 x SP
  SERUM_BLOCK_SPRITE_DYNAMIC_MASK_EXTRA,      // sprite: uint8 x SP
  SERUM_BLOCK_SPRITE_SHAPE_MODE,              // sprite: uint8
  SERUM_BLOCK_COUNT
} Serum_Block;

// Serum_LoadFromSource(): writes element `index` of `block` into `out`
// (`size` bytes, see Serum_Block).
typedef void(SERUM_CALLBACK* Serum_FillCallback)(void* userData,
                                                 Serum_Block block,
                                                 uint32_t index, void* out,
                                                 size_t size);

// A colorization handed over by an authoring tool instead of a cROM: what a
// cROM's header says, the blocks through fill(), and the payload of the cROM
// settings block (scenes, scaling, lights, original-only dynamic sets; see
// AGENTS.md "cROM settings block"), without its magic and size.
typedef struct {
  char romName[64];
  uint32_t width;
  uint32_t height;
  uint32_t widthExtra;
  uint32_t heightExtra;
  uint32_t frames;
  uint32_t colors;
  uint32_t comparisonMasks;
  uint32_t sprites;
  uint16_t backgrounds;
  uint8_t is256x64;
  uint32_t spritesPerFrame;
  Serum_FillCallback fill;
  void* userData;
  const uint8_t* settings;
  size_t settingsSize;
} Serum_Colorization_Source;

typedef void(SERUM_CALLBACK* Serum_LogCallback)(const char* format,
                                                va_list args,
                                                const void* userData);

// mask for the mutually exclusive scene-finish behavior bits
#define FLAG_SCENE_FINISH_MODE_MASK 3

enum {
  // default: when scene is finished, show last frame of the scene until a
  // new frame is matched.
  // black screen after scene is finished
  FLAG_SCENE_BLACK_WHEN_FINISHED = 1,
  // show last frame before scene started
  FLAG_SCENE_SHOW_PREVIOUS_FRAME_WHEN_FINISHED = 2,
  // run scene as background
  FLAG_SCENE_AS_BACKGROUND = 4,
  // keep only dynamic zones, sprites and shadows in the foreground
  FLAG_SCENE_ONLY_DYNAMIC_CONTENT = 8,
  FLAG_SCENE_RESUME_IF_RETRIGGERED = 16,
  // with background scenes, dynamic-zone pixels that resolve to black let the
  // background scene show through, do not generate dynamic shadows, and do not
  // replace already-written dynamic shadow pixels. Unlit dynamic-zone pixels
  // that resolve to a colour keep it instead of showing the background scene.
  // They cast dynamic shadows like lit dynamic content.
  FLAG_SCENE_REPLACE_DYNAMIC_BLACK = 32,
};

enum  // returned by Serum_Load in *SerumVersion
{
  SERUM_V1 = 1,
  SERUM_V2 = 2
};

// Algorithm used whenever original-resolution content has to be scaled up to
// the extra resolution (e.g. 128x32 source content rendered into a 256x64
// output): the whole colorized frame when a frame has no 64p content, and the
// ROM shade that selects dynamic-zone and dynamic-sprite colors when it does.
// Stored in the cROMc header from concentrate version 8 on and selectable per
// colorization through altcolor/<romname>/scaling.txt.
//
// Scale2x is the default (value 0): colorizations without an explicit choice,
// and every cROMc older than version 8, resolve to it. Downscaling is never
// performed by libserum.
enum {
  // replicate every source pixel into a 2x2 block
  SERUM_SCALING_LINE_DOUBLING = 1,
  // Scale2x, rounding a convex corner only where the glyph is solid behind it
  // (default).
  //
  // Reference Scale2x rounds every convex corner by taking a neighbour, and
  // where that neighbour is empty the pixel is lost. DMD text is often five
  // pixels tall, where a glyph is almost nothing but corners, and that erosion
  // makes letters like S, R and C unreadable. Rounding only what is backed by
  // more of the same glyph keeps large digits' corners and small text's
  // strokes both.
  SERUM_SCALING_SCALE2X = 2,
  // These are libframeutil's ScalingAlgorithm values, so a host can pass what
  // Serum_GetScalingAlgorithm() returns straight to it and scale the way
  // libserum did. Its value 0 is reference Scale2x, which libserum does not
  // offer: SERUM_SCALING_SCALE2X is its Scale2xPreserve.
};

// How far a dynamic shadow is offset when the extra plane is produced by
// upscaling. Stored in the cROMc header from concentrate version 8 on and
// selectable through altcolor/<romname>/scaling.txt.
//
// Shadows are generated from the *upscaled* dynamic content, at extra-plane
// resolution. SERUM_SHADOW_OFFSET_NATIVE reproduces what libserum has always
// rendered into the extra plane (a one-pixel offset in extra-plane
// coordinates), which is what colorization authors tuned against.
// SERUM_SHADOW_OFFSET_PROPORTIONAL keeps the shadow's thickness proportional to
// the glyph, matching the original-resolution output. Tight glyphs such as "8"
// can lose the gap between their loops under PROPORTIONAL.
enum {
  SERUM_SHADOW_OFFSET_NATIVE = 0,       // 1 extra-plane pixel (default)
  SERUM_SHADOW_OFFSET_PROPORTIONAL = 1  // 2 extra-plane pixels
};

// Flags to send to Serum_Load
enum {
  FLAG_REQUEST_32P_FRAMES = 1,  // there is a output DMD which is 32 leds high
  FLAG_REQUEST_64P_FRAMES = 2,  // there is a output DMD which 64 leds high
  FLAG_REQUEST_FILL_MODIFIED_ELEMENTS =
      4,  // does the modifiedelementsXX structures must be allocated and
          // returned
  FLAG_REQUEST_FORCE = 8,  // force the output
  FLAG_REQUEST_FALLBACK =
      16,  // if extra-only output is requested, fall back to original output
           // when no extra-resolution frame is available
};

enum  // returned values in Serum_Frame_Sttruc::flags for v2+ format
{
  FLAG_RETURNED_32P_FRAME_OK =
      1,  // the 32p frame has been filled (updated by Serum_Colorize)
  FLAG_RETURNED_64P_FRAME_OK =
      2,  // the 64p frame has been filled (updated by Serum_Colorize)
  FLAG_RETURNED_EXTRA_AVAILABLE =
      4,  // at least 1 frame has an extra resolution version (updated by
          // Serum_Load)
};

enum  // returned flags that are added to the timings if there were rotations
      // (updated by Serum_Rotate)
{
  FLAG_RETURNED_V1_ROTATED = 0x10000,
  FLAG_RETURNED_V2_ROTATED32 = 0x10000,
  FLAG_RETURNED_V2_ROTATED64 = 0x20000,
  FLAG_RETURNED_V2_SCENE = 0x40000,
};

enum  // runtime metadata feature flags
{
  SERUM_RUNTIME_FEATURE_MATCHED = 1 << 0,
  SERUM_RUNTIME_FEATURE_BACKGROUND = 1 << 1,
  SERUM_RUNTIME_FEATURE_DYNAMIC = 1 << 2,
  SERUM_RUNTIME_FEATURE_DYNAMIC_EXTRA = 1 << 3,
  SERUM_RUNTIME_FEATURE_SPRITES = 1 << 4,
  SERUM_RUNTIME_FEATURE_SHAPE_SPRITES = 1 << 5,
  SERUM_RUNTIME_FEATURE_COLOR_ROTATION = 1 << 6,
  SERUM_RUNTIME_FEATURE_SCENE = 1 << 7,
  SERUM_RUNTIME_FEATURE_TRIGGER = 1 << 8,
  SERUM_RUNTIME_FEATURE_MONOCHROME_FALLBACK = 1 << 9,
};

typedef struct _Serum_Runtime_Metadata {
  uint32_t size;
  uint32_t serumVersion;
  uint32_t frameID;
  uint32_t triggerID;
  uint32_t rotationtimer;
  uint32_t featureFlags;
  uint32_t reserved;
} Serum_Runtime_Metadata;

// Speaker lights (WS2812FX strips driven by the DMD's firmware): up to
// SERUM_LIGHT_CHANNELS channels, each running one effect at a time.
#define SERUM_LIGHT_CHANNELS 4

// An effect for one channel, in WS2812FX terms: mode is an FX_MODE_* value
// (FX_MODE_STATIC, 0, with colors[0] for a static color), speed the WS2812FX
// speed (ms per cycle), options its option bits (REVERSE, FADE_*, SIZE_*,
// GAMMA), colors 0xWWRRGGBB. brightness scales the brightness set on the
// device (255: as set there). durationMs: how long it plays before the
// channel returns to its default effect, 0 until the next effect.
typedef struct _Serum_Light_Effect {
  uint8_t mode;
  uint8_t options;
  uint16_t speed;
  uint32_t durationMs;
  uint32_t colors[3];
  uint8_t brightness;
  uint8_t reserved[3];
} Serum_Light_Effect;

typedef struct _Serum_Frame_Struc {
  // data for v1 Serum format
  uint8_t* frame;      // return the colorized frame
  uint8_t* palette;    // and its palette
  uint8_t* rotations;  // and its color rotations
  // data for v2 Serum format
  // the frame (frame32 or frame64) corresponding to the resolution of the ROM
  // must ALWAYS be defined if a frame pointer is defined, its width, rotations
  // and rotationsinframe pointers must be defined
  uint16_t* frame32;
  uint32_t width32;  // 0 is returned if the 32p colorized frame is not
                     // available for this frame
  uint16_t* rotations32;
  uint16_t*
      rotationsinframe32;  // [width32*32*2] precalculated array to tell if a
                           // color is in a color rotations of the frame
                           // ([X*Y*0]=0xffff if not part of a rotation)
  uint8_t* modifiedelements32;  // (optional) 32P pixels modified during the
                                // last rotation
  uint16_t* frame64;
  uint32_t width64;  // 0 is returned if the 64p colorized frame is not
                     // available for this frame
  uint16_t* rotations64;
  uint16_t*
      rotationsinframe64;  // [width64*64*2] precalculated array to tell if a
                           // color is in a color rotations of the frame
                           // ([X*Y*0]=0xffff if not part of a rotation)
  uint8_t* modifiedelements64;  // (optional) 64P pixels modified during the
                                // last rotation
  // common data
  uint32_t SerumVersion;  // SERUM_V1 or SERUM_V2
  /// <summary>
  /// flags for return:
  /// if flags & FLAG_RETURNED_32P_FRAME_OK (after Serum_Colorize) : frame32 has
  /// been filled if flags & FLAG_RETURNED_64P_FRAME_OK (after Serum_Colorize) :
  /// frame64 has been filled if flags & FLAG_RETURNED_EXTRA_AVAILABLE (after
  /// Serum_Load) : at least 1 frame in the file has an extra resolution version
  /// none of them, display the original frame
  /// </summary>
  uint8_t flags;
  uint32_t nocolors;   // number of shades of orange in the ROM
  uint32_t ntriggers;  // number of triggers in the Serum file
  uint32_t triggerID;  // return 0xffff if no trigger for that frame, the ID of
                       // the trigger if one is set for that frame
  uint32_t frameID;    // for CDMD ingame tester
  uint32_t rotationtimer;
} Serum_Frame_Struc;

const int MAX_DYNA_4COLS_PER_FRAME =
    16;  // max number of color sets for dynamic content for each frame (old
         // version)
const int MAX_DYNA_SETS_PER_FRAME_V2 =
    32;  // max number of color sets for dynamic content for each frame (new
         // version)
const int MAX_DYNA_SETS_PER_SPRITE =
    9;  // max number of color sets for dynamic content for each sprite
const int MAX_SPRITE_SIZE = 128;   // maximum size of the sprites
const int MAX_SPRITE_WIDTH = 256;  // maximum width of the new sprites
const int MAX_SPRITE_HEIGHT = 64;  // maximum height of the new sprites
const int MAX_SPRITES_PER_FRAME =
    128;  // maximum amount of sprites to look for per frame; a colorization
          // carries its own count (SerumData::SpritesPerFrame())
const int LEGACY_SPRITES_PER_FRAME =
    32;  // sprites per frame of a cROM before format version 1 and of a cROMc
         // before concentrate version 10
const uint16_t SERUM_NO_SPRITE =
    0xffff;  // an empty sprite slot of a frame (255 in files before format
             // version 1 and concentrate version 10)
const int MAX_COLOR_ROTATIONS =
    8;  // maximum amount of color rotations per frame
const int MAX_COLOR_ROTATION_V2 =
    4;  // maximum number of new color rotations per frame
const int MAX_LENGTH_COLOR_ROTATION =
    64;  // maximum number of new colors in a rotation
const int MAX_SPRITE_DETECT_AREAS =
    4;  // maximum number of areas to detect the sprite

const int PALETTE_SIZE = 64 * 3;  // size of a palette
const int ROTATION_SIZE =
    3 * MAX_COLOR_ROTATIONS;  // size of a color rotation block
const int MAX_BACKGROUND_IMAGES =
    65535;  // max number of background images (16-bit IDs, 0xffff for none)

const uint32_t IDENTIFY_NO_FRAME =
    0xffffffff;  // returned in Serum_Frame_Struc::frameID if no matching frame
                 // detected in the Serum file
const uint32_t IDENTIFY_SAME_FRAME = 0xfffffffe;

typedef Serum_Frame_Struc* (*Serum_LoadFunc)(const char* const altcolorpath,
                                             const char* const romname,
                                             uint8_t flags);
typedef void (*Serum_DisposeFunc)(void);
typedef uint32_t (*Serum_ColorizeFunc)(uint8_t* frame);
typedef uint32_t (*Serum_RotateFunc)(void);
typedef const char* (*Serum_GetVersionFunc)(void);
typedef const char* (*Serum_GetMinorVersionFunc)(void);
typedef const char* (*Serum_GetLastErrorMessageFunc)(void);
typedef void (*Serum_SetLogCallbackFunc)(Serum_LogCallback callback,
                                         const void* userData);
typedef void (*Serum_SetIgnoreUnknownFramesTimeoutFunc)(uint16_t milliseconds);
typedef void (*Serum_SetMaximumUnknownFramesToSkipFunc)(uint8_t maximum);
typedef void (*Serum_SetStandardPaletteFunc)(const uint8_t* palette,
                                             int bitDepth);
typedef void (*Serum_SetGenerateCRomCFunc)(bool generate);
typedef uint8_t (*Serum_GetScalingAlgorithmFunc)(void);
typedef void (*Serum_DisableColorizationFunc)(void);
typedef void (*Serum_EnableColorizationFunc)(void);
typedef void (*Serum_DisablePupTriggersFunc)(void);
typedef void (*Serum_EnablePupTrigersFunc)(void);
typedef bool (*Serum_GetRuntimeMetadataFunc)(Serum_Runtime_Metadata* metadata);
typedef bool (*Serum_CRZ_ReadFunc)(const char* crzPath, uint8_t** data,
                                   size_t* size);
typedef void (*Serum_CRZ_FreeFunc)(uint8_t* data);
typedef bool (*Serum_CRZ_GetEntryNameFunc)(const char* crzPath, char* name,
                                           size_t nameSize);
typedef bool (*Serum_CRZ_WriteFromFileFunc)(const char* crzPath,
                                            const char* entryName,
                                            const char* cromPath);
typedef bool (*Serum_Scene_ParseCSVFunc)(const char* const csv_filename);
typedef bool (*Serum_Scene_GenerateDumpFunc)(const char* const dump_filename,
                                             int id);
typedef bool (*Serum_Scene_GetInfoFunc)(uint16_t sceneId, uint16_t* frameCount,
                                        uint16_t* durationPerFrame,
                                        bool* interruptable,
                                        bool* startImmediately, uint8_t* repeat,
                                        uint8_t* sceneOptions);
typedef bool (*Serum_Scene_GenerateFrameFunc)(uint16_t sceneId,
                                              uint16_t frameIndex,
                                              uint8_t* buffer, int group);
typedef uint32_t (*Serum_Scene_TriggerFunc)(uint16_t sceneId);
typedef void (*Serum_Scene_SetDepthFunc)(uint8_t depth);
typedef int (*Serum_Scene_GetDepthFunc)(void);
typedef bool (*Serum_Scene_IsActiveFunc)(void);
typedef void (*Serum_Scene_ResetFunc)(void);
