#!/usr/bin/env python3
"""Compile the real MercurySession against a deterministic fake transport."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CSPOT = ROOT / "cspot" / "cspot"

STUBS = {
    "BellTask.h": """
#pragma once
#include <string>
namespace bell {
class Task {
 public:
  int stackSize;
  bool runOnPSRAM;
  Task(std::string, int size, int, int, bool external = true)
      : stackSize(size), runOnPSRAM(external) {}
  virtual ~Task() = default;
  virtual void runTask() = 0;
};
}
""",
    "BellUtils.h": """
#pragma once
void testSleep();
#define BELL_SLEEP_MS(ms) testSleep()
""",
    "BellLogger.h": "#pragma once\n",
    "Logger.h": "#pragma once\n#define CSPOT_LOG(...) ((void)0)\n",
    "Queue.h": """
#pragma once
#include <deque>
#include <mutex>
namespace bell {
template<class T> class Queue {
  std::deque<T> values;
  std::mutex mutex;
 public:
  void push(const T& value) {
    std::scoped_lock lock(mutex);
    values.push_back(value);
  }
  bool wtpop(T& value, int) {
    std::scoped_lock lock(mutex);
    if (values.empty()) return false;
    value = values.front();
    values.pop_front();
    return true;
  }
};
}
""",
    "protobuf/mercury.pb.h": """
#pragma once
struct Header {
  char uri[256] = {};
  char method[32] = {};
  bool has_uri = false;
  bool has_method = false;
};
constexpr int Header_fields = 0;
""",
    "NanoPBHelper.h": """
#pragma once
#include <cstring>
#include <string>
#include <vector>
#include "protobuf/mercury.pb.h"
template<size_t N> void pbPutString(const std::string& value, char (&out)[N]) {
  std::strncpy(out, value.c_str(), N - 1);
}
inline std::vector<uint8_t> pbEncode(int, Header*) { return {0}; }
inline void pbDecode(Header&, int, const std::vector<uint8_t>&) {}
""",
    "PlainConnection.h": """
#pragma once
namespace cspot {
class PlainConnection {
 public:
  void close() {}
};
}
""",
    "ShannonConnection.h": """
#pragma once
#include <functional>
#include "Packet.h"
extern std::function<void()> onSend;
namespace cspot {
class ShannonConnection {
 public:
  void sendPacket(uint8_t, std::vector<uint8_t>&) { onSend(); }
  Packet recvPacket() { return {}; }
};
}
""",
    "TimeProvider.h": """
#pragma once
#include <vector>
namespace cspot {
class TimeProvider {
 public:
  unsigned long long getSyncedTimestamp() { return 1000; }
  void syncWithPingPacket(const std::vector<uint8_t>&) {}
};
}
""",
    "Utils.h": """
