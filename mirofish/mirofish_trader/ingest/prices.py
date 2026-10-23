"""Daily bars: CSV load, Stooq download, and the return helpers every layer uses.

CSV format (long): ``date,ticker,open,high,low,close,volume``. The benchmark
(``QQQ``) must be in the file; idiosyncratic returns are taken against it.
"""

from __future__ import annotations

import io
import logging
import urllib.request
from datetime import date
from pathlib import Path

import numpy as np
import pandas as pd

from .. import universe

log = logging.getLogger(__name__)


class MarketData:
    def __init__(self, bars: pd.DataFrame):
        bars = bars.copy()
        bars["date"] = pd.to_datetime(bars["date"]).dt.date
        bars["ticker"] = bars["ticker"].str.upper()
        self.bars = bars.sort_values(["date", "ticker"])
        self.open = bars.pivot(index="date", columns="ticker", values="open").sort_index()
        self.close = bars.pivot(index="date", columns="ticker", values="close").sort_index()
        self.volume = bars.pivot(index="date", columns="ticker", values="volume").sort_index()
        self.sessions: list[date] = list(self.close.index)
        self._pos = {d: i for i, d in enumerate(self.sessions)}
        self.returns = self.close.pct_change()
        bench = universe.BENCHMARK
        if bench not in self.close:
            raise ValueError(f"price data has no {bench} benchmark")
        betas = pd.Series({t: (universe.get(t).beta if universe.get(t) else 1.0)
                           for t in self.close.columns})
        self.idio = self.returns - self.returns[[bench]].values * betas.values[None, :]
        self.vol20 = self.returns.rolling(20, min_periods=10).std()
        self.dollar_vol20 = (self.close * self.volume).rolling(20, min_periods=5).mean()

    @classmethod
    def from_csv(cls, path: str | Path) -> "MarketData":
        return cls(pd.read_csv(path))

    def tickers(self) -> list[str]:
        return list(self.close.columns)

    def index(self, d: date) -> int | None:
        return self._pos.get(d)

    def next_session(self, d: date, n: int = 1) -> date | None:
        i = self._pos.get(d)
        if i is None or i + n >= len(self.sessions):
            return None
        return self.sessions[i + n]

    def _get(self, frame: pd.DataFrame, ticker: str, d: date) -> float | None:
        try:
            v = frame.at[d, ticker]
        except KeyError:
            return None
        return None if pd.isna(v) else float(v)

    def px_open(self, ticker: str, d: date) -> float | None:
        return self._get(self.open, ticker, d)

    def px_close(self, ticker: str, d: date) -> float | None:
        return self._get(self.close, ticker, d)

    def idio_return(self, ticker: str, d: date) -> float | None:
        return self._get(self.idio, ticker, d)

    def daily_vol(self, ticker: str, d: date, default: float = 0.02) -> float:
        v = self._get(self.vol20, ticker, d)
        return max(0.005, v) if v is not None else default

    def dollar_volume(self, ticker: str, d: date) -> float | None:
        return self._get(self.dollar_vol20, ticker, d)

    def forward_idio(self, ticker: str, start: date, end: date) -> float | None:
        """Idiosyncratic return from ``start``'s open to ``end``'s close."""
        o, c = self.px_open(ticker, start), self.px_close(ticker, end)
        bo, bc = self.px_open(universe.BENCHMARK, start), self.px_close(universe.BENCHMARK, end)
        if None in (o, c, bo, bc) or o <= 0 or bo <= 0:
            return None
        beta = universe.get(ticker).beta if universe.get(ticker) else 1.0
        return (c / o - 1.0) - beta * (bc / bo - 1.0)

    def history_until(self, d: date) -> "MarketData":
        """Everything up to and including ``d`` (for no-lookahead checks)."""
        return MarketData(self.bars[self.bars["date"] <= d])


def fetch_stooq(tickers: list[str], timeout: float = 15.0) -> pd.DataFrame:
    """Daily bars from Stooq (free, no key). Missing tickers are skipped with a warning."""
    frames = []
    for t in tickers:
        url = f"https://stooq.com/q/d/l/?s={t.lower()}.us&i=d"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "mirofish-trader/0.1"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                text = r.read().decode()
            df = pd.read_csv(io.StringIO(text))
            if "Close" not in df:
                raise ValueError(text[:80])
            df = df.rename(columns=str.lower)
            df["ticker"] = t.upper()
            frames.append(df[["date", "ticker", "open", "high", "low", "close", "volume"]])
        except Exception as e:  # noqa: BLE001
            log.warning("stooq %s failed: %s", t, e)
    if not frames:
        raise RuntimeError("no price data downloaded")
    return pd.concat(frames, ignore_index=True)


def annualised_sharpe(daily: pd.Series) -> float:
    s = daily.std()
    return float(np.sqrt(252) * daily.mean() / s) if s and s > 0 else 0.0
