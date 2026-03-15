#pragma once

#include <algorithm>
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

    for (size_t i = 0; i < elementSize; ++i) {
      if (modeBits == kValuePackedMode1Bit) {
        const bool isSet = (payload[2 + (i / 8)] & (1u << (i % 8))) != 0;
        lastDecompressed[i] = static_cast<T>(isSet ? 1 : 0);
      } else if (modeBits == kValuePackedMode2Bit) {
        const size_t bitPos = i * 2;
        lastDecompressed[i] =
            static_cast<T>((payload[2 + (bitPos / 8)] >> (bitPos % 8)) & 0x3u);
      } else {
        const size_t bitPos = i * 4;
        lastDecompressed[i] =
            static_cast<T>((payload[2 + (bitPos / 8)] >> (bitPos % 8)) & 0xFu);
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
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
  }

  void ensurePackedIndex() const {
    if (packedIds.empty()) {
      packedIndexById.clear();
      packedDenseIndexById.clear();
      return;
    }
    if (packedIndexById.size() == packedIds.size() &&
        !packedDenseIndexById.empty()) {
      return;
    }
    packedIndexById.clear();
    packedIndexById.reserve(packedIds.size());
    for (uint32_t i = 0; i < packedIds.size(); ++i) {
      packedIndexById.emplace(packedIds[i], i);
    }

    // Fast path for dense/small ID spaces (frame IDs, sprite IDs, etc).
    // This avoids hash lookup overhead in operator[] hot loops.
    packedDenseIndexById.clear();
    const uint32_t maxPackedId = packedIds.back();
    if (maxPackedId <= 1000000 &&
        maxPackedId <= static_cast<uint32_t>(packedIds.size() * 8)) {
      packedDenseIndexById.assign(static_cast<size_t>(maxPackedId) + 1,
                                  UINT32_MAX);
      for (uint32_t i = 0; i < packedIds.size(); ++i) {
        packedDenseIndexById[packedIds[i]] = i;
      }
    }
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

  template <typename U = T>
  void set(uint32_t elementId, const T *values, size_t size,
           SparseVector<U> *parent = nullptr) {
    if (useIndex) {
      throw std::runtime_error("set() must not be used for index");
    }

    restoreDataFromPacked();
    elementSize = size;
    clearPacked();
    resetDecodedCaches();

    if (decompBuffer.size() < (elementSize * sizeof(T))) {
      decompBuffer.resize(elementSize * sizeof(T));
    }

    if (noData.size() < elementSize) {
      noData.resize(elementSize, noData[0]);
    }

    if (parent == nullptr || parent->hasData(elementId)) {
      if (memcmp(values, noData.data(), elementSize * sizeof(T)) != 0) {
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

          int compressedSize =
              LZ4_compress_HC(reinterpret_cast<const char *>(storeBytes),
                              reinterpret_cast<char *>(compBuffer.data()),
                              static_cast<int>(storeByteSize),
                              static_cast<int>(maxCompressedSize),
#ifdef WRITE_CROMC
                              LZ4HC_CLEVEL_MAX  // max compression level
#else
                              LZ4HC_CLEVEL_MIN  // min compression level
#endif
              );

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

  template <typename U = T>
  void readFromCRomFile(size_t elementSize, uint32_t numElements, FILE *stream,
                        SparseVector<U> *parent = nullptr) {
    if (useIndex) {
      index.resize(numElements);
      for (uint32_t i = 0; i < numElements; ++i) {
        index[i].resize(elementSize);
        if (fread(index[i].data(), sizeof(T), elementSize, stream) !=
            elementSize) {
          fprintf(stderr, "File read error\n");
          exit(1);
        }
      }
    } else {
      std::vector<T> tmp(elementSize);

      for (uint32_t i = 0; i < numElements; ++i) {
        if (fread(tmp.data(), elementSize * sizeof(T), 1, stream) != 1) {
          fprintf(stderr, "File read error\n");
          exit(1);
        }

        set(i, tmp.data(), elementSize, parent);
      }
    }
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
  }

  template <typename U = T>
  void setParent(SparseVector<U> *parent) {
    if (!parent || useIndex) {
      return;  // Parent cannot be set for index-based vectors or if no parent
               // is provided
    }

    if (!packedIds.empty()) {
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
        if (!parent->hasData(elementId)) {
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
      packedIndexById.clear();
      packedDenseIndexById.clear();
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

  friend class cereal::access;

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
        ar(packedIds, packedOffsets, packedSizes, packedBlob);
        ensurePackedIndex();
      }
    }

    // Clear cache
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
    resetDecodedCaches();
    forceDecodedReads = false;
    forcedDecoded.clear();
  }
};
