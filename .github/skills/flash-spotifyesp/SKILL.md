---
name: flash-spotifyesp
description: Build, flash and verify the spotifyesp firmware on an attached ESP32-S3 board (devkit-n16r8, zero-n8r8, xiao-esp32s3, hifi-esp32-s3-plus), or publish release images. Use when asked to flash, reflash, rebuild, check the boot log of, or release the spotifyesp project.
---

# Flash spotifyesp

## Quick path

From the repo root:

```sh
./flash.sh <board> -p <port> --verify
```

Boards are the files in `boards/*.defaults`. `--verify` reads the boot log afterwards via
`tools/bootlog.py` and prints the network, device name, address and any crash.
`flash.sh` sources `env.sh`, which finds ESP-IDF 5.5.6 (activated install,
`~/esp/esp-idf`, `$ESP_IDF_DIR`, or ESPHome's cache). `flash.sh` asks for WiFi details
the first time and writes `sdkconfig.local` (not committed).

## Non-interactive build and flash (agent use)

`flash.sh` prompts when `sdkconfig.local` is missing, so run the build directly:

```sh
. env.sh          # or: . <esp-idf>/export.sh with IDF_TOOLS_PATH set
b=<board>
idf -B build-$b -D SDKCONFIG=sdkconfig.$b \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/$b.defaults" \
  -D CMAKE_POLICY_VERSION_MINIMUM=3.5 build
idf -B build-$b -D SDKCONFIG=sdkconfig.$b -p <port> flash
```

(`idf` is the function from `env.sh`; otherwise use `idf.py`.) Without `sdkconfig.local`
the image has no WiFi details and starts the WiFi setup portal. A successful flash prints
three "Hash of data verified." lines.

## Finding the port

`ls /dev/cu.usbmodem*`. The port name changes per board and sometimes after a
reset (for example `usbmodem101` or `usbmodem1101`). Only one board attached means one
port. Ask which board is attached if several ports exist.

## Checking the result

```sh
python tools/bootlog.py <port> 20 [logfile]
```

It reads without toggling DTR/RTS, so it doesn't reset the board; press RST for the
boot messages. Healthy output: `Connected over WiFi|Ethernet, starting Spotify receiver as '<name>'`,
an `mDNS address`, and `Config page on port 80`. On the Plus board also a PCM5122 line.

## Gotchas

- Opening the port with other tools (a leftover `serial_watch`, `idf.py monitor`) can
  hold it busy or reset the board, and then `esptool` reports "No serial data received".
  Check `pgrep -fl "serial_watch|esptool|idf.py"`, kill leftovers by PID, replug USB.
- The Zero has no auto-reset: hold BOOT, plug in USB, release. The Plus has BOOT and RST buttons.
- Flashing a merged release image (offset 0) wipes NVS: saved WiFi, Spotify login, name
  and equalizer are lost. A normal `flash` keeps them.
- No `timeout` command on macOS.

## Release images

Build all four boards as above, then merge each into one image and attach them:

```sh
(cd build-$b && python -m esptool --chip esp32s3 merge_bin -o /tmp/rel/spotifyesp-$b.bin @flash_args)
(cd /tmp/rel && shasum -a 256 *.bin > SHA256SUMS)
gh release create vX.Y.Z /tmp/rel/*.bin /tmp/rel/SHA256SUMS --target main --title vX.Y.Z --notes "..."
```

Build from a clean, pushed `main`, run `tests/test_*.py` first, and never ship images
built with a personal `sdkconfig.local`. Test one image on hardware.
