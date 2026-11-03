#!/usr/bin/env python3
"""Exercise the real CDN stream with native and fake ESP32 memory allocation."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CSPOT = ROOT / "cspot/cspot"

STUBS = {
    "Crypto.h": """
#pragma once
#include <cstdint>
#include <vector>
class Crypto {
 public:
  void aesCTRXcrypt(const std::vector<uint8_t>&, const std::vector<uint8_t>&,
                   uint8_t*, size_t) {}
};
""",
    "Utils.h": """
#pragma once
#include <vector>
inline std::vector<uint8_t> bigNumAdd(const std::vector<uint8_t>& iv, size_t) {
  return iv;
}
""",
    "Logger.h": "#pragma once\n#define CSPOT_LOG(...) ((void)0)\n",
    "HTTPClient.h": """
#pragma once
#include <cassert>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
inline const std::string audioFile = [] {
  std::string data(100167, '\\0');
  for (size_t i = 0; i < data.size(); ++i) data[i] = char(i % 251);
  return data;
}();
namespace bell {
class HTTPClient {
 public:
  struct RangeHeader {
    size_t start, length;
    bool tail;
    static RangeHeader range(size_t start, size_t end) {
      return {start, end - start + 1, false};
    }
    static RangeHeader last(size_t length) { return {0, length, true}; }
  };
  class Response {
    std::istringstream input;
    size_t length = 0;
   public:
    void get(const std::string&, const std::vector<RangeHeader>& ranges) {
      assert(ranges.size() == 1);
      auto range = ranges[0];
      size_t start = range.tail ? audioFile.size() - range.length : range.start;
      auto data = audioFile.substr(start, range.length);
      length = data.size();
      input.clear();
      input.str(data);
    }
    std::istream& stream() { return input; }
    size_t totalLength() { return audioFile.size(); }
    size_t contentLength() { return length; }
  };
  static std::unique_ptr<Response> get(
      const std::string& url, const std::vector<RangeHeader>& ranges) {
    auto response = std::make_unique<Response>();
    response->get(url, ranges);
    return response;
  }
};
}
""",
    "esp_heap_caps.h": """
#pragma once
#include <cassert>
#include <cstdlib>
#include <map>
#include <vector>
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
constexpr unsigned MALLOC_CAP_8BIT = 2;
inline std::map<void*, size_t> allocations;
inline std::vector<size_t> requested;
inline size_t failOn = 0;
inline void* heap_caps_malloc(size_t size, unsigned caps) {
  assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  requested.push_back(size);
  if (failOn == requested.size()) return nullptr;
  void* ptr = std::malloc(size);
  assert(ptr);
  allocations[ptr] = size;
  return ptr;
}
inline void heap_caps_free(void* ptr) {
  assert(allocations.erase(ptr) == 1);
  std::free(ptr);
}
""",
    "esp_log.h": """
#pragma once
inline int allocationErrors = 0;
#define ESP_LOGE(...) (++allocationErrors)
""",
}

TEST = r"""
#include "CDNAudioFile.h"
#include <algorithm>
#include <cassert>
#include <iostream>

void checkRead(cspot::CDNAudioFile& stream, size_t offset) {
  stream.seek(offset);
  uint8_t buffer[256];
  assert(stream.readBytes(buffer, sizeof(buffer)) == sizeof(buffer));
  assert(stream.getPosition() == offset + sizeof(buffer));
  assert(std::equal(buffer, buffer + sizeof(buffer),
                    reinterpret_cast<const uint8_t*>(audioFile.data()) + 167 + offset));
}

int main() {
  for (int iteration = 0; iteration < 100; ++iteration) {
#ifdef ESP_PLATFORM
    requested.clear();
#endif
    {
      cspot::CDNAudioFile stream("fake://track", std::vector<uint8_t>(16));
#ifdef ESP_PLATFORM
      assert(requested.empty());  // Allocate inside the guarded openStream call.
#endif
      stream.openStream();
      assert(stream.getSize() == 100000);
      checkRead(stream, 0);      // Header.
      checkRead(stream, 9000);   // HTTP range.
      checkRead(stream, 9256);   // Reuse cached range.
      checkRead(stream, 55000);  // New range.
      checkRead(stream, 90000);  // Footer.
      checkRead(stream, 1000);   // Backward seek.
#ifdef ESP_PLATFORM
      const size_t footerSize =
          audioFile.size() - ((audioFile.size() - 12 * 1024) / 16 * 16);
      assert((requested == std::vector<size_t>{8192, 14336, footerSize}));
      assert(allocations.size() == 3);
#endif
      stream.openStream();
      checkRead(stream, 0);
    }
#ifdef ESP_PLATFORM
    assert(allocations.empty());
#endif
  }
#ifdef ESP_PLATFORM
  for (size_t failure = 1; failure <= 3; ++failure) {
    requested.clear();
    failOn = failure;
    bool threw = false;
    try {
      cspot::CDNAudioFile stream("fake://track", std::vector<uint8_t>(16));
      stream.openStream();
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    assert(threw);
    assert(requested.size() == failure);  // No fallback into internal memory.
    assert(allocationErrors == int(failure));
    assert(allocations.empty());
  }
#endif
  std::cout << "Stream sizes, read/seek, reopen and allocation lifecycle passed\n";
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="spotifyesp-stream-") as directory:
        work = Path(directory)
        # Copy unchanged source so only its external dependencies are stubbed.
        for name in ("CDNAudioFile.h", "StreamBuffer.h"):
            shutil.copyfile(CSPOT / "include" / name, work / name)
        shutil.copyfile(CSPOT / "src/CDNAudioFile.cpp", work / "CDNAudioFile.cpp")
        for name in ("AccessKeyFetcher.h", "BellLogger.h", "Packet.h",
                     "SocketStream.h", "WrappedSemaphore.h", "cJSON.h"):
            (work / name).write_text("#pragma once\n")
        for name, contents in STUBS.items():
            (work / name).write_text(contents)
        (work / "test.cpp").write_text(TEST)
        for platform in ("native", "esp32"):
            binary = work / platform
            command = [
                os.environ.get("CXX", "clang++"), "-std=c++20",
                "-fsanitize=address,undefined", "-g", "-DBELL_ONLY_CJSON",
                f"-I{work}", str(work / "CDNAudioFile.cpp"),
                str(work / "test.cpp"), "-o", str(binary),
            ]
            if platform == "esp32":
                command.append("-DESP_PLATFORM")
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)
        print("Native and ESP32 allocation-path regressions passed")


if __name__ == "__main__":
    main()
