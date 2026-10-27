#include <MDNSService.h>
#include <atomic>
#include <memory>
#include <string>

#include <CSpotContext.h>
#include <LoginBlob.h>
#include <SpircHandler.h>

#include "BellHTTPServer.h"
#include "BellTask.h"
#include "BellUtils.h"
#include "CircularBuffer.h"
#include "Logger.h"
#include "civetweb.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "I2SAudioSink.h"

static const char* TAG = "spotifyesp";

extern "C" {
void app_main(void);
}

static EventGroupHandle_t wifiEvents;
static constexpr int WIFI_CONNECTED_BIT = BIT0;

static void wifiEventHandler(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "WiFi disconnected, retrying");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(wifiEvents, WIFI_CONNECTED_BIT);
  }
}

static void wifiConnect() {
  wifiEvents = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifiEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifiEventHandler, nullptr));

  wifi_config_t wifiCfg = {};
  strlcpy((char*)wifiCfg.sta.ssid, CONFIG_SPOTIFY_WIFI_SSID,
          sizeof(wifiCfg.sta.ssid));
  strlcpy((char*)wifiCfg.sta.password, CONFIG_SPOTIFY_WIFI_PASSWORD,
          sizeof(wifiCfg.sta.password));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiCfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);

  xEventGroupWaitBits(wifiEvents, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                      portMAX_DELAY);
}

class CSpotPlayer : public bell::Task {
 private:
  std::shared_ptr<cspot::SpircHandler> handler;
  std::unique_ptr<I2SAudioSink> audioSink;
  std::unique_ptr<bell::CircularBuffer> circularBuffer;
  std::atomic<bool> isPaused;

 public:
  CSpotPlayer(std::shared_ptr<cspot::SpircHandler> handler)
      : bell::Task("player", 8 * 1024, 0, 0) {
    this->handler = handler;
    this->audioSink = std::make_unique<I2SAudioSink>(
        CONFIG_SPOTIFY_I2S_BCLK, CONFIG_SPOTIFY_I2S_LRCLK,
        CONFIG_SPOTIFY_I2S_DOUT);
    this->audioSink->setParams(44100, 2, 16);

    this->circularBuffer =
        std::make_unique<bell::CircularBuffer>(1024 * 128 * 4);

    this->handler->getTrackPlayer()->setDataCallback(
        [this](uint8_t* data, size_t bytes) { this->feedData(data, bytes); });
    this->isPaused = false;

    this->handler->setEventHandler(
        [this](std::unique_ptr<cspot::SpircHandler::Event> event) {
          switch (event->eventType) {
            case cspot::SpircHandler::EventType::PLAY_PAUSE:
              this->isPaused = std::get<bool>(event->data);
              break;
            case cspot::SpircHandler::EventType::FLUSH:
            case cspot::SpircHandler::EventType::SEEK:
            case cspot::SpircHandler::EventType::PLAYBACK_START:
              this->circularBuffer->emptyBuffer();
              break;
            default:
              break;
          }
        });
    startTask();
  }

  void feedData(uint8_t* data, size_t len) {
    size_t toWrite = len;
    while (toWrite > 0) {
      size_t written =
          this->circularBuffer->write(data + (len - toWrite), toWrite);
      if (written == 0) {
        BELL_SLEEP_MS(10);
      }
      toWrite -= written;
    }
  }

  void runTask() {
    std::vector<uint8_t> outBuf(1024);
    while (true) {
      if (!this->isPaused) {
        size_t read = this->circularBuffer->read(outBuf.data(), outBuf.size());
        this->audioSink->feedPCMFrames(outBuf.data(), read);
        if (read == 0) {
          BELL_SLEEP_MS(100);
        }
      } else {
        BELL_SLEEP_MS(100);
      }
    }
  }
};

class CSpotTask : public bell::Task {
 public:
  CSpotTask() : bell::Task("cspot", 32 * 1024, 0, 1) { startTask(); }

  void runTask() {
    mdns_init();
    mdns_hostname_set("spotifyesp");
    std::atomic<bool> gotBlob = false;

    auto blob = std::make_shared<LoginBlob>(CONFIG_SPOTIFY_DEVICE_NAME);

    auto server = std::make_unique<bell::BellHTTPServer>(8080);
    server->registerGet(
        "/spotify_info", [&server, blob](struct mg_connection* conn) {
          return server->makeJsonResponse(blob->buildZeroconfInfo());
        });
    server->registerPost(
        "/spotify_info", [&server, blob, &gotBlob](struct mg_connection* conn) {
          nlohmann::json obj;
          obj["status"] = 101;
          obj["spotifyError"] = 0;
          obj["statusString"] = "ERROR-OK";

          std::string body = "";
          auto requestInfo = mg_get_request_info(conn);
          if (requestInfo->content_length > 0) {
            body.resize(requestInfo->content_length);
            mg_read(conn, body.data(), requestInfo->content_length);

            mg_header hd[10];
            int num = mg_split_form_urlencoded(body.data(), hd, 10);
            std::map<std::string, std::string> queryMap;
            for (int i = 0; i < num; i++) {
              queryMap[hd[i].name] = hd[i].value;
            }

            blob->loadZeroconfQuery(queryMap);
            gotBlob = true;
          }

          return server->makeJsonResponse(obj.dump());
        });

    bell::MDNSService::registerService(
        blob->getDeviceName(), "_spotify-connect", "_tcp", "", 8080,
        {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});

    while (!gotBlob) {
      BELL_SLEEP_MS(1000);
      BELL_LOG(info, "cspot", "Waiting for spotify app to connect...");
    }

    BELL_LOG(info, "cspot", "Got blob!");
    auto ctx = cspot::Context::createFromBlob(blob);
    CSPOT_LOG(info, "Creating player");
    ctx->session->connectWithRandomAp();
    auto token = ctx->session->authenticate(blob);

    if (token.size() > 0) {
      ctx->session->startTask();
      auto handler = std::make_shared<cspot::SpircHandler>(ctx);
      handler->subscribeToMercury();
      auto player = std::make_shared<CSpotPlayer>(handler);

      while (true) {
        ctx->session->handlePacket();
      }
    }
  }
};

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifiConnect();
  ESP_LOGI(TAG, "Connected to WiFi, starting Spotify receiver");

  bell::setDefaultLogger();
  auto task = std::make_unique<CSpotTask>();
  vTaskSuspend(NULL);
}
