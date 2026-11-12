# spotifyesp

Spotify Connect receiver for an ESP32-S3 with an I2S DAC (PCM5102A), based on
[cspot](https://github.com/feelfreelinux/cspot). Requires Spotify Premium.

Tested on an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.

## Supported boards

| Board name (`flash.sh`) | Board | Flash / PSRAM |
|---|---|---|
| `devkit-n16r8` | ESP32-S3-DevKitC-1 style N16R8 | 16 MB / 8 MB octal |
| `zero-n8r8` | Waveshare ESP32-S3-Zero-N8R8 | 8 MB / 8 MB octal |
| `xiao-esp32s3` | Seeed Studio XIAO ESP32-S3 (also the Sense) | 8 MB / 8 MB octal |
| `hifi-esp32-s3-plus` | Sonocotta HiFi-ESP32-S3-Plus (PCM5122, optional W5500 Ethernet) | 8 MB / 8 MB octal |

On the XIAO the wiring is BCK→D4 (GPIO5), LCK→D3 (GPIO4), DIN→D5 (GPIO6). To flash it, hold BOOT while plugging in USB.

### HiFi-ESP32-S3-Plus (Ethernet)

Tested on a rev H1+ board with the W5500 module: Ethernet gets an address and Spotify audio plays. It uses the board's own pins (I2S 14/15/16, from Sonocotta's firmware repo).
- **Ethernet:** at boot it tries the W5500 module for `CONFIG_SPOTIFY_ETH_TIMEOUT_S` seconds (default 15). If there is no module, no link or no address, it falls back to WiFi and the setup portal. The BOOT button still opens the portal.
- **DAC:** the PCM5122 is unmuted through XSMT (GPIO4), reset, and set over I2C to take its clock from the I2S bit clock and to ignore the missing master clock (address 0x4D on rev H1 and later, 0x4C on rev H; change `CONFIG_SPOTIFY_PCM5122_ADDR`).
- The OLED shares the SPI bus with the Ethernet module, so it is not supported.
- Software volume is still used; the DAC's digital volume and EQ are not.

Board settings live in `boards/<name>.defaults`; add a file there for another board.

## Wiring (PCM5102A)

Pin numbers are GPIO numbers, as printed on the board. The same pins are used on both boards.

| PCM5102A | ESP32-S3 GPIO |
|----------|---------------|
| BCK      | 5             |
| LCK      | 4             |
| DIN      | 6             |
| VIN      | 3V3 (or 5V)   |
| GND      | GND           |
| SCK      | GND           |

PCM5102A solder pads: H1 (FLT) = L, H2 (DEMP) = L, H3 (XSMT) = H, H4 (FMT) = L.
XSMT must be high or the DAC stays muted.

Pins can be changed in `idf.py menuconfig` under *Spotify ESP*, or per board in `boards/*.defaults`.

### Waveshare ESP32-S3-Zero pinout notes

GPIO1 to GPIO13 are broken out on the side headers, so GPIO4, 5 and 6 are available.
Avoid these pins:

- GPIO0: BOOT button (strapping)
- GPIO19, GPIO20: USB
- GPIO21: onboard WS2812 LED
- GPIO33 to GPIO37: used by the octal PSRAM (not broken out)
- GPIO43, GPIO44: UART0 TX/RX
- GPIO45, GPIO46: strapping pins

There is no USB-UART chip. To flash, hold BOOT, plug in USB, then release.
The 5V pad takes 3.7 to 6 V when powering externally.

## Quick start: pre-built images

