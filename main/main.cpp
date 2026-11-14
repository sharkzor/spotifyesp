#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <cstring>
#include <MDNSService.h>
#include <atomic>
#include <cctype>
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
#include "ConfigServer.h"
#include "I2SAudioSink.h"
#include "WifiSetup.h"

static const char* TAG = "spotifyesp";

extern "C" {
void app_main(void);
}

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

// A different Spotify account picked this device while another login is active. The new
// login survives the restart in RTC memory and is used instead of waiting for the phone.
struct PendingLogin {
  uint32_t magic;
  int authType;
  uint16_t dataLen;
  char username[96];
  uint8_t data[1024];
};
static constexpr uint32_t PENDING_LOGIN_MAGIC = 0x4C4F4731;
static RTC_NOINIT_ATTR PendingLogin pendingLogin;
static std::atomic<bool> receiverRunning{false};

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
    // Lower-case letters and digits from the device name, so two boards get different
    // addresses (for example "Spotify Zolder" becomes spotify-zolder.local).
    std::string host;
    for (char c : deviceName) {
      if (isalnum(static_cast<unsigned char>(c))) {
        host += static_cast<char>(tolower(static_cast<unsigned char>(c)));
      } else if (!host.empty() && host.back() != '-') {
        host += '-';
      }
    }
    while (!host.empty() && host.back() == '-') host.pop_back();
    if (host.size() > 32) host.resize(32);
    if (host.empty()) host = "spotifyesp";
    mdns_hostname_set(host.c_str());
    mdns_instance_name_set(deviceName.c_str());
    ESP_LOGI("spotifyesp", "mDNS address: %s.local", host.c_str());
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
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
              std::string prevUser = blob->username;
              auto prevData = blob->authData;
              int prevType = blob->authType;
              blob->loadZeroconfQuery(queryMap);
              if (receiverRunning) {
                if (blob->username == prevUser) {
                  // Same account asking again: keep the running session untouched.
                  blob->authData = prevData;
                  blob->authType = prevType;
                } else if (blob->username.size() < sizeof(pendingLogin.username) &&
                           blob->authData.size() <= sizeof(pendingLogin.data)) {
                  BELL_LOG(info, "cspot", "New account '%s', switching login",
                           blob->username.c_str());
                  pendingLogin.magic = PENDING_LOGIN_MAGIC;
                  pendingLogin.authType = blob->authType;
                  pendingLogin.dataLen = blob->authData.size();
                  memcpy(pendingLogin.username, blob->username.c_str(),
                         blob->username.size() + 1);
                  memcpy(pendingLogin.data, blob->authData.data(),
                         blob->authData.size());
                  wifi_setup::clearLogin();
                  esp_timer_handle_t timer;
                  esp_timer_create_args_t args = {};
                  args.callback = [](void*) { esp_restart(); };
                  args.name = "login_switch";
                  if (esp_timer_create(&args, &timer) == ESP_OK) {
                    esp_timer_start_once(timer, 1000 * 1000);
                  }
                }
              }
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

    if (pendingLogin.magic == PENDING_LOGIN_MAGIC && pendingLogin.dataLen <= sizeof(pendingLogin.data)) {
      pendingLogin.username[sizeof(pendingLogin.username) - 1] = 0;
      blob->username = pendingLogin.username;
      blob->authType = pendingLogin.authType;
      blob->authData.assign(pendingLogin.data, pendingLogin.data + pendingLogin.dataLen);
      usingSavedLogin = false;
      gotBlob = true;
      BELL_LOG(info, "cspot", "Using the login of the account that just connected");
    }
    pendingLogin.magic = 0;

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
      receiverRunning = true;
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
  config_server::start(deviceName, bitrateKbps, onEthernet ? "Ethernet" : "WiFi");

  bell::setDefaultLogger();
  BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      CSpotTask::entry, "cspot", 32 * 1024, nullptr, 5, nullptr, 1,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
  // Returning lets ESP-IDF reclaim the main task's stack after local cleanup.
}