#pragma once
#include <cstring>
#include <vector>
inline uint64_t hton64(uint64_t value) { return __builtin_bswap64(value); }
template<class T> std::vector<uint8_t> pack(T value) {
  std::vector<uint8_t> result(sizeof(T));
  std::memcpy(result.data(), &value, sizeof(T));
  return result;
}
template<class T> T extract(const std::vector<uint8_t>& value, size_t offset) {
  T result;
  std::memcpy(&result, value.data() + offset, sizeof(T));
  return result;
}
""",
}

HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "MercurySession.h"
#include "PlainConnection.h"
#include "ShannonConnection.h"
#include "TimeProvider.h"

std::function<void()> onSend = [] {};
std::function<void()> onSleep = [] {};
void testSleep() { onSleep(); }
int attempts = 0, connectFailures = 0, authFailures = 0;
uintptr_t stackMin = UINTPTR_MAX, stackMax = 0;

namespace cspot {
class AuthChallenges {};
Session::Session() = default;
Session::~Session() = default;
void Session::connectWithRandomAp() {
  auto stackAddress = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  stackMin = std::min(stackMin, stackAddress);
  stackMax = std::max(stackMax, stackAddress);
  ++attempts;
  conn = std::make_shared<PlainConnection>();
  shanConn = std::make_shared<ShannonConnection>();
  if (connectFailures-- > 0) throw std::runtime_error("simulated offline");
}
std::vector<uint8_t> Session::authenticate(std::shared_ptr<LoginBlob>) {
  return authFailures-- > 0 ? std::vector<uint8_t>{} : std::vector<uint8_t>{1};
}
}

int main() {
  using cspot::MercurySession;
  auto clock = std::make_shared<cspot::TimeProvider>();
  MercurySession session(clock);
  assert(session.stackSize == 32 * 1024 && session.runOnPSRAM);
  session.isRunning = true;

  // Thousands of failed attempts must not grow the call stack.
  connectFailures = 5000;
  authFailures = 2;
  session.reconnect();
  assert(attempts == 5003);
  assert(stackMax - stackMin < 1024);
  assert(!session.isReconnecting && session.executeEstabilishedCallback);
  std::cout << "Repeated failures and rejected authentication recovered\n";

  session.executeEstabilishedCallback = false;
  attempts = 0;
  connectFailures = 10;
  onSleep = [&] { session.isRunning = false; };
  session.reconnect();
  assert(attempts == 1 && !session.executeEstabilishedCallback);
  onSleep = [] {};

  // Request failures must notify callers, not leave them waiting forever.
  int failures = 0;
  session.execute(MercurySession::RequestType::GET, "test",
                  [&](auto& response) { assert(response.fail); ++failures; });
  session.requestAudioKey({}, {}, [&](bool ok, auto&) {
    assert(!ok);
    ++failures;
  });
  assert(failures == 2);
  assert(session.callbacks.empty() && session.audioKeyCallbacks.empty());

  // Failed callbacks may unregister or submit other requests without deadlock.
  session.callbacks.emplace(10, [&](auto& response) {
    assert(response.fail);
    session.unregister(10);
    ++failures;
  });
  session.audioKeyCallbacks.emplace(11, [&](bool ok, auto&) {
    assert(!ok);
    session.unregisterAudioKey(11);
    ++failures;
  });
  session.failAllPending();
  assert(failures == 4);

  // Concurrent request submission must produce distinct sequence IDs.
  session.isReconnecting = false;
  onSend = [] {};
  auto submit = [&] {
    for (int i = 0; i < 1000; ++i) {
      session.execute(MercurySession::RequestType::GET, "test", [](auto&) {});
    }
  };
  std::thread first(submit), second(submit);
  first.join();
  second.join();
  assert(session.callbacks.size() == 2000);
  session.failAllPending();

  // Replacing the active connection must not destroy an in-flight sender.
  std::promise<void> entered, released;
  auto release = released.get_future().share();
  onSend = [&] {
    entered.set_value();
    release.wait();
  };
  std::weak_ptr<cspot::ShannonConnection> old = session.shanConn;
  std::thread sender([&] {
    std::vector<uint8_t> data;
    session.sendPacket(1, data);
  });
  entered.get_future().wait();
  {
    std::scoped_lock lock(session.connectionMutex);
    session.isReconnecting = true;
    session.shanConn.reset();
  }
  assert(!old.expired());
  released.set_value();
  sender.join();
  assert(old.expired());

  // A cancelled track may unregister concurrently with a response.
  session.callbacks.emplace(20, [](auto&) {});
  std::thread cancel([&] { session.unregister(20); });
  auto callback = session.takeCallback(20);
  cancel.join();
  assert(session.callbacks.empty());
  std::cout << "Cancellation, callback locking and connection lifetime passed\n";
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="spotifyesp-reconnect-") as directory:
        work = Path(directory)
        for name, contents in STUBS.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        (work / "test.cpp").write_text(HARNESS)
        executable = work / "test"
        subprocess.run(
            [
                os.environ.get("CXX", "clang++"),
                "-std=c++20",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-fno-access-control",
                "-pthread",
                "-I", str(work),
                "-I", str(CSPOT / "include"),
                str(work / "test.cpp"),
                str(CSPOT / "src" / "MercurySession.cpp"),
                "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
