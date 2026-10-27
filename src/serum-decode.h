#pragma once

#ifdef _MSC_VER
#define SERUM_API extern "C" __declspec(dllexport)
#else
#define SERUM_API extern "C" __attribute__((visibility("default")))
#endif

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

/** @brief Load a Serum colorization from a cROM held in memory
 *
 * Takes the bytes of a .cROM file and loads them as Serum_Load() loads that
 * file when cROMc generation is off: same flags, same result.
 *
 * No file is read or written: no cROMc, no .pup.csv, no scaling.txt, no
 * skip-cromc.txt. The upscaling algorithm and shadow offset are the defaults.
 * A cROMc or a cRZ is not accepted.
 *
 * The buffer is only read during the call and stays owned by the caller.
 * A colorization loaded before is released first, also when the call fails.
 *
 * @param crom: the cROM bytes
 * @param size: number of bytes
 * @param flags: as for Serum_Load()
 *
 * @return As Serum_Load(). NULL if the data cannot be loaded; nothing is
 * loaded then and Serum_GetLastErrorMessage() says why.
 */
SERUM_API Serum_Frame_Struc* Serum_LoadFromMemory(const uint8_t* crom,
                                                  size_t size, uint8_t flags);

/** @brief Load a raw cROM from memory, with the scenes of a pup.csv
 *
 * As Serum_LoadFromMemory(), and for a v2 colorization the scenes of the
 * pup.csv at csvPath are parsed before the lookups are built, in the order
 * Serum_Load() uses for a pup.csv beside a cROM. An editor that holds its
 * project in memory gets the scenes the runtime would have. A NULL or empty
 * csvPath, or one that cannot be parsed, loads without scenes, as
 * Serum_LoadFromMemory() does.
 *
 * @param crom: the cROM bytes
 * @param size: number of bytes
 * @param csvPath: the pup.csv to read the scenes from, or NULL
 * @param flags: as for Serum_Load()
 *
 * @return As Serum_LoadFromMemory().
 */
SERUM_API Serum_Frame_Struc* Serum_LoadFromMemoryWithScenes(
    const uint8_t* crom, size_t size, const char* const csvPath, uint8_t flags);

/** @brief Load a raw cROM that a callback delivers
 *
 * As Serum_LoadFromMemoryWithScenes() for the `size` bytes of a cROM that
 * read() delivers in order, so that the caller never holds the whole cROM: an
 * editor can write it into the load as it serializes its project. libserum
 * asks for a few MB at most at a time and never for more than `size` bytes in
 * all. A read() that returns 0 before all bytes came fails the load as a
 * truncated cROM does.
 *
 * @param read: delivers the bytes, on the calling thread
 * @param userData: passed to read()
 * @param size: number of bytes of the cROM
 * @param csvPath: the pup.csv to read the scenes from, or NULL
 * @param flags: as for Serum_Load()
 *
 * @return As Serum_LoadFromMemory().
 */
/** @brief Load a colorization an authoring tool hands over
 *
 * As Serum_LoadFromMemoryWithScenes(), for a colorization that comes from the
 * tool's own data instead of a cROM: libserum asks for every element of every
 * block through source->fill(), in cROM order, and builds the same
 * colorization a cROM of that data loads. Serum_SaveConcentrate() then writes
 * its cROMc.
 *
 * @param source: the header values, fill() and the settings payload
 * @param csvPath: the pup.csv to read the scenes from, or NULL
 * @param flags: as for Serum_Load()
 *
 * @return As Serum_LoadFromMemory().
 */
SERUM_API Serum_Frame_Struc* Serum_LoadFromSource(
    const Serum_Colorization_Source* source, const char* const csvPath,
    uint8_t flags);

/** @brief Write the cROMc of a colorization an authoring tool hands over
 *
 * Builds the colorization from the source as Serum_LoadFromSource() does, but
 * with the packing of Serum_Load(), and writes its cROMc to cromcPath: the
 * cROMc Serum_Load() writes for a cROM of the same data. Nothing stays loaded.
 * Scenes and scaling settings come from the source's settings payload only.
 *
 * @return false with Serum_GetLastErrorMessage() when it fails.
 */
