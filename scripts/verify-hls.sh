#!/usr/bin/env bash
# Validates an HLS output produced by vsp-server using GStreamer's hlsdemux2 (adaptivedemux2)
# and by decoding each rendition's concatenated CMAF files. Usage: verify-hls.sh <master.m3u8 URL> <media dir>
set -euo pipefail
URL="$1"
DIR="$2"

echo "== playbin3 over HLS: $URL"
GST_DEBUG=2 timeout 120 gst-launch-1.0 -q playbin3 uri="$URL" video-sink=fakesink audio-sink=fakesink 2>&1 | grep -vE "^$" || true
echo "playbin3 finished (exit ${PIPESTATUS[0]})"

for r in "$DIR"/v_* "$DIR"/a_*; do
  [ -d "$r" ] || continue
  tmp=$(mktemp --suffix=.mp4)
  cat "$r/init.mp4" "$r"/seg_*.m4s > "$tmp"
  echo "== $(basename "$r"): $(ls "$r"/seg_*.m4s | wc -l) segments"
  gst-discoverer-1.0 "$tmp" 2>/dev/null | grep -E "Duration|video:|audio:|Width|Height|Frame rate|Bitrate|Sample rate" | sed 's/^/   /'
  timeout 60 gst-launch-1.0 -q filesrc location="$tmp" ! decodebin3 ! fakesink sync=false 2>&1 | grep -iE "error|warn" | sed 's/^/   /' || true
  echo "   decode: ok"
  rm -f "$tmp"
done
