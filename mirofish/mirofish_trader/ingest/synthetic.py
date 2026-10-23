"""A synthetic market for demos and tests: news plus prices that react to it.

The reaction model is planted on purpose, and it matches the hypothesis the
swarm encodes, so the demo shows the pipeline working end to end. It is not
evidence that the strategy works on real data.

- Each company event has a true impact (sign x magnitude). On the session the
  news lands, the stock moves by ``f`` of it, where ``f`` grows with the name's
  retail share; retail-heavy names overshoot (f > 1) and partly reverse,
  institutional names under-react and drift over the next three sessions.
- 15% of headlines are noise (no true impact), 10% of real events carry an
  ambiguous headline the engines cannot read, and some stories are re-posted
  by other outlets (the novelty filter should count them once).
- Policy and macro events shock themes; every exposed name moves.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date, datetime, time, timedelta

import numpy as np
import pandas as pd

from .. import universe
from ..models import NewsItem
from .news import NY, session_date

# (event_type, sign, magnitude, templates). {n} is the company name.
COMPANY_EVENTS = [
    ("earnings_beat", 1, 0.035, ["{n} beats estimates as revenue jumps on strong demand",
                                  "{n} quarterly profit tops expectations, shares climb",
                                  "{n} posts record revenue, beats consensus forecasts"]),
    ("earnings_miss", -1, 0.04, ["{n} misses estimates as sales decline",
                                  "{n} quarterly results fall short of expectations",
                                  "{n} reports disappointing quarter, profit misses consensus"]),
    ("guidance_raise", 1, 0.04, ["{n} raises full-year guidance on robust demand",
                                  "{n} lifts forecast after strong quarter"]),
    ("guidance_cut", -1, 0.045, ["{n} cuts guidance, warns of weaker demand",
                                  "{n} lowers outlook amid slowdown in orders"]),
    ("upgrade", 1, 0.018, ["Analyst upgrades {n} to buy, raises price target",
                            "{n} upgraded to outperform at major broker"]),
    ("downgrade", -1, 0.018, ["Analyst downgrades {n} to sell on valuation concerns",
                               "{n} downgraded, price target cut on weak outlook"]),
    ("product_launch", 1, 0.012, ["{n} unveils new AI product lineup", "{n} wins major contract"]),
    ("product_issue", -1, 0.02, ["{n} hit by widespread outage", "{n} recalls products over defect"]),
    ("regulatory_action", -1, 0.025, ["Regulators open antitrust probe into {n}",
                                       "{n} fined by EU regulators over competition rules"]),
    ("litigation", -1, 0.015, ["{n} sued in class action over disclosures"]),
    ("buyback", 1, 0.015, ["{n} announces $10 billion share buyback program"]),
    ("exec_change", -1, 0.03, ["{n} CFO resigns unexpectedly"]),
]
AMBIGUOUS = ["{n} shares in focus ahead of investor day", "{n} executives meet with investors",
             "What to watch for {n} this week"]
NOISE = ["{n} stock: is it a buy after the latest moves?", "{n} could rally, some traders say",
         "Three reasons {n} may climb"]

MACRO_EVENTS = [
    ({"export_controls": -1.0}, ["US to tighten export controls on advanced chips to China",
                                 "Commerce adds Chinese firms to entity list, expands chip curbs"]),
    ({"export_controls": 1.0}, ["US eases chip export restrictions in trade talks"]),
    ({"tariffs": -1.0}, ["White House imposes sweeping new tariffs on imports",
                         "Trade war escalates as retaliatory tariffs hit electronics"]),
    ({"tariffs": 1.0}, ["US and China reach trade truce, tariffs paused",
                        "Electronics granted tariff exemption"]),
    ({"rates": 1.0}, ["Fed cuts rates by 25 basis points, signals more easing",
                      "Cooler-than-expected inflation boosts rate cut bets"]),
    ({"rates": -1.0}, ["Hotter-than-expected CPI print sends yields surging",
                       "Powell signals hawkish stance, rate hikes on the table"]),
    ({"antitrust": -1.0}, ["DOJ sues to break up big tech platforms in landmark antitrust case"]),
    ({"drug_pricing": -1.0}, ["Medicare negotiation expands drug price caps to more medicines"]),
    ({"ai_capex": 1.0}, ["Hyperscalers boost AI spending plans, record capex expected"]),
    ({"ai_capex": -1.0}, ["Cloud giants pause data center build-outs as AI spending slows"]),
    ({"election": -1.0}, ["Government shutdown begins as Congress fails to pass budget"]),
    ({"consumer": -1.0}, ["Weak retail sales stoke recession fears"]),
]
SOURCES = ["reuters", "bloomberg", "cnbc", "wsj", "marketwatch", "yahoo", "businesswire"]


@dataclass
class SyntheticWorld:
    bars: pd.DataFrame
    news: list[NewsItem]
    events: pd.DataFrame  # ground truth: session, ticker, true_impact, kind


def _business_days(start: date, n: int) -> list[date]:
    out, d = [], start
    while len(out) < n:
        if d.weekday() < 5:
            out.append(d)
        d += timedelta(days=1)
    return out


def generate(days: int = 252, start: date = date(2025, 1, 6), events_per_day: float = 3.0,
             macro_per_day: float = 0.25, seed: int = 42) -> SyntheticWorld:
    rng = np.random.default_rng(seed)
    sessions = _business_days(start, days)
    names = universe.tickers()
    n_t, n_d = len(names), len(sessions)
    idx = {t: i for i, t in enumerate(names)}
    eff = np.zeros((n_d, n_t))  # event-driven returns by session
    news: list[NewsItem] = []
    truth = []

    def stamp(d: date) -> datetime:
        if rng.random() < 0.25:  # after the close: lands on the next session
            t = time(16, 5 + int(rng.integers(0, 50)))
        else:
            t = time(9 + int(rng.integers(0, 6)), int(rng.integers(0, 60)))
        return datetime.combine(d, t, NY)

    def apply(si: int, ti: int, impact: float, f: float) -> None:
        eff[si, ti] += f * impact
        rest = (1.0 - f) * impact
        for k in range(1, 4):
            if si + k < n_d:
                eff[si + k, ti] += rest / 3.0

    for di, d in enumerate(sessions):
        for _ in range(rng.poisson(events_per_day)):
            c = universe.UNIVERSE[names[int(rng.integers(n_t))]]
            kind, sign, mag, templates = COMPANY_EVENTS[int(rng.integers(len(COMPANY_EVENTS)))]
            ts = stamp(d)
            sd = session_date(ts, sessions)
            u = rng.random()
            if u < 0.15:
                headline, impact = NOISE[int(rng.integers(len(NOISE)))].format(n=c.name), 0.0
                kind = "noise"
            else:
                impact = sign * mag * rng.uniform(0.6, 1.4)
                headline = (AMBIGUOUS[int(rng.integers(len(AMBIGUOUS)))] if u < 0.25
                            else templates[int(rng.integers(len(templates)))]).format(n=c.name)
            src = SOURCES[int(rng.integers(len(SOURCES)))]
            news.append(NewsItem(ts, src, headline))
            if rng.random() < 0.3:  # re-posted by another outlet later
                news.append(NewsItem(ts + timedelta(minutes=int(rng.integers(5, 90))),
                                     SOURCES[int(rng.integers(len(SOURCES)))], f"UPDATE 1-{headline}"))
            if sd is None or impact == 0.0:
                continue
            f = 0.3 + 1.0 * c.retail_share + max(0.0, c.retail_share - 0.5) * 0.8
            apply(sessions.index(sd), idx[c.ticker], impact, f)
            truth.append((sd, c.ticker, impact, kind))

        for _ in range(rng.poisson(macro_per_day)):
            themes, templates = MACRO_EVENTS[int(rng.integers(len(MACRO_EVENTS)))]
            ts = stamp(d)
            sd = session_date(ts, sessions)
            news.append(NewsItem(ts, SOURCES[int(rng.integers(3))],
                                 templates[int(rng.integers(len(templates)))]))
            if sd is None:
                continue
            u = rng.uniform(0.6, 1.4)
            for t, c in universe.UNIVERSE.items():
                expo = sum(s * c.exposure(th) for th, s in themes.items())
                if abs(expo) < 0.2:
                    continue
                impact = 0.012 * expo * u
                apply(sessions.index(sd), idx[t], impact, 0.6)
                truth.append((sd, t, impact, "macro"))

    mkt = rng.normal(0.0004, 0.011, n_d)
    betas = np.array([universe.UNIVERSE[t].beta for t in names])
    noise = rng.normal(0.0, 0.014, (n_d, n_t))
    rets = mkt[:, None] * betas[None, :] + noise + eff
    # Macro events also move the benchmark through its average exposure.
    qqq = mkt + eff.mean(axis=1) * 0.5

    rows = []
    for t, r in [*zip(names, rets.T), (universe.BENCHMARK, qqq)]:
        px = float(rng.uniform(40, 500)) if t != universe.BENCHMARK else 450.0
        base_vol = float(rng.uniform(5e6, 6e7)) / px * 50
        for di, d in enumerate(sessions):
            prev = px
            px = prev * (1.0 + r[di])
            gap = 0.4 * r[di] + rng.normal(0, 0.002)
            o = prev * (1.0 + gap)
            hi = max(o, px) * (1 + abs(rng.normal(0, 0.004)))
            lo = min(o, px) * (1 - abs(rng.normal(0, 0.004)))
            v = base_vol * float(rng.lognormal(0, 0.3)) * (1 + 20 * abs(r[di]))
            rows.append((d.isoformat(), t, round(o, 4), round(hi, 4), round(lo, 4), round(px, 4),
                         int(v)))
    bars = pd.DataFrame(rows, columns=["date", "ticker", "open", "high", "low", "close", "volume"])
    events = pd.DataFrame(truth, columns=["session", "ticker", "true_impact", "kind"])
    return SyntheticWorld(bars, sorted(news, key=lambda i: i.ts), events)
