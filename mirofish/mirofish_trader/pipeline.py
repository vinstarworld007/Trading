"""Pipeline: one decision cycle, shared by the backtester and the paper trader.

``decide(as_of, items, market, portfolio)`` runs after the close of ``as_of``:

1. sentiment layer: every item through the forum, then the aggregator;
2. swarm layer: one simulation per ticker with a signal, anchored on the
   stock's observed reaction today, then calibrated to a return;
3. decision layer, then the risk manager;
4. orders (signed share quantities) to fill at the next open.

``market`` may hold data past ``as_of`` in a backtest; the pipeline reads only
``as_of`` and earlier (tests/test_pipeline.py checks this).
"""

from __future__ import annotations

import pickle
from datetime import date
from pathlib import Path

import numpy as np

from .broker import Portfolio
from .config import Config
from .decision import decide
from .ingest.prices import MarketData
from .models import DecisionBatch, NewsItem, Order
from .risk import RiskManager, RiskState
from .sentiment.forum import Forum, SignalAggregator
from .swarm.calibration import Calibrator
from .swarm.graph import KnowledgeGraph
from .swarm.simulation import SwarmSimulation

FORWARD_SESSIONS = 3  # calibration window: next open to the close three sessions later


def busday(d: date, n: int) -> date:
    return np.busday_offset(np.datetime64(d), n, roll="forward").astype(object)


class Pipeline:
    def __init__(self, cfg: Config | None = None, claude=None):
        self.cfg = cfg or Config()
        graph = KnowledgeGraph()
        self.forum = Forum(self.cfg.sentiment, claude=claude, graph=graph)
        self.aggregator = SignalAggregator(self.cfg.sentiment)
        self.swarm = SwarmSimulation(self.cfg.swarm, graph)
        self.calibrator = Calibrator()
        self.risk = RiskManager(self.cfg.risk, RiskState())

    def decide(self, as_of: date, items: list[NewsItem], market: MarketData,
               portfolio: Portfolio) -> DecisionBatch:
        self.calibrator.mature(as_of, market.forward_idio)

        assessments = [a for item in items for a in self.forum.assess(item)]
        signals = self.aggregator.aggregate(as_of, assessments)

        decisions, forecasts = [], {}
        for ticker, sig in signals.items():
            if market.px_close(ticker, as_of) is None:
                continue
            fc = self.swarm.run(sig, observed_move=market.idio_return(ticker, as_of))
            features = (fc.continuation, fc.gap)
            fc.calibrated_return = self.calibrator.apply(features)
            forecasts[ticker] = fc
            start = busday(as_of, 1)
            self.calibrator.add_forecast(start, busday(start, FORWARD_SESSIONS - 1), ticker, features)
            decisions.append(decide(sig, fc, market.daily_vol(ticker, as_of), self.cfg.decision))

        prices = {t: p for t in set(portfolio.positions) | set(signals)
                  if (p := market.px_close(t, as_of)) is not None}
        dvol = {t: market.dollar_volume(t, as_of) or 0.0 for t in signals}
        res = self.risk.review(as_of, portfolio, prices, decisions, dvol)

        equity = portfolio.equity(prices)
        orders = []
        for t, w in res.targets.items():
            px = prices.get(t)
            if not px:
                continue
            have = portfolio.positions[t].qty if t in portfolio.positions else 0
            want = int(w * equity / px)
            if want != have:
                why = dict(res.exits).get(t) or ("enter" if have == 0 else "adjust")
                orders.append(Order(t, want - have, why))
        return DecisionBatch(as_of, decisions, orders, res.exits, res.notes, signals, forecasts)

    # --- persistence for the paper loop (memory, calibration, risk state) ---
    def save_state(self, path: str | Path) -> None:
        Path(path).write_bytes(pickle.dumps(
            {"memory": self.aggregator.memory, "calibrator": self.calibrator, "risk": self.risk.state}))

    def load_state(self, path: str | Path) -> None:
        p = Path(path)
        if not p.exists():
            return
        s = pickle.loads(p.read_bytes())
        self.aggregator.memory, self.calibrator, self.risk.state = s["memory"], s["calibrator"], s["risk"]
