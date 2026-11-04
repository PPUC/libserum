#pragma once

#include <algorithm>
#include <atomic>
#include <cereal/access.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "LZ4Stream.h"
#include "serum-accel.h"

bool is_real_machine();

// Whether set() packs with LZ4's fast mode instead of LZ4 HC. For a
// colorization loaded to be played only (Serum_LoadFromMemory() and the other
// authoring loads): no cROMc is written from it, and the values read back are
// the same.
inline bool &SparseVectorFastPacking() {
  static bool fast = false;
  return fast;
}

namespace sparse_vector_serialization {
inline bool &LegacyLoadExpectedFlag() {
  static bool flag = false;
  return flag;
}

inline void SetLegacyLoadExpected(bool expected) {
  LegacyLoadExpectedFlag() = expected;
}

inline bool IsLegacyLoadExpected() { return LegacyLoadExpectedFlag(); }
}  // namespace sparse_vector_serialization

// A value no two states of the elements of any sparse vector share: taken
// anew whenever a vector's elements may have changed.
inline uint64_t NextSparseVectorStamp() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

template <typename T>
class SparseVector {
  static_assert(
      std::is_trivial<T>::value && std::is_standard_layout<T>::value,
      "SparseVector only supports trivial types like uint8_t or uint16_t");

 protected:
  std::vector<std::vector<T>> index;
  std::unordered_map<uint32_t, std::vector<uint8_t>>
      data;  // Changed to uint8_t for compressed data
  std::vector<T> noData;
  uint64_t elementSize = 0;  // Size of each element in bytes
  std::vector<T> decompBuffer;
  bool useIndex;
  bool useCompression;
  bool useBinaryBitPacking;
  T bitPackFalseValue;
  T bitPackTrueValue;
  mutable uint32_t lastPayloadId = UINT32_MAX;
  mutable const uint8_t *lastPayloadPtr = nullptr;
  mutable uint32_t lastPayloadSize = 0;
  mutable uint32_t lastAccessedId = UINT32_MAX;
  mutable uint32_t secondAccessedId = UINT32_MAX;
  mutable std::vector<T> lastDecompressed;
  mutable std::vector<T> secondDecompressed;
  struct DecodedCacheEntry {
    uint32_t id = UINT32_MAX;
    uint64_t stamp = 0;
    std::vector<T> values;
  };
  static constexpr size_t kDecodedCacheCapacity = 6;
  mutable std::vector<DecodedCacheEntry> decodedCache;
  mutable uint64_t decodedCacheStamp = 0;
  mutable std::vector<uint8_t> decodeScratch;
  mutable bool forceDecodedReads = false;
  mutable std::unordered_map<uint32_t, std::vector<T>> forcedDecoded;
  const char *profileLabel = nullptr;
  mutable uint64_t profileAccessCount = 0;
  mutable uint64_t profileDecodeCount = 0;
  mutable uint64_t profileCacheHitCount = 0;
  mutable uint64_t profileDirectHitCount = 0;
  std::vector<uint32_t> packedIds;
  std::vector<uint32_t> packedOffsets;
  std::vector<uint32_t> packedSizes;
  std::vector<uint8_t> packedBlob;
  mutable std::unordered_map<uint32_t, uint32_t> packedIndexById;
  mutable std::vector<uint32_t> packedDenseIndexById;
  mutable bool packedIndexReady = false;
  // The element each element is stored as the difference to (its values XOR
  // that element's), by element ID; UINT32_MAX for one stored whole. Empty
  // when none is.
  std::vector<uint32_t> deltaBases;
  uint64_t elementsStamp = NextSparseVectorStamp();
  const void *loadParent = nullptr;
  bool (*loadParentHasData)(const void *, uint32_t) = nullptr;
  bool keptAtLoad = false;

  static constexpr uint8_t kLegacyBitPackedMagic = 0xB1;
  static constexpr uint8_t kValuePackedMagic = 0xB2;
  static constexpr uint8_t kValuePackedMode1Bit = 1;
  static constexpr uint8_t kValuePackedMode2Bit = 2;
  static constexpr uint8_t kValuePackedMode4Bit = 4;

  static bool isProfilingEnabled() {
    static bool initialized = false;
    static bool enabled = false;
    if (!initialized) {
      const char *value = std::getenv("SERUM_PROFILE_SPARSE_VECTORS");
      enabled = value && value[0] != '\0' &&
                (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                 strcmp(value, "TRUE") == 0 || strcmp(value, "yes") == 0 ||
                 strcmp(value, "YES") == 0 || strcmp(value, "on") == 0 ||
                 strcmp(value, "ON") == 0);
      initialized = true;
    }
    return enabled;
  }

  size_t rawByteSize() const { return elementSize * sizeof(T); }

  size_t legacyBitPackedByteSize() const { return 1 + ((elementSize + 7) / 8); }

  size_t valuePackedByteSize(uint8_t modeBits) const {
    return 2 + (((elementSize * modeBits) + 7) / 8);
  }

  size_t maxPackedPayloadByteSize() const {
    if (!useBinaryBitPacking || !std::is_same<T, uint8_t>::value) {
      return rawByteSize();
    }
    const size_t maxPacked =
        std::max(legacyBitPackedByteSize(), valuePackedByteSize(4));
    return std::max(rawByteSize(), maxPacked);
  }

  bool isLegacyBitPackedPayload(const uint8_t *payload, size_t size) const {
    if (!useBinaryBitPacking || !payload) {
      return false;
    }
    if (!std::is_same<T, uint8_t>::value) {
      return false;
    }
    return size == legacyBitPackedByteSize() &&
           payload[0] == kLegacyBitPackedMagic;
  }

