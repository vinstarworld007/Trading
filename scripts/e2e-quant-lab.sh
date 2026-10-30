#!/usr/bin/env bash
# End-to-end check of the quant lab: quant-lab runs four strategies for a
# shortened year on the matching engine, ITCH feed, BookBuilder, risk and OMS,
# then research/quant_lab.py builds the Quant Research Lab page from the run.
# Passes when:
#   - every day's rebuilt books have zero integrity errors and equal the
#     engine's books at the close, and every OMS response is consistent;
#   - each strategy traded, and no position went past its risk limit;
#   - the page builds.
#
#   scripts/e2e-quant-lab.sh [BUILD_DIR]
set -euo pipefail

build=${1:-build/release}
days=${DAYS:-80}  # trend needs 50 days of closes before it trades
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "== quant-lab, $days days"
"$build/tools/quant-lab/quant-lab" --days "$days" --out "$work/run.json"

python3 - "$work/run.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
m = d["meta"]
assert m["integrity_errors"] == 0, m
assert m["book_mismatches"] == 0, m
assert m["oms_inconsistent"] == 0, m
for k in ("mm", "pairs", "trend", "fund"):
    s = d[k]["summary"]
    assert s["fills"] > 0, f"{k} never traded"
    assert len(s["daily"]) == m["days"], k
assert all(abs(f["pos"]) <= 1000 for f in d["mm"]["frames"]), "market maker past its cap"
assert all(abs(r[4]) <= 4000 and abs(r[5]) <= 4000 for r in d["pairs"]["rows"]), "pairs past its cap"
assert all(r[2] <= 650000 for r in d["fund"]["rows"]), "fundamental past its cap"
print("ok: clean books, consistent OMS, all four strategies traded within limits")
PY

python3 research/quant_lab.py --bin "$build/tools/quant-lab/quant-lab" --days "$days" \
  --page "$work/quant-research-lab.html"
echo "== e2e-quant-lab passed"
