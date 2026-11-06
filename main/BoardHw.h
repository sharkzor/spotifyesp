#pragma once

// Network and DAC bring-up that only some boards need.
namespace board {

// Starts the W5500 Ethernet module and waits for an IP address.
// Returns false when the module is absent, has no link or gets no address in time.
// Always false when CONFIG_SPOTIFY_ETHERNET is off.
bool ethernetConnect();

// Unmutes the PCM5122 DAC and points its clock PLL at the I2S bit clock.
// Does nothing when CONFIG_SPOTIFY_PCM5122 is off.
void dacInit();

}  // namespace board
