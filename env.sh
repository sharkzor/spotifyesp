#!/usr/bin/env bash
# Source this file to get an ESP-IDF 5.5 toolchain on the PATH and an `idf` helper.
# It uses, in this order:
#   1. an already activated ESP-IDF (IDF_PATH set, after `. export.sh`)
#   2. a normal install in $ESP_IDF_DIR, ~/esp/esp-idf or ~/.espressif/esp-idf
#   3. the toolchain ESPHome downloaded into ~/Library/Caches/esphome/idf

_spotifyesp_find_idf() {
  if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then echo "$IDF_PATH"; return; fi
  for d in "${ESP_IDF_DIR:-}" "$HOME/esp/esp-idf" "$HOME/.espressif/esp-idf"; do
    [ -n "$d" ] && [ -f "$d/export.sh" ] && { echo "$d"; return; }
  done
}

C="$HOME/Library/Caches/esphome/idf"
_IDF_DIR="$(_spotifyesp_find_idf)"

if [ -n "$_IDF_DIR" ]; then
  export IDF_PATH="$_IDF_DIR"
  . "$IDF_PATH/export.sh" >/dev/null
  idf() { python "$IDF_PATH/tools/idf.py" "$@"; }
elif [ -d "$C/frameworks/5.5.6" ] && ls -d "$C"/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin >/dev/null 2>&1; then
  export IDF_PATH="$C/frameworks/5.5.6"
  export IDF_TOOLS_PATH="$C"
  export IDF_PYTHON_ENV_PATH="$C/penvs/5.5.6"
  export IDF_COMPONENT_MANAGER=1
  TOOLS="$(ls -d $C/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)"
  export PATH="$IDF_PYTHON_ENV_PATH/bin:$C/tools/cmake/3.30.2/CMake.app/Contents/bin:$C/tools/ninja/1.12.1:$TOOLS:$PATH"
  idf() { python "$IDF_PATH/tools/idf.py" "$@"; }
  export ESP_ROM_ELF_DIR="$(ls -d $C/tools/esp-rom-elfs/* | head -1)/"
else
  cat >&2 <<'MSG'
ESP-IDF 5.5 was not found. Install it once, then run this script again:

  mkdir -p ~/esp && cd ~/esp
  git clone -b v5.5.6 --recursive https://github.com/espressif/esp-idf.git
  ./esp-idf/install.sh esp32s3

(Needs git, python3 and cmake. See the ESP-IDF getting started guide for macOS.)
If ESP-IDF is installed somewhere else, run `export ESP_IDF_DIR=/path/to/esp-idf` first.
MSG
  return 1 2>/dev/null || exit 1
fi
unset -f _spotifyesp_find_idf
