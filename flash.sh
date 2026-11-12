#!/usr/bin/env bash
# Build and flash the Spotify Connect firmware for a chosen board.
#
#   ./flash.sh                       choose board from a menu
#   ./flash.sh zero-n8r8             build + flash Waveshare ESP32-S3-Zero-N8R8
#   ./flash.sh devkit-n16r8 -p /dev/cu.usbmodem1101
#   ./flash.sh zero-n8r8 --monitor   also open the serial monitor afterwards
#   ./flash.sh zero-n8r8 --verify    afterwards read the boot log and report success
#   ./flash.sh zero-n8r8 --build-only
set -euo pipefail
cd "$(dirname "$0")"

BOARD=""
PORT=""
MONITOR=0
VERIFY=0
BUILD_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    -p|--port) PORT="$2"; shift 2 ;;
    -m|--monitor) MONITOR=1; shift ;;
    --verify) VERIFY=1; shift ;;
    --build-only) BUILD_ONLY=1; shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    -*) echo "Unknown option $1"; exit 1 ;;
    *) BOARD="$1"; shift ;;
  esac
done

BOARDS=($(ls boards/*.defaults | sed 's#boards/##; s#\.defaults##'))
if [ -z "$BOARD" ]; then
  echo "Choose a board:"
  select BOARD in "${BOARDS[@]}"; do [ -n "$BOARD" ] && break; done
fi
if [ ! -f "boards/$BOARD.defaults" ]; then
  echo "Unknown board '$BOARD'. Available: ${BOARDS[*]}"; exit 1
fi

if [ ! -f sdkconfig.local ]; then
  read -rp "WiFi SSID (empty = set it up later through the board's setup portal): " SSID
  PASS=""; NAME=""
  if [ -n "$SSID" ]; then
    read -rsp "WiFi password: " PASS; echo
    read -rp "Device name shown in Spotify [Spotify ESP]: " NAME
  fi
  cat > sdkconfig.local <<EOF
CONFIG_SPOTIFY_WIFI_SSID="$SSID"
CONFIG_SPOTIFY_WIFI_PASSWORD="$PASS"
CONFIG_SPOTIFY_DEVICE_NAME="${NAME:-Spotify ESP}"
EOF
fi

. ./env.sh
python -c "import google.protobuf, grpc_tools" 2>/dev/null || ./setup.sh

if [ -z "$PORT" ] && [ "$BUILD_ONLY" = 0 ]; then
  PORTS=($(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true))
  if [ ${#PORTS[@]} -eq 0 ]; then
    echo "No serial port found. For the Zero: hold BOOT, plug in USB, then release."; exit 1
  elif [ ${#PORTS[@]} -eq 1 ]; then
    PORT="${PORTS[0]}"
  else
    echo "Choose a port:"
    select PORT in "${PORTS[@]}"; do [ -n "$PORT" ] && break; done
  fi
fi

export SDKCONFIG="sdkconfig.$BOARD"
ARGS=(-B "build-$BOARD" -D "SDKCONFIG=$SDKCONFIG"
      -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/$BOARD.defaults;sdkconfig.local")

echo "==> Building for $BOARD"
idf "${ARGS[@]}" build
[ "$BUILD_ONLY" = 1 ] && exit 0

echo "==> Flashing $PORT"
idf "${ARGS[@]}" -p "$PORT" flash
if [ "$VERIFY" = 1 ]; then
  echo "==> Reading the boot log (20 s)"
  sleep 2   # the USB port can re-enumerate right after the reset
  python tools/bootlog.py "$PORT" 20 || echo "Boot check failed, see the output above."
fi
[ "$MONITOR" = 1 ] && idf "${ARGS[@]}" -p "$PORT" monitor
echo "Done. Open Spotify and pick the device from the speaker menu."
