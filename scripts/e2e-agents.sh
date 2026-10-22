#!/usr/bin/env bash
# End-to-end check of agent-based liquidity: simex starts with empty books and
# no replay; agents (makers, noise, value and momentum traders) make the
# market; the firm's trader makes markets in the same symbols from simex's
# feed. Passes when agents, trader and simex all exit 0 (agents' and trader's
# own checks, and simex's engine books equal to its feed's books), and the
# trader got fills in a market nobody replayed.
#
#   scripts/e2e-agents.sh [BUILD_DIR]
set -euo pipefail

build=${1:-build/release}
md=${MD:-127.0.0.1:31301}
retrans_port=${RETRANS_PORT:-31302}
duration=${DURATION:-12}
symbols=SYM0001,SYM0002,SYM0003
work=$(mktemp -d)
cleanup() {
  for pid in ${trader_pid:-} ${simex_pid:-}; do kill "$pid" 2>/dev/null || true; done
  [[ -n "${KEEP:-}" ]] && cp -r "$work" "$KEEP" || true
  rm -rf "$work"
}
trap cleanup EXIT

echo "== simex with empty books, agents and trader on $symbols for ${duration}s"
"$build/firm/trader/trader" --md "$md" --retrans "127.0.0.1:$retrans_port" \
  --account FIRM01:lab --symbols "$symbols" --duration "$duration" > "$work/trader.out" 2>&1 &
trader_pid=$!
sleep 0.3
"$build/exchange/simex/simex" --symbols "$symbols" --md-dest "$md" \
  --md-retrans-port "$retrans_port" --duration $((duration + 4)) > "$work/simex.out" 2>&1 &
simex_pid=$!

agents_status=0
"$build/exchange/agents/agents" --symbols "$symbols" --duration "$duration" \
  > "$work/agents.out" 2>&1 || agents_status=$?
trader_status=0
wait "$trader_pid" || trader_status=$?
trader_pid=
simex_status=0
wait "$simex_pid" || simex_status=$?
simex_pid=

echo "== agents"
cat "$work/agents.out"
echo "== trader"
cat "$work/trader.out"

fail=0
[[ $agents_status -eq 0 ]] || { echo "FAIL: agents exited $agents_status" >&2; fail=1; }
[[ $trader_status -eq 0 ]] || { echo "FAIL: trader exited $trader_status" >&2; fail=1; }
[[ $simex_status -eq 0 ]] || { echo "FAIL: simex exited $simex_status" >&2; fail=1; }
[[ $fail -eq 0 ]] || { cat "$work/simex.out"; exit 1; }
fills=$(awk '/^Orders/ {for (i = 1; i < NF; i++) if ($(i + 1) ~ /^executions/) print $i}' \
  "$work/trader.out")
[[ -n "$fills" && "$fills" -gt 0 ]] || { echo "FAIL: the trader got no fills" >&2; exit 1; }
echo "PASS: the trader made markets among the agents with $fills fills; all checks clean"
