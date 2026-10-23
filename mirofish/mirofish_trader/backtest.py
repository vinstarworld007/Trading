"""Event-driven daily backtest (DESIGN.md 3, timing rules).

Each session: fill yesterday's orders at today's open, mark to today's close,
then run the pipeline on today's news to make tomorrow's orders.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from datetime import date, datetime, time

import numpy as np
import pandas as pd

from . import universe
from .broker import PaperBroker, Portfolio
from .config import Config
from .ingest.news import NY, bucket_by_session
from .ingest.prices import MarketData, annualised_sharpe
from .models import DecisionBatch, NewsItem, Order
from .pipeline import Pipeline

log = logging.getLogger(__name__)


@dataclass
class Trade:
    ticker: str
    opened: date
    closed: date
    side: int
    qty: int
    entry: float
    exit: float
    pnl: float
    reason: str


@dataclass
class BacktestResult:
    equity: pd.Series
    benchmark: pd.Series
    trades: list[Trade]
    batches: list[DecisionBatch]
    portfolio: Portfolio
    calibration: tuple[float, float]
    metrics: dict[str, float] = field(default_factory=dict)


def metrics(equity: pd.Series, benchmark: pd.Series, trades: list[Trade], fees: float) -> dict:
    r = equity.pct_change().dropna()
    br = benchmark.pct_change().dropna()
    years = max(len(r) / 252, 1e-9)
    dd = equity / equity.cummax() - 1.0
    wins = [t for t in trades if t.pnl > 0]
    excess = (r - br.reindex(r.index).fillna(0.0))
    return {
        "total_return": float(equity.iloc[-1] / equity.iloc[0] - 1.0),
        "cagr": float((equity.iloc[-1] / equity.iloc[0]) ** (1 / years) - 1.0),
        "ann_vol": float(r.std() * np.sqrt(252)),
        "sharpe": annualised_sharpe(r),
        "max_drawdown": float(dd.min()),
        "benchmark_return": float(benchmark.iloc[-1] / benchmark.iloc[0] - 1.0),
        "benchmark_sharpe": annualised_sharpe(br),
        "correlation_to_benchmark": float(r.corr(br)) if len(r) > 2 else 0.0,
        "information_ratio": annualised_sharpe(excess),
        "trades": len(trades),
        "hit_rate": len(wins) / len(trades) if trades else 0.0,
        "avg_trade_pnl": float(np.mean([t.pnl for t in trades])) if trades else 0.0,
        "fees": fees,
    }


def run_backtest(news: list[NewsItem], market: MarketData, cfg: Config | None = None,
                 claude=None, warmup: int = 20) -> BacktestResult:
    cfg = cfg or Config()
    pipe = Pipeline(cfg, claude=claude)
    portfolio = Portfolio(cfg.initial_cash)
    broker = PaperBroker(portfolio, cfg.execution)
    buckets = bucket_by_session(news, market.sessions)
    bench = universe.BENCHMARK

    equity, bench_px, trades, batches = {}, {}, [], []
    pending: list[Order] = []
    pending_vol: dict[str, float] = {}
    sessions = market.sessions
    for i, d in enumerate(sessions):
        if pending:
            opens = {o.ticker: market.px_open(o.ticker, d) for o in pending}
            before = {t: (p.qty, p.avg_price, p.opened) for t, p in portfolio.positions.items()}
            fills = broker.execute(pending, {t: p for t, p in opens.items() if p},
                                   datetime.combine(d, time(9, 30), NY), d, pending_vol)
            for f in fills:
                if f.ticker in before and (before[f.ticker][0] > 0) != (f.qty > 0):
                    qty0, avg, opened = before[f.ticker]
                    closed = min(abs(f.qty), abs(qty0))
                    side = 1 if qty0 > 0 else -1
                    trades.append(Trade(f.ticker, opened, d, side, closed, avg, f.price,
                                        closed * side * (f.price - avg) - f.fees, f.reason))
            pending = []
        closes = {t: c for t in portfolio.positions if (c := market.px_close(t, d)) is not None}
        equity[d] = portfolio.equity(closes)
        bench_px[d] = market.px_close(bench, d)
        if i < warmup or i == len(sessions) - 1:
            if i < warmup:  # still learn buzz baselines and novelty during warmup
                pipe.aggregator.aggregate(d, [a for it in buckets.get(d, []) for a in pipe.forum.assess(it)])
            continue
        batch = pipe.decide(d, buckets.get(d, []), market, portfolio)
        batches.append(batch)
        pending = batch.orders
        pending_vol = {o.ticker: market.daily_vol(o.ticker, d) for o in pending}

    eq = pd.Series(equity).sort_index()
    bp = pd.Series(bench_px).sort_index()
    bm = bp / bp.iloc[0] * cfg.initial_cash
    res = BacktestResult(eq, bm, trades, batches, portfolio, pipe.calibrator.weights)
    res.metrics = metrics(eq.iloc[warmup:], bm.iloc[warmup:], trades, portfolio.fees_paid)
    res.metrics["calibration_continuation"], res.metrics["calibration_gap"] = pipe.calibrator.weights
    res.metrics["calibration_samples"] = pipe.calibrator.n
    return res
