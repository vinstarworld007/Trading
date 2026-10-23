"""One paper-trading cycle, meant to run once a day after the close (DESIGN.md 9).

With the local ledger, orders made today are kept as pending and fill at the
next session's open on the next run, which is when that open is known. With
``--broker alpaca`` they go to the Alpaca paper account as market-on-open.
"""

from __future__ import annotations

import json
import logging
from datetime import datetime, time
from pathlib import Path

from . import universe
from .broker import AlpacaPaperBroker, PaperBroker, load_portfolio, save_portfolio
from .config import Config
from .ingest.news import NY, bucket_by_session, fetch_rss, load_jsonl
from .ingest.prices import MarketData, fetch_stooq
from .models import Order
from .pipeline import Pipeline
from .report import batch_report

log = logging.getLogger(__name__)


def run_cycle(state_dir: str | Path, cfg: Config, prices_csv: str | None = None,
              news_jsonl: str | None = None, broker: str = "local", claude=None,
              force: bool = False) -> str:
    sd = Path(state_dir)
    sd.mkdir(parents=True, exist_ok=True)
    meta_path = sd / "meta.json"
    meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}

    tickers = universe.tickers()
    if prices_csv:
        market = MarketData.from_csv(prices_csv)
    else:
        market = MarketData(fetch_stooq(tickers + [universe.BENCHMARK]))
    as_of = market.sessions[-1]
    if meta.get("last_run") == as_of.isoformat() and not force:
        return f"Already ran for {as_of}; pass --force to run again."

    portfolio = load_portfolio(sd / "portfolio.json", cfg.initial_cash)
    pipe = Pipeline(cfg, claude=claude)
    pipe.load_state(sd / "pipeline.pkl")

    notes = []
    if broker == "local":
        pending = [Order(**o) for o in meta.get("pending", [])]
        placed = meta.get("pending_date")
        if pending and placed:
            fill_day = next((d for d in market.sessions if d.isoformat() > placed), None)
            if fill_day:
                opens = {o.ticker: market.px_open(o.ticker, fill_day) for o in pending}
                fills = PaperBroker(portfolio, cfg.execution).execute(
                    pending, {t: p for t, p in opens.items() if p},
                    datetime.combine(fill_day, time(9, 30), NY), fill_day,
                    meta.get("pending_vol", {}))
                notes.append(f"Filled {len(fills)} pending orders at the {fill_day} open.")
    else:
        alpaca = AlpacaPaperBroker()
        alpaca.sync(portfolio, as_of)

    news = load_jsonl(news_jsonl) if news_jsonl else fetch_rss(tickers)
    today = bucket_by_session(news, market.sessions).get(as_of, [])
    batch = pipe.decide(as_of, today, market, portfolio)

    if broker == "local":
        meta["pending"] = [o.__dict__ for o in batch.orders]
        meta["pending_date"] = as_of.isoformat()
        meta["pending_vol"] = {o.ticker: market.daily_vol(o.ticker, as_of) for o in batch.orders}
    else:
        alpaca.submit(batch.orders)
        notes.append(f"Sent {len(batch.orders)} market-on-open orders to Alpaca paper.")

    closes = {t: c for t in portfolio.positions if (c := market.px_close(t, as_of)) is not None}
    equity = portfolio.equity(closes)
    meta["last_run"] = as_of.isoformat()
    save_portfolio(portfolio, sd / "portfolio.json")
    pipe.save_state(sd / "pipeline.pkl")
    meta_path.write_text(json.dumps(meta, indent=2))

    report = "\n".join([
        f"# Paper cycle {as_of}", "",
        f"Equity ${equity:,.0f}, cash ${portfolio.cash:,.0f}, {len(portfolio.positions)} positions, "
        f"{len(today)} news items for the session.", *notes, "",
        batch_report(batch)])
    (sd / "reports").mkdir(exist_ok=True)
    (sd / "reports" / f"paper-{as_of}.md").write_text(report)
    return report
