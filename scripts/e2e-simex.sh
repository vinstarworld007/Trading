#!/usr/bin/env bash
# End-to-end check of the exchange: simex replays a day as historical
# liquidity while ouch-client trades into it over OUCH/SoupBinTCP on three
# accounts, and itch-fh rebuilds books from simex's market data feed. Passes
# when:
#   - ouch-client's protocol checks pass (every request answered, fills inside
#     limits, sequence numbers in order, a re-login replays byte for byte);
#   - simex's engine books equal the books rebuilt from its own feed, with
#     zero integrity errors;
#   - itch-fh loses nothing and ends with the same book digest as simex.
#
#   scripts/e2e-simex.sh [BUILD_DIR] [DAY_FILE]
#
# Without DAY_FILE a synthetic day is generated.
set -euo pipefail

build=${1:-build/release}
day=${2:-}
md=${MD:-127.0.0.1:31001}
retrans_port=${RETRANS_PORT:-31002}
rate=${RATE:-100000}
orders=${ORDERS:-20000}
order_rate=${ORDER_RATE:-10000}
tools=$build/tools
work=$(mktemp -d)
cleanup() {
  for pid in ${fh_pid:-} ${simex_pid:-}; do kill "$pid" 2>/dev/null || true; done
  rm -rf "$work"
}
trap cleanup EXIT

if [[ -z "$day" ]]; then
  day=$work/synth.itch50.gz
  "$tools/itch-gen/itch-gen" "$day" --symbols 100 --messages 1000000 > /dev/null
fi

echo "== itch-fh on $md, simex replaying $day at $rate msgs/s, ouch-client $orders orders"
"$tools/itch-fh/itch-fh" --group "$md" --retrans "127.0.0.1:$retrans_port" --idle-seconds 30 \
  --top 0 > "$work/fh.out" 2>&1 &
fh_pid=$!
sleep 0.3
"$build/exchange/simex/simex" --replay "$day" --rate "$rate" --md-dest "$md" \
  --md-retrans-port "$retrans_port" --journal "$work/journal.bin" --linger 2 \
  > "$work/simex.out" 2>&1 &
simex_pid=$!

client_status=0
"$tools/ouch-client/ouch-client" --accounts FIRM01:lab,FIRM02:lab,FIRM03:lab \
  --reference "$day" --orders "$orders" --rate "$order_rate" --replay-check --wait-eos \
  > "$work/client.out" 2>&1 || client_status=$?
simex_status=0
wait "$simex_pid" || simex_status=$?
simex_pid=
fh_status=0
wait "$fh_pid" || fh_status=$?
fh_pid=

echo "== ouch-client"
cat "$work/client.out"
echo "== simex"
cat "$work/simex.out"
echo "== itch-fh"
cat "$work/fh.out"

want=$(awk '/^Book digest/ {print $3}' "$work/simex.out")
got=$(awk '/^Book digest/ {print $3}' "$work/fh.out")
fail=0
[[ $client_status -eq 0 ]] || { echo "FAIL: ouch-client exited $client_status" >&2; fail=1; }
[[ $simex_status -eq 0 ]] || { echo "FAIL: simex exited $simex_status" >&2; fail=1; }
[[ $fh_status -eq 0 ]] || { echo "FAIL: itch-fh exited $fh_status" >&2; fail=1; }
if [[ -z "$want" || "$got" != "$want" ]]; then
  echo "FAIL: feed handler digest '$got' differs from simex digest '$want'" >&2
  fail=1
fi
[[ $fail -eq 0 ]] || exit 1
echo "PASS: firm orders traded against replayed liquidity; feed books match the engine (digest $got)"
