#include "WifiSetup.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "dhcpserver/dhcpserver.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace wifi_setup {
namespace {

const char* TAG = "wifi_setup";
const char* NVS_NAMESPACE = "spotifyesp";
const size_t MAX_LOGIN_BYTES = 1024;
constexpr uint32_t PORTAL_MAGIC = 0x57495349;
constexpr size_t MAX_BODY = 1024;
constexpr size_t MAX_NETWORKS = 20;

// Survives a software restart, but not a power cycle.
RTC_NOINIT_ATTR uint32_t portalRequest;

Settings currentSettings;
std::vector<std::string> networks;
esp_timer_handle_t idleTimer = nullptr;
uint32_t idleMs = 0;
char apIp[16] = "192.168.4.1";
uint32_t apIpAddr = 0;  // network byte order
// The DHCP server keeps a pointer to the captive portal URI.
char captivePortalUri[32];

// ---------- NVS ----------

bool validBitrate(uint32_t kbps) {
  return kbps == 96 || kbps == 160 || kbps == 320;
}

bool readStr(nvs_handle_t h, const char* key, std::string& out) {
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len <= 1) return false;
  std::string tmp(len, '\0');
  if (nvs_get_str(h, key, tmp.data(), &len) != ESP_OK) return false;
  tmp.resize(len - 1);
  out = tmp;
  return true;
}

bool save(const Settings& s) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_str(h, "ssid", s.ssid.c_str());
  if (err == ESP_OK) err = nvs_set_str(h, "pass", s.password.c_str());
  if (err == ESP_OK) err = nvs_set_str(h, "name", s.deviceName.c_str());
  if (err == ESP_OK) err = nvs_set_u32(h, "kbps", s.bitrate);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

// ---------- Form helpers ----------

