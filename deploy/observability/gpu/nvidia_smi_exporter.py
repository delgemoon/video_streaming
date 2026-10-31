#!/usr/bin/env python3
"""Tiny Prometheus exporter for NVIDIA GPUs using nvidia-smi (stdlib only, runs on the host).

Exposes utilisation (GPU, encoder, decoder), memory, temperature, power and clocks, plus the
NVENC/NVDEC session counts that matter for a transcoding box. Usage:

    nvidia_smi_exporter.py [--port 9835] [--interval 2]
"""
import argparse
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

FIELDS = [
    ("index", None, None),
    ("name", None, None),
    ("uuid", None, None),
    ("utilization.gpu", "nvidia_gpu_utilization_ratio", 0.01),
    ("utilization.memory", "nvidia_gpu_memory_utilization_ratio", 0.01),
    ("utilization.encoder", "nvidia_gpu_encoder_utilization_ratio", 0.01),
    ("utilization.decoder", "nvidia_gpu_decoder_utilization_ratio", 0.01),
    ("memory.used", "nvidia_gpu_memory_used_bytes", 1024 * 1024),
    ("memory.total", "nvidia_gpu_memory_total_bytes", 1024 * 1024),
    ("temperature.gpu", "nvidia_gpu_temperature_celsius", 1),
    ("power.draw", "nvidia_gpu_power_watts", 1),
    ("power.limit", "nvidia_gpu_power_limit_watts", 1),
    ("clocks.sm", "nvidia_gpu_clock_sm_hertz", 1e6),
    ("clocks.mem", "nvidia_gpu_clock_mem_hertz", 1e6),
    ("fan.speed", "nvidia_gpu_fan_speed_ratio", 0.01),
    ("pstate", None, None),
    ("encoder.stats.sessionCount", "nvidia_gpu_encoder_sessions", 1),
    ("encoder.stats.averageFps", "nvidia_gpu_encoder_average_fps", 1),
    ("encoder.stats.averageLatency", "nvidia_gpu_encoder_average_latency_microseconds", 1),
]
HELP = {
    "nvidia_gpu_utilization_ratio": "GPU core utilisation (0-1)",
    "nvidia_gpu_memory_utilization_ratio": "GPU memory bandwidth utilisation (0-1)",
    "nvidia_gpu_encoder_utilization_ratio": "NVENC utilisation (0-1)",
    "nvidia_gpu_decoder_utilization_ratio": "NVDEC utilisation (0-1)",
    "nvidia_gpu_memory_used_bytes": "Framebuffer memory in use",
    "nvidia_gpu_memory_total_bytes": "Framebuffer memory total",
    "nvidia_gpu_temperature_celsius": "GPU temperature",
    "nvidia_gpu_power_watts": "Power draw",
    "nvidia_gpu_power_limit_watts": "Power limit",
    "nvidia_gpu_clock_sm_hertz": "SM clock",
    "nvidia_gpu_clock_mem_hertz": "Memory clock",
    "nvidia_gpu_fan_speed_ratio": "Fan speed (0-1)",
    "nvidia_gpu_encoder_sessions": "Active NVENC sessions",
    "nvidia_gpu_encoder_average_fps": "Average NVENC fps across sessions",
    "nvidia_gpu_encoder_average_latency_microseconds": "Average NVENC latency",
}

_lock = threading.Lock()
_payload = "# nvidia-smi not sampled yet\n"
_ok = 0


def sample():
    global _payload, _ok
    query = ",".join(f for f, _, _ in FIELDS)
    try:
        out = subprocess.run(
            ["nvidia-smi", f"--query-gpu={query}", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5, check=True).stdout
        ok = 1
    except Exception as e:  # noqa: BLE001
        out, ok = "", 0
    lines = []
    seen = set()
    for row in out.strip().splitlines():
        cols = [c.strip() for c in row.split(",")]
        if len(cols) != len(FIELDS):
            continue
        vals = dict(zip((f for f, _, _ in FIELDS), cols))
        labels = f'gpu="{vals["index"]}",name="{vals["name"]}",uuid="{vals["uuid"]}"'
        for field, metric, scale in FIELDS:
            if not metric:
                continue
            raw = vals[field]
            try:
                v = float(raw) * scale
            except ValueError:
                continue
            if metric not in seen:
                lines.append(f"# HELP {metric} {HELP[metric]}\n# TYPE {metric} gauge")
                seen.add(metric)
            lines.append(f"{metric}{{{labels}}} {v:g}")
        lines.append(f'nvidia_gpu_pstate{{{labels}}} {vals["pstate"].lstrip("P") or 0}')
    lines.append("# HELP nvidia_smi_up 1 if nvidia-smi could be queried\n# TYPE nvidia_smi_up gauge")
    lines.append(f"nvidia_smi_up {ok}")
    with _lock:
        _payload = "\n".join(lines) + "\n"
        _ok = ok


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if self.path != "/metrics":
            self.send_response(404)
            self.end_headers()
            return
        with _lock:
            body = _payload.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9835)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    def loop():
        while True:
            sample()
            time.sleep(args.interval)

    threading.Thread(target=loop, daemon=True).start()
    HTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
