#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../esp"
idf.py build
