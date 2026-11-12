#!/usr/bin/env python3
"""Read the board's serial output for a few seconds without resetting it.

    python tools/bootlog.py PORT [SECONDS] [LOGFILE]

Prints a short summary (network, device name, address, DAC, crashes) and saves the
full log when LOGFILE is given. Needs pyserial, which ESP-IDF's Python already has.
Exit code: 0 when the receiver started, 1 when it did not, 2 when the port can't be read.
"""
import re
import sys
import time

import serial


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    port = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20
    logfile = sys.argv[3] if len(sys.argv) > 3 else None

    try:
        link = serial.Serial()
        link.port, link.baudrate, link.timeout = port, 115200, 1
        # Leave DTR/RTS low: toggling them resets the board.
        link.dtr = link.rts = False
        link.open()
    except (serial.SerialException, OSError) as err:
        print(f"Cannot open {port}: {err}")
        return 2

    data = b""
    end = time.time() + seconds
    while time.time() < end:
        data += link.read(4096)
    link.close()
    text = data.decode(errors="replace")
    if logfile:
        with open(logfile, "w") as out:
            out.write(text)

    def find(pattern):
        match = re.search(pattern, text)
        return match.group(1) if match else None

    started = find(r"spotifyesp: (Connected over \w+, starting Spotify receiver as '[^']*')")
    print("Receiver :", started or "not started (yet)")
    print("Address  :", find(r"mDNS address: (\S+)") or "-")
    print("Config   :", "page up" if "Config page on port 80" in text else "-")
    if "PCM5122" in text:
        print("DAC      :", find(r"(PCM5122[^\n\r]*)"))
    print("Login    :", "saved login" if "Using saved Spotify login" in text else
          "waiting for the Spotify app" if "Waiting for spotify app" in text else "-")
    portal = find(r"(Setup network '[^']*' open[^\n\r]*)")
    if portal:
        print("Portal   :", portal)
    crash = find(r"(Guru Meditation[^\n\r]*|abort\(\)[^\n\r]*|assert failed[^\n\r]*)")
    if crash:
        print("CRASH    :", crash)
    if not text.strip():
        print("No output. The board may be past its boot messages; press RST and run again.")
    return 0 if (started or portal) and not crash else 1


if __name__ == "__main__":
    sys.exit(main())
