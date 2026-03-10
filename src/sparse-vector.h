#pragma once

#include <cereal/access.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
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
  mutable uint32_t lastAccessedId = UINT32_MAX;
  mutable std::vector<T> lastDecompressed;
  std::vector<uint32_t> packedIds;
  std::vector<uint32_t> packedOffsets;
  std::vector<uint32_t> packedSizes;
  std::vector<uint8_t> packedBlob;

  void clearPacked() {
    packedIds.clear();
    packedOffsets.clear();
    packedSizes.clear();
    packedBlob.clear();
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
  }

  const uint8_t *getPackedPayload(uint32_t elementId,
                                  uint32_t *payloadSize) const {
    if (packedIds.empty()) {
      return nullptr;
    }

    const auto it = std::lower_bound(packedIds.begin(), packedIds.end(),
                                     elementId);
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
  SparseVector(T noDataSignature, bool index, bool compress = false)
      : useIndex(index), useCompression(compress) {
    noData.resize(1, noDataSignature);
  }

  SparseVector(T noDataSignature) : useIndex(false), useCompression(false) {
    noData.resize(1, noDataSignature);
  }

  T *operator[](const uint32_t elementId) {
    if (useIndex) {
      if (elementId >= index.size()) return noData.data();
      return index[elementId].data();
    } else {
      if (packedIds.empty() && !data.empty()) {
        buildPackedFromData();
      }

      const uint8_t *payload = nullptr;
      uint32_t payloadSize = 0;

      if (!packedIds.empty()) {
        payload = getPackedPayload(elementId, &payloadSize);
      } else {
        auto it = data.find(elementId);
        if (it != data.end()) {
          payload = it->second.data();
          payloadSize = static_cast<uint32_t>(it->second.size());
        }
      }

      if (!payload) return noData.data();

      if (useCompression) {
        // Cache-Hit
        if (elementId == lastAccessedId) {
          return lastDecompressed.data();
        }

        // ensure decompBuffer is large enough
        if (lastDecompressed.size() < elementSize) {
          lastDecompressed.resize(elementSize);
        }

        int decompressedSize = LZ4_decompress_safe(
            reinterpret_cast<const char *>(payload),
            reinterpret_cast<char *>(lastDecompressed.data()),
            static_cast<int>(payloadSize),
            static_cast<int>(elementSize * sizeof(T)));

        if (decompressedSize < 0) {
          // Backward compatibility: older payloads may store raw bytes even if
          // this vector now defaults to compression (e.g. legacy sprite data).
          if (payloadSize == elementSize * sizeof(T)) {
            return reinterpret_cast<T *>(const_cast<uint8_t *>(payload));
          }
          return noData.data();
        }

        // Cache-Update
        lastAccessedId = elementId;
        return lastDecompressed.data();
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

    elementSize = size;
    clearPacked();

    if (decompBuffer.size() < (elementSize * sizeof(T))) {
      decompBuffer.resize(elementSize * sizeof(T));
    }

    if (noData.size() < elementSize) {
      noData.resize(elementSize, noData[0]);
    }

    if (parent == nullptr || parent->hasData(elementId)) {
      if (memcmp(values, noData.data(), elementSize * sizeof(T)) != 0) {
        if (useCompression) {
          const size_t maxCompressedSize =
              LZ4_compressBound(static_cast<int>(elementSize * sizeof(T)));
          std::vector<uint8_t> compBuffer(maxCompressedSize);

          int compressedSize =
              LZ4_compress_HC(reinterpret_cast<const char *>(values),
                              reinterpret_cast<char *>(compBuffer.data()),
                              static_cast<int>(elementSize * sizeof(T)),
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
          const uint8_t *byteValues = reinterpret_cast<const uint8_t *>(values);
          data[elementId].assign(byteValues,
                                 byteValues + elementSize * sizeof(T));
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
    lastAccessedId = UINT32_MAX;
    lastDecompressed.clear();
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
        if (oldOffset > packedBlob.size() || size > packedBlob.size() - oldOffset) {
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
      data.clear();

      lastAccessedId = UINT32_MAX;
      lastDecompressed.clear();
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
    lastAccessedId = UINT32_MAX;
    lastDecompressed.clear();
  }

  friend class cereal::access;

  template <class Archive>
  void serialize(Archive &ar) {
    if constexpr (Archive::is_saving::value) {
      if (!useIndex && packedIds.empty() && !data.empty()) {
        buildPackedFromData();
      }

      ar(index, noData, elementSize, useIndex, useCompression);
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
      ar(index, noData, elementSize, useIndex, useCompression);
      data.clear();
      decompBuffer.clear();
      clearPacked();
      if (!useIndex) {
        ar(packedIds, packedOffsets, packedSizes, packedBlob);
      }
    }

    // Clear cache
    lastAccessedId = UINT32_MAX;
    lastDecompressed.clear();
  }
};
