#pragma once

#include <chrono>
#include <cstdint>

// Monotonic millisecond clock to avoid issues from system clock jumps (e.g. NTP
// adjustments).
inline uint32_t GetMonotonicTimeMs() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
