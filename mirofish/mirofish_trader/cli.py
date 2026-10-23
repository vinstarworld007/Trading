"""Command line: demo, backtest, analyze, paper, fetch-prices, synth."""

from __future__ import annotations

import argparse
import copy
import logging
import sys
from datetime import datetime, timezone
from pathlib import Path

from .config import Config


def _claude(args):
    if not getattr(args, "llm", False):
        return None
    from .sentiment.llm import ClaudeEngine

    return ClaudeEngine(cache_path=args.llm_cache)


def cmd_demo(args) -> int:
    from .backtest import run_backtest
    from .ingest.news import save_jsonl
    from .ingest.prices import MarketData
    from .ingest.synthetic import generate
    from .report import backtest_report

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    world = generate(days=args.days, seed=args.seed)
    save_jsonl(world.news, out / "synthetic_news.jsonl")
    world.bars.to_csv(out / "synthetic_prices.csv", index=False)
    market = MarketData(world.bars)
    cfg = Config.load(args.config)

    print(f"Synthetic market: {len(market.sessions)} sessions, {len(world.news)} news items", file=sys.stderr)
    res = run_backtest(world.news, market, cfg, claude=_claude(args))
    comparison = {"Sentiment + swarm (default)": res.metrics}
    if not args.no_compare:
        for name, ws, wm in [("Sentiment only", 1.0, 0.0), ("Swarm only", 0.0, 1.0)]:
            c = copy.deepcopy(cfg)
            c.decision.w_sentiment, c.decision.w_swarm = ws, wm
            comparison[name] = run_backtest(world.news, market, c).metrics
    notes = [
        "Synthetic data: the reaction model is planted (retail-heavy names overshoot and reverse,",
        "institutional names drift), so these numbers show the pipeline working end to end, not",
        "an edge on real markets.",
    ]
    text = backtest_report(res, "MiroFish Trader demo (synthetic)", comparison=comparison, notes=notes)
    (out / "demo_report.md").write_text(text)
    print(text)
    return 0


def cmd_backtest(args) -> int:
    from .backtest import run_backtest
    from .ingest.news import load_jsonl
    from .ingest.prices import MarketData
    from .report import backtest_report

    res = run_backtest(load_jsonl(args.news), MarketData.from_csv(args.prices), Config.load(args.config),
                       claude=_claude(args), warmup=args.warmup)
    text = backtest_report(res, f"Backtest {args.news}")
    if args.report:
        Path(args.report).write_text(text)
    if args.equity_csv:
        res.equity.rename("equity").to_frame().join(res.benchmark.rename("qqq")).to_csv(args.equity_csv)
    print(text)
    return 0


def cmd_analyze(args) -> int:
    from . import universe
    from .models import NewsItem
    from .pipeline import Pipeline
    from .report import decision_block
    from .decision import decide

    pipe = Pipeline(Config.load(args.config), claude=_claude(args))
    item = NewsItem(datetime.now(timezone.utc), args.source, args.headline, args.body or "")
    today = datetime.now(timezone.utc).date()
    signals = pipe.aggregator.aggregate(today, pipe.forum.assess(item))
    if not signals:
        print("No tradeable signal: no universe stock linked, or sentiment too weak.")
        return 0
    for t, sig in sorted(signals.items(), key=lambda kv: -abs(kv[1].score)):
        fc = pipe.swarm.run(sig, observed_move=args.observed)
        fc.calibrated_return = pipe.calibrator.apply((fc.continuation, fc.gap))
        d = decide(sig, fc, args.vol, pipe.cfg.decision)
        name = universe.get(t).name if universe.get(t) else t
        print(f"\n{name}")
        print("\n".join(decision_block(d)))
    return 0


def cmd_paper(args) -> int:
    from .paper import run_cycle

    print(run_cycle(args.state, Config.load(args.config), args.prices, args.news, args.broker,
                    claude=_claude(args), force=args.force))
    return 0


def cmd_fetch_prices(args) -> int:
    from . import universe
    from .ingest.prices import fetch_stooq

    df = fetch_stooq(universe.tickers() + [universe.BENCHMARK])
    if args.start:
        df = df[df["date"] >= args.start]
    df.to_csv(args.out, index=False)
    print(f"wrote {len(df)} bars for {df['ticker'].nunique()} tickers to {args.out}")
    return 0


def cmd_synth(args) -> int:
    from .ingest.news import save_jsonl
    from .ingest.synthetic import generate

    w = generate(days=args.days, seed=args.seed)
    save_jsonl(w.news, args.news)
    w.bars.to_csv(args.prices, index=False)
    print(f"wrote {len(w.news)} news items to {args.news} and {len(w.bars)} bars to {args.prices}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="mirofish_trader", description=__doc__)
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--config", help="JSON file overriding config defaults")
        sp.add_argument("--llm", action="store_true", help="add the Claude engine to the forum")
        sp.add_argument("--llm-cache", default=".cache/claude_sentiment.jsonl")

    d = sub.add_parser("demo", help="synthetic year: generate, backtest, compare layers, report")
    common(d)
    d.add_argument("--out", default="out")
    d.add_argument("--days", type=int, default=252)
    d.add_argument("--seed", type=int, default=42)
    d.add_argument("--no-compare", action="store_true", help="skip the sentiment-only and swarm-only runs")
    d.set_defaults(fn=cmd_demo)

    b = sub.add_parser("backtest", help="backtest on your news JSONL and price CSV")
    common(b)
    b.add_argument("--news", required=True)
    b.add_argument("--prices", required=True)
    b.add_argument("--warmup", type=int, default=20)
    b.add_argument("--report")
    b.add_argument("--equity-csv")
    b.set_defaults(fn=cmd_backtest)

    a = sub.add_parser("analyze", help="show every layer's reading of one headline")
    common(a)
    a.add_argument("headline")
    a.add_argument("--body")
    a.add_argument("--source", default="reuters")
    a.add_argument("--observed", type=float, default=None,
                   help="the stock's idiosyncratic move today, e.g. 0.04 for +4%%")
    a.add_argument("--vol", type=float, default=0.02, help="daily vol for sizing")
    a.set_defaults(fn=cmd_analyze)

    pp = sub.add_parser("paper", help="one paper-trading cycle (run daily after the close)")
    common(pp)
    pp.add_argument("--state", default="paper_state")
    pp.add_argument("--prices", help="price CSV instead of downloading from Stooq")
    pp.add_argument("--news", help="news JSONL instead of fetching RSS")
    pp.add_argument("--broker", choices=["local", "alpaca"], default="local")
    pp.add_argument("--force", action="store_true")
    pp.set_defaults(fn=cmd_paper)

    f = sub.add_parser("fetch-prices", help="download daily bars for the universe from Stooq")
    f.add_argument("--out", default="prices.csv")
    f.add_argument("--start", help="YYYY-MM-DD")
    f.set_defaults(fn=cmd_fetch_prices)

    s = sub.add_parser("synth", help="write a synthetic news JSONL and price CSV")
    s.add_argument("--news", default="synthetic_news.jsonl")
    s.add_argument("--prices", default="synthetic_prices.csv")
    s.add_argument("--days", type=int, default=252)
    s.add_argument("--seed", type=int, default=42)
    s.set_defaults(fn=cmd_synth)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING,
                        format="%(levelname)s %(name)s: %(message)s")
    return args.fn(args)
