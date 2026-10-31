#!/usr/bin/env bash
# Installs everything needed to build and run the platform on Ubuntu 24.04+ (tested on 26.04).
set -euo pipefail

echo "==> apt packages (build tools, GStreamer, Boost, SQLite, JSON, spdlog)"
sudo apt-get update -q
sudo apt-get install -y -q \
  build-essential cmake ninja-build pkg-config git curl \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-tools gstreamer1.0-vaapi \
  libboost-dev libsqlite3-dev nlohmann-json3-dev libspdlog-dev

if [[ "${VSP_WITH_DOCKER:-1}" == "1" ]]; then
  echo "==> Docker (for the optional observability stack)"
  sudo apt-get install -y -q docker.io docker-compose-v2
  sudo systemctl enable --now docker
  sudo usermod -aG docker "$USER" && echo "   (log out/in for docker group membership; scripts fall back to sudo meanwhile)"
fi

if [[ "${1:-}" != "--no-node" ]]; then
  if ! command -v node >/dev/null 2>&1; then
    echo "==> Node.js LTS via nvm"
    export NVM_DIR="$HOME/.nvm"
    if [ ! -s "$NVM_DIR/nvm.sh" ]; then
      curl -fsSL https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.3/install.sh | bash
    fi
    # shellcheck disable=SC1091
    . "$NVM_DIR/nvm.sh"
    nvm install --lts
  fi
  echo "node $(node --version), npm $(npm --version)"
fi

echo
echo "GStreamer $(gst-launch-1.0 --version | head -1 | awk '{print $NF}')"
echo "Encoders available:"
for p in nvh264enc vah264enc vaapih264enc qsvh264enc x264enc; do
  printf "  %-14s %s\n" "$p" "$(gst-inspect-1.0 "$p" >/dev/null 2>&1 && echo yes || echo no)"
done
echo
echo "Done. Next: ./scripts/build-backend.sh && (cd desktop && npm install && npm run dev)"
