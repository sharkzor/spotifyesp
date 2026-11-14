#include "MercurySession.h"

#include <string.h>     // for memcpy
#include <memory>       // for shared_ptr
#include <mutex>        // for scoped_lock
#include <stdexcept>    // for runtime_error
#include <type_traits>  // for remove_extent_t, __underlying_type_impl<>:...
#include <utility>      // for pair
#ifndef _WIN32
#include <arpa/inet.h>  // for htons, ntohs, htonl, ntohl
#endif
#include "BellLogger.h"         // for AbstractLogger
#include "BellTask.h"           // for Task
#include "BellUtils.h"          // for BELL_SLEEP_MS
#include "Logger.h"             // for CSPOT_LOG
#include "NanoPBHelper.h"       // for pbPutString, pbDecode, pbEncode
#include "PlainConnection.h"    // for PlainConnection
#include "ShannonConnection.h"  // for ShannonConnection
#include "TimeProvider.h"       // for TimeProvider
#include "Utils.h"              // for extract, pack, hton64

using namespace cspot;

MercurySession::MercurySession(std::shared_ptr<TimeProvider> timeProvider)
    : bell::Task("mercury_dispatcher", 32 * 1024, 3, 1) {
  this->timeProvider = timeProvider;
}

MercurySession::~MercurySession() {
  std::scoped_lock lock(this->isRunningMutex);
}

void MercurySession::runTask() {
  isRunning = true;
  std::scoped_lock lock(this->isRunningMutex);

  this->executeEstabilishedCallback = true;
  while (isRunning) {
    cspot::Packet packet = {};
    try {
      packet = shanConn->recvPacket();
      CSPOT_LOG(info, "Received packet, command: %d", packet.command);

      if (static_cast<RequestType>(packet.command) == RequestType::PING) {
        timeProvider->syncWithPingPacket(packet.data);

        this->lastPingTimestamp = timeProvider->getSyncedTimestamp();
        sendPacket(0x49, packet.data);
        logStackHeadroom();
      } else {
        this->packetQueue.push(packet);
      }
    } catch (const std::runtime_error& e) {
      CSPOT_LOG(error, "Error while receiving packet: %s", e.what());
      isReconnecting = true;
      failAllPending();

      if (!isRunning)
        return;

      reconnect();
      continue;
    }
  }
}

void MercurySession::reconnect() {
  isReconnecting = true;

  while (isRunning) {
    try {
      {
        std::scoped_lock lock(connectionMutex);
        this->shanConn.reset();
        this->conn.reset();
        // Authentication may need socket retries before the next Spotify ping.
        lastPingTimestamp = timeProvider->getSyncedTimestamp();
        this->connectWithRandomAp();
        if (this->authenticate(this->authBlob).empty()) {
          throw std::runtime_error("Reauthentication was rejected");
        }
        lastPingTimestamp = timeProvider->getSyncedTimestamp();
        isReconnecting = false;
      }

      CSPOT_LOG(info, "Reconnection successful");
      logStackHeadroom();
      this->executeEstabilishedCallback = true;
      return;
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "Cannot reconnect: %s; retrying in 5s", e.what());
      logStackHeadroom();
      for (int wait = 0; wait < 50 && isRunning; ++wait) {
        BELL_SLEEP_MS(100);
      }
    }
  }
}

void MercurySession::sendPacket(uint8_t command, std::vector<uint8_t>& data) {
  if (isReconnecting) {
    throw std::runtime_error("Spotify session is reconnecting");
  }
  std::shared_ptr<ShannonConnection> connection;
  {
    std::scoped_lock lock(connectionMutex);
    if (isReconnecting || !shanConn) {
      throw std::runtime_error("Spotify session is unavailable");
    }
    // Keep an in-flight sender's connection alive even if reconnect replaces it.
    connection = shanConn;
  }
  connection->sendPacket(command, data);
}

