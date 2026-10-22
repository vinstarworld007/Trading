#!/usr/bin/env bash
# End-to-end check of the firm's trading engine: simex replays a day as
# historical liquidity, ouch-client sends noise orders (some large enough to
# sweep a level) on two accounts, and trader makes markets in five symbols on
# a third, from simex's market data feed. Passes when:
#   - trader's checks pass: no market data lost, clean books, every OUCH
#     response consistent with what it sent, no position past its risk limit,
#     and every order canceled when it stops;
#   - ouch-client's protocol checks pass;
#   - simex's engine books equal the books rebuilt from its own feed.
#
#   scripts/e2e-firm.sh [BUILD_DIR]
#
# The synthetic day spreads 1M messages over 500 symbols, so books are thin
# enough for the noise flow to reach the trader's quotes.
set -euo pipefail

build=${1:-build/release}
md=${MD:-127.0.0.1:31001}
retrans_port=${RETRANS_PORT:-31002}
rate=${RATE:-100000}
orders=${ORDERS:-10000}
order_rate=${ORDER_RATE:-2000}
duration=${DURATION:-8}
symbols=SYM0001,SYM0002,SYM0003,SYM0004,SYM0005
tools=$build/tools
work=$(mktemp -d)
cleanup() {
  for pid in ${trader_pid:-} ${simex_pid:-}; do kill "$pid" 2>/dev/null || true; done
  [[ -n "${KEEP:-}" ]] && cp -r "$work" "$KEEP" || true
  rm -rf "$work"
}
trap cleanup EXIT

day=$work/synth.itch50.gz
"$tools/itch-gen/itch-gen" "$day" --symbols 500 --messages 1000000 > /dev/null

echo "== trader on $symbols for ${duration}s, simex replaying at $rate msgs/s, $orders noise orders"
"$build/firm/trader/trader" --md "$md" --retrans "127.0.0.1:$retrans_port" \
  --account FIRM01:lab --symbols "$symbols" --duration "$duration" > "$work/trader.out" 2>&1 &
trader_pid=$!
sleep 0.3
"$build/exchange/simex/simex" --replay "$day" --rate "$rate" --md-dest "$md" \
  --md-retrans-port "$retrans_port" --linger 2 > "$work/simex.out" 2>&1 &
simex_pid=$!

client_status=0
"$tools/ouch-client/ouch-client" --accounts FIRM02:lab,FIRM03:lab --reference "$day" \
  --reference-symbols 5 --orders "$orders" --rate "$order_rate" --max-lots "${MAX_LOTS:-20}" --ioc 0.5 \
  --wait-eos > "$work/client.out" 2>&1 || client_status=$?
trader_status=0
wait "$trader_pid" || trader_status=$?
trader_pid=
simex_status=0
wait "$simex_pid" || simex_status=$?
simex_pid=

echo "== trader"
cat "$work/trader.out"
echo "== ouch-client"
cat "$work/client.out"
echo "== simex"
cat "$work/simex.out"

fail=0
[[ $trader_status -eq 0 ]] || { echo "FAIL: trader exited $trader_status" >&2; fail=1; }
[[ $client_status -eq 0 ]] || { echo "FAIL: ouch-client exited $client_status" >&2; fail=1; }
[[ $simex_status -eq 0 ]] || { echo "FAIL: simex exited $simex_status" >&2; fail=1; }
[[ $fail -eq 0 ]] || exit 1
fills=$(awk '/^Orders/ {for (i = 1; i < NF; i++) if ($(i + 1) ~ /^executions/) print $i}' \
  "$work/trader.out")
echo "PASS: trader made markets with ${fills:-0} fills; all checks clean"
