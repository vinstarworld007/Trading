"""Calibrator: maps the swarm's forecast to an expected return (DESIGN.md 5.5).

The swarm gives two readings of what is left to happen after the signal day:

- ``continuation``: the simulated move from the end of day one to the end of
  the simulation (drift if positive with the news, reversal if against it);
- ``gap``: the simulated end state minus the move the stock actually made
  today (positive means the market has not yet moved as far as the crowd
  would push it).

Simulation units are only roughly returns, and how much each reading is worth
is an empirical question, so the calibrator fits both weights: a ridge
regression of the realized forward idiosyncratic return on the two readings,
shrunk toward a prior and clamped at zero (a reading the data does not support
is switched off, never inverted). A sample enters the fit only once its
outcome window has closed, so a backtest never learns from the future.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date

import numpy as np


@dataclass
class Calibrator:
    prior: tuple[float, float] = (1.0, 0.5)
    prior_weight: float = 40.0  # samples' worth of belief in the prior
    max_weight: float = 3.0
    window: int = 750
    _pending: list[tuple[date, date, str, tuple[float, float]]] = field(default_factory=list)
    _samples: list[tuple[float, float, float]] = field(default_factory=list)
    _beta: tuple[float, float] | None = None

    def add_forecast(self, start: date, matures: date, ticker: str,
                     features: tuple[float, float]) -> None:
        self._pending.append((start, matures, ticker, features))

    def mature(self, today: date, realized) -> None:
        """Move forecasts whose window closed before ``today`` into the fit.

        ``realized(ticker, start, matures)`` returns the realized idiosyncratic
        return over the window, or ``None`` when the data is missing.
        """
        keep, added = [], False
        for s, m, t, f in self._pending:
            if m < today:
                r = realized(t, s, m)
                if r is not None:
                    self._samples.append((f[0], f[1], r))
                    added = True
            else:
                keep.append((s, m, t, f))
        self._pending = keep
        if len(self._samples) > self.window:
            self._samples = self._samples[-self.window:]
        if added:
            self._beta = None

    @property
    def weights(self) -> tuple[float, float]:
        if self._beta is not None:
            return self._beta
        if not self._samples:
            return self.prior
        a = np.asarray(self._samples)
        x, y = a[:, :2], a[:, 2]
        xtx = x.T @ x
        scale = self.prior_weight * np.diag(np.diag(xtx) / len(a) + 1e-12)
        beta = np.linalg.solve(xtx + scale, x.T @ y + scale @ np.asarray(self.prior))
        beta = np.clip(beta, 0.0, self.max_weight)
        self._beta = (float(beta[0]), float(beta[1]))
        return self._beta

    @property
    def n(self) -> int:
        return len(self._samples)

    def apply(self, features: tuple[float, float]) -> float:
        a, b = self.weights
        return a * features[0] + b * features[1]
