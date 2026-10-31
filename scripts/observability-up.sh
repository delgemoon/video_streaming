#!/usr/bin/env bash
# Starts the observability stack (Prometheus, Grafana, Tempo, OTel collector, Alertmanager,
# node_exporter) with Docker Compose and the host-side NVIDIA exporter.
#   scripts/observability-up.sh         start
#   scripts/observability-up.sh down    stop containers (keeps data volumes)
#   scripts/observability-up.sh status
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/deploy/observability"
PIDFILE="$DIR/.gpu-exporter.pid"
DOCKER="docker"
docker info >/dev/null 2>&1 || DOCKER="sudo docker"

gpu_up() {
  if command -v nvidia-smi >/dev/null 2>&1; then
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "gpu exporter already running (pid $(cat "$PIDFILE"))"
    else
      nohup python3 "$DIR/gpu/nvidia_smi_exporter.py" --port 9835 >"$DIR/.gpu-exporter.log" 2>&1 &
      echo $! >"$PIDFILE"
      echo "gpu exporter started on :9835 (pid $!)"
    fi
  else
    echo "nvidia-smi not found; skipping GPU exporter"
  fi
}
gpu_down() {
  if [ -f "$PIDFILE" ]; then
    kill "$(cat "$PIDFILE")" 2>/dev/null || true
    rm -f "$PIDFILE"
    echo "gpu exporter stopped"
  fi
}

case "${1:-up}" in
  up)
    python3 "$DIR/grafana/gen/gen_dashboards.py" >/dev/null
    (cd "$DIR" && $DOCKER compose up -d)
    gpu_up
    echo
    echo "Grafana      http://localhost:3000   (admin / vsp)"
    echo "Prometheus   http://localhost:9090"
    echo "Alertmanager http://localhost:9093"
    echo "Tempo        http://localhost:3200"
    echo
    echo "Run the server with tracing:  backend/build/vsp-server --otlp-endpoint http://127.0.0.1:4318/v1/traces"
    ;;
  down)
    (cd "$DIR" && $DOCKER compose down)
    gpu_down
    ;;
  status)
    (cd "$DIR" && $DOCKER compose ps)
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && echo "gpu exporter: running" || echo "gpu exporter: stopped"
    ;;
  *)
    echo "usage: $0 [up|down|status]"; exit 2 ;;
esac
