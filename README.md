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
git clone --recursive <this repo>
cd spotifyesp
./flash.sh                 # asks for board, WiFi (first time) and port
./flash.sh zero-n8r8       # or name the board directly
./flash.sh zero-n8r8 -p /dev/cu.usbmodem1101 --monitor
```

`flash.sh` creates `sdkconfig.local` with your WiFi details (not committed), runs
`setup.sh` when needed, builds into `build-<board>/` and flashes. `--build-only`
skips flashing.

The project builds with ESP-IDF 5.5. `env.sh` reuses the toolchain that ESPHome
downloads into `~/Library/Caches/esphome/idf`; with a normal ESP-IDF install,
export your own environment instead and adjust `env.sh`.

Open Spotify, press the *Devices* button and pick the device name. The login is
not stored, so after a reboot you have to select the device again in the app.

## Notes

- `patches/nanopb-protobuf7.patch` and `patches/cspot-login-mac.patch` fix cspot for current protobuf and error handling (applied by `setup.sh`).
- Volume is applied in software in `main/I2SAudioSink.cpp`.
