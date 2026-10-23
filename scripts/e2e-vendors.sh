#!/usr/bin/env bash
# End-to-end check of the three market data vendors. simex runs empty books
# with agents making the market; icf-server, rt-server and bpipe-server each
# consolidate simex's MoldUDP64 feed and serve one vendor-client apiece (ICF
# over SoupBinTCP, OMM JSON over WebSocket, B-PIPE). Each client joins the
# same feed directly and checks its vendor's final view of every symbol
# against it. Passes when simex, agents, the servers (zero book integrity
# errors) and the clients all exit 0.
#
#   scripts/e2e-vendors.sh [BUILD_DIR]
set -euo pipefail

build=${1:-build/release}
md=${MD:-239.1.4.1:31401}
retrans=127.0.0.1:${RETRANS_PORT:-31402}
duration=${DURATION:-10}
symbols=SYM0001,SYM0002,SYM0003
work=$(mktemp -d)
pids=()
cleanup() {
  for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  [[ -n "${KEEP:-}" ]] && cp -r "$work" "$KEEP" || true
  rm -rf "$work"
}
trap cleanup EXIT

feed=(--md "$md" --retrans "$retrans")
echo "== vendor servers on $md"
"$build/vendors/icf/icf-server" "${feed[@]}" --listen 127.0.0.1:17101 > "$work/icf-server.out" 2>&1 &
icf_server=$!
"$build/vendors/rtws/rt-server" "${feed[@]}" --listen 127.0.0.1:17102 > "$work/rt-server.out" 2>&1 &
rt_server=$!
"$build/vendors/bpipe/bpipe-server" "${feed[@]}" --listen 127.0.0.1:17103 \
  > "$work/bpipe-server.out" 2>&1 &
bpipe_server=$!
pids+=("$icf_server" "$rt_server" "$bpipe_server")
sleep 0.3

echo "== clients: ICF tick by tick, Reuters tick by tick, Bloomberg conflated to 50 ms"
"$build/tools/vendor-client/vendor-client" --vendor icf --connect 127.0.0.1:17101 \
  --symbols "$symbols" --idle-us 20 "${feed[@]}" > "$work/icf-client.out" 2>&1 &
icf_client=$!
"$build/tools/vendor-client/vendor-client" --vendor reuters --connect 127.0.0.1:17102 \
  --symbols "$symbols" --idle-us 20 "${feed[@]}" > "$work/rt-client.out" 2>&1 &
rt_client=$!
"$build/tools/vendor-client/vendor-client" --vendor bloomberg --connect 127.0.0.1:17103 \
  --symbols "$symbols" --interval-ms 50 "${feed[@]}" > "$work/bpipe-client.out" 2>&1 &
bpipe_client=$!
pids+=("$icf_client" "$rt_client" "$bpipe_client")
sleep 0.3

echo "== simex with agents on $symbols for ${duration}s"
"$build/exchange/simex/simex" --symbols "$symbols" --md-dest "$md" \
  --md-retrans-port "${retrans##*:}" --duration $((duration + 3)) > "$work/simex.out" 2>&1 &
simex=$!
pids+=("$simex")
agents_status=0
"$build/exchange/agents/agents" --symbols "$symbols" --duration "$duration" \
  > "$work/agents.out" 2>&1 || agents_status=$?

fail=0
check() {  # name pid
  local status=0
  wait "$2" || status=$?
  if [[ -f "$work/$1.out" ]]; then echo "== $1"; cat "$work/$1.out"; fi
  [[ $status -eq 0 ]] || { echo "FAIL: $1 exited $status" >&2; fail=1; }
}
check simex "$simex"
for name in icf-server rt-server bpipe-server; do
  case $name in
    icf-server) pid=$icf_server ;; rt-server) pid=$rt_server ;; bpipe-server) pid=$bpipe_server ;;
  esac
  check "$name" "$pid"
done
check icf-client "$icf_client"
check rt-client "$rt_client"
check bpipe-client "$bpipe_client"
[[ $agents_status -eq 0 ]] || { cat "$work/agents.out"; echo "FAIL: agents exited $agents_status" >&2; fail=1; }
pids=()
[[ $fail -eq 0 ]] || exit 1
echo "PASS: ICF, Reuters and Bloomberg clients each match the direct feed; all checks clean"
