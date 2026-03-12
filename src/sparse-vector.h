#pragma once

#include <algorithm>
#include <cereal/access.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>
#include <cstdint>
#include <cstring>
#include <stdexcept>
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
  mutable std::vector<T> lastDecompressed;
  mutable std::vector<uint8_t> decodeScratch;
  std::vector<uint32_t> packedIds;
  std::vector<uint32_t> packedOffsets;
  std::vector<uint32_t> packedSizes;
  std::vector<uint8_t> packedBlob;

  static constexpr uint8_t kBitPackedMagic = 0xB1;

  size_t rawByteSize() const { return elementSize * sizeof(T); }

  size_t bitPackedByteSize() const { return 1 + ((elementSize + 7) / 8); }

  bool isBitPackedPayload(const uint8_t *payload, size_t size) const {
    if (!useBinaryBitPacking || !payload) {
      return false;
    }
    if (!std::is_same<T, uint8_t>::value) {
      return false;
    }
    return size == bitPackedByteSize() && payload[0] == kBitPackedMagic;
  }

  void encodeBitPacked(const T *values, std::vector<uint8_t> &encoded) const {
    if (!useBinaryBitPacking) {
      encoded.clear();
      return;
    }
    if (!std::is_same<T, uint8_t>::value) {
      throw std::runtime_error(
          "Binary bit packing is only supported for uint8_t");
    }

    encoded.assign(bitPackedByteSize(), 0);
    encoded[0] = kBitPackedMagic;
    for (size_t i = 0; i < elementSize; ++i) {
      if (values[i] != bitPackFalseValue) {
        encoded[1 + (i / 8)] |= (1u << (i % 8));
      }
    }
  }

  T *decodeBitPackedAndCache(uint32_t elementId, const uint8_t *payload) {
    if (lastDecompressed.size() < elementSize) {
      lastDecompressed.resize(elementSize);
    }

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
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
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

    const auto it =
        std::lower_bound(packedIds.begin(), packedIds.end(), elementId);
    if (it == packedIds.end() || *it != elementId) {
      return nullptr;
    }

    const size_t idx = static_cast<size_t>(it - packedIds.begin());
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
  }

  SparseVector(T noDataSignature)
      : useIndex(false),
        useCompression(false),
        useBinaryBitPacking(false),
        bitPackFalseValue(noDataSignature),
        bitPackTrueValue(static_cast<T>(1)) {
    noData.resize(1, noDataSignature);
  }

  T *operator[](const uint32_t elementId) {
    if (useIndex) {
      if (elementId >= index.size()) return noData.data();
      return index[elementId].data();
    } else {
      if (useCompression && elementId == lastAccessedId &&
          !lastDecompressed.empty()) {
        return lastDecompressed.data();
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
        // Cache hit only applies to decoded cache-backed payloads.
        if (elementId == lastAccessedId) {
          return lastDecompressed.data();
        }

        const size_t rawBytes = rawByteSize();
        if (decodeScratch.size() < rawBytes) {
          decodeScratch.resize(rawBytes);
        }

        int decompressedSize = LZ4_decompress_safe(
            reinterpret_cast<const char *>(payload),
            reinterpret_cast<char *>(decodeScratch.data()),
            static_cast<int>(payloadSize), static_cast<int>(rawBytes));

        if (decompressedSize < 0) {
          // Backward compatibility: some payloads may be stored raw.
          if (isBitPackedPayload(payload, payloadSize)) {
            return decodeBitPackedAndCache(elementId, payload);
          }

          // Backward compatibility: older payloads may store raw bytes even if
          // this vector now defaults to compression.
          if (payloadSize == rawBytes) {
            return reinterpret_cast<T *>(const_cast<uint8_t *>(payload));
          }
          return noData.data();
        }

        if (isBitPackedPayload(decodeScratch.data(),
                               static_cast<size_t>(decompressedSize))) {
          return decodeBitPackedAndCache(elementId, decodeScratch.data());
        }

        if (static_cast<size_t>(decompressedSize) != rawBytes) {
          return noData.data();
        }

        if (lastDecompressed.size() < elementSize) {
          lastDecompressed.resize(elementSize);
        }
        memcpy(lastDecompressed.data(), decodeScratch.data(), rawBytes);
        lastAccessedId = elementId;
        return lastDecompressed.data();
      }

      if (isBitPackedPayload(payload, payloadSize)) {
        return decodeBitPackedAndCache(elementId, payload);
      }

      if (payloadSize != rawByteSize()) {
        return noData.data();
      }

      return reinterpret_cast<T *>(const_cast<uint8_t *>(payload));
    }
  }

  bool hasData(uint32_t elementId) const {
    if (useIndex)
      return elementId < index.size() && !index[elementId].empty() &&
             index[elementId][0] != noData[0];
    if (!packedIds.empty()) {
      return std::binary_search(packedIds.begin(), packedIds.end(), elementId);
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
    lastAccessedId = UINT32_MAX;
    lastDecompressed.clear();
    decodeScratch.clear();

    if (decompBuffer.size() < (elementSize * sizeof(T))) {
      decompBuffer.resize(elementSize * sizeof(T));
    }

    if (noData.size() < elementSize) {
      noData.resize(elementSize, noData[0]);
    }

    if (parent == nullptr || parent->hasData(elementId)) {
      if (memcmp(values, noData.data(), elementSize * sizeof(T)) != 0) {
        std::vector<uint8_t> bitPacked;
        const uint8_t *storeBytes = reinterpret_cast<const uint8_t *>(values);
        size_t storeByteSize = elementSize * sizeof(T);

        if (useBinaryBitPacking) {
          encodeBitPacked(values, bitPacked);
          storeBytes = bitPacked.data();
          storeByteSize = bitPacked.size();
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
    lastAccessedId = UINT32_MAX;
    lastDecompressed.clear();
    decodeScratch.clear();
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
      data.clear();

      lastPayloadId = UINT32_MAX;
      lastPayloadPtr = nullptr;
      lastPayloadSize = 0;
      lastAccessedId = UINT32_MAX;
      lastDecompressed.clear();
      decodeScratch.clear();
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
    lastAccessedId = UINT32_MAX;
    lastDecompressed.clear();
    decodeScratch.clear();
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
      }
    } else {
      ar(index, noData, elementSize, useIndex, useCompression,
         useBinaryBitPacking, bitPackFalseValue, bitPackTrueValue);
      data.clear();
      decompBuffer.clear();
      clearPacked();
      if (!useIndex) {
        ar(packedIds, packedOffsets, packedSizes, packedBlob);
      }
    }

    // Clear cache
    lastPayloadId = UINT32_MAX;
    lastPayloadPtr = nullptr;
    lastPayloadSize = 0;
    lastAccessedId = UINT32_MAX;
    lastDecompressed.clear();
    decodeScratch.clear();
  }
};
