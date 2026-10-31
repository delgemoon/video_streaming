#!/usr/bin/env bash
# Starts the backend and the Electron app together for development. Ctrl-C stops both.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${VSP_PORT:-8080}"

if [ ! -x "$ROOT/backend/build/vsp-server" ]; then
  "$ROOT/scripts/build-backend.sh"
fi

"$ROOT/backend/build/vsp-server" --port "$PORT" --data-dir "$ROOT/data" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

until curl -sf "http://127.0.0.1:$PORT/api/health" >/dev/null; do sleep 0.5; done
echo "backend ready on http://127.0.0.1:$PORT"

cd "$ROOT/desktop"
if [ ! -d node_modules ]; then npm install; fi
if [ -n "${VSP_DEBUG_PORT:-}" ]; then
  VSP_SERVER_URL="http://127.0.0.1:$PORT" npm run dev -- -- --remote-debugging-port="$VSP_DEBUG_PORT"
else
  VSP_SERVER_URL="http://127.0.0.1:$PORT" npm run dev
fi