std::string htmlEscape(const std::string& in) {
  std::string out;
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string urlDecode(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size() && hexValue(in[i + 1]) >= 0 &&
               hexValue(in[i + 2]) >= 0) {
      out += static_cast<char>(hexValue(in[i + 1]) * 16 + hexValue(in[i + 2]));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

std::map<std::string, std::string> parseForm(const std::string& body) {
  std::map<std::string, std::string> fields;
  size_t pos = 0;
  while (pos <= body.size()) {
    size_t end = body.find('&', pos);
    if (end == std::string::npos) end = body.size();
    std::string pair = body.substr(pos, end - pos);
    size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      fields[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    pos = end + 1;
  }
  return fields;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// ---------- Idle timeout ----------

void touchIdleTimer() {
  if (!idleTimer || idleMs == 0) return;
  esp_timer_stop(idleTimer);
  esp_timer_start_once(idleTimer, static_cast<uint64_t>(idleMs) * 1000);
}

void startIdleTimer() {
  if (idleMs == 0) return;
  esp_timer_create_args_t args = {};
  args.callback = [](void*) {
    ESP_LOGI(TAG, "Setup portal idle, restarting");
    esp_restart();
  };
  args.name = "portal_idle";
  if (esp_timer_create(&args, &idleTimer) == ESP_OK) touchIdleTimer();
}

void restartSoon() {
  esp_timer_create_args_t args = {};
  args.callback = [](void*) { esp_restart(); };
  args.name = "portal_restart";
  esp_timer_handle_t timer;
  if (esp_timer_create(&args, &timer) == ESP_OK) {
    esp_timer_start_once(timer, 2000 * 1000);
  } else {
    esp_restart();
  }
}

// ---------- Web page ----------

const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Spotify ESP setup</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:26em;margin:2em auto;padding:0 1em}"
    "label{display:block;margin-top:1em;font-weight:600}"
    "input{width:100%;box-sizing:border-box;padding:.6em;font-size:1em;margin-top:.3em}"
    "button{margin-top:1.5em;padding:.7em 1.2em;font-size:1em}"
    "small{color:#666;display:block;margin-top:.3em}"
    "</style></head><body>";
const char PAGE_TAIL[] = "</body></html>";

esp_err_t sendHtml(httpd_req_t* req, const char* status, const std::string& body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  std::string page = std::string(PAGE_HEAD) + body + PAGE_TAIL;
  return httpd_resp_send(req, page.c_str(), page.size());
}

void scanNetworks() {
  wifi_scan_config_t scanCfg = {};
  scanCfg.show_hidden = false;
  if (esp_wifi_scan_start(&scanCfg, true) != ESP_OK) {
    ESP_LOGW(TAG, "WiFi scan failed");
    return;
  }
  uint16_t num = MAX_NETWORKS;
  std::vector<wifi_ap_record_t> records(MAX_NETWORKS);
  if (esp_wifi_scan_get_ap_records(&num, records.data()) != ESP_OK) return;
  networks.clear();
  for (uint16_t i = 0; i < num; i++) {
    std::string ssid(reinterpret_cast<char*>(records[i].ssid));
    if (!ssid.empty() &&
        std::find(networks.begin(), networks.end(), ssid) == networks.end()) {
      networks.push_back(ssid);
    }
  }
  ESP_LOGI(TAG, "Found %u networks", (unsigned)networks.size());
}

esp_err_t rootGet(httpd_req_t* req) {
  touchIdleTimer();
  std::string body = "<h2>Spotify ESP setup</h2><form method=\"post\" action=\"/save\">";
  body += "<label>WiFi network<input name=\"ssid\" list=\"nets\" maxlength=\"32\" required "
          "autocapitalize=\"none\" autocorrect=\"off\" value=\"" +
          htmlEscape(currentSettings.ssid) + "\"></label><datalist id=\"nets\">";
  for (const auto& ssid : networks) {
    body += "<option value=\"" + htmlEscape(ssid) + "\">";
  }
  body += "</datalist><small><a href=\"/rescan\">Scan again</a></small>";
  body += "<label>WiFi password<input name=\"pass\" type=\"password\" maxlength=\"63\" "
          "autocomplete=\"off\"></label><small>";
  body += currentSettings.ssid.empty() ? "Leave empty for an open network."
                                       : "Leave empty to keep the saved password.";
  body += "</small><label>Device name in Spotify<input name=\"name\" maxlength=\"32\" "
          "required value=\"" +
          htmlEscape(currentSettings.deviceName) + "\"></label>";
  body += "<label>Streaming quality<select name=\"kbps\">";
  for (uint32_t kbps : {320u, 160u, 96u}) {
    body += "<option value=\"" + std::to_string(kbps) + "\"" +
            (kbps == currentSettings.bitrate ? " selected" : "") + ">" +
            std::to_string(kbps) + " kbps" +
            (kbps == 320 ? " (best, needs Premium)" : "") + "</option>";
  }
  body += "</select></label>";
  body += "<label><input type=\"checkbox\" name=\"forget\" value=\"1\"> Forget the saved "
          "Spotify login</label>";
  body += "<button type=\"submit\">Save and restart</button></form>";
  return sendHtml(req, "200 OK", body);
}

esp_err_t rescanGet(httpd_req_t* req) {
  touchIdleTimer();
  scanNetworks();
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, nullptr, 0);
}

esp_err_t formError(httpd_req_t* req, const std::string& message) {
  return sendHtml(req, "400 Bad Request",
                  "<h2>Check your input</h2><p>" + htmlEscape(message) +
                      "</p><p><a href=\"/\">Back</a></p>");
}

esp_err_t savePost(httpd_req_t* req) {
  touchIdleTimer();
  if (req->content_len == 0 || req->content_len > MAX_BODY) {
    return formError(req, "Invalid form data.");
  }
  std::string body(req->content_len, '\0');
  size_t got = 0;
  int failures = 0;
  while (got < body.size()) {
    int n = httpd_req_recv(req, body.data() + got, body.size() - got);
    if (n > 0) {
      got += n;
    } else if (n == HTTPD_SOCK_ERR_TIMEOUT && ++failures < 3) {
      continue;
    } else {
      return ESP_FAIL;
    }
  }

  auto fields = parseForm(body);
  Settings s;
  s.ssid = fields["ssid"];
  s.password = fields["pass"];
  s.deviceName = trim(fields["name"]);
  s.bitrate = static_cast<uint32_t>(strtoul(fields["kbps"].c_str(), nullptr, 10));
  if (!validBitrate(s.bitrate)) {
    return formError(req, "Pick a streaming quality from the list.");
  }

  if (s.ssid.empty() || s.ssid.size() > 32) {
    return formError(req, "The WiFi network name must be 1 to 32 characters.");
  }
  if (s.password.empty() && s.ssid == currentSettings.ssid) {
    s.password = currentSettings.password;
  }
  if (!s.password.empty() && (s.password.size() < 8 || s.password.size() > 63)) {
    return formError(req, "The WiFi password must be 8 to 63 characters.");
  }
  if (s.deviceName.empty() || s.deviceName.size() > 32) {
    return formError(req, "The device name must be 1 to 32 characters.");
  }

  if (fields["forget"] == "1") clearLogin();

  if (!save(s)) {
    return sendHtml(req, "500 Internal Server Error",
                    "<h2>Could not save</h2><p>Writing the settings failed. Try again.</p>"
                    "<p><a href=\"/\">Back</a></p>");
  }
  ESP_LOGI(TAG, "Saved settings for '%s' as '%s'", s.ssid.c_str(), s.deviceName.c_str());

  esp_err_t err = sendHtml(
      req, "200 OK",
      "<h2>Saved</h2><p>The device restarts now and joins <b>" + htmlEscape(s.ssid) +
          "</b>. Reconnect your phone to your normal WiFi, then pick <b>" +
          htmlEscape(s.deviceName) +
          "</b> in the Spotify app.</p><p>If the connection fails, the setup network "
          "comes back.</p>");
  restartSoon();
  return err;
}

// Every other URL (connectivity checks of phones and laptops) goes to the form.
esp_err_t redirectToRoot(httpd_req_t* req, httpd_err_code_t) {
  touchIdleTimer();
  std::string location = std::string("http://") + apIp + "/";
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location.c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, nullptr, 0);
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 6144;
  config.max_uri_handlers = 8;
  config.lru_purge_enable = true;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Web server start failed, restarting");
    esp_restart();
  }

  httpd_uri_t root = {};
  root.uri = "/";
  root.method = HTTP_GET;
  root.handler = rootGet;
  httpd_register_uri_handler(server, &root);

  httpd_uri_t rescan = {};
  rescan.uri = "/rescan";
  rescan.method = HTTP_GET;
  rescan.handler = rescanGet;
  httpd_register_uri_handler(server, &rescan);

  httpd_uri_t saveUri = {};
  saveUri.uri = "/save";
  saveUri.method = HTTP_POST;
  saveUri.handler = savePost;
  httpd_register_uri_handler(server, &saveUri);

  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirectToRoot);
  httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, redirectToRoot);
}

