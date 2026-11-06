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

#include "BoardHw.h"
#include "I2SAudioSink.h"
#include "WifiSetup.h"

static const char* TAG = "spotifyesp";

extern "C" {
void app_main(void);
}

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

static EventGroupHandle_t wifiEvents;
static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_FAIL_BIT = BIT1;
// Attempts before the first connection; after that the board retries forever.
static constexpr int WIFI_MAX_INITIAL_RETRIES = 8;
// Without web requests the setup portal restarts to retry the saved network.
static constexpr uint32_t PORTAL_IDLE_REBOOT_MS = 5 * 60 * 1000;

static std::atomic<int> wifiRetries = 0;
static std::atomic<bool> wifiEverConnected = false;
static std::string deviceName;
static uint32_t bitrateKbps = 320;

static void wifiEventHandler(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (!wifiEverConnected && ++wifiRetries >= WIFI_MAX_INITIAL_RETRIES) {
      xEventGroupSetBits(wifiEvents, WIFI_FAIL_BIT);
      return;
    }
    ESP_LOGW(TAG, "WiFi disconnected, retrying");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    wifiEverConnected = true;
    xEventGroupSetBits(wifiEvents, WIFI_CONNECTED_BIT);
  }
}

static bool wifiConnect(const std::string& ssid, const std::string& password) {
  wifiEvents = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  esp_err_t loopErr = esp_event_loop_create_default();
  if (loopErr != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(loopErr);
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifiEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifiEventHandler, nullptr));

  wifi_config_t wifiCfg = {};
  strlcpy((char*)wifiCfg.sta.ssid, ssid.c_str(), sizeof(wifiCfg.sta.ssid));
  strlcpy((char*)wifiCfg.sta.password, password.c_str(),
          sizeof(wifiCfg.sta.password));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiCfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);

  EventBits_t bits =
      xEventGroupWaitBits(wifiEvents, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                          pdFALSE, pdFALSE, portMAX_DELAY);
  return bits & WIFI_CONNECTED_BIT;
}

class CSpotTask {
 public:
  static void entry(void*) {
    CSpotTask().runTask();
    vTaskDelete(nullptr);
  }

  void runTask() {
    mdns_init();
    mdns_hostname_set("spotifyesp");
    std::atomic<bool> gotBlob = false;

    auto blob = std::make_shared<cspot::LoginBlob>(deviceName);

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

    // A login saved by an earlier session lets the board sign in without the phone.
    wifi_setup::SavedLogin saved;
    bool usingSavedLogin = wifi_setup::loadLogin(saved);
    if (usingSavedLogin) {
      blob->username = saved.username;
      blob->authData = saved.credentials;
      blob->authType =
          AuthenticationType_AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS;
      gotBlob = true;
      BELL_LOG(info, "cspot", "Using saved Spotify login");
    }

    while (!gotBlob) {
      BELL_SLEEP_MS(1000);
      BELL_LOG(info, "cspot", "Waiting for spotify app to connect...");
    }

    BELL_LOG(info, "cspot", "Got blob!");
    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.audioFormat =
        bitrateKbps == 96    ? AudioFormat_OGG_VORBIS_96
        : bitrateKbps == 160 ? AudioFormat_OGG_VORBIS_160
                             : AudioFormat_OGG_VORBIS_320;
    CSPOT_LOG(info, "Streaming quality: %u kbps", (unsigned)bitrateKbps);
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

    if (ctx->config.authData.empty() && usingSavedLogin) {
      // Spotify refused the saved credentials; wait for the phone after the restart.
      BELL_LOG(error, "cspot", "Saved login rejected, forgetting it");
      wifi_setup::clearLogin();
      esp_restart();
    }

    if (ctx->config.authData.size() > 0) {
      if (!usingSavedLogin || ctx->config.authData != saved.credentials) {
        wifi_setup::SavedLogin fresh{blob->username, ctx->config.authData};
        if (!wifi_setup::saveLogin(fresh)) {
          BELL_LOG(error, "cspot", "Could not save the Spotify login");
        }
      }
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
  ESP_LOGI(TAG, "Reset reason: %d", static_cast<int>(esp_reset_reason()));
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Settings saved by the setup portal win over the compile-time defaults.
  wifi_setup::Settings settings;
  wifi_setup::load(settings);
  if (settings.ssid.empty()) {
    settings.ssid = CONFIG_SPOTIFY_WIFI_SSID;
    settings.password = CONFIG_SPOTIFY_WIFI_PASSWORD;
  }
  if (settings.deviceName.empty()) {
    settings.deviceName = CONFIG_SPOTIFY_DEVICE_NAME;
  }

  bool portalRequested = wifi_setup::consumePortalRequest();
  bool onEthernet = false;
  if (!portalRequested) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    onEthernet = board::ethernetConnect();
  }

  if (!onEthernet) {
    if (portalRequested || settings.ssid.empty()) {
      wifi_setup::runPortal(settings,
                            settings.ssid.empty() ? 0 : PORTAL_IDLE_REBOOT_MS);
    }

    if (!wifiConnect(settings.ssid, settings.password)) {
      ESP_LOGE(TAG, "Could not join '%s', starting WiFi setup",
               settings.ssid.c_str());
      wifi_setup::restartIntoPortal();
    }
  }
  deviceName = settings.deviceName;
  bitrateKbps = settings.bitrate;
  wifi_setup::startButtonWatcher(CONFIG_SPOTIFY_SETUP_BUTTON_GPIO);
  ESP_LOGI(TAG, "Connected over %s, starting Spotify receiver as '%s'",
           onEthernet ? "Ethernet" : "WiFi", deviceName.c_str());
  board::dacInit();

  bell::setDefaultLogger();
  BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      CSpotTask::entry, "cspot", 32 * 1024, nullptr, 5, nullptr, 1,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
  // Returning lets ESP-IDF reclaim the main task's stack after local cleanup.
}
