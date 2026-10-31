#!/usr/bin/env python3
"""Option C colo model: the lab's firm on Arista 7130 L1 + MetaMux with kernel-bypass hosts.

    python3 research/colo.py
    python3 research/colo.py --trader-log trader.out --page colo.html --json colo.json

Simulates the network path the lab would run on in an exchange colo (Layer 1
fan-out of the feed, kernel-bypass hosts, MetaMux order aggregation) at
nanosecond resolution and prints tick-to-trade and wire-to-wire percentiles.
With --trader-log (trader's stdout), adds a run whose host decision time is the
trader's own measured "Tick to wire" histogram. Stdlib only.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lab_research import colo  # noqa: E402

PAGE = Path(__file__).resolve().parent / "lab_research" / "colo" / "page.html"
SWEEP_BURSTS = [0, 10, 30, 60, 100]


def fmt(ns: float) -> str:
    return f"{ns / 1000:.2f} us" if ns >= 1000 else f"{ns:.0f} ns"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--trader-log", help="trader's stdout, for its measured Tick to wire")
    ap.add_argument("--config", help="JSON with optional keys: scenarios, traffic, switches, hosts")
    ap.add_argument("--duration-ms", type=float, help="simulated time per run (default 30)")
    ap.add_argument("--no-sweep", action="store_true", help="skip the microburst sweep")
    ap.add_argument("--json", help="write the results here")
    ap.add_argument("--page", help="write a self-contained HTML page here")
    args = ap.parse_args(argv)

    t2w = None
    if args.trader_log:
        try:
            t2w = colo.parse_tick_to_wire(Path(args.trader_log).read_text())
        except ValueError as e:
            print(f"colo: {args.trader_log}: {e}", file=sys.stderr)
            return 1
    scenarios, traffic = colo.option_c_scenarios(t2w), dict(colo.DEFAULT_TRAFFIC)
    if args.config:
        cfg = json.loads(Path(args.config).read_text())
        scenarios = cfg.get("scenarios", scenarios)
        traffic.update(cfg.get("traffic", {}))
        colo.SWITCHES.update(cfg.get("switches", {}))
        colo.HOSTS.update(cfg.get("hosts", {}))
    if args.duration_ms:
        traffic["duration_ms"] = args.duration_ms

    results = []
    print(f"{'':6} {'configuration':40} {'tick-to-trade p50':>18} {'p99':>10} "
          f"{'wire-to-wire p50':>17} {'p99':>10} {'p99.9':>10}")
    for sc in scenarios:
        r = colo.run_scenario(sc, traffic)
        results.append(r)
        t, w = r["tick_to_trade"], r["wire_to_wire"]
        print(f"{sc['id']:6} {sc['label'][:40]:40} {fmt(t['p50']):>18} {fmt(t['p99']):>10} "
              f"{fmt(w['p50']):>17} {fmt(w['p99']):>10} {fmt(w['p999']):>10}")
    for r in results:
        print(f"\n{r['id']}: mean time per stage")
        for b in r["breakdown"]:
            if b["mean_ns"] >= 0.5:
                print(f"  {b['stage']:42} {fmt(b['mean_ns']):>10}  {b['category']}")

    matrix = colo.head_to_head(results)
    sweep = {"bursts": SWEEP_BURSTS, "series": []}
    if not args.no_sweep:
        short = {**traffic, "duration_ms": min(traffic["duration_ms"], 10)}
        print("\nmicroburst sweep, wire-to-wire p99 by frames per burst")
        for sc in scenarios:
            pts = []
            for b in SWEEP_BURSTS:
                r = colo.run_scenario(sc, {**short, "burst_size": b})
                pts.append({"p50": r["wire_to_wire"]["p50"], "p99": r["wire_to_wire"]["p99"],
                            "util": r["host_utilisation"]})
            sweep["series"].append({"id": sc["id"], "points": pts})
            print(f"  {sc['id']:6} " + "  ".join(f"{b}: {fmt(p['p99'])}"
                                                  for b, p in zip(SWEEP_BURSTS, pts)))

    for r in results:
        r.pop("_race")
    payload = {"traffic": traffic, "trader_tick_to_wire": t2w, "scenarios": results,
               "head_to_head": matrix, "sweep": sweep}
    payload = json.loads(json.dumps(payload).replace("NaN", "null"))
    if args.json:
        Path(args.json).write_text(json.dumps(payload, indent=1))
    if args.page:
        body = PAGE.read_text().replace("__DATA__", json.dumps(payload, separators=(",", ":")))
        Path(args.page).write_text(
            '<!doctype html>\n<html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">'
            '<style>body{margin:0}</style></head><body>\n' + body + "\n</body></html>\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
