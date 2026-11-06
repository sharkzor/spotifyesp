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

On the XIAO the wiring is BCK→D4 (GPIO5), LCK→D3 (GPIO4), DIN→D5 (GPIO6). To flash it, hold BOOT while plugging in USB.

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

## Build and flash

```sh
git clone <this repo>
cd spotifyesp
./flash.sh                 # asks for board, WiFi (first time, optional) and port
./flash.sh zero-n8r8       # or name the board directly
./flash.sh zero-n8r8 -p /dev/cu.usbmodem1101 --monitor
```

`flash.sh` creates `sdkconfig.local` with your WiFi details (not committed), runs
`setup.sh` when needed, builds into `build-<board>/` and flashes. `--build-only`
skips flashing. Leave the WiFi SSID empty to set up WiFi on the board itself, see below.

The project builds with ESP-IDF 5.5. `env.sh` reuses the toolchain that ESPHome
downloads into `~/Library/Caches/esphome/idf`; with a normal ESP-IDF install,
export your own environment instead and adjust `env.sh`.

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
