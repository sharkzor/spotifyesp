#!/usr/bin/env python3
"""Verify clean-checkout application, idempotency, and conflict detection."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PATCHES = (
    ("cspot/cspot/bell", "nanopb-protobuf7.patch"),
    ("cspot", "cspot-login-mac.patch"),
    ("cspot/cspot/bell", "bell-civetweb-threads.patch"),
    ("cspot", "cspot-reconnect.patch"),
    ("cspot", "cspot-stream-errors.patch"),
    ("cspot/cspot/bell", "bell-http-eof.patch"),
)


def main():
    with tempfile.TemporaryDirectory(prefix="spotifyesp-patches-") as directory:
        work = Path(directory)
        shutil.copytree(ROOT / "patches", work / "patches")
        for repository, patch in PATCHES:
            contents = (ROOT / "patches" / patch).read_text()
            for name in re.findall(r"^--- a/(.+)$", contents, re.MULTILINE):
                original = subprocess.check_output(
                    ["git", "-C", str(ROOT / repository), "show", f"HEAD:{name}"]
                )
                target = work / repository / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(original)

        command = ["bash", str(work / "patches" / "apply.sh")]
        first = subprocess.run(command, capture_output=True, text=True, check=True)
        assert first.stdout.count("Applied ") == len(PATCHES), first.stdout
        second = subprocess.run(command, capture_output=True, text=True, check=True)
        assert second.stdout.count("already applied") == len(PATCHES), second.stdout

        source = work / "cspot/cspot/src/MercurySession.cpp"
        source.write_text(source.read_text().replace(
            '"mercury_dispatcher", 32 * 1024', '"mercury_dispatcher", 16 * 1024'
        ))
        conflict = subprocess.run(command, capture_output=True, text=True)
        assert conflict.returncode != 0
        assert "Cannot apply cspot-reconnect.patch" in conflict.stderr, conflict.stderr
        print("Fresh patches, repeated application and conflict detection passed")


if __name__ == "__main__":
    main()