void MercurySession::logStackHeadroom() {
#ifdef ESP_PLATFORM
  CSPOT_LOG(info, "Dispatcher stack minimum free: %u bytes",
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
#endif
}

void MercurySession::setConnectedHandler(
    ConnectionEstabilishedCallback callback) {
  this->connectionReadyCallback = callback;
}

bool MercurySession::triggerTimeout() {
  if (!isRunning)
    return true;
  auto currentTimestamp = timeProvider->getSyncedTimestamp();

  if (currentTimestamp - this->lastPingTimestamp.load() > PING_TIMEOUT_MS) {
    CSPOT_LOG(debug, "Reconnection required, no ping received");
    return true;
  }

  return false;
}

void MercurySession::unregister(uint64_t sequenceId) {
  std::scoped_lock lock(requestsMutex);
  callbacks.erase(sequenceId);
}

void MercurySession::unregisterAudioKey(uint32_t sequenceId) {
  std::scoped_lock lock(requestsMutex);
  audioKeyCallbacks.erase(sequenceId);
}

MercurySession::ResponseCallback MercurySession::takeCallback(uint64_t id) {
  std::scoped_lock lock(requestsMutex);
  auto it = callbacks.find(id);
  if (it == callbacks.end()) return {};
  auto callback = std::move(it->second);
  callbacks.erase(it);
  return callback;
}

MercurySession::AudioKeyCallback MercurySession::takeAudioKeyCallback(uint32_t id) {
  std::scoped_lock lock(requestsMutex);
  auto it = audioKeyCallbacks.find(id);
  if (it == audioKeyCallbacks.end()) return {};
  auto callback = std::move(it->second);
  audioKeyCallbacks.erase(it);
  return callback;
}

void MercurySession::disconnect() {
  CSPOT_LOG(info, "Disconnecting mercury session");
  this->isRunning = false;
  {
    std::scoped_lock lock(connectionMutex);
    if (conn) conn->close();
  }
  std::scoped_lock lock(this->isRunningMutex);
}

std::string MercurySession::getCountryCode() {
  return this->countryCode;
}

void MercurySession::handlePacket() {
  Packet packet = {};

  this->packetQueue.wtpop(packet, 200);

  if (executeEstabilishedCallback && this->connectionReadyCallback != nullptr) {
    executeEstabilishedCallback = false;
    this->connectionReadyCallback();
  }

  switch (static_cast<RequestType>(packet.command)) {
    case RequestType::COUNTRY_CODE_RESPONSE: {
      this->countryCode = std::string();
      this->countryCode.resize(2);
      memcpy(this->countryCode.data(), packet.data.data(), 2);
      CSPOT_LOG(debug, "Received country code %s", this->countryCode.c_str());
      break;
    }
    case RequestType::AUDIO_KEY_FAILURE_RESPONSE:
    case RequestType::AUDIO_KEY_SUCCESS_RESPONSE: {
      // this->lastRequestTimestamp = -1;

      // First four bytes mark the sequence id
      auto seqId = ntohl(extract<uint32_t>(packet.data, 0));

      if (auto callback = takeAudioKeyCallback(seqId)) {
        auto success = static_cast<RequestType>(packet.command) ==
                       RequestType::AUDIO_KEY_SUCCESS_RESPONSE;
        if (!success) {
          CSPOT_LOG(error, "Audio key refused by Spotify, seq %u, %u data bytes, code %02x%02x",
                    (unsigned)seqId, (unsigned)packet.data.size(),
                    packet.data.size() > 4 ? packet.data[4] : 0,
                    packet.data.size() > 5 ? packet.data[5] : 0);
        }
        callback(success, packet.data);
      }

      break;
    }
    case RequestType::SEND:
    case RequestType::SUB:
    case RequestType::UNSUB: {
      CSPOT_LOG(debug, "Received mercury packet");

      auto response = this->decodeResponse(packet.data);
      if (auto callback = takeCallback(response.sequenceId)) {
        callback(response);
      }
      break;
    }
    case RequestType::SUBRES: {
      auto response = decodeResponse(packet.data);

      auto uri = std::string(response.mercuryHeader.uri);
      ResponseCallback callback;
      {
        std::scoped_lock lock(requestsMutex);
        auto it = subscriptions.find(uri);
        if (it != subscriptions.end()) callback = it->second;
      }
      if (callback) callback(response);
      break;
    }
    default:
      break;
  }
}

void MercurySession::failAllPending() {
  Response response = {};
  response.fail = true;

  decltype(callbacks) failedCallbacks;
  decltype(subscriptions) failedSubscriptions;
  decltype(audioKeyCallbacks) failedAudioKeys;
  {
    std::scoped_lock lock(requestsMutex);
    failedCallbacks.swap(callbacks);
    failedSubscriptions.swap(subscriptions);
    failedAudioKeys.swap(audioKeyCallbacks);
  }

  // Callbacks can take track locks or unregister requests; never hold our lock.
  for (auto& it : failedCallbacks) {
    if (it.second) it.second(response);
  }
  for (auto& it : failedSubscriptions) {
    if (it.second) it.second(response);
  }
  for (auto& it : failedAudioKeys) {
    if (it.second) it.second(false, {});
  }
}

MercurySession::Response MercurySession::decodeResponse(
    const std::vector<uint8_t>& data) {
  Response response = {};
  response.parts = {};

  auto sequenceLength = ntohs(extract<uint16_t>(data, 0));
  response.sequenceId = hton64(extract<uint64_t>(data, 2));

  auto partsNumber = ntohs(extract<uint16_t>(data, 11));

  auto headerSize = ntohs(extract<uint16_t>(data, 13));
  auto headerBytes =
      std::vector<uint8_t>(data.begin() + 15, data.begin() + 15 + headerSize);

  auto pos = 15 + headerSize;
  while (pos < data.size()) {
    auto partSize = ntohs(extract<uint16_t>(data, pos));

    response.parts.push_back(std::vector<uint8_t>(
        data.begin() + pos + 2, data.begin() + pos + 2 + partSize));
    pos += 2 + partSize;
  }

  pbDecode(response.mercuryHeader, Header_fields, headerBytes);
  response.fail = false;

  return response;
}

uint64_t MercurySession::executeSubscription(RequestType method,
                                             const std::string& uri,
                                             ResponseCallback callback,
                                             ResponseCallback subscription,
                                             DataParts& payload) {
  CSPOT_LOG(debug, "Executing Mercury Request, type %s",
            RequestTypeMap[method].c_str());

  // Encode header
  Header tempMercuryHeader = {};
  pbPutString(uri, tempMercuryHeader.uri);
  pbPutString(RequestTypeMap[method], tempMercuryHeader.method);

  tempMercuryHeader.has_method = true;
  tempMercuryHeader.has_uri = true;

  // GET and SEND are actually the same. Therefore the override
  // The difference between them is only in header's method
  if (method == RequestType::GET) {
    method = RequestType::SEND;
  }

  auto headerBytes = pbEncode(Header_fields, &tempMercuryHeader);

  uint64_t requestId;
  {
    std::scoped_lock lock(requestsMutex);
    requestId = sequenceId++;
    callbacks.emplace(requestId, std::move(callback));
    if (method == RequestType::SUB) subscriptions[uri] = std::move(subscription);
  }

  // Structure: [Sequence size] [SequenceId] [0x1] [Payloads number]
  // [Header size] [Header] [Payloads (size + data)]

  // Pack sequenceId
  auto sequenceIdBytes = pack<uint64_t>(hton64(requestId));
  auto sequenceSizeBytes = pack<uint16_t>(htons(sequenceIdBytes.size()));

  sequenceIdBytes.insert(sequenceIdBytes.begin(), sequenceSizeBytes.begin(),
                         sequenceSizeBytes.end());
  sequenceIdBytes.push_back(0x01);

  auto payloadNum = pack<uint16_t>(htons(payload.size() + 1));
  sequenceIdBytes.insert(sequenceIdBytes.end(), payloadNum.begin(),
                         payloadNum.end());

  auto headerSizePayload = pack<uint16_t>(htons(headerBytes.size()));
  sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                         headerSizePayload.end());
  sequenceIdBytes.insert(sequenceIdBytes.end(), headerBytes.begin(),
                         headerBytes.end());

  // Encode all the payload parts
  for (int x = 0; x < payload.size(); x++) {
    headerSizePayload = pack<uint16_t>(htons(payload[x].size()));
    sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                           headerSizePayload.end());
    sequenceIdBytes.insert(sequenceIdBytes.end(), payload[x].begin(),
                           payload[x].end());
  }

  try {
    sendPacket(
        static_cast<std::underlying_type<RequestType>::type>(method),
        sequenceIdBytes);
  } catch (const std::exception& e) {
    CSPOT_LOG(error, "Mercury request send failed: %s", e.what());
    if (auto failedCallback = takeCallback(requestId)) {
      Response response = {};
      response.fail = true;
      failedCallback(response);
    }
  }

  return requestId;
}

