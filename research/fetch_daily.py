#!/usr/bin/env python3
"""Downloads daily bars from Nasdaq's historical quotes for quant-real.

    python3 research/fetch_daily.py --out ~/data/daily --from 2018-06-01 --to 2019-12-31 \
        QQQ:etf AAPL:stocks

Writes OUT/SYM.csv as `date,open,high,low,close,volume`, oldest first, the
format tools/quant-real reads. Prices are kept as the decimal text Nasdaq
publishes; nothing here goes through floating point.

Nasdaq reports prices adjusted for later splits. `--unsplit AAPL:4:2020-08-31`
multiplies prices before that date by 4 (and divides volume by 4), giving
the prices that traded at the time, which is what the ITCH day shows. Data
files are never committed.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from decimal import Decimal
from pathlib import Path

API = "https://api.nasdaq.com/api/quote/{sym}/historical?assetclass={cls}&fromdate={start}&todate={end}&limit=9999"
HEADERS = {
    "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) trading-lab/1.0",
    "Accept": "application/json, text/plain, */*",
}


def money(text: str) -> str:
    """'$1,165.25' -> '1165.25'."""
    return text.replace("$", "").replace(",", "").strip()


def iso(us_date: str) -> str:
    """'01/30/2019' -> '2019-01-30'."""
    m, d, y = us_date.split("/")
    return f"{y}-{int(m):02d}-{int(d):02d}"


def parse_nasdaq(payload: dict) -> list[dict]:
    """Rows of Nasdaq's historical quotes JSON, oldest first."""
    rows = ((payload.get("data") or {}).get("tradesTable") or {}).get("rows") or []
    out = []
    for r in rows:
        out.append({
            "date": iso(r["date"]),
            "open": money(r["open"]),
            "high": money(r["high"]),
            "low": money(r["low"]),
            "close": money(r["close"]),
            "volume": money(r["volume"]).split(".")[0],
        })
    return sorted(out, key=lambda r: r["date"])


def unsplit(rows: list[dict], factor: int, before: str) -> list[dict]:
    """Undo a later split for bars dated before `before`. Exact decimal arithmetic."""
    out = []
    for r in rows:
        if r["date"] < before:
            r = dict(r)
            for k in ("open", "high", "low", "close"):
                r[k] = f"{(Decimal(r[k]) * factor).quantize(Decimal('0.0001')).normalize():f}"
            r["volume"] = str(int(r["volume"]) // factor)
        out.append(r)
    return out


def to_csv(rows: list[dict]) -> str:
    lines = ["date,open,high,low,close,volume"]
    lines += [f"{r['date']},{r['open']},{r['high']},{r['low']},{r['close']},{r['volume']}" for r in rows]
    return "\n".join(lines) + "\n"


def fetch(sym: str, cls: str, start: str, end: str) -> list[dict]:
    req = urllib.request.Request(API.format(sym=sym, cls=cls, start=start, end=end), headers=HEADERS)
    with urllib.request.urlopen(req, timeout=60) as resp:
        return parse_nasdaq(json.load(resp))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("symbols", nargs="+", help="SYM:stocks or SYM:etf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--from", dest="start", default="2018-06-01")
    ap.add_argument("--to", dest="end", default="2019-12-31")
    ap.add_argument("--unsplit", action="append", default=[], help="SYM:FACTOR:FIRST_DATE_AFTER")
    a = ap.parse_args(argv)
    splits = {}
    for s in a.unsplit:
        sym, factor, before = s.split(":")
        splits[sym] = (int(factor), before)
    out = Path(a.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    for spec in a.symbols:
        sym, _, cls = spec.partition(":")
        rows = fetch(sym, cls or "stocks", a.start, a.end)
        if not rows:
            print(f"{sym}: no rows", file=sys.stderr)
            return 1
        if sym in splits:
            rows = unsplit(rows, *splits[sym])
        (out / f"{sym}.csv").write_text(to_csv(rows))
        print(f"{sym}: {len(rows)} bars {rows[0]['date']} to {rows[-1]['date']}, "
              f"last close {rows[-1]['close']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
