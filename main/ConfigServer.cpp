#include "ConfigServer.h"

#include <cstdio>
#include <cstring>

#include "Equalizer.h"
#include "WifiSetup.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

namespace config_server {
namespace {

const char* TAG = "config";
const char* NVS_NAMESPACE = "spotifyesp";
constexpr size_t MAX_BODY = 512;
constexpr size_t MAX_NAME_BYTES = 40;

std::string deviceName;
uint32_t bitrate = 320;
const char* networkName = "";

const char PAGE[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Spotify ESP</title><style>
body{font-family:sans-serif;max-width:560px;margin:1em auto;padding:0 1em;color:#222}
h1{font-size:1.3em}h2{font-size:1.05em;margin-top:1.6em}
label{display:block;margin:.6em 0 .2em}input,select{font-size:1em;padding:.3em;box-sizing:border-box}
input[type=text]{width:100%}button{font-size:1em;padding:.5em 1em;margin-top:.8em}
table{border-collapse:collapse;width:100%}td,th{padding:.15em;text-align:left;font-size:.85em}
td input,td select{width:100%}.s{color:#666;font-size:.9em}#msg{margin-top:1em;font-weight:bold}
</style></head><body>
<h1>Spotify ESP</h1><div class="s" id="status">Loading...</div>
<h2>Device</h2>
<label>Spotify device name<input type="text" id="name" maxlength="40"></label>
<label>Streaming quality<select id="kbps"><option value="320">320 kbps (best, needs Premium)</option>
<option value="160">160 kbps</option><option value="96">96 kbps</option></select></label>
<button id="saveDev">Save and restart</button>
<h2>Equalizer</h2>
<label><input type="checkbox" id="en"> Enabled</label>
<label>Pre-gain (dB), lower it when boosting to avoid clipping<input type="number" id="pre" step="0.5" min="-24" max="6" value="0"></label>
<label>Preset<select id="preset"><option value="">Choose a preset...</option></select></label>
<table><thead><tr><th>Type</th><th>Hz</th><th>Q</th><th>dB</th></tr></thead><tbody id="bands"></tbody></table>
<div class="s">A band with 0 dB is skipped. Changes apply right away and are saved.</div>
<div id="msg"></div>
<script>
var H={'X-Requested-With':'spotifyesp'},$=function(i){return document.getElementById(i)};
function msg(t){$('msg').textContent=t}
function row(i){return '<tr><td><select id="t'+i+'"><option value="P">Peak</option><option value="L">Low shelf</option><option value="H">High shelf</option></select></td>'+
['f','q','g'].map(function(k){return '<td><input type="number" id="'+k+i+'" step="'+(k=='f'?1:k=='q'?.1:.5)+'"></td>'}).join('')+'</tr>'}
var h='';for(var i=0;i<6;i++)h+=row(i);$('bands').innerHTML=h;
function load(){fetch('/api/state').then(function(r){return r.json()}).then(function(s){
$('status').textContent=s.network+' - free memory '+Math.round(s.heap/1024)+' KB internal, '+Math.round(s.psram/1024)+' KB PSRAM';
$('name').value=s.name;$('kbps').value=s.kbps;
var p=s.eq.split(';');$('en').checked=p[0]=='1';$('pre').value=p[1];
for(var i=0;i<6;i++){var b=(p[2+i]||'P,1000,1,0').split(',');$('t'+i).value=b[0];$('f'+i).value=b[1];$('q'+i).value=b[2];$('g'+i).value=b[3]}
}).catch(function(){msg('Could not read the state')})}
function eq(){var s=($('en').checked?'1;':'0;')+$('pre').value;
for(var i=0;i<6;i++)s+=';'+$('t'+i).value+','+$('f'+i).value+','+$('q'+i).value+','+$('g'+i).value;return s}
var Z=',P,1000,1,0',F='1;0'+Z+Z+Z+Z+Z+Z;
var PRE={'Flat':'0;0','Bass boost':'1;-5;L,100,0.7,5;P,200,1,2','Treble boost':'1;-4;H,6000,0.7,4;P,3500,1,1.5',
'Loudness':'1;-6;L,80,0.7,6;P,400,1,-2;H,8000,0.7,5','Vocal':'1;-3;L,120,0.7,-3;P,2500,1,3;P,5000,1.2,2',
'Rock':'1;-5;L,100,0.7,4;P,500,1,-3;P,3000,1,2;H,8000,0.7,4','Classical':'1;-3;L,100,0.7,2;H,7000,0.7,3',
'Late night (soft bass)':'1;-2;L,120,0.7,-5;P,3000,1,1','Warm':'1;-3;L,150,0.7,3;H,7000,0.7,-3'};
for(var k in PRE){var o=document.createElement('option');o.value=k;o.textContent=k;$('preset').appendChild(o)}
function fill(t){var p=t.split(';');$('en').checked=p[0]=='1';$('pre').value=p[1];
for(var i=0;i<6;i++){var b=(p[2+i]||'P,1000,1,0').split(',');$('t'+i).value=b[0];$('f'+i).value=b[1];$('q'+i).value=b[2];$('g'+i).value=b[3]}}
$('preset').onchange=function(){if(this.value){fill(PRE[this.value]);if(this.value=='Flat')$('en').checked=false;else $('en').checked=true;applyEq();this.value=''}};
var timer;function applyEq(){clearTimeout(timer);timer=setTimeout(function(){
fetch('/api/eq',{method:'POST',headers:H,body:eq()}).then(function(r){return r.text()}).then(function(t){msg(t)})},300)}
document.querySelectorAll('#en,#pre,#bands input,#bands select').forEach(function(e){e.addEventListener('input',applyEq)});
$('saveDev').onclick=function(){fetch('/api/device',{method:'POST',headers:H,body:$('kbps').value+'\n'+$('name').value})
.then(function(r){return r.text()}).then(function(t){msg(t);setTimeout(load,8000)})};
load();
</script></body></html>)HTML";

bool saveEq(const std::string& text) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_str(h, "eq", text.c_str());
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

bool loadEq(std::string& out) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = 0;
  bool ok = nvs_get_str(h, "eq", nullptr, &len) == ESP_OK && len > 1;
  if (ok) {
    out.assign(len, '\0');
    ok = nvs_get_str(h, "eq", out.data(), &len) == ESP_OK;
    out.resize(ok ? len - 1 : 0);
  }
  nvs_close(h);
  return ok;
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      out += ' ';
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

// Pages that change settings must come from this page, not from another site: a
// custom header makes the browser run a CORS check that cross-site requests fail.
bool fromOurPage(httpd_req_t* req) {
  char value[24];
  return httpd_req_get_hdr_value_str(req, "X-Requested-With", value, sizeof(value)) ==
             ESP_OK &&
         strcmp(value, "spotifyesp") == 0;
}

bool readBody(httpd_req_t* req, std::string& body) {
  if (req->content_len == 0 || req->content_len > MAX_BODY) return false;
  body.resize(req->content_len);
  size_t got = 0;
  while (got < body.size()) {
    int n = httpd_req_recv(req, body.data() + got, body.size() - got);
    if (n <= 0) return false;
    got += n;
  }
  return true;
}

esp_err_t reply(httpd_req_t* req, const char* status, const char* type,
                const std::string& text) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, text.c_str(), text.size());
}

esp_err_t rootGet(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

esp_err_t stateGet(httpd_req_t* req) {
  char numbers[96];
  snprintf(numbers, sizeof(numbers), "\"kbps\":%u,\"heap\":%u,\"psram\":%u",
           (unsigned)bitrate,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  std::string json = "{\"name\":\"" + jsonEscape(deviceName) + "\",\"network\":\"" +
                     networkName + "\"," + numbers + ",\"eq\":\"" +
                     eq::serialize(eq::instance().config()) + "\"}";
  return reply(req, "200 OK", "application/json", json);
}

esp_err_t eqPost(httpd_req_t* req) {
  if (!fromOurPage(req)) return reply(req, "403 Forbidden", "text/plain", "Forbidden");
  std::string body;
  eq::Config config;
  if (!readBody(req, body) || !eq::parse(body, config)) {
    return reply(req, "400 Bad Request", "text/plain", "Invalid equalizer settings");
  }
  eq::instance().setConfig(config);
  if (!saveEq(eq::serialize(config))) {
    return reply(req, "200 OK", "text/plain", "Applied, but could not be saved");
  }
  return reply(req, "200 OK", "text/plain", "Applied and saved");
}

esp_err_t devicePost(httpd_req_t* req) {
  if (!fromOurPage(req)) return reply(req, "403 Forbidden", "text/plain", "Forbidden");
  std::string body;
  size_t newline;
  if (!readBody(req, body) || (newline = body.find('\n')) == std::string::npos) {
    return reply(req, "400 Bad Request", "text/plain", "Invalid request");
  }
  uint32_t kbps = static_cast<uint32_t>(strtoul(body.substr(0, newline).c_str(), nullptr, 10));
  std::string name = body.substr(newline + 1);
  bool valid = !name.empty() && name.size() <= MAX_NAME_BYTES;
  for (unsigned char c : name) valid = valid && c >= 0x20 && c != 0x7f;
  if (!valid) {
    return reply(req, "400 Bad Request", "text/plain",
                 "The name must be 1 to 40 characters");
  }
  if (!wifi_setup::saveDeviceSettings(name, kbps)) {
    return reply(req, "400 Bad Request", "text/plain", "Invalid quality or could not save");
  }
  esp_timer_handle_t timer;
  esp_timer_create_args_t args = {};
  args.callback = [](void*) { esp_restart(); };
  args.name = "cfg_restart";
  if (esp_timer_create(&args, &timer) == ESP_OK) {
    esp_timer_start_once(timer, 1000 * 1000);
  }
  return reply(req, "200 OK", "text/plain", "Saved, restarting...");
}

}  // namespace

void start(const std::string& name, uint32_t kbps, const char* network) {
  deviceName = name;
  bitrate = kbps;
  networkName = network;

  std::string saved;
  eq::Config config;
  if (loadEq(saved) && eq::parse(saved, config)) {
    eq::instance().setConfig(config);
    ESP_LOGI(TAG, "Equalizer loaded: %s", saved.c_str());
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 6144;
  cfg.max_open_sockets = 3;
  cfg.max_uri_handlers = 6;
  cfg.lru_purge_enable = true;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Could not start the config server");
    return;
  }
  const httpd_uri_t routes[] = {
      {"/", HTTP_GET, rootGet, nullptr},
      {"/api/state", HTTP_GET, stateGet, nullptr},
      {"/api/eq", HTTP_POST, eqPost, nullptr},
      {"/api/device", HTTP_POST, devicePost, nullptr},
  };
  for (const auto& r : routes) httpd_register_uri_handler(server, &r);
  ESP_LOGI(TAG, "Config page on port 80, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

}  // namespace config_server
