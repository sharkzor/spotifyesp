#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

apply_patch() {
  local directory="$ROOT/$1" patch="$ROOT/patches/$2"
  if git -C "$directory" apply --reverse --check "$patch" 2>/dev/null; then
    echo "$2 already applied"
  elif git -C "$directory" apply --check "$patch"; then
    git -C "$directory" apply "$patch"
    echo "Applied $2"
  else
    echo "Cannot apply $2; dependency sources differ from the expected version." >&2
    return 1
  fi
}

apply_patch cspot/cspot/bell nanopb-protobuf7.patch
apply_patch cspot cspot-login-mac.patch
apply_patch cspot/cspot/bell bell-civetweb-threads.patch
apply_patch cspot cspot-reconnect.patch
apply_patch cspot cspot-stream-errors.patch
apply_patch cspot/cspot/bell bell-http-eof.patch
apply_patch cspot cspot-bitrate.patch
