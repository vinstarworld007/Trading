#!/usr/bin/env bash
# End-to-end check of the market scenarios. simex runs --scenario all (an
# opening and a closing auction with imbalance messages, a news halt with a
# quotation period, a LULD limit state, pause and reopening, an operational
# halt, an IPO release, a level 1 market-wide circuit breaker, a broken trade
# and a Reg SHO trigger) plus an implied strip ST12=SYM0001+SYM0002. Agents
# trade day orders so the auctions have volume, shock SYM0003 down 12% so it
# reaches its LULD band, and trade the strip. itch-fh rebuilds every book
# from the feed with the same strip; icf-server and rt-server publish the
# halt reasons, imbalances and LULD bands to a vendor-client each, and the
# firm's trader makes markets through all of it.
#
# Passes when simex's engine books equal its feed books with zero integrity
# errors, every scenario happened at least once, itch-fh ends with simex's
# book digest and zero errors, both vendor clients match the direct feed, and
# the trader's own checks pass (orders refused while halted are expected).
#
#   scripts/e2e-scenarios.sh [BUILD_DIR]
set -euo pipefail

build=${1:-build/release}
md=${MD:-239.1.5.1:31501}
retrans=127.0.0.1:${RETRANS_PORT:-31502}
duration=${DURATION:-20}
symbols=SYM0001,SYM0002,SYM0003,SYM0004
strip=ST12=SYM0001+SYM0002
band_pct=2  # what --scenario all sets
work=$(mktemp -d)
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  [[ -n "${KEEP:-}" ]] && cp -r "$work" "$KEEP" || true
  rm -rf "$work"
}
trap cleanup EXIT

feed=(--md "$md" --retrans "$retrans")
echo "== itch-fh and vendors on $md"
"$build/tools/itch-fh/itch-fh" --group "$md" --retrans "$retrans" --strip "$strip" \
  --idle-seconds 20 --top 0 > "$work/itch-fh.out" 2>&1 &
fh=$!
"$build/vendors/icf/icf-server" "${feed[@]}" --idle-us 20 --luld-band-pct $band_pct \
  --listen 127.0.0.1:17201 > "$work/icf-server.out" 2>&1 &
icf_server=$!
"$build/vendors/rtws/rt-server" "${feed[@]}" --idle-us 20 --luld-band-pct $band_pct \
  --listen 127.0.0.1:17202 > "$work/rt-server.out" 2>&1 &
rt_server=$!
pids+=("$fh" "$icf_server" "$rt_server")
sleep 0.3
"$build/tools/vendor-client/vendor-client" --vendor icf --connect 127.0.0.1:17201 \
  --symbols "$symbols,IPO0001" --idle-us 20 --luld-band-pct $band_pct "${feed[@]}" \
  > "$work/icf-client.out" 2>&1 &
icf_client=$!
"$build/tools/vendor-client/vendor-client" --vendor reuters --connect 127.0.0.1:17202 \
  --symbols "$symbols,IPO0001" --idle-us 20 --luld-band-pct $band_pct "${feed[@]}" \
  > "$work/rt-client.out" 2>&1 &
rt_client=$!
pids+=("$icf_client" "$rt_client")
"$build/firm/trader/trader" --md "$md" --retrans "$retrans" --account FIRM01:lab \
  --symbols "$symbols" --duration "$duration" > "$work/trader.out" 2>&1 &
trader=$!
pids+=("$trader")
sleep 0.3

echo "== simex --scenario all, $strip, ${duration}s"
"$build/exchange/simex/simex" --symbols "$symbols" --md-dest "$md" \
  --md-retrans-port "${retrans##*:}" --duration $((duration + 3)) --scenario all \
  --strip "$strip" > "$work/simex.out" 2>&1 &
simex=$!
pids+=("$simex")
agents_status=0
"$build/exchange/agents/agents" --symbols "$symbols" --duration "$duration" \
  --day-orders 0.4 --shock "SYM0003@$((duration / 4)):-12" --strip "$strip" \
  > "$work/agents.out" 2>&1 || agents_status=$?

fail=0
check() {  # name pid
  local status=0
  wait "$2" || status=$?
  if [[ -f "$work/$1.out" ]]; then echo "== $1"; cat "$work/$1.out"; fi
  [[ $status -eq 0 ]] || { echo "FAIL: $1 exited $status" >&2; fail=1; }
}
check simex "$simex"
check itch-fh "$fh"
check icf-server "$icf_server"
check rt-server "$rt_server"
check trader "$trader"
check icf-client "$icf_client"
check rt-client "$rt_client"
[[ $agents_status -eq 0 ]] || { cat "$work/agents.out"; echo "FAIL: agents exited $agents_status" >&2; fail=1; }
pids=()

want=$(awk '/^Book digest/ {print $3}' "$work/simex.out")
got=$(awk '/^Book digest/ {print $3}' "$work/itch-fh.out")
if [[ -z "$want" || "$got" != "$want" ]]; then
  echo "FAIL: itch-fh book digest '$got' differs from simex's '$want'" >&2
  fail=1
fi
line=$(grep '^Scenarios' "$work/simex.out" || true)
for what in crosses "imbalance messages" halts "LULD limit states" pauses "operational halts" \
  "broken trades" "Reg SHO" "implied strip fills"; do
  n=$(sed -E "s/.* ([0-9]+) $what.*/\1/" <<< "$line")
  if ! [[ "$n" =~ ^[0-9]+$ ]] || [[ "$n" -eq 0 ]]; then
    echo "FAIL: no $what in: $line" >&2
    fail=1
  fi
done
[[ $fail -eq 0 ]] || exit 1
echo "PASS: every scenario ran; simex, itch-fh (digest $got) and both vendors agree; the trader stayed clean"
