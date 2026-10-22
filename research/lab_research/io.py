"""Loading the lab's CSV outputs into pandas DataFrames."""

from __future__ import annotations

import pandas as pd

FILL_COLUMNS = [
    "exchange_ns", "symbol", "side", "qty", "price", "match", "liquidity", "user_ref",
    "bid", "ask",
]
MARKET_COLUMNS = ["feed_ns", "symbol", "bid", "bid_qty", "ask", "ask_qty"]
DROPCOPY_COLUMNS = [
    "exchange_ns", "account", "user_ref", "side", "qty", "symbol", "price", "match",
    "liquidity", "order_ref",
]
TOB_COLUMNS = ["feed_ns", "bid", "bid_qty", "ask", "ask_qty"]

_INT = "int64"


def _read(path: str, columns: list[str], strings: tuple[str, ...]) -> pd.DataFrame:
    df = pd.read_csv(path, dtype={c: str for c in strings})
    missing = [c for c in columns if c not in df.columns]
    if missing:
        raise ValueError(f"{path}: missing columns {missing}")
    for c in columns:
        if c not in strings:
            df[c] = df[c].astype(_INT)
    return df[columns]


def read_fills(path: str) -> pd.DataFrame:
    """Fills as the trader's OMS saw them (``trader --fills-csv``)."""
    return _read(path, FILL_COLUMNS, ("symbol", "side", "liquidity"))


def read_market(path: str) -> pd.DataFrame:
    """Top of book the trader saw (``trader --market-csv``)."""
    return _read(path, MARKET_COLUMNS, ("symbol",))


def read_dropcopy(path: str) -> pd.DataFrame:
    """Executions as the exchange reported them (``dropcopy --csv``)."""
    return _read(path, DROPCOPY_COLUMNS, ("account", "side", "symbol", "liquidity"))


def read_tob(path: str) -> pd.DataFrame:
    """One symbol's top of book from an ITCH day (``itch-dump --tob-csv``),
    with the shares traded at the best bid and ask between rows."""
    return _read(path, TOB_COLUMNS + ["traded_bid", "traded_ask"], ())