// ---------- DNS catch-all ----------

// Answers every A query with the access point address so that phones open the portal.
void dnsTask(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket failed");
    vTaskDelete(nullptr);
    return;
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed");
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  uint8_t buf[512];
  while (true) {
    sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n < 12 || (buf[2] & 0x80) || buf[5] != 1) continue;  // not a single query

    int pos = 12;
    while (pos < n && buf[pos] != 0 && (buf[pos] & 0xC0) == 0) pos += buf[pos] + 1;
    if (pos + 5 > n || buf[pos] != 0) continue;
    int questionEnd = pos + 5;
    uint16_t qtype = (buf[pos + 1] << 8) | buf[pos + 2];
    bool answer = qtype == 1;  // A record; other types get an empty answer

    buf[2] = 0x80 | (buf[2] & 0x01);  // response, copy recursion desired
    buf[3] = 0x80;                    // recursion available, no error
    buf[4] = 0; buf[5] = 1;           // one question
    buf[6] = 0; buf[7] = answer ? 1 : 0;
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    int len = questionEnd;
    if (answer) {
      const uint8_t head[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                              0x00, 0x00, 0x00, 0x1E, 0x00, 0x04};
      memcpy(buf + len, head, sizeof(head));
      len += sizeof(head);
      memcpy(buf + len, &apIpAddr, 4);
      len += 4;
    }
    sendto(sock, buf, len, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
  }
}

// ---------- Button ----------

void apClientEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  touchIdleTimer();
  if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    auto* e = static_cast<wifi_event_ap_staconnected_t*>(data);
    ESP_LOGI(TAG, "Client " MACSTR " joined", MAC2STR(e->mac));
  } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
    auto* e = static_cast<ip_event_ap_staipassigned_t*>(data);
    ESP_LOGI(TAG, "Client got IP " IPSTR, IP2STR(&e->ip));
  }
}

void buttonTask(void* arg) {
  auto pin = static_cast<gpio_num_t>(reinterpret_cast<intptr_t>(arg));
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << pin;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);

  int heldMs = 0;
  bool announced = false;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (gpio_get_level(pin) == 0) {
      heldMs += 100;
      if (heldMs >= 3000 && !announced) {
        announced = true;
        ESP_LOGI(TAG, "Release the button to start WiFi setup");
      }
    } else {
      // Restart on release: holding BOOT while the chip resets enters flash mode.
      if (heldMs >= 3000) restartIntoPortal();
      heldMs = 0;
      announced = false;
    }
  }
}

}  // namespace

