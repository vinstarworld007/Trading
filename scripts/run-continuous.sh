#!/usr/bin/env bash
# Runs simex + itch-fh + trader continuously all day, looping instantly
# when each session ends. No rate limiting — full speed replay.
set -euo pipefail

BUILD=${1:-build/release}
DAY=${2:-/home/vinay/data/itch/synthetic.bin}
MD=127.0.0.1:31001
RETRANS=127.0.0.1:31002
LISTEN=127.0.0.1:15000
SYMBOLS=SYM0001,SYM0002,SYM0003,SYM0004,SYM0005
SESSION=0

cleanup() {
  echo "[run-continuous] shutting down..."
  kill "${simex_pid:-}" "${fh_pid:-}" "${trader_pid:-}" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[run-continuous] starting — full-speed loop, Ctrl-C to stop"

while true; do
  SESSION=$((SESSION + 1))
  echo "" >> /tmp/simex.out
  echo "========== SESSION $SESSION $(date '+%H:%M:%S') ==========" >> /tmp/simex.out
  echo "========== SESSION $SESSION $(date '+%H:%M:%S') ==========" >> /tmp/itch-fh.out
  echo "========== SESSION $SESSION $(date '+%H:%M:%S') ==========" >> /tmp/trader.out

  # Start feed handler first so it's ready before simex sends anything
  "$BUILD/tools/itch-fh/itch-fh" \
    --group "$MD" \
    --retrans "$RETRANS" \
    --idle-seconds 10 \
    >> /tmp/itch-fh.out 2>&1 &
  fh_pid=$!

  # Small pause so itch-fh is bound before simex starts publishing
  sleep 0.2

  # Start simex — speed 5 = 5× real-time (~30 s per synthetic day)
  "$BUILD/exchange/simex/simex" \
    --replay "$DAY" \
    --speed 5 \
    --md-dest "$MD" \
    --md-retrans-port 31002 \
    --listen "$LISTEN" \
    --linger 3 \
    --eos-seconds 2 \
    >> /tmp/simex.out 2>&1 &
  simex_pid=$!

  # Give simex a moment to open its ports before trader connects
  sleep 0.3

  # Start trader — exits when session ends, which is fine
  "$BUILD/firm/trader/trader" \
    --account FIRM01:lab \
    --connect "$LISTEN" \
    --md "$MD" \
    --retrans "$RETRANS" \
    --symbols "$SYMBOLS" \
    --duration 86400 \
    >> /tmp/trader.out 2>&1 &
  trader_pid=$!

  # Wait for simex to finish the day
  wait "$simex_pid" || true
  simex_pid=

  # Let itch-fh drain the last packets, then kill it
  sleep 1
  kill "$fh_pid" "$trader_pid" 2>/dev/null || true
  wait "$fh_pid" "$trader_pid" 2>/dev/null || true
  fh_pid=
  trader_pid=

  echo "[run-continuous] session $SESSION done, restarting..."
  sleep 0.5
done
