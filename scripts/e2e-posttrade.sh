#!/usr/bin/env bash
# End-to-end check of post-trade: the same session as e2e-firm.sh (trader
# making markets on simex against noise flow), plus dropcopy recording every
# execution from simex's drop copy session. Then research/report.py
# reconciles the trader's fills against the drop copy and prints positions,
# PnL and TCA, and research/backtest.py sweeps the strategy on one symbol's
# top of book from the same day, and research/colo.py places the trader's
# measured latency on the colo network design(s) in COLO_OPTION (C, D, G, a
# list such as C,G, or all; default C), writing colo.html. Passes when every
# program exits 0 and the
# reconciliation finds no breaks.
#
#   scripts/e2e-posttrade.sh [BUILD_DIR] [OUT_DIR]
#
# OUT_DIR keeps the CSVs and logs (default: a temporary directory).
set -euo pipefail

build=${1:-build/release}
out=${2:-}
md=${MD:-127.0.0.1:31001}
retrans_port=${RETRANS_PORT:-31002}
rate=${RATE:-100000}
orders=${ORDERS:-10000}
order_rate=${ORDER_RATE:-2000}
duration=${DURATION:-8}
symbols=SYM0001,SYM0002,SYM0003,SYM0004,SYM0005
tools=$build/tools
work=${out:-$(mktemp -d)}
mkdir -p "$work"
cleanup() {
  for pid in ${trader_pid:-} ${simex_pid:-} ${dc_pid:-}; do kill "$pid" 2>/dev/null || true; done
  [[ -n "$out" ]] || rm -rf "$work"
}
trap cleanup EXIT

day=$work/synth.itch50.gz
"$tools/itch-gen/itch-gen" "$day" --symbols 500 --messages 1000000 > /dev/null

echo "== session: trader on $symbols, noise flow, drop copy"
"$build/firm/trader/trader" --md "$md" --retrans "127.0.0.1:$retrans_port" \
  --account FIRM01:lab --symbols "$symbols" --duration "$duration" \
  --fills-csv "$work/fills.csv" --market-csv "$work/market.csv" > "$work/trader.out" 2>&1 &
trader_pid=$!
sleep 0.3
"$build/exchange/simex/simex" --replay "$day" --rate "$rate" --md-dest "$md" \
  --md-retrans-port "$retrans_port" --linger 2 > "$work/simex.out" 2>&1 &
simex_pid=$!
"$tools/dropcopy/dropcopy" --account DROP01:lab --csv "$work/dropcopy.csv" \
  > "$work/dropcopy.out" 2>&1 &
dc_pid=$!

status=0
"$tools/ouch-client/ouch-client" --accounts FIRM02:lab,FIRM03:lab --reference "$day" \
  --symbols "$symbols" --orders "$orders" --rate "$order_rate" --max-lots "${MAX_LOTS:-20}" \
  --ioc 0.5 --wait-eos > "$work/client.out" 2>&1 || { echo "FAIL: ouch-client" >&2; status=1; }
wait "$trader_pid" || { echo "FAIL: trader" >&2; status=1; }
trader_pid=
wait "$simex_pid" || { echo "FAIL: simex" >&2; status=1; }
simex_pid=
wait "$dc_pid" || { echo "FAIL: dropcopy" >&2; status=1; }
dc_pid=
cat "$work/dropcopy.out"
grep -E '^(Orders|Trader checks)' "$work/trader.out"
[[ $status -eq 0 ]] || { cat "$work/trader.out" "$work/simex.out"; exit 1; }

echo "== post-trade report"
"${PYTHON:-python3}" research/report.py --fills "$work/fills.csv" --market "$work/market.csv" \
  --dropcopy "$work/dropcopy.csv" --account FIRM01

echo "== backtest on SYM0001's top of book"
"$tools/itch-dump/itch-dump" "$day" --symbol SYM0001 --tob-csv "$work/tob.csv" --quiet \
  --top 0 > /dev/null
"${PYTHON:-python3}" research/backtest.py "$work/tob.csv" --top 5
echo "== colo model, option ${COLO_OPTION:-C}, with this run's latency"
"${PYTHON:-python3}" research/colo.py --option "${COLO_OPTION:-C}" --trader-log "$work/trader.out" \
  --duration-ms 5 --no-sweep \
  --page "$work/colo.html"
echo "PASS: drop copy reconciles with the trader's fills"
