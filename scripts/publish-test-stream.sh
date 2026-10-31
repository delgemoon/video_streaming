#!/usr/bin/env bash
# Publishes a synthetic live stream to the RTMP ingest, like OBS would.
# Usage: publish-test-stream.sh <rtmp-url> [seconds] [width] [height] [fps] [kbps]
set -euo pipefail
URL="$1"; SECS="${2:-60}"; W="${3:-1280}"; H="${4:-720}"; FPS="${5:-30}"; KBPS="${6:-3000}"
KEYINT=$((FPS * 2))   # 2 s keyframe interval, matching the server's live segment length

exec gst-launch-1.0 -q \
  videotestsrc is-live=true num-buffers=$((SECS * FPS)) pattern=ball \
    ! "video/x-raw,format=I420,width=${W},height=${H},framerate=${FPS}/1" \
    ! timeoverlay halignment=center valignment=center font-desc="Sans 48" \
    ! x264enc tune=zerolatency speed-preset=veryfast bitrate=${KBPS} key-int-max=${KEYINT} bframes=0 \
    ! h264parse ! queue ! flvmux streamable=true name=mux \
    ! rtmp2sink location="${URL}" \
  audiotestsrc is-live=true num-buffers=$((SECS * 47)) wave=ticks \
    ! audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=128000 ! aacparse ! queue ! mux.