bool load(Settings& out) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  bool haveSsid = readStr(h, "ssid", out.ssid);
  readStr(h, "pass", out.password);
  readStr(h, "name", out.deviceName);
  uint32_t kbps = 0;
  if (nvs_get_u32(h, "kbps", &kbps) == ESP_OK && validBitrate(kbps)) {
    out.bitrate = kbps;
  }
  nvs_close(h);
  return haveSsid;
}

bool loadLogin(SavedLogin& out) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  std::string user;
  size_t len = 0;
  bool ok = readStr(h, "spuser", user) &&
            nvs_get_blob(h, "spauth", nullptr, &len) == ESP_OK && len > 0 &&
            len <= MAX_LOGIN_BYTES;
  if (ok) {
    std::vector<uint8_t> creds(len);
    ok = nvs_get_blob(h, "spauth", creds.data(), &len) == ESP_OK && len == creds.size();
    if (ok) {
      out.username = user;
      out.credentials = std::move(creds);
    }
  }
  nvs_close(h);
  return ok;
}

bool saveLogin(const SavedLogin& login) {
  if (login.username.empty() || login.credentials.empty() ||
      login.credentials.size() > MAX_LOGIN_BYTES) {
    return false;
  }
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_str(h, "spuser", login.username.c_str());
  if (err == ESP_OK) {
    err = nvs_set_blob(h, "spauth", login.credentials.data(), login.credentials.size());
  }
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

void clearLogin() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, "spuser");
  nvs_erase_key(h, "spauth");
  nvs_commit(h);
  nvs_close(h);
}

bool consumePortalRequest() {
  bool requested = portalRequest == PORTAL_MAGIC;
  portalRequest = 0;
  return requested;
}

void restartIntoPortal() {
  portalRequest = PORTAL_MAGIC;
  esp_restart();
  while (true) {
  }
}

void runPortal(const Settings& current, uint32_t idleRebootMs) {
  currentSettings = current;
  idleMs = idleRebootMs;

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &apClientEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &apClientEvent, nullptr));
  esp_netif_t* ap = esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();

  esp_netif_ip_info_t ipInfo;
  if (esp_netif_get_ip_info(ap, &ipInfo) == ESP_OK) {
    apIpAddr = ipInfo.ip.addr;
    inet_ntoa_r(ipInfo.ip, apIp, sizeof(apIp));
  }

  // Configure the DHCP server before the access point starts it: clients get the
  // portal address as DNS server and as captive portal URI. Don't stop the server
  // here, a stopped server is not started by the access point.
  esp_netif_dns_info_t dns = {};
  dns.ip.type = IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = apIpAddr;
  dhcps_offer_t offerDns = OFFER_DNS;
  snprintf(captivePortalUri, sizeof(captivePortalUri), "http://%s", apIp);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
      ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offerDns, sizeof(offerDns)));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
      ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, captivePortalUri,
      strlen(captivePortalUri)));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  wifi_config_t apCfg = {};
  int ssidLen = snprintf(reinterpret_cast<char*>(apCfg.ap.ssid), sizeof(apCfg.ap.ssid),
                         "%s-%02X%02X", CONFIG_SPOTIFY_SETUP_AP_SSID, mac[4], mac[5]);
  apCfg.ap.ssid_len = std::min<int>(ssidLen, sizeof(apCfg.ap.ssid) - 1);
  apCfg.ap.channel = 1;
  apCfg.ap.authmode = WIFI_AUTH_OPEN;
  apCfg.ap.max_connection = 4;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apCfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "Setup network '%s' open, portal at http://%s/",
           reinterpret_cast<char*>(apCfg.ap.ssid), apIp);

  scanNetworks();
  startWebServer();
  xTaskCreate(dnsTask, "dns", 4096, nullptr, 5, nullptr);
  startIdleTimer();

  vTaskSuspend(nullptr);
  while (true) {
  }
}

void startButtonWatcher(int gpio) {
  if (gpio < 0) return;
  // Stack in PSRAM: internal RAM is scarce during playback.
  xTaskCreatePinnedToCoreWithCaps(buttonTask, "setup_btn", 4096,
                                  reinterpret_cast<void*>(static_cast<intptr_t>(gpio)), 1,
                                  nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace wifi_setup
