#!/usr/bin/env python3
"""Runs the quant lab and builds the Quant Research Lab page.

    python3 research/quant_lab.py --bin build/release/tools/quant-lab/quant-lab \
        --seed 3 --seeds 1-10 --page /tmp/quant-research-lab.html

Runs `quant-lab` once for the replayed seed and once per extra seed (summary
only), then writes one self-contained HTML page from
tools/quant-lab/web/index.html. Exits 1 if any run reports an integrity
error, a book mismatch or an inconsistent OMS response.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "tools" / "quant-lab" / "web" / "index.html"
KEYS = {"mm": "mm", "arb": "pairs", "trend": "trend", "fund": "fund"}


def parse_seeds(text: str) -> list[int]:
    """'1-3,7' -> [1, 2, 3, 7]."""
    out: list[int] = []
    for part in filter(None, text.split(",")):
        lo, _, hi = part.partition("-")
        out.extend(range(int(lo), int(hi or lo) + 1))
    return out


def clean(run: dict) -> bool:
    m = run["meta"]
    return m["integrity_errors"] == 0 and m["book_mismatches"] == 0 and m["oms_inconsistent"] == 0


def seed_row(run: dict) -> dict:
    """One row of the seeds table: P&L and Sharpe per strategy."""
    row: dict = {"seed": run["meta"]["seed"]}
    for key, section in KEYS.items():
        s = run[section]["summary"]
        row[key] = {"pnl": s["pnl"], "sharpe": s["sharpe"], "max_drawdown": s["max_drawdown"]}
    return row


def embed(data: object) -> str:
    """JSON safe to put inside a <script type="application/json"> element."""
    return json.dumps(data, separators=(",", ":")).replace("</", "<\\/")


def build_page(template: str, run: dict, seeds: list[dict]) -> str:
    return template.replace("/*LAB_DATA*/", embed(run)).replace("/*SEED_DATA*/", embed(seeds))


def run_lab(binary: str, seed: int, days: int, out: Path) -> dict:
    subprocess.run([binary, "--seed", str(seed), "--days", str(days), "--out", str(out)],
                   check=False, stdout=subprocess.DEVNULL)
    return json.loads(out.read_text())


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bin", required=True, help="path to the quant-lab executable")
    ap.add_argument("--seed", type=int, default=3, help="the run the page replays")
    ap.add_argument("--seeds", default="", help="extra seeds for the table, e.g. 1-10")
    ap.add_argument("--days", type=int, default=252)
    ap.add_argument("--page", help="write the HTML page here")
    ap.add_argument("--json", help="also keep the replayed run's JSON here")
    a = ap.parse_args(argv)

    with tempfile.TemporaryDirectory() as tmp:
        main_run = run_lab(a.bin, a.seed, a.days, Path(tmp) / "main.json")
        runs = [main_run]
        for s in parse_seeds(a.seeds):
            if s != a.seed:
                runs.append(run_lab(a.bin, s, a.days, Path(tmp) / f"s{s}.json"))
    rows = sorted((seed_row(r) for r in runs), key=lambda r: r["seed"])
    print(f"{'seed':>5} " + " ".join(f"{k:>14}" for k in KEYS))
    for r in rows:
        print(f"{r['seed']:>5} " + " ".join(f"{r[k]['pnl']:>14,.0f}" for k in KEYS))
    bad = [r["meta"]["seed"] for r in runs if not clean(r)]
    if a.json:
        Path(a.json).write_text(json.dumps(main_run))
    if a.page:
        page = build_page(TEMPLATE.read_text(), main_run, rows if len(rows) > 1 else [])
        Path(a.page).write_text(page)
        print(f"wrote {a.page} ({len(page):,} bytes)")
    if bad:
        print(f"integrity problems in seeds {bad}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
