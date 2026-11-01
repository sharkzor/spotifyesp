#!/usr/bin/env python3
"""Run the real bell HTTP parser under ASan: a closed connection must throw, not corrupt memory."""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BELL = ROOT / "cspot/cspot/bell"


def main():
    cxx = os.environ.get("CXX", "clang++")
    with tempfile.TemporaryDirectory(prefix="spotifyesp-http-") as directory:
        binary = Path(directory) / "repro"
        subprocess.run(
            [cxx, "-x", "c++", "-std=c++20", "-fsanitize=address,undefined", "-g",
             f"-I{BELL}/external/fmt/include", f"-I{BELL}/main/io/include",
             f"-I{BELL}/main/utilities/include", f"-I{BELL}/main/platform/include",
             str(ROOT / "tests/http_eof_repro.cpp"), f"{BELL}/main/io/HTTPClient.cpp",
             f"{BELL}/main/io/URLParser.cpp", "-x", "c++",
             f"{BELL}/main/io/picohttpparser.c", "-o", str(binary)],
            check=True, capture_output=True, text=True)
        for args in ([], ["closed"]):
            run = subprocess.run([str(binary), *args], capture_output=True, text=True)
            assert run.returncode == 0, run.stderr
        print("HTTP EOF handling passed")


if __name__ == "__main__":
    main()
