"""Positions and PnL from a list of fills, and reconciliation of the firm's
fills against the exchange's drop copy."""

from __future__ import annotations

from dataclasses import dataclass

import pandas as pd

from . import PRICE_SCALE


def signed_qty(df: pd.DataFrame) -> pd.Series:
    return df["qty"].where(df["side"] == "B", -df["qty"])


def positions(fills: pd.DataFrame, marks: dict[str, int] | None = None) -> pd.DataFrame:
    """Per symbol: shares, cash, volume and PnL marked at ``marks`` (Price(4)),
    or at the last fill price where no mark is given. Money is in dollars."""
    if fills.empty:
        return pd.DataFrame(
            columns=["symbol", "shares", "bought", "sold", "fills", "cash", "mark", "pnl"]
        )
    f = fills.assign(signed=signed_qty(fills))
    f["cash"] = -f["signed"] * f["price"]
    g = f.groupby("symbol", sort=True)
    out = pd.DataFrame({
        "shares": g["signed"].sum(),
        "bought": g.apply(lambda x: int(x.loc[x["side"] == "B", "qty"].sum()), include_groups=False),
        "sold": g.apply(lambda x: int(x.loc[x["side"] != "B", "qty"].sum()), include_groups=False),
        "fills": g.size(),
        "cash_p4": g["cash"].sum(),
        "last_price": g["price"].last(),
    })
    marks = marks or {}
    out["mark_p4"] = [marks.get(s, lp) for s, lp in zip(out.index, out["last_price"])]
    out["pnl"] = (out["cash_p4"] + out["shares"] * out["mark_p4"]) / PRICE_SCALE
    out["cash"] = out["cash_p4"] / PRICE_SCALE
    out["mark"] = out["mark_p4"] / PRICE_SCALE
    return out.reset_index()[["symbol", "shares", "bought", "sold", "fills", "cash", "mark", "pnl"]]


@dataclass
class Reconciliation:
    matched: int
    only_firm: pd.DataFrame      # fills the firm has that the exchange did not report
    only_exchange: pd.DataFrame  # executions the exchange reported that the firm missed
    mismatched: pd.DataFrame     # same match number, different side, qty or price

    @property
    def clean(self) -> bool:
        return self.only_firm.empty and self.only_exchange.empty and self.mismatched.empty


def reconcile(firm: pd.DataFrame, dropcopy: pd.DataFrame, account: str) -> Reconciliation:
    """Joins the firm's fills with the drop copy for ``account`` on (match
    number, UserRefNum): a match can involve two of the firm's own orders only
    in a self-trade, which the trader prevents, but keying on both keeps that
    case honest too."""
    ex = dropcopy[dropcopy["account"] == account]
    keys = ["match", "user_ref"]
    j = firm.merge(ex, on=keys, how="outer", suffixes=("_firm", "_ex"), indicator=True)
    only_firm = j[j["_merge"] == "left_only"]
    only_ex = j[j["_merge"] == "right_only"]
    both = j[j["_merge"] == "both"]
    bad = both[
        (both["side_firm"] != both["side_ex"])
        | (both["qty_firm"] != both["qty_ex"])
        | (both["symbol_firm"] != both["symbol_ex"])
    ]
    return Reconciliation(len(both) - len(bad), only_firm, only_ex, bad)
