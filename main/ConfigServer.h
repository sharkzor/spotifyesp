#pragma once
#include <cstdint>
#include <string>

// Small web page that stays up while the board is playing. It shows the device
// status and edits the Spotify name, streaming quality and the equalizer.
// Equalizer changes apply immediately; name and quality restart the board.
namespace config_server {

// Loads the saved equalizer, applies it, and starts the web server on port 80.
// `network` is shown on the page, for example "Ethernet" or "WiFi".
void start(const std::string& deviceName, uint32_t bitrateKbps, const char* network);

}  // namespace config_server
