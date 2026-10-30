#!/usr/bin/env bash
# Backtests the Quant Research Lab strategies on real Nasdaq data and builds the
# real-data page. Needs internet access to emi.nasdaq.com and api.nasdaq.com.
#
#   scripts/run-quant-real.sh [BUILD_DIR] [OUT_DIR]
#
# 1. scripts/fetch-itch-sample.sh: Nasdaq's TotalView-ITCH sample for
#    Jan 30, 2019 (4.8 GB, into ~/data/itch; skipped if already there).
# 2. research/fetch_daily.py: daily bars for QQQ and AAPL, Jun 2018 to Dec 2019,
#    with Apple's Aug 2020 4:1 split undone so prices match the ITCH day.
# 3. quant-real, then research/quant_lab.py --real.
# Data stays in ~/data; nothing here is committed.
set -euo pipefail

build=${1:-build/release}
out=${2:-$HOME/data/quant-real}
itch_dir=${DATA_DIR:-$HOME/data/itch}
day=${ITCH_FILE:-01302019.NASDAQ_ITCH50.gz}
mkdir -p "$out"

if [ ! -s "$itch_dir/$day" ]; then
  scripts/fetch-itch-sample.sh "$day"
fi
python3 research/fetch_daily.py --out "$HOME/data/daily" --from 2018-06-01 --to 2019-12-31 \
  --unsplit AAPL:4:2020-08-31 QQQ:etf AAPL:stocks

"$build/tools/quant-real/quant-real" --itch "$itch_dir/$day" --daily "$HOME/data/daily" \
  --out "$out/quant-real.json" "${@:3}"
python3 research/quant_lab.py --real "$out/quant-real.json" --page "$out/quant-research-lab-real.html"
echo "page: $out/quant-research-lab-real.html"
