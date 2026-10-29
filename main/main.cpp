#include "esp_system.h"
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
#include "SpotifyPlayer.h"
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

class CSpotTask : public bell::Task {
 public:
  CSpotTask() : bell::Task("cspot", 32 * 1024, 0, 1) { startTask(); }

  void runTask() {
    mdns_init();
    mdns_hostname_set("spotifyesp");
    std::atomic<bool> gotBlob = false;

    auto blob = std::make_shared<cspot::LoginBlob>(CONFIG_SPOTIFY_DEVICE_NAME);

    std::shared_ptr<bell::BellHTTPServer> server =
        std::make_shared<bell::BellHTTPServer>(8080);
    server->registerGet(
        "/spotify_info", [server, blob](struct mg_connection* conn) {
          return server->makeJsonResponse(blob->buildZeroconfInfo());
        });
    server->registerPost(
        "/spotify_info", [server, blob, &gotBlob](struct mg_connection* conn) {
          nlohmann::json obj;
          obj["status"] = 101;
          obj["spotifyError"] = 0;
          obj["statusString"] = "ERROR-OK";

          std::string body = "";
          auto requestInfo = mg_get_request_info(conn);
          if (requestInfo->content_length > 0) {
            size_t total = requestInfo->content_length;
            body.resize(total);
            size_t got = 0;
            while (got < total) {
              int n = mg_read(conn, body.data() + got, total - got);
              if (n <= 0) break;
              got += n;
            }
            body.resize(got);

            mg_header hd[16];
            int num = mg_split_form_urlencoded(body.data(), hd, 16);
            std::map<std::string, std::string> queryMap;
            for (int i = 0; i < num; i++) {
              queryMap[hd[i].name] = hd[i].value;
            }
            BELL_LOG(info, "cspot", "Login POST: %d bytes, %d fields", (int)got,
                     num);

            try {
              blob->loadZeroconfQuery(queryMap);
              gotBlob = true;
            } catch (const std::exception& e) {
              BELL_LOG(error, "cspot", "Login failed: %s", e.what());
              obj["status"] = 303;
              obj["statusString"] = "ERROR-INVALID-ARGUMENTS";
            }
          }

          return server->makeJsonResponse(obj.dump());
        });

    MDNSService::registerService(
        blob->getDeviceName(), "_spotify-connect", "_tcp", "", 8080,
        {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});

    while (!gotBlob) {
      BELL_SLEEP_MS(1000);
      BELL_LOG(info, "cspot", "Waiting for spotify app to connect...");
    }

    BELL_LOG(info, "cspot", "Got blob!");
    auto ctx = cspot::Context::createFromBlob(blob);
    CSPOT_LOG(info, "Creating player");
    bool connected = false;
    for (int attempt = 0; attempt < 5 && !connected; attempt++) {
      try {
        ctx->session->connectWithRandomAp();
        connected = true;
      } catch (const std::exception& e) {
        BELL_LOG(error, "cspot", "AP connect failed (%d): %s", attempt, e.what());
        BELL_SLEEP_MS(1000);
      }
    }
    if (!connected) {
      esp_restart();
    }
    try {
      ctx->config.authData = ctx->session->authenticate(blob);
    } catch (const std::exception& e) {
      BELL_LOG(error, "cspot", "Authenticate failed: %s", e.what());
      esp_restart();
    }

    if (ctx->config.authData.size() > 0) {
      auto handler = std::make_shared<cspot::SpircHandler>(ctx);
      ctx->session->startTask();
      auto sink = std::make_unique<I2SAudioSink>(CONFIG_SPOTIFY_I2S_BCLK,
                                                 CONFIG_SPOTIFY_I2S_LRCLK,
                                                 CONFIG_SPOTIFY_I2S_DOUT);
      auto player = std::make_shared<SpotifyPlayer>(std::move(sink), handler);

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