SERUM_API bool Serum_ConcentrateFromSource(
    const Serum_Colorization_Source* source, const char* const cromcPath);

SERUM_API Serum_Frame_Struc* Serum_LoadFromCallback(Serum_ReadCallback read,
                                                    void* userData, size_t size,
                                                    const char* const csvPath,
                                                    uint8_t flags);

/** @brief Set timeout for skipping unknown frames
 *
 * Unknown frames are ignored until this timeout elapses and a full lookup is
 * retried.
 *
 * @param milliseconds: Timeout in milliseconds (0 disables timeout handling)
 */
SERUM_API void Serum_SetIgnoreUnknownFramesTimeout(uint16_t milliseconds);

/** @brief Set maximum number of consecutive unknown frames to skip
 *
 * @param maximum: Maximum number of unknown frames before forcing lookup
 */
SERUM_API void Serum_SetMaximumUnknownFramesToSkip(uint8_t maximum);

/** @brief Set fallback monochrome palette used by libserum
 *
 * @param palette: Pointer to palette data (RGB triplets)
 * @param bitDepth: Bit depth of source frames (typically 2 or 4)
 */
SERUM_API void Serum_SetStandardPalette(const uint8_t* palette,
                                        const int bitDepth);

/** @brief Enable or disable automatic cROMc generation
 *
 * @param generate: true to generate/save cROMc data, false to disable
 */
SERUM_API void Serum_SetGenerateCRomC(bool generate);

/** @brief Get the upscaling algorithm selected for the loaded colorization
 *
 * Valid after a successful Serum_Load(). The value comes from the cROMc header
 * and may be overridden at authoring time by altcolor/<romname>/scaling.txt.
 *
 * Callers that scale libserum's output frame further (for example to fit a
 * physical DMD) should use this so their scaling matches the scaling libserum
 * applied internally. The value is the authored choice and is reported even
 * when this particular load has no extra-resolution plane to upscale into.
 *
 * @return One of the SERUM_SCALING_* constants; SERUM_SCALING_SCALE2X (the
 * default) if nothing is loaded
 */
SERUM_API uint8_t Serum_GetScalingAlgorithm(void);

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

/** @brief Disable frame colorization output
 */
SERUM_API void Serum_DisableColorization(void);

/** @brief Enable frame colorization output
 */
SERUM_API void Serum_EnableColorization(void);

/** @brief Suppress external PUP trigger reporting
 */
SERUM_API void Serum_DisablePupTriggers(void);

/** @brief Enable external PUP trigger reporting
 */
SERUM_API void Serum_EnablePupTrigers(void);

/** @brief Get runtime metadata for the last Serum colorize/rotate result
 *
 * Provides frame-level metadata for the most recently processed Serum frame.
 * This is intended for diagnostics/profiling tools.
 *
 * @param metadata: Output structure. metadata->size should be set to
 * sizeof(Serum_Runtime_Metadata); zero is also accepted for current versions.
 * @return true if metadata was filled, false on invalid arguments
 */
SERUM_API bool Serum_GetRuntimeMetadata(Serum_Runtime_Metadata* metadata);
// The speaker light effects the frames colorized since the last call start:
// bit n of the result set means effects[n] is the new effect of channel n. 0
// when nothing changed. A host sends them to the lights as they come; the
// frame rendering does not wait for them.
SERUM_API uint32_t
Serum_GetLightEffects(Serum_Light_Effect effects[SERUM_LIGHT_CHANNELS]);
// The effects the colorization gives each channel when nothing else plays
// (off for a channel without one); bit n set for every channel the
// colorization has light settings for, 0 for one without speaker lights.
SERUM_API uint32_t
Serum_GetDefaultLightEffects(Serum_Light_Effect effects[SERUM_LIGHT_CHANNELS]);
// The DOF events of the frames shown since the last call, in the order they
// came: up to `capacity` of them into `events`, returning how many. Those
// that did not fit stay for the next call. A frame emits its events when it is
// shown, not again while it stays on the display; an event with a turn-off
// value has that value emitted with the next frame shown, unless that frame
// sets the same element itself. A host passes them to libdof
// (DOF::DataReceive()) as they come; libserum keeps up to
// SERUM_DOF_EVENTS_KEPT of them and drops more until they are fetched.
SERUM_API uint32_t Serum_GetDofEvents(Serum_Dof_Event* events,
                                      uint32_t capacity);

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

