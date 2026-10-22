"""A market-making backtest on one symbol's top of book.

The strategy is the trader's (``firm/strategy``): quote one lot at the best
bid and ask, step inside when the spread is wide, lean against the position,
stop adding past a position cap, and leave a quote alone while it is within
``hold_ticks`` of where it should be.

Fills use a queue model on ``itch-dump --tob-csv`` data, which carries the
shares traded against the best bid and ask between updates:

- joining an existing level puts us behind every share already there; stepping
  inside the spread puts us first;
- shares traded at our price come off the queue ahead of us, and shares that
  leave the level otherwise (cancels) come off it pro rata, the usual
  assumption when individual queue positions are unknown;
- once more shares trade at our price than are ahead of us, we fill; a market
  that trades through our price (the ask falls to our bid) fills us too;
- a replace goes to the back of the queue, which is what ``hold_ticks`` trades
  against.

It fills a whole lot at a time and ignores our own effect on the market, so
use it to compare parameter settings, not as a PnL forecast.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import pandas as pd

from . import PRICE_SCALE


@dataclass
class Params:
    quote_qty: int = 100
    tick: int = 100
    max_position: int = 1_000
    improve_spread_ticks: int = 3
    skew_ticks: int = 1
    hold_ticks: int = 1


@dataclass
class Result:
    params: Params
    fills: int = 0
    volume: int = 0
    position: int = 0
    cash: int = 0  # Price(4) x shares
    pnl: float = 0.0  # dollars, marked at the last mid
    max_abs_position: int = 0
    trades: list[tuple[int, str, int, int]] = field(default_factory=list)  # (ns, side, qty, px)


def quote(p: Params, bid: int, ask: int, position: int) -> tuple[int, int, int, int]:
    """(bid_px, bid_qty, ask_px, ask_qty), as MarketMaker::quote."""
    if bid <= 0 or ask <= 0 or ask <= bid:
        return 0, 0, 0, 0
    b, a = bid, ask
    if (ask - bid) // p.tick >= p.improve_spread_ticks:
        b += p.tick
        a -= p.tick
    lean = int(position / p.quote_qty) * p.skew_ticks * p.tick
    b -= lean
    a -= lean
    if a <= bid:
        a = bid + p.tick
    if b >= ask:
        b = ask - p.tick
    if b >= a or b <= 0:
        return 0, 0, 0, 0
    bq = p.quote_qty if position + p.quote_qty <= p.max_position else 0
    aq = p.quote_qty if position - p.quote_qty >= -p.max_position else 0
    return b, bq, a, aq


class _Side:
    """Our resting order on one side and the queue ahead of it."""

    def __init__(self) -> None:
        self.price = 0  # 0: no order
        self.ahead = 0.0

    def place(self, price: int, best: int, best_qty: int, better) -> None:
        self.price = price
        # Joining the best level: behind everyone there. Inside it: first.
        # Behind it: we only learn about our level when it becomes best.
        self.ahead = float(best_qty) if price == best else 0.0 if better(price, best) else 1e18

    def update(self, best: int, best_qty: int, prev_best: int, prev_qty: int, traded: int,
               better) -> bool:
        """Applies one top-of-book update; returns True if we filled."""
        if not self.price:
            return False
        if self.price == best and self.ahead >= 1e18:
            self.ahead = float(best_qty)  # our level just became the best
        if self.price == prev_best:
            if traded > self.ahead:
                return True
            gone = max(0, prev_qty - traded - (best_qty if best == prev_best else 0))
            if prev_qty > 0 and self.ahead < 1e18:
                self.ahead = max(0.0, self.ahead - traded - gone * self.ahead / prev_qty)
        return False


def run(tob: pd.DataFrame, p: Params) -> Result:
    r = Result(params=p)
    bid_side, ask_side = _Side(), _Side()
    higher = lambda a, b: b == 0 or a > b  # noqa: E731
    lower = lambda a, b: b == 0 or a < b  # noqa: E731
    cols = ["feed_ns", "bid", "bid_qty", "ask", "ask_qty"]
    has_trades = "traded_bid" in tob.columns
    data = tob[cols + (["traded_bid", "traded_ask"] if has_trades else [])].to_numpy()
    prev_bid = prev_bid_qty = prev_ask = prev_ask_qty = 0
    last_mid = 0
    for row in data:
        t, bid, bid_qty, ask, ask_qty = (int(x) for x in row[:5])
        tb, ta = (int(row[5]), int(row[6])) if has_trades else (0, 0)
        bought = bid_side.update(bid, bid_qty, prev_bid, prev_bid_qty, tb, higher) or (
            bid_side.price and 0 < ask <= bid_side.price)
        sold = ask_side.update(ask, ask_qty, prev_ask, prev_ask_qty, ta, lower) or (
            ask_side.price and bid >= ask_side.price > 0)
        if bought:
            r.position += p.quote_qty
            r.cash -= p.quote_qty * bid_side.price
            r.trades.append((t, "B", p.quote_qty, bid_side.price))
            bid_side.price = 0
        if sold:
            r.position -= p.quote_qty
            r.cash += p.quote_qty * ask_side.price
            r.trades.append((t, "S", p.quote_qty, ask_side.price))
            ask_side.price = 0
        r.max_abs_position = max(r.max_abs_position, abs(r.position))
        if bid > 0 and ask > bid:
            last_mid = (bid + ask) // 2
        qb, qbq, qa, qaq = quote(p, bid, ask, r.position)
        hold = p.hold_ticks * p.tick
        if not qbq:
            bid_side.price = 0
        elif not bid_side.price or abs(bid_side.price - qb) > hold:
            bid_side.place(qb, bid, bid_qty, higher)
        if not qaq:
            ask_side.price = 0
        elif not ask_side.price or abs(ask_side.price - qa) > hold:
            ask_side.place(qa, ask, ask_qty, lower)
        prev_bid, prev_bid_qty, prev_ask, prev_ask_qty = bid, bid_qty, ask, ask_qty
    r.fills = len(r.trades)
    r.volume = r.fills * p.quote_qty
    r.pnl = (r.cash + r.position * last_mid) / PRICE_SCALE
    return r


def sweep(tob: pd.DataFrame, grid: dict[str, list[int]], base: Params | None = None) -> pd.DataFrame:
    """Runs every combination in ``grid`` (field name -> values); one row each."""
    base = base or Params()
    keys = list(grid)
    rows = []
    for combo in np.array(np.meshgrid(*[grid[k] for k in keys])).T.reshape(-1, len(keys)):
        p = Params(**{**base.__dict__, **{k: int(v) for k, v in zip(keys, combo)}})
        res = run(tob, p)
        rows.append({**{k: getattr(p, k) for k in keys}, "fills": res.fills, "pnl": res.pnl,
                     "position": res.position, "max_abs_position": res.max_abs_position})
    return pd.DataFrame(rows).sort_values("pnl", ascending=False, ignore_index=True)
