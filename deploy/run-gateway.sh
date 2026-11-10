#!/usr/bin/env bash
# Start the scrcpy gateway.
# Usage: LISTEN_HOST=<tailscale-ip> SCRCPY_DIR=<dir with adb and scrcpy-server v5.0.1> ./run-gateway.sh
set -euo pipefail
: "${LISTEN_HOST:?set LISTEN_HOST, e.g. the output of: tailscale ip -4}"
: "${SCRCPY_DIR:?set SCRCPY_DIR to the extracted scrcpy-linux-x86_64-v5.0.1 directory}"
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
docker run -d --name scrcpy-gateway --restart unless-stopped --network host \
  -v "$REPO_DIR/gateway":/gateway:ro -v "$SCRCPY_DIR":/scrcpy:ro \
  -e HOME=/tmp -e LISTEN_HOST="$LISTEN_HOST" \
  -e ADB=/scrcpy/adb -e SERVER_JAR=/scrcpy/scrcpy-server \
  python:3.12-slim python3 -u /gateway/scrcpy_gateway.py
