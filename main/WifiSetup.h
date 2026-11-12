#pragma once
#include <cstdint>
#include <string>
#include <vector>

// WiFi setup portal: the board opens its own access point with a captive web page
// where the WiFi network and the Spotify device name can be entered.
namespace wifi_setup {

struct Settings {
  std::string ssid;
  std::string password;
  std::string deviceName;
  // Streaming quality in kbps: 96, 160 or 320.
  uint32_t bitrate = 320;
};

// Loads the settings saved by the portal. Fields that were never saved stay empty.
// Returns true when a WiFi SSID is stored.
bool load(Settings& out);

// Reusable Spotify credentials returned after a successful login, so the board can
// sign in again after a reboot without the phone sending them.
struct SavedLogin {
  std::string username;
  std::vector<uint8_t> credentials;
};

// Returns true when a complete saved login exists.
bool loadLogin(SavedLogin& out);
bool saveLogin(const SavedLogin& login);
void clearLogin();

// Saves only the Spotify device name and streaming quality (kbps: 96, 160 or 320).
bool saveDeviceSettings(const std::string& deviceName, uint32_t bitrate);

// True when the previous boot asked for the portal. The request is cleared.
bool consumePortalRequest();

// Restarts the board and opens the portal on the next boot.
[[noreturn]] void restartIntoPortal();

// Opens the access point, DNS catch-all and web page. Saving the form restarts the
// board. `current` pre-fills the form and lets an empty password keep the old one.
// With idleRebootMs > 0 the board restarts after that long without web requests, so
// a temporarily unreachable network does not leave it stuck in setup mode.
[[noreturn]] void runPortal(const Settings& current, uint32_t idleRebootMs);

// Watches a button (active low). Holding it for 3 seconds and releasing it restarts
// into the portal. Does nothing for a negative gpio.
void startButtonWatcher(int gpio);

}  // namespace wifi_setup