If you only want to use the receiver, you don't need ESP-IDF. Download the image for
your board from the [Releases page](https://github.com/sharkzor/spotifyesp/releases):

| Board | File |
|---|---|
| ESP32-S3 DevKitC-1 N16R8 | `spotifyesp-devkit-n16r8.bin` |
| Waveshare ESP32-S3-Zero N8R8 | `spotifyesp-zero-n8r8.bin` |
| Seeed XIAO ESP32-S3 | `spotifyesp-xiao-esp32s3.bin` |
| HiFi-ESP32-S3-Plus | `spotifyesp-hifi-esp32-s3-plus.bin` |

Each file is a complete image (bootloader, partition table and app) that goes at flash
offset 0. Flash it with Python's `esptool` (`pip install esptool`):

```sh
esptool --chip esp32s3 -p /dev/cu.usbmodem101 write_flash 0x0 spotifyesp-zero-n8r8.bin
```

Use your own serial port (on Linux usually `/dev/ttyACM0`). On the Zero, hold BOOT while
plugging in USB first. Without a command line you can use a browser flasher such as
<https://espressif.github.io/esptool-js/> (Chrome or Edge): connect, set the offset to
`0x0`, pick the file and press *Program*. Flashing replaces everything on the board,
including saved WiFi, Spotify login and equalizer settings.

The images contain no WiFi details. After flashing, join the board's `SpotifyESP-Setup-XXXX`
network and enter your WiFi, see [WiFi setup portal](#wifi-setup-portal). The HiFi-ESP32-S3-Plus
tries Ethernet first. Compare `SHA256SUMS` to check your download.

The sections below are only needed to build the firmware yourself.

## Prerequisites

- A Mac or Linux computer with a USB port (Windows is untested; WSL may work).
- An ESP32-S3 board from the list above, a USB cable that carries data, and a
  Spotify Premium account (Spotify Connect needs it).
- `git`, `python3` (3.9 or newer) and `cmake`. On macOS: `xcode-select --install`, then
  `brew install cmake python`.
- ESP-IDF 5.5, installed once (about 1.5 GB, a few minutes):

  ```sh
  mkdir -p ~/esp && cd ~/esp
  git clone -b v5.5.6 --recursive https://github.com/espressif/esp-idf.git
  ./esp-idf/install.sh esp32s3
  ```

`env.sh` finds ESP-IDF by itself: an already activated one, `~/esp/esp-idf`,
`~/.espressif/esp-idf` or `$ESP_IDF_DIR`, or the toolchain ESPHome keeps in
`~/Library/Caches/esphome/idf`. All other dependencies are vendored or downloaded by
the build, and `flash.sh` installs the two Python packages it needs.

## Build and flash

```sh
git clone https://github.com/sharkzor/spotifyesp.git
cd spotifyesp
./flash.sh                 # asks for board, WiFi (first time, optional) and port
./flash.sh zero-n8r8       # or name the board directly
./flash.sh zero-n8r8 -p /dev/cu.usbmodem1101 --monitor
```

`flash.sh` creates `sdkconfig.local` with your WiFi details (not committed), runs
`setup.sh` when needed, builds into `build-<board>/` and flashes. `--build-only`
skips flashing, and `--verify` reads the boot log afterwards and reports the network,
device name and address (`tools/bootlog.py`). Leave the WiFi SSID empty to set up WiFi on the board itself, see below.

Open Spotify, press the *Devices* button and pick the device name. After the first
login the board saves Spotify's reusable credentials in NVS and signs in again by
itself after a reboot; you still start playback from the app. Tick *Forget the saved
Spotify login* in the setup portal to remove them. NVS is not encrypted.

## WiFi setup portal

The WiFi network and the Spotify device name can be entered on the board itself, so
they don't have to be compiled in.

1. The board opens the WiFi network `SpotifyESP-Setup-XXXX` (no password) when it has
   no WiFi settings, when it can't join the saved network, or after you hold the BOOT
   button for 3 seconds and release it (hold it after the board has started; holding
   BOOT while powering up enters flash mode).
2. Connect a phone or laptop to it. The setup page opens by itself; if not, browse to
   `http://192.168.4.1/`.
3. Pick the network, enter the password and the device name and press *Save and restart*.

Settings are stored in flash and take precedence over WiFi settings compiled in via
`sdkconfig.local`, which remain the fallback. If the saved network is unreachable, the
setup network comes back after about 30 seconds and the board restarts to retry after
5 minutes without use. The portal is open to anyone in range while it is active.
The BOOT button GPIO and the setup network name can be changed in `idf.py menuconfig`
under *Spotify ESP*.

## Notes

- cspot and its `bell` support library are vendored in `cspot/` as plain source
  (see `cspot/UPSTREAM.md` for origin, licence and the changes made here). Our
  fixes are edited directly in that source; git history shows each change.
- The Spotify dispatcher has a 32 KB PSRAM stack for reconnect/authentication,
  loops instead of recursing on reconnect, and protects connection replacement and
  pending request callbacks across tasks. Serial logs report its minimum free stack
  after reconnects and Spotify pings.
- The HTTP parser throws on a closed connection instead of writing before its
  buffer, and stream open/read/seek and Vorbis init failures are caught so a bad
  track is skipped rather than ending playback.
- Audio streams at 320 kbps Ogg Vorbis (Premium), falling back to 160 then 96 kbps
  when a track has no 320 kbps file. The quality (96, 160 or 320 kbps) is chosen
  in the setup portal and saved in NVS. The Spotify app's quality setting does not
  apply to Connect receivers. Lossless is not available to third-party receivers.
- Track restrictions aimed at other account types (catalogues) are ignored, as in
  librespot, so Premium tracks aren't reported as unavailable.
- Boot logs include `esp_reset_reason()` (4 = panic, 9 = brownout). A firmware
  update preserves the WiFi settings saved in NVS.
- The CDN stream explicitly allocates the CDN header, footer and read
  buffers in PSRAM on ESP32, without reducing their sizes. Allocation failures
  are logged and passed to the player's stream-open error handler; they do not
  fall back to scarce internal RAM. Native builds retain standard allocation.
  WiFi/DMA buffers, PCM queue capacity and task stack sizes are unchanged.
- After starting the receiver, `app_main()` returns so ESP-IDF can reclaim its
  8 KB task stack and task resources. Settings needed during playback are copied
  before startup returns; WiFi event handling and the setup button remain active.
- Volume is applied in software in `main/I2SAudioSink.cpp`.

The reconnect regression test compiles the session implementation with a
fake transport, using Clang and AddressSanitizer/UndefinedBehaviorSanitizer:
`python3 tests/test_reconnect.py`. It covers repeated
failures, cancellation, callback delivery, and concurrent connection replacement;
hardware playback is still needed to check the actual ESP32 stack headroom.
`python3 tests/test_http_eof.py` runs the real HTTP parser under AddressSanitizer
against a closed connection.
`python3 tests/test_stream_psram.py` checks the real CDN stream with fake HTTP,
crypto and ESP32 allocation APIs under ASan/UBSan: buffer sizes, read/seek
behavior, repeated open/close, and allocation-failure cleanup.

On the N16R8 board, separate 30-minute playback captures on 2026-11-03 confirmed
the PSRAM change and startup cleanup. With both changes, sampled free internal
RAM stayed above 57,991 bytes and ended at 63,595 bytes, compared with about
18 KB before optimization. The largest free internal/DMA block stayed at
31,744 bytes. These are sampled observations, not guaranteed memory bounds.
Active-playback reconnect recovery and longer sessions remain on the checklist
in [IDEAS.md](IDEAS.md).

## Licence

GNU General Public License v3 (see `LICENSE`), because this project includes
cspot and bell, which are GPLv3 (see `cspot/UPSTREAM.md`).

## Config page and equalizer

Once the board is on the network, open `http://<device-name>.local/` in a browser, where the name is your Spotify device name in lower case with dashes ("Spotify Zolder" becomes `spotify-zolder.local`), or use the board's IP. The serial log prints the exact address. The page edits:

- Spotify device name and streaming quality (saving restarts the board; the saved login is kept).
- A 6-band parametric equalizer (peak, low shelf, high shelf) with pre-gain. Changes apply immediately while playing and are saved.

Lower the pre-gain when boosting to avoid clipping. There is no password: anyone on your local network can open the page, so don't expose port 80 to the internet.
