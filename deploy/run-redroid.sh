#!/usr/bin/env bash
# Start ReDroid Android 12 + MindTheGapps + libndk (image built with redroid-script:
#   python3 redroid.py -a 12.0.0 -mtg -n).
# adb is only published on 127.0.0.1; the gateway is the only remote entry point.
set -euo pipefail
DATA_DIR=${DATA_DIR:-$HOME/redroid/data12}
IMAGE=${IMAGE:-redroid/redroid:12.0.0_mindthegapps_ndk}
mkdir -p "$DATA_DIR"
docker run -d --name redroid12 --restart unless-stopped --privileged \
  -v "$DATA_DIR":/data -p 127.0.0.1:5555:5555 \
  "$IMAGE" \
  androidboot.redroid_width=1080 androidboot.redroid_height=1920 androidboot.redroid_dpi=420 \
  androidboot.redroid_gpu_mode=auto \
  ro.product.cpu.abilist=x86_64,arm64-v8a,x86,armeabi-v7a,armeabi \
  ro.product.cpu.abilist64=x86_64,arm64-v8a \
  ro.product.cpu.abilist32=x86,armeabi-v7a,armeabi \
  ro.dalvik.vm.isa.arm=x86 ro.dalvik.vm.isa.arm64=x86_64 \
  ro.enable.native.bridge.exec=1 ro.vendor.enable.native.bridge.exec=1 ro.vendor.enable.native.bridge.exec64=1 \
  ro.dalvik.vm.native.bridge=libndk_translation.so ro.ndk_translation.version=0.2.3
