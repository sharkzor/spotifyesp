#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

. ./env.sh
python -m pip install --quiet protobuf grpcio-tools "setuptools<81"
