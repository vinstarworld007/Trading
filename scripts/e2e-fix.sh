#!/usr/bin/env bash
# End-to-end check of the FIX venue: simex with empty books, fixgw in front of
# it, and fix-client trading two FIX sessions against each other. Midway the
# first session drops its connection without a Logout and logs in again
# without resetting sequence numbers, so fixgw cancels its orders and must
# resend what it sent meanwhile. dropcopy records every execution from simex.
# Passes when every program exits 0, fix-client's checks hold, fixgw resent
# messages after the reconnect, and the drop copy has one Execution Report
# per FIX fill.
#
#   scripts/e2e-fix.sh [BUILD_DIR]
set -euo pipefail

build=${1:-build/release}
orders=${ORDERS:-4000}
rate=${ORDER_RATE:-1000}
reconnect_at=${RECONNECT_AT:-$((orders / 3))}
duration=${DURATION:-30}
simex_port=${SIMEX_PORT:-15000}
fix_port=${FIX_PORT:-16000}
tools=$build/tools
work=$(mktemp -d)
cleanup() {
  for pid in ${simex_pid:-} ${dc_pid:-} ${gw_pid:-}; do kill "$pid" 2>/dev/null || true; done
  rm -rf "$work"
}
trap cleanup EXIT

echo "== simex, fixgw, dropcopy; fix-client sends $orders orders at $rate/s"
"$build/exchange/simex/simex" --symbols AAPL,MSFT --accounts OUCHA:pa,OUCHB:pb \
  --listen "127.0.0.1:$simex_port" --duration "$duration" --md-dest 127.0.0.1:31201 \
  --md-retrans-port 31202 > "$work/simex.out" 2>&1 &
simex_pid=$!
sleep 0.3
"$tools/dropcopy/dropcopy" --connect "127.0.0.1:$simex_port" --account DROP01:lab \
  > "$work/dropcopy.out" 2>&1 &
dc_pid=$!
"$build/exchange/fixgw/fixgw" --listen "127.0.0.1:$fix_port" --simex "127.0.0.1:$simex_port" \
  --session FIXA:OUCHA:pa --session FIXB:OUCHB:pb > "$work/fixgw.out" 2>&1 &
gw_pid=$!

status=0
"$tools/fix-client/fix-client" --connect "127.0.0.1:$fix_port" --sessions FIXA,FIXB \
  --symbols AAPL,MSFT --orders "$orders" --rate "$rate" --reconnect-at "$reconnect_at" \
  > "$work/client.out" 2>&1 || { echo "FAIL: fix-client" >&2; status=1; }
wait "$simex_pid" || { echo "FAIL: simex" >&2; status=1; }
simex_pid=
wait "$gw_pid" || { echo "FAIL: fixgw" >&2; status=1; }
gw_pid=
wait "$dc_pid" || { echo "FAIL: dropcopy" >&2; status=1; }
dc_pid=
cat "$work/client.out" "$work/fixgw.out"
grep -E 'Execution Reports' "$work/dropcopy.out"
if [[ $status -ne 0 ]]; then
  cat "$work/simex.out" "$work/dropcopy.out"
  exit 1
fi

fills=$(awk '/^fills /{print $2}' "$work/client.out")
reports=$(awk '/Execution Reports/{print $3}' "$work/dropcopy.out")
resent=$(awk '$1=="FIXA"{print $10}' "$work/fixgw.out")
[[ -n "$fills" && "$fills" -gt 0 ]] || { echo "FAIL: no fills" >&2; exit 1; }
[[ "$fills" == "$reports" ]] || {
  echo "FAIL: $fills FIX fills but $reports drop copy reports" >&2
  exit 1
}
[[ -n "$resent" && "$resent" -gt 0 ]] || { echo "FAIL: nothing resent after reconnect" >&2; exit 1; }
echo "PASS: $fills FIX fills match the drop copy; $resent messages resent after the reconnect"
