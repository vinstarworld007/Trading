"""Data types shared by every layer (DESIGN.md section 3)."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from datetime import date, datetime


@dataclass
class NewsItem:
    ts: datetime  # timezone-aware, UTC
    source: str
    headline: str
    body: str = ""
    tickers: list[str] = field(default_factory=list)  # optional hint from the source
    id: str = ""

    def __post_init__(self) -> None:
        if not self.id:
            key = f"{self.ts.isoformat()}|{self.source}|{self.headline}"
            self.id = hashlib.sha1(key.encode()).hexdigest()[:16]

    @property
    def text(self) -> str:
        return f"{self.headline}. {self.body}" if self.body else self.headline


@dataclass
class EngineView:
    """One analyst engine's reading of one item for one ticker."""

    engine: str
    score: float  # -1 bearish .. +1 bullish
    confidence: float  # 0 .. 1
    event_type: str = "general"
    magnitude: float = 0.01  # expected absolute move as a fraction (0.03 = 3%)
    horizon_days: int = 3
    themes: dict[str, float] = field(default_factory=dict)  # theme -> signed shock
    rationale: str = ""


@dataclass
class ItemAssessment:
    """Forum output for one (item, ticker)."""

    item: NewsItem
    ticker: str
    relevance: float
    views: list[EngineView]
    score: float
    confidence: float
    disagreement: float
    magnitude: float
    horizon_days: int
    event_type: str
    credibility: float = 1.0
    novelty: float = 1.0
    spillover: bool = False


@dataclass
class SentimentSignal:
    ticker: str
    date: date
    score: float  # tanh-squashed, -1 .. 1
    confidence: float
    magnitude: float  # expected absolute move as a fraction
    buzz: float  # attention vs the ticker's own trailing average, >= 0
    horizon_days: int
    assessments: list[ItemAssessment] = field(default_factory=list)

    @property
    def direct_items(self) -> list[ItemAssessment]:
        return [a for a in self.assessments if not a.spillover]


@dataclass
class SwarmForecast:
    ticker: str
    expected_move: float  # uncalibrated blend of continuation and gap, as a return
    sim_day_one: float
    sim_total: float
    p_up: float
    dispersion: float  # std of expected_move across seeds
    herding: float  # share of agents on the majority side at the end, 0.5 .. 1
    overshoot: float  # |peak| / |end|, >= 1 when the path overshoots
    continuation: float = 0.0  # simulated move after day one, as a return
    gap: float = 0.0  # simulated end state minus today's observed move (0 if unknown)
    calibrated_return: float = 0.0  # continuation and gap weighted by the Calibrator
    persona_flow: dict[str, float] = field(default_factory=dict)


@dataclass
class Decision:
    ticker: str
    alpha: float
    conviction: float
    target_weight: float
    reasons: list[str] = field(default_factory=list)
    signal: SentimentSignal | None = None
    forecast: SwarmForecast | None = None


@dataclass
class Order:
    ticker: str
    qty: int  # signed: + buy, - sell
    reason: str = ""


@dataclass
class Fill:
    ts: datetime
    ticker: str
    qty: int
    price: float
    fees: float
    reason: str = ""


@dataclass
class DecisionBatch:
    as_of: date
    decisions: list[Decision]
    orders: list[Order]
    exits: list[tuple[str, str]] = field(default_factory=list)  # (ticker, why)
    risk_notes: list[str] = field(default_factory=list)
    signals: dict[str, SentimentSignal] = field(default_factory=dict)
    forecasts: dict[str, SwarmForecast] = field(default_factory=dict)
