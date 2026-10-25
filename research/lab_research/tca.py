"""Transaction cost analysis for a market maker's fills.

For each fill, with side sign s (+1 buy, -1 sell) and the mid m0 of the
market the trader saw just before the fill:

- spread capture  s * (m0 - price): what the fill earned against the mid;
  positive for a passive fill at the quote
- markout at h    s * (m(t + h) - price): the same, h later; the drop from
  spread capture to markout is adverse selection, the market moving against
  the position the fill opened

All in Price(4) per share; ``summary`` converts to cents per share and to
basis points of the price.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from .pnl import signed_qty

DEFAULT_HORIZONS_NS = (1_000_000, 10_000_000, 100_000_000, 1_000_000_000)  # 1 ms .. 1 s


def mids(market: pd.DataFrame) -> pd.DataFrame:
    """Mid price per market update, where both sides exist."""
    m = market[(market["bid"] > 0) & (market["ask"] > market["bid"])]
    return m.assign(mid=(m["bid"] + m["ask"]) / 2)[["feed_ns", "symbol", "mid"]]


def last_mids(market: pd.DataFrame) -> dict[str, int]:
    """Each symbol's last mid of the session, in Price(4), for marking positions."""
    m = mids(market).sort_values("feed_ns", kind="stable")
    return {s: int(g["mid"].iloc[-1]) for s, g in m.groupby("symbol")}


def mid_at(mid: pd.DataFrame, symbol: str, times_ns: np.ndarray,
           strictly_before: bool = False) -> np.ndarray:
    """The last mid at or before each time (strictly before, if asked), NaN
    before the first. The exchange stamps a fill and the market data it causes
    with the same time, so the mid a fill traded against is strictly before."""
    s = mid[mid["symbol"] == symbol].sort_values("feed_ns", kind="stable")
    if s.empty:
        return np.full(len(times_ns), np.nan)
    side = "left" if strictly_before else "right"
    idx = np.searchsorted(s["feed_ns"].to_numpy(), times_ns, side=side) - 1
    vals = s["mid"].to_numpy()
    out = np.where(idx >= 0, vals[np.clip(idx, 0, None)], np.nan)
    return out


def markouts(fills: pd.DataFrame, market: pd.DataFrame,
             horizons_ns: tuple[int, ...] = DEFAULT_HORIZONS_NS) -> pd.DataFrame:
    """One row per fill with spread capture and markouts (Price(4) per share)."""
    m = mids(market)
    out = fills.copy()
    sign = np.where(out["side"] == "B", 1.0, -1.0)
    out["capture"] = np.nan
    for h in horizons_ns:
        out[f"markout_{h // 1_000_000}ms"] = np.nan
    for sym, rows in out.groupby("symbol"):
        t = rows["exchange_ns"].to_numpy()
        px = rows["price"].to_numpy(dtype=float)
        sg = sign[rows.index]
        out.loc[rows.index, "capture"] = sg * (mid_at(m, sym, t, strictly_before=True) - px)
        for h in horizons_ns:
            out.loc[rows.index, f"markout_{h // 1_000_000}ms"] = sg * (mid_at(m, sym, t + h) - px)
    return out


def summary(marked: pd.DataFrame) -> pd.DataFrame:
    """Share-weighted averages per symbol and overall, in cents per share."""
    cols = [c for c in marked.columns if c == "capture" or c.startswith("markout_")]
    rows = []
    for name, g in list(marked.groupby("symbol")) + [("ALL", marked)]:
        w = g["qty"].to_numpy(dtype=float)
        row = {"symbol": name, "fills": len(g), "shares": int(w.sum()),
               "net_shares": int(signed_qty(g).sum())}
        for c in cols:
            v = g[c].to_numpy(dtype=float)
            ok = ~np.isnan(v)
            row[f"{c}_cents"] = (v[ok] * w[ok]).sum() / w[ok].sum() / 100 if ok.any() else np.nan
        rows.append(row)
    return pd.DataFrame(rows)