uint32_t MercurySession::requestAudioKey(const std::vector<uint8_t>& trackId,
                                         const std::vector<uint8_t>& fileId,
                                         AudioKeyCallback audioCallback) {
  auto buffer = fileId;

  // Store callback
  uint32_t requestId;
  {
    std::scoped_lock lock(requestsMutex);
    requestId = audioKeySequence++;
    audioKeyCallbacks.emplace(requestId, std::move(audioCallback));
  }

  // Structure: [FILEID] [TRACKID] [4 BYTES SEQUENCE ID] [0x00, 0x00]
  buffer.insert(buffer.end(), trackId.begin(), trackId.end());
  auto audioKeySequenceBuffer = pack<uint32_t>(htonl(requestId));
  buffer.insert(buffer.end(), audioKeySequenceBuffer.begin(),
                audioKeySequenceBuffer.end());
  auto suffix = std::vector<uint8_t>({0x00, 0x00});
  buffer.insert(buffer.end(), suffix.begin(), suffix.end());

  // Used for broken connection detection
  // this->lastRequestTimestamp = timeProvider->getSyncedTimestamp();
  try {
    sendPacket(
        static_cast<uint8_t>(RequestType::AUDIO_KEY_REQUEST_COMMAND), buffer);
  } catch (const std::exception& e) {
    CSPOT_LOG(error, "Audio key request send failed: %s", e.what());
    if (auto failedCallback = takeAudioKeyCallback(requestId)) {
      failedCallback(false, {});
    }
  }
  return requestId;
}
