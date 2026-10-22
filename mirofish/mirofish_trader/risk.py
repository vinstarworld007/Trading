"""RiskManager: exits, halts, and caps on what the decision layer wants (DESIGN.md 7).

Input is the portfolio, today's closes and the new decisions; output is a
target weight for every name that should change, with a note for every clip.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import date

import numpy as np

from . import universe
from .broker import Portfolio
from .config import RiskConfig
from .models import Decision


@dataclass
class RiskState:
    peak_equity: float = 0.0
    last_equity: float = 0.0
    halted: bool = False  # drawdown halt; cleared only by reset()

    def reset(self) -> None:
        self.halted = False
        self.peak_equity = 0.0


@dataclass
class RiskResult:
    targets: dict[str, float]  # ticker -> weight; 0 closes
    exits: list[tuple[str, str]] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    entries: list[str] = field(default_factory=list)


def sessions_between(a: date, b: date) -> int:
    return int(np.busday_count(a, b))


class RiskManager:
    def __init__(self, cfg: RiskConfig | None = None, state: RiskState | None = None):
        self.cfg = cfg or RiskConfig()
        self.state = state or RiskState()

    def review(self, today: date, portfolio: Portfolio, prices: dict[str, float],
               decisions: list[Decision], dollar_volume: dict[str, float]) -> RiskResult:
        cfg, st = self.cfg, self.state
        equity = portfolio.equity(prices)
        weights = portfolio.weights(prices)
        res = RiskResult(targets={})

        st.peak_equity = max(st.peak_equity, equity)
        day_pnl = equity / st.last_equity - 1.0 if st.last_equity else 0.0
        st.last_equity = equity
        dd = equity / st.peak_equity - 1.0 if st.peak_equity else 0.0
        if dd <= -cfg.drawdown_halt and not st.halted:
            st.halted = True
            res.notes.append(f"drawdown {dd:.1%} breached {-cfg.drawdown_halt:.0%}: halted")
        if st.halted or day_pnl <= -cfg.daily_loss_halt:
            if not st.halted:
                res.notes.append(f"daily loss {day_pnl:.1%} breached {-cfg.daily_loss_halt:.0%}: "
                                 "flatten, no entries today")
            for t in portfolio.positions:
                res.targets[t] = 0.0
                res.exits.append((t, "risk halt"))
            return res

        by_ticker = {d.ticker: d for d in decisions}
        for t, pos in portfolio.positions.items():
            px = prices.get(t)
            if px is None:
                continue
            ret = (px / pos.avg_price - 1.0) * pos.side
            held = sessions_between(pos.opened, today)
            why = None
            if ret <= -cfg.stop_vol_mult * pos.entry_vol:
                why = f"stop {ret:+.1%}"
            elif ret >= cfg.take_profit_vol_mult * pos.entry_vol:
                why = f"take profit {ret:+.1%}"
            elif held >= cfg.max_hold_days:
                why = f"max hold {held} sessions"
            else:
                d = by_ticker.get(t)
                if d and d.target_weight and math.copysign(1, d.target_weight) != pos.side:
                    why = f"signal flipped (conviction {d.conviction:.2f})"
            if why:
                res.targets[t] = 0.0
                res.exits.append((t, why))

        # Weights that stay on after exits.
        book = {t: w for t, w in weights.items() if res.targets.get(t, None) != 0.0}

        def sector_w(sector: str, w: dict[str, float]) -> float:
            return sum(abs(x) for t, x in w.items() if universe.get(t) and universe.get(t).sector == sector)

        candidates = sorted((d for d in decisions if d.target_weight != 0.0),
                            key=lambda d: d.conviction, reverse=True)
        for d in candidates:
            t, w = d.ticker, d.target_weight
            if t in book:
                d.reasons.append("risk: already holding, no top-ups")
                continue
            px = prices.get(t)
            if px is None or px < cfg.min_price:
                d.reasons.append(f"risk: price below ${cfg.min_price:.0f}")
                d.target_weight = 0.0
                continue
            if dollar_volume.get(t, 0.0) < cfg.min_dollar_volume:
                d.reasons.append("risk: too illiquid")
                d.target_weight = 0.0
                continue
            if len(book) >= cfg.max_positions:
                d.reasons.append("risk: max positions")
                d.target_weight = 0.0
                continue
            if abs(w) > cfg.max_weight:
                d.reasons.append(f"risk: weight {w:+.1%} capped at {cfg.max_weight:.0%}")
                w = math.copysign(cfg.max_weight, w)
            c = universe.get(t)
            room = cfg.max_sector - sector_w(c.sector, book) if c else cfg.max_sector
            gross_room = cfg.max_gross - sum(abs(x) for x in book.values())
            net = sum(book.values())
            net_room = cfg.max_net - net if w > 0 else cfg.max_net + net
            allowed = min(abs(w), room, gross_room, net_room)
            if allowed < 0.005:
                d.reasons.append("risk: no room under sector, gross or net caps")
                d.target_weight = 0.0
                continue
            if allowed < abs(w):
                d.reasons.append(f"risk: sized down to {allowed:.1%} by exposure caps")
            w = math.copysign(allowed, w)
            d.target_weight = w  # the report shows what risk let through
            book[t] = w
            res.targets[t] = w
            res.entries.append(t)
        return res
