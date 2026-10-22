"""Execution: the portfolio ledger, a paper broker, and an Alpaca paper adapter (DESIGN.md 8).

There is no live broker. ``AlpacaPaperBroker`` talks only to Alpaca's paper
endpoint; the URL is a constant, not a setting, so turning on live trading is
a code change that needs the owner's sign-off.
"""

from __future__ import annotations

import json
import logging
import os
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import date, datetime, timezone
from pathlib import Path

from .config import ExecutionConfig
from .models import Fill, Order

log = logging.getLogger(__name__)


@dataclass
class Position:
    qty: int
    avg_price: float
    opened: date
    entry_vol: float = 0.02  # daily vol at entry, for stops

    @property
    def side(self) -> int:
        return 1 if self.qty > 0 else -1


@dataclass
class Portfolio:
    cash: float
    positions: dict[str, Position] = field(default_factory=dict)
    fills: list[Fill] = field(default_factory=list)
    fees_paid: float = 0.0

    def market_value(self, prices: dict[str, float]) -> float:
        return sum(p.qty * prices.get(t, p.avg_price) for t, p in self.positions.items())

    def equity(self, prices: dict[str, float]) -> float:
        return self.cash + self.market_value(prices)

    def weights(self, prices: dict[str, float]) -> dict[str, float]:
        eq = self.equity(prices)
        return {t: p.qty * prices.get(t, p.avg_price) / eq for t, p in self.positions.items()} if eq else {}

    def apply_fill(self, fill: Fill, opened: date, entry_vol: float) -> None:
        self.cash -= fill.qty * fill.price + fill.fees
        self.fees_paid += fill.fees
        self.fills.append(fill)
        pos = self.positions.get(fill.ticker)
        if pos is None:
            self.positions[fill.ticker] = Position(fill.qty, fill.price, opened, entry_vol)
            return
        new_qty = pos.qty + fill.qty
        if new_qty == 0:
            del self.positions[fill.ticker]
        elif (pos.qty > 0) != (new_qty > 0):  # flipped through zero
            self.positions[fill.ticker] = Position(new_qty, fill.price, opened, entry_vol)
        elif abs(new_qty) > abs(pos.qty):  # added
            pos.avg_price = (pos.avg_price * pos.qty + fill.price * fill.qty) / new_qty
            pos.qty = new_qty
        else:  # reduced
            pos.qty = new_qty

    # --- persistence for the paper loop ---
    def to_dict(self) -> dict:
        return {
            "cash": self.cash, "fees_paid": self.fees_paid,
            "positions": {t: {**asdict(p), "opened": p.opened.isoformat()}
                          for t, p in self.positions.items()},
            "fills": [{**asdict(f), "ts": f.ts.isoformat()} for f in self.fills],
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Portfolio":
        p = cls(d["cash"], fees_paid=d.get("fees_paid", 0.0))
        for t, v in d.get("positions", {}).items():
            p.positions[t] = Position(v["qty"], v["avg_price"], date.fromisoformat(v["opened"]),
                                      v.get("entry_vol", 0.02))
        for f in d.get("fills", []):
            p.fills.append(Fill(datetime.fromisoformat(f["ts"]), f["ticker"], f["qty"], f["price"],
                                f["fees"], f.get("reason", "")))
        return p


class PaperBroker:
    """Fills orders at a given reference price plus slippage and commission."""

    def __init__(self, portfolio: Portfolio, cfg: ExecutionConfig | None = None):
        self.portfolio = portfolio
        self.cfg = cfg or ExecutionConfig()

    def execute(self, orders: list[Order], prices: dict[str, float], ts: datetime, opened: date,
                vols: dict[str, float] | None = None) -> list[Fill]:
        fills = []
        for o in orders:
            px = prices.get(o.ticker)
            if o.qty == 0 or px is None or px <= 0:
                continue
            slip = px * self.cfg.slippage_bps / 1e4
            price = px + slip if o.qty > 0 else px - slip
            fees = max(self.cfg.min_commission, abs(o.qty) * self.cfg.commission_per_share)
            f = Fill(ts, o.ticker, o.qty, price, fees, o.reason)
            self.portfolio.apply_fill(f, opened, (vols or {}).get(o.ticker, 0.02))
            fills.append(f)
        return fills


class AlpacaPaperBroker:
    """Alpaca paper trading. Orders are market-on-open (``time_in_force=opg``)."""

    BASE_URL = "https://paper-api.alpaca.markets"  # paper only, by design

    def __init__(self, key_id: str | None = None, secret: str | None = None):
        self.key_id = key_id or os.environ.get("APCA_API_KEY_ID")
        self.secret = secret or os.environ.get("APCA_API_SECRET_KEY")
        if not self.key_id or not self.secret:
            raise RuntimeError("set APCA_API_KEY_ID and APCA_API_SECRET_KEY for Alpaca paper trading")

    def _req(self, method: str, path: str, body: dict | None = None):
        req = urllib.request.Request(
            self.BASE_URL + path, method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={"APCA-API-KEY-ID": self.key_id, "APCA-API-SECRET-KEY": self.secret,
                     "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read() or b"null")

    def sync(self, portfolio: Portfolio, today: date) -> None:
        """Overwrite cash and quantities with the account's, keeping local entry metadata."""
        acct = self._req("GET", "/v2/account")
        portfolio.cash = float(acct["cash"])
        remote = {p["symbol"]: p for p in self._req("GET", "/v2/positions")}
        for t in list(portfolio.positions):
            if t not in remote:
                del portfolio.positions[t]
        for t, p in remote.items():
            qty, avg = int(float(p["qty"])), float(p["avg_entry_price"])
            if t in portfolio.positions:
                portfolio.positions[t].qty, portfolio.positions[t].avg_price = qty, avg
            else:
                portfolio.positions[t] = Position(qty, avg, today)

    def submit(self, orders: list[Order]) -> list[dict]:
        out = []
        for o in orders:
            if o.qty == 0:
                continue
            body = {"symbol": o.ticker, "qty": str(abs(o.qty)), "side": "buy" if o.qty > 0 else "sell",
                    "type": "market", "time_in_force": "opg",
                    "client_order_id": f"mf-{o.ticker}-{datetime.now(timezone.utc):%Y%m%d%H%M%S}"}
            try:
                out.append(self._req("POST", "/v2/orders", body))
            except Exception as e:  # noqa: BLE001 - report and continue with the rest
                log.error("Alpaca rejected %s: %s", body, e)
        return out


def load_portfolio(path: str | Path, initial_cash: float) -> Portfolio:
    p = Path(path)
    if p.exists():
        return Portfolio.from_dict(json.loads(p.read_text()))
    return Portfolio(initial_cash)


def save_portfolio(portfolio: Portfolio, path: str | Path) -> None:
    Path(path).write_text(json.dumps(portfolio.to_dict(), indent=2))
