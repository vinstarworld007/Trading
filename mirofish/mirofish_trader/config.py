"""Every tunable, one dataclass per layer. Defaults follow DESIGN.md."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field, fields, is_dataclass
from pathlib import Path


@dataclass
class SentimentConfig:
    engine_weights: dict[str, float] = field(
        default_factory=lambda: {"lexicon": 1.0, "event": 1.2, "macro": 1.0, "claude": 2.0}
    )
    source_credibility: dict[str, float] = field(
        default_factory=lambda: {
            "sec": 1.0, "reuters": 1.0, "bloomberg": 1.0, "dowjones": 1.0, "ap": 0.95,
            "wsj": 0.9, "cnbc": 0.85, "nasdaq": 0.85, "yahoo": 0.8, "marketwatch": 0.8,
            "businesswire": 0.6, "prnewswire": 0.6, "globenewswire": 0.6,
            "seekingalpha": 0.5, "reddit": 0.4, "twitter": 0.4, "x": 0.4,
        }
    )
    default_credibility: float = 0.7
    novelty_jaccard: float = 0.6  # above this a headline counts as a rewrite
    novelty_lookback_days: int = 3
    spillover_weight: float = 0.5  # peer read-across vs a direct mention
    theme_weight: float = 0.8  # policy/macro theme reach vs a direct mention
    min_abs_score: float = 0.05  # drop signals weaker than this
    buzz_lookback_days: int = 20


@dataclass
class SwarmConfig:
    agents: int = 300
    rounds: int = 12
    day_one_rounds: int = 4
    seeds: int = 16
    neighbours: int = 6  # Watts-Strogatz k
    rewire: float = 0.1
    confidence_radius: float = 0.5  # Hegselmann-Krause epsilon
    price_impact: float = 1.3  # lambda; about depth, so price ~ the crowd's mean belief
    demand_gain: float = 0.4  # position = capital * tanh(k * belief) / k
    full_move_score: float = 0.6  # sentiment score that means a typical full-size move
    observed_blend: float = 0.5  # weight on (sim_total - observed) in expected_move
    seed: int = 7


@dataclass
class DecisionConfig:
    w_sentiment: float = 0.4
    w_swarm: float = 0.6
    disagree_factor: float = 0.5
    dispersion_penalty: float = 0.5
    entry_threshold: float = 0.12
    flip_threshold: float = 0.2
    risk_budget: float = 0.004  # target weight = alpha * risk_budget / daily_vol
    allow_short: bool = True


@dataclass
class RiskConfig:
    max_weight: float = 0.10
    max_gross: float = 1.0
    max_net: float = 0.6
    max_sector: float = 0.3
    max_positions: int = 12
    stop_vol_mult: float = 2.5
    take_profit_vol_mult: float = 4.0
    max_hold_days: int = 5
    daily_loss_halt: float = 0.03
    drawdown_halt: float = 0.15
    min_price: float = 5.0
    min_dollar_volume: float = 20e6


@dataclass
class ExecutionConfig:
    slippage_bps: float = 5.0
    commission_per_share: float = 0.005
    min_commission: float = 1.0


@dataclass
class Config:
    sentiment: SentimentConfig = field(default_factory=SentimentConfig)
    swarm: SwarmConfig = field(default_factory=SwarmConfig)
    decision: DecisionConfig = field(default_factory=DecisionConfig)
    risk: RiskConfig = field(default_factory=RiskConfig)
    execution: ExecutionConfig = field(default_factory=ExecutionConfig)
    initial_cash: float = 1_000_000.0

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2)

    @classmethod
    def load(cls, path: str | Path | None) -> "Config":
        cfg = cls()
        if path:
            _merge(cfg, json.loads(Path(path).read_text()))
        return cfg


def _merge(obj, overrides: dict) -> None:
    names = {f.name for f in fields(obj)}
    for key, value in overrides.items():
        if key not in names:
            raise KeyError(f"unknown config key {key!r} in {type(obj).__name__}")
        current = getattr(obj, key)
        if is_dataclass(current) and isinstance(value, dict):
            _merge(current, value)
        elif isinstance(current, dict) and isinstance(value, dict):
            current.update(value)
        else:
            setattr(obj, key, value)
