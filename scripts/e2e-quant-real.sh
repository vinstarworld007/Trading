#!/usr/bin/env bash
# End-to-end check of quant-real, the real-data backtest, without the multi-GB
# Nasdaq file: itch-gen writes a synthetic ITCH day and a random walk stands in
# for Nasdaq's daily bars. Passes when:
#   - the replayed books have zero integrity errors and every OMS response is
#     consistent;
#   - market making, trend and fundamental traded, within their risk limits;
#   - research/quant_lab.py --real builds the page.
# scripts/run-quant-real.sh does the same with real data.
#
#   scripts/e2e-quant-real.sh [BUILD_DIR]
set -euo pipefail

build=${1:-build/release}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$build/tools/itch-gen/itch-gen" "$work/day.itch50.gz" --symbols 20 --messages 2000000 --seed 5 \
  >/dev/null
mkdir "$work/daily"
python3 - "$work/daily" <<'PY'
import datetime, random, sys
for sym, px in (("TREND", 150.0), ("SYM0001", 120.0)):
    rng = random.Random(sym)
    day, rows = datetime.date(2018, 6, 1), []
    while day <= datetime.date(2019, 12, 31):
        if day.weekday() < 5:
            o = px
            px *= 1 + 0.0007 + rng.gauss(0, 0.012)
            rows.append(f"{day},{o:.2f},{max(o, px) * 1.004:.2f},{min(o, px) * 0.996:.2f},"
                        f"{px:.2f},{rng.randint(20_000_000, 40_000_000)}")
        day += datetime.timedelta(days=1)
    open(f"{sys.argv[1]}/{sym}.csv", "w").write("date,open,high,low,close,volume\n" + "\n".join(rows) + "\n")
PY

echo "== quant-real on a synthetic day"
"$build/tools/quant-real/quant-real" --itch "$work/day.itch50.gz" --itch-date 2019-01-30 \
  --daily "$work/daily" --mm SYM0001 --pair SYM0002,SYM0003 --trend TREND --fund SYM0001 \
  --fund-eps 10 --fund-pe 15 --out "$work/run.json"

python3 - "$work/run.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
m = d["meta"]
assert m["mode"] == "real" and m["integrity_errors"] == 0 and m["oms_inconsistent"] == 0, m
for k in ("mm", "trend", "fund"):
    assert d[k]["summary"]["fills"] > 0, f"{k} never traded"
assert len(d["pairs"]["rows"]) == 390 and len(d["mm"]["summary"]["daily"]) == 390
assert len(d["trend"]["rows"]) == len(d["trend"]["dates"]) == m["days"] > 200
assert d["mm"]["frames"] and all(abs(f["pos"]) <= 1000 for f in d["mm"]["frames"]), "mm past its cap"
assert all(abs(r[4]) <= 4000 and abs(r[5]) <= 4000 for r in d["pairs"]["rows"]), "pairs past its cap"
assert all(r[2] <= 650000 for r in d["fund"]["rows"]), "fundamental past its cap"
assert d["fund"]["catalyst_day"] > 0 and len(d["fund"]["rows"][d["fund"]["catalyst_day"] - 1][6]) == 13
print("ok: clean books, consistent OMS, strategies traded within limits")
PY

python3 research/quant_lab.py --real "$work/run.json" --page "$work/quant-real.html"
echo "== e2e-quant-real passed"
