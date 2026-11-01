#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

git submodule update --init --recursive

bash ./patches/apply.sh

. ./env.sh
python -m pip install --quiet protobuf grpcio-tools "setuptools<81"
