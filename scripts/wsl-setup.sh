#!/usr/bin/env bash
# One-time setup of a WSL2 Ubuntu 24.04 distro for the trading lab.
#
#   scripts/wsl-setup.sh              install packages, configure the box, build, run lab-check
#   scripts/wsl-setup.sh --no-build   skip the build
#   HUGEPAGES=2048 scripts/wsl-setup.sh
#
# Safe to re-run. Asks for sudo once. Windows-side settings (.wslconfig) cannot
# be changed from inside WSL; the script prints what to put there.
set -euo pipefail

HUGEPAGES="${HUGEPAGES:-1024}"
DATA_DIR="${DATA_DIR:-$HOME/data/itch}"
BUILD=1
for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
step() { printf '\n==> %s\n' "$*"; }

if ! grep -qi microsoft /proc/version; then
  echo "Note: this does not look like WSL. Continuing, but the boot hook below is WSL-only."
fi
case "$REPO_DIR" in
  /mnt/*) echo "Warning: the repo is on the Windows drive ($REPO_DIR). Clone it under ~ instead;" \
               "builds and data access through /mnt are much slower." ;;
esac

step "Installing packages"
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  build-essential g++-14 clang-18 libclang-rt-18-dev clang-format-18 clang-tidy-18 cmake ninja-build gdb \
  libgtest-dev libbenchmark-dev zlib1g-dev libisal-dev pigz \
  python3 python3-venv python3-pip iproute2 net-tools tcpdump tshark curl ca-certificates

step "Enabling multicast on loopback (lab feeds stay inside the VM)"
sudo ip link set lo multicast on
sudo ip route replace 239.0.0.0/8 dev lo

step "Reserving $HUGEPAGES huge pages"
echo "vm.nr_hugepages = $HUGEPAGES" | sudo tee /etc/sysctl.d/99-trading-lab.conf >/dev/null
sudo sysctl -q -p /etc/sysctl.d/99-trading-lab.conf || \
  echo "Could not reserve huge pages now; they apply after the next WSL restart."

step "Making network settings survive restarts"
# WSL runs [boot] command= as root at distro start. Keep any existing command.
BOOT_CMD="ip link set lo multicast on; ip route replace 239.0.0.0/8 dev lo; sysctl -q -p /etc/sysctl.d/99-trading-lab.conf"
if [ -f /etc/wsl.conf ] && grep -q "99-trading-lab" /etc/wsl.conf; then
  echo "/etc/wsl.conf already has the lab boot command."
elif [ -f /etc/wsl.conf ] && grep -q "^command" /etc/wsl.conf; then
  echo "/etc/wsl.conf already has a [boot] command; add this to it yourself:"
  echo "  $BOOT_CMD"
else
  printf '\n[boot]\ncommand = "%s"\n' "$BOOT_CMD" | sudo tee -a /etc/wsl.conf >/dev/null
  echo "Added a [boot] command to /etc/wsl.conf."
fi

step "Creating data directory $DATA_DIR"
mkdir -p "$DATA_DIR"

if [ "$BUILD" = 1 ]; then
  step "Building (release) and running tests"
  cd "$REPO_DIR"
  cmake --preset release
  cmake --build --preset release
  ctest --preset release
  step "Machine check"
  ./build/release/tools/lab-check/lab-check
fi

step "Windows side (do this once, then run 'wsl --shutdown' in PowerShell)"
echo "Copy scripts/wslconfig.example to %UserProfile%\\.wslconfig and adjust memory= and processors=."
echo "Download a sample day with: scripts/fetch-itch-sample.sh"
