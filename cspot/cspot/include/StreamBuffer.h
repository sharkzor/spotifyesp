#pragma once

#include <cstdint>
#include <vector>

#ifdef ESP_PLATFORM
#include <limits>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

namespace cspot {

#ifdef ESP_PLATFORM
template <typename T>
struct StreamAllocator {
  using value_type = T;

  StreamAllocator() = default;
  template <typename U>
  StreamAllocator(const StreamAllocator<U>&) noexcept {}

  T* allocate(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }
    auto* memory = static_cast<T*>(
        heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!memory) {
      ESP_LOGE("cspot", "Cannot allocate %u bytes of stream buffer in PSRAM",
               static_cast<unsigned>(count * sizeof(T)));
      throw std::bad_alloc();
    }
    return memory;
  }

  void deallocate(T* memory, std::size_t) noexcept {
    heap_caps_free(memory);
  }

  template <typename U>
  bool operator==(const StreamAllocator<U>&) const noexcept {
    return true;
  }
};

using StreamBuffer = std::vector<uint8_t, StreamAllocator<uint8_t>>;
#else
using StreamBuffer = std::vector<uint8_t>;
#endif

}  // namespace cspot
