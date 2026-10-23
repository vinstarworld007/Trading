#!/usr/bin/env bash
# End-to-end check of the live feed: rebuilds a day's books from the file with
# itch-dump, then again over UDP with itch-replay -> itch-fh while the replay
# drops, duplicates and reorders packets. Passes when itch-fh loses nothing,
# reports zero integrity errors, and ends with the same book digest.
#
#   scripts/e2e-live-feed.sh [BUILD_DIR] [DAY_FILE]
#
# Without DAY_FILE a synthetic day is generated. Uses unicast on 127.0.0.1 so
# it runs anywhere; set GROUP=239.1.1.1:30001 to test loopback multicast.
set -euo pipefail

build=${1:-build/release}
day=${2:-}
group=${GROUP:-127.0.0.1:30001}
retrans_port=${RETRANS_PORT:-30002}
faults=${FAULTS:---drop 0.005 --burst 0.0005 --burst-len 20 --dup 0.005 --reorder 0.005 --seed 42}
rate=${RATE:-500000}
tools=$build/tools
work=$(mktemp -d)
trap 'rm -rf "$work"; [[ -n "${fh_pid:-}" ]] && kill "$fh_pid" 2>/dev/null || true' EXIT

if [[ -z "$day" ]]; then
  day=$work/synth.itch50.gz
  "$tools/itch-gen/itch-gen" "$day" --symbols 100 --messages 1000000 > /dev/null
fi

echo "== itch-dump (file)"
"$tools/itch-dump/itch-dump" "$day" --quiet --top 0 > "$work/dump.out"
want=$(awk '/^Book digest/ {print $3}' "$work/dump.out")
echo "book digest $want"

echo "== itch-fh <- itch-replay $faults"
"$tools/itch-fh/itch-fh" --group "$group" --retrans "127.0.0.1:$retrans_port" --idle-seconds 20 \
  --top 0 > "$work/fh.out" 2>&1 &
fh_pid=$!
sleep 0.5
# shellcheck disable=SC2086  # $faults is a list of flags
"$tools/itch-replay/itch-replay" "$day" --dest "$group" --retrans-port "$retrans_port" \
  --rate "$rate" --eos-seconds 2 $faults
fh_status=0
wait "$fh_pid" || fh_status=$?
fh_pid=
cat "$work/fh.out"
got=$(awk '/^Book digest/ {print $3}' "$work/fh.out")

if [[ $fh_status -ne 0 ]]; then
  echo "FAIL: itch-fh exited $fh_status" >&2
  exit 1
fi
if [[ "$got" != "$want" ]]; then
  echo "FAIL: live book digest $got differs from file digest $want" >&2
  exit 1
fi
echo "PASS: live feed rebuilt identical books (digest $got)"
