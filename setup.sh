#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

git submodule update --init --recursive

# cspot's bundled nanopb generator uses an API removed in protobuf 5+
if ! git -C cspot/cspot/bell apply --check ../../../patches/nanopb-protobuf7.patch 2>/dev/null; then
  echo "nanopb patch already applied or not needed"
else
  git -C cspot/cspot/bell apply ../../../patches/nanopb-protobuf7.patch
fi

. ./env.sh
python -m pip install --quiet protobuf grpcio-tools "setuptools<81"
