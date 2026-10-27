#!/usr/bin/env bash
# Source this file to use the ESP-IDF toolchain that ESPHome already downloaded.
C="$HOME/Library/Caches/esphome/idf"
export IDF_PATH="$C/frameworks/5.5.6"
export IDF_TOOLS_PATH="$C"
export IDF_PYTHON_ENV_PATH="$C/penvs/5.5.6"
export IDF_COMPONENT_MANAGER=1
TOOLS="$(ls -d $C/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$C/tools/cmake/3.30.2/CMake.app/Contents/bin:$C/tools/ninja/1.12.1:$TOOLS:$PATH"
idf() { python "$IDF_PATH/tools/idf.py" "$@"; }
export ESP_ROM_ELF_DIR="$(ls -d $C/tools/esp-rom-elfs/* | head -1)/"
