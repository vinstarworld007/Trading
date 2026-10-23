"""Fuse the sentiment signal and the swarm forecast into a decision (DESIGN.md 6)."""

from __future__ import annotations

import math

from .config import DecisionConfig
from .models import Decision, SentimentSignal, SwarmForecast


def _sign(x: float) -> int:
    return (x > 0) - (x < 0)


def decide(signal: SentimentSignal, forecast: SwarmForecast, daily_vol: float,
           cfg: DecisionConfig | None = None) -> Decision:
    cfg = cfg or DecisionConfig()
    s = signal.score
    m = forecast.calibrated_return / max(daily_vol, 1e-4)
    alpha = cfg.w_sentiment * s + cfg.w_swarm * math.tanh(m)
    reasons = [f"sentiment {s:+.2f} (conf {signal.confidence:.2f})",
               f"swarm {forecast.calibrated_return:+.2%} expected ({m:+.2f} vol), p_up {forecast.p_up:.2f}"]
    agreement = 1.0
    if abs(m) > 0.05 and _sign(s) != _sign(m):
        agreement = cfg.disagree_factor
        reasons.append("sentiment and swarm disagree"
                       + (": swarm sees an overshoot to fade" if forecast.overshoot > 1.2 else ""))
    rel_disp = forecast.dispersion / (abs(forecast.expected_move) + forecast.dispersion + 1e-9)
    disp = 1.0 - cfg.dispersion_penalty * rel_disp
    conviction = abs(alpha) * agreement * (0.5 + 0.5 * signal.confidence) * disp
    target = 0.0
    if conviction >= cfg.entry_threshold:
        target = math.copysign(abs(alpha) * cfg.risk_budget / max(daily_vol, 1e-4), alpha)
        if target < 0 and not cfg.allow_short:
            target = 0.0
            reasons.append("short signal ignored (shorting off)")
    else:
        reasons.append(f"conviction {conviction:.2f} below entry {cfg.entry_threshold:.2f}")
    return Decision(signal.ticker, alpha, conviction, target, reasons, signal, forecast)