  bool isValuePackedPayload(const uint8_t *payload, size_t size) const {
    if (!useBinaryBitPacking || !payload) {
      return false;
    }
    if (!std::is_same<T, uint8_t>::value) {
      return false;
    }
    if (size < 2 || payload[0] != kValuePackedMagic) {
      return false;
    }
    const uint8_t mode = payload[1];
    if (mode != kValuePackedMode1Bit && mode != kValuePackedMode2Bit &&
        mode != kValuePackedMode4Bit) {
      return false;
    }
    return size == valuePackedByteSize(mode);
  }

  bool encodeValuePacked(const T *values, std::vector<uint8_t> &encoded) const {
    if (!useBinaryBitPacking) {
      encoded.clear();
      return false;
    }
    if (!std::is_same<T, uint8_t>::value) {
      throw std::runtime_error("Value packing is only supported for uint8_t");
    }

    uint8_t maxValue = 0;
    for (size_t i = 0; i < elementSize; ++i) {
      const uint8_t value = values[i];
      if (value > maxValue) {
        maxValue = value;
      }
    }

    uint8_t modeBits = 0;
    if (maxValue <= 1) {
      modeBits = kValuePackedMode1Bit;
    } else if (maxValue <= 3) {
      modeBits = kValuePackedMode2Bit;
    } else if (maxValue <= 15) {
      modeBits = kValuePackedMode4Bit;
    } else {
      return false;
    }

    if (valuePackedByteSize(modeBits) >= rawByteSize()) {
      return false;
    }

    encoded.assign(valuePackedByteSize(modeBits), 0);
    encoded[0] = kValuePackedMagic;
    encoded[1] = modeBits;
    for (size_t i = 0; i < elementSize; ++i) {
      const uint8_t value = values[i];
      if (modeBits == kValuePackedMode1Bit) {
        if (value > 0) {
          encoded[2 + (i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
        }
      } else if (modeBits == kValuePackedMode2Bit) {
        const size_t bitPos = i * 2;
        encoded[2 + (bitPos / 8)] |=
            static_cast<uint8_t>((value & 0x3u) << (bitPos % 8));
      } else {
        const size_t bitPos = i * 4;
        encoded[2 + (bitPos / 8)] |=
            static_cast<uint8_t>((value & 0xFu) << (bitPos % 8));
      }
    }
    return true;
  }

  void prepareDecodedCacheForWrite(uint32_t elementId) {
    if (lastAccessedId != UINT32_MAX && lastAccessedId != elementId &&
        !lastDecompressed.empty()) {
      secondAccessedId = lastAccessedId;
      secondDecompressed.swap(lastDecompressed);
    }
    if (lastDecompressed.size() < elementSize) {
      lastDecompressed.resize(elementSize);
    }
  }

  DecodedCacheEntry *findDecodedCacheEntry(uint32_t elementId) const {
    for (auto &entry : decodedCache) {
      if (entry.id == elementId && !entry.values.empty()) {
        entry.stamp = ++decodedCacheStamp;
        return &entry;
      }
    }
    return nullptr;
  }

  DecodedCacheEntry *reserveDecodedCacheEntry(uint32_t elementId) const {
    DecodedCacheEntry *target = nullptr;

    for (auto &entry : decodedCache) {
      if (entry.id == elementId) {
        target = &entry;
        break;
      }
      if (!target && entry.id == UINT32_MAX) {
        target = &entry;
      }
    }

    if (!target) {
      target = &decodedCache.front();
      for (auto &entry : decodedCache) {
        if (entry.stamp < target->stamp) {
          target = &entry;
        }
      }
    }

    target->id = elementId;
    target->stamp = ++decodedCacheStamp;
    if (target->values.size() < elementSize) {
      target->values.resize(elementSize);
    }
    return target;
  }

  T *cacheDecodedFromBytes(uint32_t elementId, const uint8_t *bytes,
                           size_t bytesSize) const {
    const size_t rawBytes = rawByteSize();
    if (!bytes || bytesSize != rawBytes) {
      return const_cast<T *>(noData.data());
    }
    auto *entry = reserveDecodedCacheEntry(elementId);
    memcpy(entry->values.data(), bytes, rawBytes);
    return entry->values.data();
  }

  void resetDecodedCaches() {
    lastAccessedId = UINT32_MAX;
    secondAccessedId = UINT32_MAX;
    lastDecompressed.clear();
    secondDecompressed.clear();
    decodedCache.clear();
    decodedCache.resize(kDecodedCacheCapacity);
    decodedCacheStamp = 0;
    decodeScratch.clear();
  }

  T *decodeValuePackedAndCache(uint32_t elementId, const uint8_t *payload) {
    const uint8_t modeBits = payload[1];
    prepareDecodedCacheForWrite(elementId);

    // Value `i` is the `modeBits` bits at bit `i * modeBits` of the payload,
    // lowest bit first.
    if constexpr (std::is_same<T, uint8_t>::value) {
      serum_accel::UnpackValues(payload + 2, modeBits, lastDecompressed.data(),
                                elementSize);
    } else {
      for (size_t i = 0; i < elementSize; ++i) {
        if (modeBits == kValuePackedMode1Bit) {
          const bool isSet = (payload[2 + (i / 8)] & (1u << (i % 8))) != 0;
          lastDecompressed[i] = static_cast<T>(isSet ? 1 : 0);
        } else if (modeBits == kValuePackedMode2Bit) {
          const size_t bitPos = i * 2;
          lastDecompressed[i] = static_cast<T>(
              (payload[2 + (bitPos / 8)] >> (bitPos % 8)) & 0x3u);
        } else {
          const size_t bitPos = i * 4;
          lastDecompressed[i] = static_cast<T>(
              (payload[2 + (bitPos / 8)] >> (bitPos % 8)) & 0xFu);
        }
      }
    }

    lastAccessedId = elementId;
    return lastDecompressed.data();
  }

  T *decodeLegacyBitPackedAndCache(uint32_t elementId, const uint8_t *payload) {
    prepareDecodedCacheForWrite(elementId);

    for (size_t i = 0; i < elementSize; ++i) {
      const bool isSet = (payload[1 + (i / 8)] & (1u << (i % 8))) != 0;
      lastDecompressed[i] = isSet ? bitPackTrueValue : bitPackFalseValue;
    }

    lastAccessedId = elementId;
    return lastDecompressed.data();
  }

  void clearPacked() {
    packedIds.clear();
    packedOffsets.clear();
    packedSizes.clear();
    packedBlob.clear();
    packedIndexById.clear();
    packedDenseIndexById.clear();
    packedIndexReady = false;
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
  }

  void ensurePackedIndex() const {
    if (packedIds.empty()) {
      packedIndexById.clear();
      packedDenseIndexById.clear();
      packedIndexReady = false;
      return;
    }
    if (packedIndexReady) {
      return;
    }

    // Build hash index from packedIds (assumed to be pre-sorted from save/load)
    packedIndexById.clear();
    packedIndexById.reserve(packedIds.size());
    for (uint32_t i = 0; i < packedIds.size(); ++i) {
      packedIndexById.emplace(packedIds[i], i);
    }

    // Fast path for dense/small ID spaces (frame IDs, sprite IDs, etc).
    // This avoids hash lookup overhead in operator[] hot loops.
    packedDenseIndexById.clear();

    // Find actual maximum ID
    uint32_t maxPackedId = 0;
    for (uint32_t i = 0; i < packedIds.size(); ++i) {
      if (packedIds[i] > maxPackedId) {
        maxPackedId = packedIds[i];
      }
    }

    // Safety check: if max ID seems unreasonable, it indicates data corruption.
    // Typical frame/sprite IDs should be < 100000 for most archives, but we
    // allow up to 1000000 for safety, and the ratio check prevents huge
    // allocations.
    const uint32_t kMaxReasonableId = 1000000;
    const uint32_t kMaxRatioAllocation = 8;  // max(id) <= count * 8.

    if (maxPackedId <= kMaxReasonableId &&
        maxPackedId <=
            static_cast<uint32_t>(packedIds.size() * kMaxRatioAllocation)) {
      packedDenseIndexById.assign(static_cast<size_t>(maxPackedId) + 1,
                                  UINT32_MAX);
      for (uint32_t i = 0; i < packedIds.size(); ++i) {
        const uint32_t id = packedIds[i];
        // Bounds check to prevent out-of-bounds write
        if (id < packedDenseIndexById.size()) {
          packedDenseIndexById[id] = i;
        }
      }
    }
    packedIndexReady = true;
  }

  static uint64_t hashPayload(const uint8_t *bytes, size_t size) {
    uint64_t hash = 1469598103934665603ull;  // FNV-1a 64-bit offset basis
    for (size_t i = 0; i < size; ++i) {
      hash ^= static_cast<uint64_t>(bytes[i]);
      hash *= 1099511628211ull;  // FNV prime
    }
    hash ^= static_cast<uint64_t>(size);
    hash *= 1099511628211ull;
    return hash;
  }

  void deduplicatePackedBlob() {
    if (packedIds.empty() || packedOffsets.size() != packedIds.size() ||
        packedSizes.size() != packedIds.size()) {
      return;
    }

    std::vector<uint8_t> dedupBlob;
    dedupBlob.reserve(packedBlob.size());

    // hash -> list of (offset,size) candidates in dedupBlob
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>
        dedupIndex;
    dedupIndex.reserve(packedIds.size());

    for (size_t i = 0; i < packedIds.size(); ++i) {
      const uint32_t oldOffset = packedOffsets[i];
      const uint32_t size = packedSizes[i];
      if (oldOffset > packedBlob.size() ||
          size > packedBlob.size() - oldOffset) {
        continue;
      }

      const uint8_t *payload = packedBlob.data() + oldOffset;
      const uint64_t payloadHash = hashPayload(payload, size);

      uint32_t foundOffset = UINT32_MAX;
      auto it = dedupIndex.find(payloadHash);
      if (it != dedupIndex.end()) {
        for (const auto &candidate : it->second) {
          const uint32_t candidateOffset = candidate.first;
          const uint32_t candidateSize = candidate.second;
          if (candidateSize != size) {
            continue;
          }
          if (candidateOffset > dedupBlob.size() ||
              size > dedupBlob.size() - candidateOffset) {
            continue;
          }
          if (memcmp(payload, dedupBlob.data() + candidateOffset, size) == 0) {
            foundOffset = candidateOffset;
            break;
          }
        }
      }

      if (foundOffset == UINT32_MAX) {
        foundOffset = static_cast<uint32_t>(dedupBlob.size());
        dedupBlob.insert(dedupBlob.end(), payload, payload + size);
        dedupIndex[payloadHash].push_back({foundOffset, size});
      }

      packedOffsets[i] = foundOffset;
      packedSizes[i] = size;
    }

    packedBlob = std::move(dedupBlob);
  }

  void buildPackedFromData() {
    if (useIndex || data.empty()) {
      return;
    }

    std::vector<uint32_t> ids;
    ids.reserve(data.size());
    for (const auto &entry : data) {
      ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    clearPacked();
    packedIds.reserve(ids.size());
    packedOffsets.reserve(ids.size());
    packedSizes.reserve(ids.size());

    uint32_t offset = 0;
    for (const uint32_t id : ids) {
      const auto it = data.find(id);
      if (it == data.end()) {
        continue;
      }

      const auto &payload = it->second;
      packedIds.push_back(id);
      packedOffsets.push_back(offset);
      packedSizes.push_back(static_cast<uint32_t>(payload.size()));
      packedBlob.insert(packedBlob.end(), payload.begin(), payload.end());
      offset += static_cast<uint32_t>(payload.size());
    }

    data.clear();
    deduplicatePackedBlob();
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
  }

  void restoreDataFromPacked() {
    if (useIndex || packedIds.empty() || !data.empty()) {
      return;
    }

    for (size_t i = 0; i < packedIds.size(); ++i) {
      if (i >= packedOffsets.size() || i >= packedSizes.size()) {
        continue;
      }
      const uint32_t offset = packedOffsets[i];
      const uint32_t size = packedSizes[i];
      if (offset > packedBlob.size() || size > packedBlob.size() - offset) {
        continue;
      }
      data[packedIds[i]].assign(packedBlob.begin() + offset,
                                packedBlob.begin() + offset + size);
    }
  }

  const uint8_t *getPackedPayload(uint32_t elementId,
                                  uint32_t *payloadSize) const {
    if (packedIds.empty()) {
      return nullptr;
    }

    ensurePackedIndex();
    uint32_t packedIndex = UINT32_MAX;
    if (elementId < packedDenseIndexById.size()) {
      packedIndex = packedDenseIndexById[elementId];
      if (packedIndex == UINT32_MAX) {
        return nullptr;
      }
    } else {
      auto it = packedIndexById.find(elementId);
      if (it == packedIndexById.end()) {
        return nullptr;
      }
      packedIndex = it->second;
    }

    const size_t idx = static_cast<size_t>(packedIndex);
    if (idx >= packedOffsets.size() || idx >= packedSizes.size()) {
      return nullptr;
    }

    const uint32_t offset = packedOffsets[idx];
    const uint32_t size = packedSizes[idx];
    if (offset > packedBlob.size() || size > packedBlob.size() - offset) {
      return nullptr;
    }

    *payloadSize = size;
    return packedBlob.data() + offset;
  }

  // The decoded values of an element if a cache holds them, or null.
  const uint8_t *cachedValues(uint32_t elementId) const {
    if (elementId == lastAccessedId && !lastDecompressed.empty()) {
      return reinterpret_cast<const uint8_t *>(lastDecompressed.data());
    }
    if (elementId == secondAccessedId && !secondDecompressed.empty()) {
      return reinterpret_cast<const uint8_t *>(secondDecompressed.data());
    }
    for (const auto &entry : decodedCache) {
      if (entry.id == elementId && !entry.values.empty()) {
        return reinterpret_cast<const uint8_t *>(entry.values.data());
      }
    }
    return serum_accel::FindResolvedElement(this, elementsStamp, elementId,
                                            rawByteSize());
  }

  // The element's payload unpacked into `out`, without its base applied.
  bool unpackOwnPayload(uint32_t elementId, std::vector<uint8_t> &out) const {
    uint32_t payloadSize = 0;
    const uint8_t *payload = nullptr;
    if (!packedIds.empty()) {
      payload = getPackedPayload(elementId, &payloadSize);
    } else {
      const auto it = data.find(elementId);
      if (it != data.end()) {
        payload = it->second.data();
        payloadSize = static_cast<uint32_t>(it->second.size());
      }
    }
    if (!payload) return false;
    const size_t rawBytes = rawByteSize();
    out.resize(rawBytes);
    return LZ4_decompress_safe(reinterpret_cast<const char *>(payload),
                               reinterpret_cast<char *>(out.data()),
                               static_cast<int>(payloadSize),
                               static_cast<int>(rawBytes)) ==
           static_cast<int>(rawBytes);
  }

  // The values of an element stored as a difference, whose own payload is
  // unpacked in decodeScratch: XORed with each base in turn until one whose
  // values are known or that is stored whole.
  T *resolveDifference(uint32_t elementId) {
    const size_t rawBytes = rawByteSize();
    std::vector<uint8_t> values(decodeScratch.begin(),
                                decodeScratch.begin() + rawBytes);
    std::vector<uint8_t> own;
    uint32_t base = deltaBases[elementId];
    for (size_t steps = 0;; ++steps) {
      if (steps > deltaBases.size()) return noData.data();
      if (const uint8_t *known = cachedValues(base)) {
        for (size_t i = 0; i < rawBytes; ++i) values[i] ^= known[i];
        break;
      }
      if (!unpackOwnPayload(base, own)) return noData.data();
      for (size_t i = 0; i < rawBytes; ++i) values[i] ^= own[i];
      if (!isStoredAsDifference(base)) break;
      base = deltaBases[base];
    }
    serum_accel::ElementResolved(this, elementsStamp, elementId, values.data(),
                                 rawBytes);
    prepareDecodedCacheForWrite(elementId);
    memcpy(lastDecompressed.data(), values.data(), rawBytes);
    lastAccessedId = elementId;
    cacheDecodedFromBytes(elementId, values.data(), rawBytes);
    return lastDecompressed.data();
  }

 public:
  SparseVector(T noDataSignature, bool index, bool compress = false,
               bool binaryBitPack = false, T bitPackFalse = 0,
               T bitPackTrue = 1)
      : useIndex(index),
        useCompression(compress),
        useBinaryBitPacking(binaryBitPack),
        bitPackFalseValue(bitPackFalse),
        bitPackTrueValue(bitPackTrue) {
    if (useBinaryBitPacking && !std::is_same<T, uint8_t>::value) {
      throw std::runtime_error(
          "Binary bit packing is only supported for uint8_t SparseVector");
    }
    noData.resize(1, noDataSignature);
    decodedCache.resize(kDecodedCacheCapacity);
  }

  SparseVector(T noDataSignature)
      : useIndex(false),
        useCompression(false),
        useBinaryBitPacking(false),
        bitPackFalseValue(noDataSignature),
        bitPackTrueValue(static_cast<T>(1)) {
    noData.resize(1, noDataSignature);
    decodedCache.resize(kDecodedCacheCapacity);
  }

  // The number of values per element (0 before the first element was set).
  size_t valuesPerElement() const { return static_cast<size_t>(elementSize); }

  T *operator[](const uint32_t elementId) {
    if (isProfilingEnabled()) {
      ++profileAccessCount;
    }
    if (useIndex) {
      if (elementId >= index.size()) return noData.data();
      if (isProfilingEnabled()) {
        ++profileDirectHitCount;
      }
      return index[elementId].data();
    } else {
      if (forceDecodedReads) {
        auto cached = forcedDecoded.find(elementId);
        if (cached != forcedDecoded.end()) {
          if (isProfilingEnabled()) {
            ++profileCacheHitCount;
          }
          return cached->second.data();
        }
        return noData.data();
      }
      if (useCompression && elementId == lastAccessedId &&
          !lastDecompressed.empty()) {
        if (isProfilingEnabled()) {
          ++profileCacheHitCount;
        }
        return lastDecompressed.data();
      }
      if (useCompression && elementId == secondAccessedId &&
          !secondDecompressed.empty()) {
        std::swap(lastAccessedId, secondAccessedId);
        std::swap(lastDecompressed, secondDecompressed);
        if (isProfilingEnabled()) {
          ++profileCacheHitCount;
        }
        return lastDecompressed.data();
      }
      if (useCompression) {
        if (auto *entry = findDecodedCacheEntry(elementId)) {
          if (isProfilingEnabled()) {
            ++profileCacheHitCount;
          }
          return entry->values.data();
        }
      }

      const uint8_t *payload = nullptr;
      uint32_t payloadSize = 0;

      if (elementId == lastPayloadId && lastPayloadPtr != nullptr) {
        payload = lastPayloadPtr;
        payloadSize = lastPayloadSize;
      } else {
        if (!packedIds.empty()) {
          payload = getPackedPayload(elementId, &payloadSize);
        } else {
          auto it = data.find(elementId);
          if (it != data.end()) {
            payload = it->second.data();
            payloadSize = static_cast<uint32_t>(it->second.size());
          }
        }
        if (payload) {
          lastPayloadId = elementId;
          lastPayloadPtr = payload;
          lastPayloadSize = payloadSize;
        } else {
          lastPayloadId = UINT32_MAX;
          lastPayloadPtr = nullptr;
          lastPayloadSize = 0;
        }
      }

      if (!payload) return noData.data();

      if (useCompression) {
        if (isProfilingEnabled()) {
          ++profileDecodeCount;
        }
        // Cache hit only applies to decoded cache-backed payloads.
        if (elementId == lastAccessedId) {
          if (isProfilingEnabled()) {
            ++profileCacheHitCount;
          }
          return lastDecompressed.data();
        }

        const size_t rawBytes = rawByteSize();
        const size_t maxDecodedSize = maxPackedPayloadByteSize();
        if (decodeScratch.size() < maxDecodedSize) {
          decodeScratch.resize(maxDecodedSize);
        }

        int decompressedSize = LZ4_decompress_safe(
            reinterpret_cast<const char *>(payload),
            reinterpret_cast<char *>(decodeScratch.data()),
            static_cast<int>(payloadSize), static_cast<int>(maxDecodedSize));

        if (decompressedSize < 0) {
          if (isValuePackedPayload(payload, payloadSize)) {
            return decodeValuePackedAndCache(elementId, payload);
          }
          if (isLegacyBitPackedPayload(payload, payloadSize)) {
            return decodeLegacyBitPackedAndCache(elementId, payload);
          }

          // Backward compatibility: older payloads may store raw bytes even if
          // this vector now defaults to compression.
          if (payloadSize == rawBytes) {
            if (isProfilingEnabled()) {
              ++profileDirectHitCount;
            }
            return cacheDecodedFromBytes(elementId, payload, payloadSize);
          }
          return noData.data();
        }

        if (isValuePackedPayload(decodeScratch.data(),
                                 static_cast<size_t>(decompressedSize))) {
          return decodeValuePackedAndCache(elementId, decodeScratch.data());
        }

        if (isLegacyBitPackedPayload(decodeScratch.data(),
                                     static_cast<size_t>(decompressedSize))) {
          return decodeLegacyBitPackedAndCache(elementId, decodeScratch.data());
        }

        if (static_cast<size_t>(decompressedSize) != rawBytes) {
          return noData.data();
        }
        if (isStoredAsDifference(elementId)) {
          return resolveDifference(elementId);
        }

        prepareDecodedCacheForWrite(elementId);
        memcpy(lastDecompressed.data(), decodeScratch.data(), rawBytes);
        lastAccessedId = elementId;
        cacheDecodedFromBytes(elementId, decodeScratch.data(), rawBytes);
        return lastDecompressed.data();
      }

      if (isValuePackedPayload(payload, payloadSize)) {
        return decodeValuePackedAndCache(elementId, payload);
      }
      if (isLegacyBitPackedPayload(payload, payloadSize)) {
        return decodeLegacyBitPackedAndCache(elementId, payload);
      }

      if (payloadSize != rawByteSize()) {
        return noData.data();
      }

      if (isProfilingEnabled()) {
        ++profileDirectHitCount;
      }
      return reinterpret_cast<T *>(const_cast<uint8_t *>(payload));
    }
  }

  bool hasData(uint32_t elementId) const {
    if (useIndex)
      return elementId < index.size() && !index[elementId].empty() &&
             index[elementId][0] != noData[0];
    if (!packedIds.empty()) {
      ensurePackedIndex();
      if (elementId < packedDenseIndexById.size()) {
        return packedDenseIndexById[elementId] != UINT32_MAX;
      }
      return packedIndexById.find(elementId) != packedIndexById.end();
    }
    return data.find(elementId) != data.end();
  }

  // Whether the element is stored as the difference to another one
  // (storeAsDifferences(), setDifferenceBases()).
  bool isStoredAsDifference(uint32_t elementId) const {
    return elementId < deltaBases.size() && deltaBases[elementId] != UINT32_MAX;
  }

  bool hasDifferences() const {
    for (uint32_t base : deltaBases)
      if (base != UINT32_MAX) return true;
    return false;
  }

  // The elements stored as differences, each with the element it is the
  // difference to, by ascending element ID; only those the vector has.
  std::vector<std::pair<uint32_t, uint32_t>> differenceBases() const {
    std::vector<std::pair<uint32_t, uint32_t>> bases;
    for (uint32_t id = 0; id < deltaBases.size(); ++id) {
      if (deltaBases[id] != UINT32_MAX && hasData(id)) {
        bases.emplace_back(id, deltaBases[id]);
      }
    }
    return bases;
  }

  // Which elements are stored as differences, as differenceBases() gives
  // them. Throws for bases that lead back to an element.
  void setDifferenceBases(
      const std::vector<std::pair<uint32_t, uint32_t>> &bases) {
    deltaBases.clear();
    for (const auto &base : bases) {
      if (base.first == UINT32_MAX || base.first == base.second) {
        throw std::runtime_error("Invalid element difference");
      }
      if (deltaBases.size() <= base.first) {
        deltaBases.resize(static_cast<size_t>(base.first) + 1, UINT32_MAX);
      }
      deltaBases[base.first] = base.second;
    }
    // Every chain of bases ends: 0 not walked yet, 1 on the walk now, 2 ends.
    std::vector<uint8_t> state(deltaBases.size(), 0);
    std::vector<uint32_t> walk;
    for (uint32_t id = 0; id < deltaBases.size(); ++id) {
      uint32_t at = id;
      walk.clear();
      while (at < deltaBases.size() && deltaBases[at] != UINT32_MAX &&
             state[at] != 2) {
        if (state[at] == 1) {
          deltaBases.clear();
          throw std::runtime_error("Element differences form a cycle");
        }
        state[at] = 1;
        walk.push_back(at);
        at = deltaBases[at];
      }
      for (uint32_t done : walk) state[done] = 2;
    }
    resetDecodedCaches();
    elementsStamp = NextSparseVectorStamp();
  }

  // Stores each element of `candidates`, in their order, as the XOR of its
  // values with those of the base element paired with it, where that makes
  // the payloads smaller. A payload the same bytes of another element also
  // use costs nothing, so an element that shares its payload stays whole, and
  // a difference that another element has already costs nothing. The values
  // read back are the same. Skips an element stored as a difference already,
  // a base that leads back to it, and values that would be the no-data
  // values; does nothing for a vector without LZ4 compression. Returns how
  // many elements it stored as differences.
  size_t storeAsDifferences(
      const std::vector<std::pair<uint32_t, uint32_t>> &candidates) {
    if (useIndex || !useCompression || useBinaryBitPacking) return 0;
    restoreDataFromPacked();
    // How many elements use each payload, by its hash.
    std::unordered_map<uint64_t, uint32_t> uses;
    for (const auto &entry : data) {
      ++uses[hashPayload(entry.second.data(), entry.second.size())];
    }
    size_t stored = 0;
    std::vector<T> difference;
    for (const auto &candidate : candidates) {
      const uint32_t elementId = candidate.first;
      const uint32_t base = candidate.second;
      if (elementId == base || base == UINT32_MAX || !hasData(elementId) ||
          !hasData(base) || isStoredAsDifference(elementId)) {
        continue;
      }
      bool leadsBack = false;
      for (uint32_t at = base; at != UINT32_MAX;
           at = at < deltaBases.size() ? deltaBases[at] : UINT32_MAX) {
        if (at == elementId) {
          leadsBack = true;
          break;
        }
      }
      if (leadsBack) continue;

      const size_t count = elementSize;
      const T *values = (*this)[elementId];
      difference.assign(values, values + count);
      const T *baseValues = (*this)[base];
      bool any = false;
      for (size_t i = 0; i < count; ++i) {
        difference[i] = static_cast<T>(difference[i] ^ baseValues[i]);
        any = any || difference[i] != noData[0];
      }
      if (!any) continue;

      const std::vector<uint8_t> whole = data[elementId];
      const uint64_t wholeHash = hashPayload(whole.data(), whole.size());
      std::vector<uint32_t> bases = std::move(deltaBases);
      set(elementId, difference.data(), count);
      deltaBases = std::move(bases);
      const std::vector<uint8_t> &packed = data[elementId];
      const uint64_t packedHash = hashPayload(packed.data(), packed.size());
      const size_t costBefore = uses[wholeHash] == 1 ? whole.size() : 0;
      const auto shared = uses.find(packedHash);
      const size_t costAfter =
          shared != uses.end() && shared->second > 0 ? 0 : packed.size();
      if (costAfter >= costBefore) {
        data[elementId] = whole;
        continue;
      }
      --uses[wholeHash];
      ++uses[packedHash];
      if (deltaBases.size() <= elementId) {
        deltaBases.resize(static_cast<size_t>(elementId) + 1, UINT32_MAX);
      }
      deltaBases[elementId] = base;
      ++stored;
    }
    resetDecodedCaches();
    elementsStamp = NextSparseVectorStamp();
    return stored;
  }

  template <typename U = T>
  void set(uint32_t elementId, const T *values, size_t size,
           SparseVector<U> *parent = nullptr,
           bool preserveNoDataPayload = false) {
    if (useIndex) {
      throw std::runtime_error("set() must not be used for index");
    }

    restoreDataFromPacked();
    elementSize = size;
    clearPacked();
    resetDecodedCaches();
    elementsStamp = NextSparseVectorStamp();
    // Stored whole from now on.
    if (elementId < deltaBases.size()) deltaBases[elementId] = UINT32_MAX;

    if (decompBuffer.size() < (elementSize * sizeof(T))) {
      decompBuffer.resize(elementSize * sizeof(T));
    }

    if (noData.size() < elementSize) {
      noData.resize(elementSize, noData[0]);
    }

    if (parent == nullptr || parent->hasData(elementId)) {
      if (!preserveNoDataPayload &&
          memcmp(values, noData.data(), elementSize * sizeof(T)) == 0) {
        data.erase(elementId);
        return;
      }
      if (preserveNoDataPayload ||
          memcmp(values, noData.data(), elementSize * sizeof(T)) != 0) {
        std::vector<uint8_t> valuePacked;
        const uint8_t *storeBytes = reinterpret_cast<const uint8_t *>(values);
        size_t storeByteSize = elementSize * sizeof(T);

        if (useBinaryBitPacking) {
          if (encodeValuePacked(values, valuePacked)) {
            storeBytes = valuePacked.data();
            storeByteSize = valuePacked.size();
          }
        }

        if (useCompression) {
          const size_t maxCompressedSize =
              LZ4_compressBound(static_cast<int>(storeByteSize));
          std::vector<uint8_t> compBuffer(maxCompressedSize);
          int compressedSize = 0;
          if (SparseVectorFastPacking()) {
            compressedSize = LZ4_compress_default(
                reinterpret_cast<const char *>(storeBytes),
                reinterpret_cast<char *>(compBuffer.data()),
                static_cast<int>(storeByteSize),
                static_cast<int>(maxCompressedSize));
          } else {
            const int compressionLevel =
                is_real_machine() ? LZ4HC_CLEVEL_MIN : LZ4HC_CLEVEL_MAX;
            compressedSize = LZ4_compress_HC(
                reinterpret_cast<const char *>(storeBytes),
                reinterpret_cast<char *>(compBuffer.data()),
                static_cast<int>(storeByteSize),
                static_cast<int>(maxCompressedSize), compressionLevel);
          }

          if (compressedSize > 0) {
            data[elementId].assign(compBuffer.begin(),
                                   compBuffer.begin() + compressedSize);
          }
        } else {
          // Without compression, store directly.
          data[elementId].assign(storeBytes, storeBytes + storeByteSize);
        }
      }
    }
  }

  // A block of numElements elements, stored one by one with storeElement():
  // what readFromCRomReader() reads from a cROM, and what the builder of an
  // authoring tool hands in instead (serum-builder.h). Both store the same.
  void beginElements(size_t elementSize, uint32_t numElements) {
    if (useIndex) {
      index.assign(numElements, std::vector<T>(elementSize));
    }
  }

  template <typename U = T>
  void storeElement(uint32_t elementId, const T *values, size_t elementSize,
                    SparseVector<U> *parent = nullptr) {
    if (useIndex) {
      index[elementId].assign(values, values + elementSize);
    } else {
      set(elementId, values, elementSize, parent);
    }
  }

  template <typename Reader, typename U = T>
  void readFromCRomReader(size_t elementSize, uint32_t numElements,
                          Reader &reader, SparseVector<U> *parent = nullptr) {
    beginElements(elementSize, numElements);
    std::vector<T> tmp(elementSize);
    for (uint32_t i = 0; i < numElements; ++i) {
      if (!reader.readExact(tmp.data(), elementSize * sizeof(T))) {
        fprintf(stderr, "File read error\n");
        exit(1);
      }
      storeElement(i, tmp.data(), elementSize, parent);
    }
  }

  template <typename U = T>
  void readFromCRomFile(size_t elementSize, uint32_t numElements, FILE *stream,
                        SparseVector<U> *parent = nullptr) {
    struct FileReader {
      FILE *stream;
      bool readExact(void *dst, size_t bytes) {
        return fread(dst, 1, bytes, stream) == bytes;
      }
    } reader{stream};
    readFromCRomReader(elementSize, numElements, reader, parent);
  }

  void reserve(size_t elementSize) {
    if (noData.size() < elementSize) {
      noData.resize(elementSize, noData[0]);
    }
  }

  void clearIndex() { index.clear(); }

  void clear() {
    index.clear();
    data.clear();
    clearPacked();
    noData.resize(1);
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
    resetDecodedCaches();
    forceDecodedReads = false;
    forcedDecoded.clear();
    deltaBases.clear();
    elementsStamp = NextSparseVectorStamp();
  }

  template <typename U = T>
  void setParent(SparseVector<U> *parent) {
    if (!parent || useIndex) {
      return;  // Parent cannot be set for index-based vectors or if no parent
               // is provided
    }
    // The archive load has filtered by this parent already.
    if (keptAtLoad) {
      keptAtLoad = false;
      return;
    }
    elementsStamp = NextSparseVectorStamp();

    if (!packedIds.empty()) {
      std::vector<uint8_t> keep(packedIds.size());
      for (size_t i = 0; i < packedIds.size(); ++i) {
        keep[i] = parent->hasData(packedIds[i]) ? 1 : 0;
      }
      const serum_accel::PackedPayloads payloads{packedIds, packedOffsets,
                                                 packedSizes, packedBlob};
      if (!serum_accel::KeepPackedPayloads(payloads, keep.data())) {
        std::vector<uint32_t> newIds;
        std::vector<uint32_t> newOffsets;
        std::vector<uint32_t> newSizes;
        std::vector<uint8_t> newBlob;

        newIds.reserve(packedIds.size());
        newOffsets.reserve(packedOffsets.size());
        newSizes.reserve(packedSizes.size());
        newBlob.reserve(packedBlob.size());

        uint32_t offset = 0;
        for (size_t i = 0; i < packedIds.size(); ++i) {
          const uint32_t elementId = packedIds[i];
          if (!keep[i]) {
            continue;
          }
          if (i >= packedOffsets.size() || i >= packedSizes.size()) {
            continue;
          }

          const uint32_t oldOffset = packedOffsets[i];
          const uint32_t size = packedSizes[i];
          if (oldOffset > packedBlob.size() ||
              size > packedBlob.size() - oldOffset) {
            continue;
          }

          newIds.push_back(elementId);
          newOffsets.push_back(offset);
          newSizes.push_back(size);
          newBlob.insert(newBlob.end(), packedBlob.begin() + oldOffset,
                         packedBlob.begin() + oldOffset + size);
          offset += size;
        }

        packedIds = std::move(newIds);
        packedOffsets = std::move(newOffsets);
        packedSizes = std::move(newSizes);
        packedBlob = std::move(newBlob);
        deduplicatePackedBlob();
      }
      packedIndexById.clear();
      packedDenseIndexById.clear();
      packedIndexReady = false;
      data.clear();

      lastPayloadId = UINT32_MAX;
      lastPayloadPtr = nullptr;
      lastPayloadSize = 0;
      resetDecodedCaches();
      forceDecodedReads = false;
      forcedDecoded.clear();
      return;
    }

    std::unordered_map<uint32_t, std::vector<uint8_t>> filteredData;

    for (const auto &entry : data) {
      uint32_t elementId = entry.first;

      if (parent->hasData(elementId)) {
        filteredData[elementId] = entry.second;
      }
    }

    data = std::move(filteredData);

    // Clear cache
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
    resetDecodedCaches();
    forceDecodedReads = false;
    forcedDecoded.clear();
  }

  void clearForcedDecodedCache() {
    forceDecodedReads = false;
    forcedDecoded.clear();
  }

  void setProfileLabel(const char *label) { profileLabel = label; }

  const char *getProfileLabel() const { return profileLabel; }

  void consumeProfileCounters(uint64_t &accesses, uint64_t &decodes,
                              uint64_t &cacheHits, uint64_t &directHits) {
    accesses = profileAccessCount;
    decodes = profileDecodeCount;
    cacheHits = profileCacheHitCount;
    directHits = profileDirectHitCount;
    profileAccessCount = 0;
    profileDecodeCount = 0;
    profileCacheHitCount = 0;
    profileDirectHitCount = 0;
  }

  void enableForcedDecodedReadsForIds(const std::vector<uint32_t> &ids) {
    forcedDecoded.clear();
    forcedDecoded.reserve(ids.size());
    forceDecodedReads = false;
    for (uint32_t id : ids) {
      if (!hasData(id)) {
        continue;
      }
      T *decoded = (*this)[id];
      if (!decoded) {
        continue;
      }
      forcedDecoded.emplace(id, std::vector<T>(decoded, decoded + elementSize));
    }
    forceDecodedReads = true;
  }

  // The parent the next archive load filters by, as setParent() does after it.
  template <typename U>
  void setLoadParent(const SparseVector<U> *parent) {
    loadParent = parent;
    loadParentHasData = [](const void *p, uint32_t elementId) {
      return static_cast<const SparseVector<U> *>(p)->hasData(elementId);
    };
  }

  friend class cereal::access;

  template <class Archive>
  static void readFromArchive(void *archive, uint8_t *dst, size_t size) {
    (*static_cast<Archive *>(archive))(cereal::binary_data(dst, size));
  }

  // The packed blob, in the layout of a std::vector<uint8_t> in the archive.
  template <class Archive>
  void loadPackedBlob(Archive &ar) {
    cereal::size_type size = 0;
    ar(cereal::make_size_tag(size));
    keptAtLoad = false;
    if (loadParent) {
      std::vector<uint8_t> keep(packedIds.size());
      for (size_t i = 0; i < packedIds.size(); ++i) {
        keep[i] = loadParentHasData(loadParent, packedIds[i]) ? 1 : 0;
      }
      const serum_accel::PackedPayloads payloads{packedIds, packedOffsets,
                                                 packedSizes, packedBlob};
      keptAtLoad = serum_accel::LoadPackedPayloads(
          payloads, keep.data(), size, &readFromArchive<Archive>, &ar);
    }
    if (!keptAtLoad) {
      packedBlob.resize(static_cast<size_t>(size));
      ar(cereal::binary_data(packedBlob.data(), static_cast<size_t>(size)));
    }
  }

  template <class Archive>
  void serialize(Archive &ar) {
    if constexpr (Archive::is_saving::value) {
      if (!useIndex && packedIds.empty() && !data.empty()) {
        buildPackedFromData();
      }

      ar(index, noData, elementSize, useIndex, useCompression,
         useBinaryBitPacking, bitPackFalseValue, bitPackTrueValue);
      if (!useIndex) {
        ar(packedIds, packedOffsets, packedSizes, packedBlob);
      }
      return;
    }

    keptAtLoad = false;
    deltaBases.clear();
    elementsStamp = NextSparseVectorStamp();
    if (sparse_vector_serialization::IsLegacyLoadExpected()) {
      ar(index, data, noData, elementSize, decompBuffer, useIndex,
         useCompression);
      clearPacked();
      if (!useIndex && !data.empty()) {
        buildPackedFromData();
        ensurePackedIndex();
      }
    } else {
      ar(index, noData, elementSize, useIndex, useCompression,
         useBinaryBitPacking, bitPackFalseValue, bitPackTrueValue);
      data.clear();
      decompBuffer.clear();
      clearPacked();
      if (!useIndex) {
        ar(packedIds, packedOffsets, packedSizes);
        loadPackedBlob(ar);
        // v6 cROMc files are guaranteed to have sorted packedIds from save-time
        // enforcement. Only ensure indices are built, don't repair (saves RAM
        // on resource-constrained devices).
        ensurePackedIndex();
      }
    }
    loadParent = nullptr;

    // Clear cache
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
    resetDecodedCaches();
    forceDecodedReads = false;
    forcedDecoded.clear();
  }
};
