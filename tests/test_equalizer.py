#!/usr/bin/env python3
"""Build the equalizer on the host and check its gains, bypass, clipping and text format."""

from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    cxx = os.environ.get("CXX", "clang++")
    with tempfile.TemporaryDirectory(prefix="spotifyesp-eq-") as directory:
        binary = Path(directory) / "eq_test"
        subprocess.run(
            [cxx, "-std=c++20", "-fsanitize=address,undefined", "-g", f"-I{ROOT}/main",
             str(ROOT / "tests/eq_test.cpp"), str(ROOT / "main/Equalizer.cpp"),
             "-o", str(binary)],
            check=True, capture_output=True, text=True)
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        assert run.returncode == 0, run.stdout + run.stderr
        print(run.stdout.strip())


if __name__ == "__main__":
    main()
