#pragma once

#ifdef _MSC_VER
#define SERUM_API extern "C" __declspec(dllexport)
#else
#define SERUM_API extern "C" __attribute__((visibility("default")))
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#include "serum.h"

/** @brief Set the log callback
 *
 *  Set the log callback.
 *
 *  @param callback
 *  @param userData
 */
SERUM_API void Serum_SetLogCallback(Serum_LogCallback callback,
                                    const void* userData);

/** @brief Load a Serum file.
 *
 *  @param altcolorpath: path to the altcolor directory (e.g. "C:\\visual
 * pinball\\vpinmame\\altcolor\\")
 *  @param romname: name of the rom (e.g. "afm_113b")
 *  @param flags (only needed for v2 files): this can be a combination of:
 * FLAG_REQUEST_32P_FRAMES (if you want the 32-pixel-high frame to be returned
 * if available) / FLAG_REQUEST_64P_FRAMES (same for 64-pixel-high frame) /
 * FLAG_REQUEST_FILL_MODIFIED_ELEMENTS (Serum_Rotate() fills the
 * modifiedelementsXX buffers to know which points have changed in the rotation)
 *
 *  @return A pointer to the Serum_Frame_Struc as described in the serum.h file
 * (to keep and read all along the use of the loaded Serum)
 */
SERUM_API Serum_Frame_Struc* Serum_Load(const char* const altcolorpath,
                                        const char* const romname,
                                        uint8_t flags);
SERUM_API void Serum_SetIgnoreUnknownFramesTimeout(uint16_t milliseconds);

SERUM_API void Serum_SetMaximumUnknownFramesToSkip(uint8_t maximum);

SERUM_API void Serum_SetStandardPalette(const uint8_t* palette,
                                        const int bitDepth);

SERUM_API void Serum_SetGenerateCRomC(bool generate);

/** @brief Release the content and memory of the loaded Serum file.
 */
SERUM_API void Serum_Dispose(void);

/** @brief Colorize a frame and set the values in the Serum_Frame_Struc
 * (corresponding to the pointer returned at Serum_Load() time)
 *
 * @param Frame: a buffer to the rom uncolorized frame containing width*height
 * bytes with [0,3] or [0,15] values (according to the ROM)
 *
 * @return Either return IDENTIFY_NO_FRAME if no frame in the Serum file matches
 * or, if a frame was identified, 0 if it has no rotation or a value giving the
 * time in milliseconds before the first color rotation
 */
SERUM_API uint32_t Serum_Colorize(uint8_t* frame);

/** @brief Perform the color rotations of the current frame. For v1, it modifies
 * "palette", for v2, it modifies "frame32" and/or "frame64"
 *
 * @return The time in milliseconds before the next rotation in the low word. If
 * there was a rotation in the 32P frame, the first bit of the high word is set
 * (0x10000) and if there was a rotation in the 64P frame, the second bit of the
 * high word is set (0x20000)
 */
SERUM_API uint32_t Serum_Rotate(void);

SERUM_API void Serum_DisableColorization(void);

SERUM_API void Serum_EnableColorization(void);

SERUM_API void Serum_DisablePupTriggers(void);

SERUM_API void Serum_EnablePupTrigers(void);

/** @brief Get the full version of this library
 *
 * @return A string formatted "major.minor.patch"
 */
SERUM_API const char* Serum_GetVersion(void);

/** @brief Get the short version of this library
 *
 * @return A string formatted "major.minor"
 */
SERUM_API const char* Serum_GetMinorVersion(void);

/** @brief Parse a scene description file in CSV format
 *
 * @param csv_filename: Path to the CSV file containing scene descriptions
 * @return true if parsing was successful
 */
SERUM_API bool Serum_Scene_ParseCSV(const char* const csv_filename);

/** @brief Generate a dump of scene data
 *
 * @param dump_filename: Path where to save the dump
 * @param id: Scene ID to dump (-1 for all scenes)
 * @return true if dump was successful
 */
SERUM_API bool Serum_Scene_GenerateDump(const char* const dump_filename,
                                        int id);

/** @brief Get information about a scene
 *
 * @param sceneId: ID of the scene
 * @param frameCount: Receives number of frames
 * @param durationPerFrame: Receives duration per frame
 * @param interruptable: Receives if scene can be interrupted
 * @param startImmediately: Receives if scene starts immediately
 * @param repeat: Receives repeat count
 * @param endFrame: Receives end frame behavior
 * @return true if scene info was found
 */
SERUM_API bool Serum_Scene_GetInfo(uint16_t sceneId, uint16_t* frameCount,
                                   uint16_t* durationPerFrame,
                                   bool* interruptable, bool* startImmediately,
                                   uint8_t* repeat, uint8_t* endFrame);

/** @brief Generate a frame for a scene
 *
 * @param sceneId: ID of the scene
 * @param frameIndex: Index of the frame to generate
 * @param buffer: Buffer to receive the frame data
 * @param group: Group number (-1 for default)
 * @return true if frame was generated
 */
SERUM_API bool Serum_Scene_GenerateFrame(uint16_t sceneId, uint16_t frameIndex,
                                         uint8_t* buffer, int group);

/** @brief Set rendering depth for scenes
 *
 * @param depth: New rendering depth
 */
SERUM_API void Serum_Scene_SetDepth(uint8_t depth);

/** @brief Get current rendering depth
 *
 * @return Current rendering depth
 */
SERUM_API int Serum_Scene_GetDepth(void);

/** @brief Check if scene generator is active
 *
 * @return true if scene generator is active
 */
SERUM_API bool Serum_Scene_IsActive(void);

/** @brief Reset scene generator to initial state
 */
SERUM_API void Serum_Scene_Reset(void);