/** @brief Get the last fatal runtime error message
 *
 * Returns the most recent top-level exception/fatal error captured by the DLL,
 * or the reason the last Serum_CRZ_* or Serum_LoadFromMemory() call failed.
 * The returned pointer is owned by libserum and remains valid until the next
 * API call that updates the stored error state.
 *
 * @return Empty string if no fatal error has been recorded
 */
SERUM_API const char* Serum_GetLastErrorMessage(void);

/** @brief Read the cROM stored in a cRZ archive
 *
 * A cRZ is a zip archive holding one cROM. The first regular entry is taken,
 * whatever it is called: entry names are not interpreted and nothing is
 * written to disk. This is the same extraction Serum_Load() uses.
 *
 * Works on the file only. It neither needs nor changes a loaded colorization
 * and may be called from any thread.
 *
 * @param crzPath: path of the archive, UTF-8 on Windows
 * @param data: receives the cROM bytes; release them with Serum_CRZ_Free()
 * @param size: receives the number of bytes
 * @return true on success. On failure *data is NULL, *size is 0 and
 * Serum_GetLastErrorMessage() says why.
 */
SERUM_API bool Serum_CRZ_Read(const char* crzPath, uint8_t** data,
                              size_t* size);

/** @brief Release a buffer returned by Serum_CRZ_Read()
 *
 * @param data: the buffer; NULL is accepted
 */
SERUM_API void Serum_CRZ_Free(uint8_t* data);

/** @brief Get the name of the entry Serum_CRZ_Read() would return
 *
 * The name is data from the archive. Do not build a path from it.
 *
 * @param crzPath: path of the archive, UTF-8 on Windows
 * @param name: receives the zero-terminated entry name
 * @param nameSize: size of the name buffer in bytes
 * @return true on success, false if there is no such entry or the buffer is
 * too small; Serum_GetLastErrorMessage() says why
 */
SERUM_API bool Serum_CRZ_GetEntryName(const char* crzPath, char* name,
                                      size_t nameSize);

/** @brief Create a cRZ archive from a cROM file
 *
 * Writes a zip archive with one deflated entry. The cROM is streamed from
 * disk, so its size does not have to fit in memory. zip64 is used only for a
 * cROM of 4 GiB or more. The entry gets the cROM file's modification time.
 *
 * An existing file at crzPath is replaced. If writing fails after the archive
 * was created, the incomplete archive is removed.
 *
 * Works on the files only. It neither needs nor changes a loaded colorization
 * and may be called from any thread.
 *
 * @param crzPath: path of the archive to create, UTF-8 on Windows
 * @param entryName: name of the entry, by convention "<romname>.cRom". It has
 * to be a plain file name: '/', '\\' and ':' are refused, as are "." and ".."
 * @param cromPath: path of the cROM file to store, UTF-8 on Windows
 * @return true on success, otherwise Serum_GetLastErrorMessage() says why
 */
SERUM_API bool Serum_CRZ_WriteFromFile(const char* crzPath,
                                       const char* entryName,
                                       const char* cromPath);

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
 * @param sceneOptions: Receives end frame behavior
 * @return true if scene info was found
 */
SERUM_API bool Serum_Scene_GetInfo(uint16_t sceneId, uint16_t* frameCount,
                                   uint16_t* durationPerFrame,
                                   bool* interruptable, bool* startImmediately,
                                   uint8_t* repeat, uint8_t* sceneOptions);

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

/** @brief Trigger a scene by ID from outside of frame matching
 *
 * Starts the given scene immediately in the internal scene pipeline.
 * The return value follows the same convention as Serum_Rotate():
 * - low word: milliseconds until next rotation/scene step
 * - high-word scene bit: FLAG_RETURNED_V2_SCENE when a scene is active
 *
 * @param sceneId: Scene ID to trigger
 * @return Timing/flags value similar to Serum_Rotate()
 */
SERUM_API uint32_t Serum_Scene_Trigger(uint16_t sceneId);

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
