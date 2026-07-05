#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "Usage: scripts/flash_photo.sh IMAGE [PORT]"
  exit 1
fi

IMAGE="$1"
PORT="${2:-/dev/ttyACM0}"

cd "$(dirname "$0")/../esp"
python flash_photo.py "$IMAGE" -p "$PORT"
