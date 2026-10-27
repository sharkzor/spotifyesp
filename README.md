# spotifyesp

Spotify Connect receiver for an ESP32-S3 with an I2S DAC (PCM5102A), based on
[cspot](https://github.com/feelfreelinux/cspot). Requires Spotify Premium.

Tested on an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM.

## Wiring (PCM5102A)

| PCM5102A | ESP32-S3 |
|----------|----------|
| BCK      | GPIO5    |
| LCK      | GPIO4    |
| DIN      | GPIO6    |
| VIN      | 3V3      |
| GND      | GND      |
| SCK      | GND      |

Pins can be changed in `idf.py menuconfig` under *Spotify ESP*.

## Build and flash

The project builds with ESP-IDF 5.5. `env.sh` reuses the toolchain that ESPHome
downloads into `~/Library/Caches/esphome/idf`; with a normal ESP-IDF install,
just `. export.sh` instead.

```sh
git clone --recursive <this repo>
cd spotifyesp
./setup.sh                       # python deps + nanopb patch for protobuf 5+
cat > sdkconfig.local <<EOF      # not committed
CONFIG_SPOTIFY_WIFI_SSID="your-ssid"
CONFIG_SPOTIFY_WIFI_PASSWORD="your-password"
CONFIG_SPOTIFY_DEVICE_NAME="Spotify ESP"
EOF
. ./env.sh
idf set-target esp32s3
idf build
idf -p /dev/cu.usbmodemXXXX flash
```

Open Spotify, press the *Devices* button and pick the device name. The login is
not stored, so after a reboot you have to select the device again in the app.

## Notes

- `patches/nanopb-protobuf7.patch` fixes cspot's old nanopb generator for current `protobuf`.
- Volume is applied in software in `main/I2SAudioSink.cpp`.
