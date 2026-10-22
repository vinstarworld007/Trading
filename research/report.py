#!/usr/bin/env python3
"""Post-trade report for one trading session.

    python3 research/report.py --fills fills.csv --market market.csv \
        --dropcopy dropcopy.csv --account FIRM01

Prints positions and PnL, reconciles the firm's fills against the exchange's
drop copy, and prints transaction cost analysis. Exits 1 if the
reconciliation finds a break.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pandas as pd  # noqa: E402

from lab_research import io, pnl, tca  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fills", required=True, help="trader --fills-csv output")
    ap.add_argument("--market", help="trader --market-csv output, for marks and TCA")
    ap.add_argument("--dropcopy", help="dropcopy --csv output, to reconcile against")
    ap.add_argument("--account", default="FIRM01", help="the trader's account in the drop copy")
    a = ap.parse_args(argv)

    pd.set_option("display.width", 120)
    fills = io.read_fills(a.fills)
    market = io.read_market(a.market) if a.market else None
    marks = {}
    if market is not None:
        m = tca.mids(market)
        marks = {s: int(g["mid"].iloc[-1]) for s, g in m.groupby("symbol")}

    print(f"== Positions and PnL ({len(fills)} fills, marked at the last mid)")
    pos = pnl.positions(fills, marks)
    print(pos.to_string(index=False) if not pos.empty else "no fills")
    if not pos.empty:
        print(f"Total PnL ${pos['pnl'].sum():,.2f}")

    ok = True
    if a.dropcopy:
        dc = io.read_dropcopy(a.dropcopy)
        rec = pnl.reconcile(fills, dc, a.account)
        print(f"\n== Reconciliation against the drop copy ({a.account})")
        print(f"{rec.matched} fills match the exchange's record; "
              f"{len(rec.only_firm)} only in the firm, {len(rec.only_exchange)} only at the "
              f"exchange, {len(rec.mismatched)} differ")
        for name, df in (("only in the firm", rec.only_firm),
                         ("only at the exchange", rec.only_exchange),
                         ("differ", rec.mismatched)):
            if not df.empty:
                print(f"-- {name}:\n{df.head(10).to_string(index=False)}")
        ok = rec.clean

    if market is not None and not fills.empty:
        print("\n== Transaction cost analysis (cents per share, share-weighted)")
        print(tca.summary(tca.markouts(fills, market)).to_string(index=False,
                                                                  float_format="%.3f"))
    print("\nReconciliation: " + ("OK" if ok else "BREAKS FOUND"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
