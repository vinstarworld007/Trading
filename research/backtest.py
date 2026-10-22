#!/usr/bin/env python3
"""Backtests the market maker on one symbol's top of book and sweeps its
parameters.

    itch-dump DAY.gz --symbol AAPL --tob-csv aapl.csv --quiet
    python3 research/backtest.py aapl.csv --hold-ticks 0 1 2 --skew-ticks 0 1 2

See lab_research/backtest.py for the fill model and its limits.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lab_research import backtest, io  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tob", help="itch-dump --tob-csv output")
    ap.add_argument("--hold-ticks", type=int, nargs="+", default=[0, 1, 2])
    ap.add_argument("--skew-ticks", type=int, nargs="+", default=[0, 1, 2])
    ap.add_argument("--improve-spread-ticks", type=int, nargs="+", default=[2, 3])
    ap.add_argument("--max-position", type=int, default=1_000)
    ap.add_argument("--top", type=int, default=10, help="rows to print")
    a = ap.parse_args(argv)

    tob = io.read_tob(a.tob)
    print(f"{len(tob)} top-of-book updates")
    grid = {"hold_ticks": a.hold_ticks, "skew_ticks": a.skew_ticks,
            "improve_spread_ticks": a.improve_spread_ticks}
    res = backtest.sweep(tob, grid, backtest.Params(max_position=a.max_position))
    print(res.head(a.top).to_string(index=False, float_format="%.2f"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
