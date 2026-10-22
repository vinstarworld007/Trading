"""SwarmSimulation: how a crowd of traders reacts to a signal (DESIGN.md 5.3, 5.4).

Each run plays ``rounds`` rounds over ``seeds`` independent worlds at once
(numpy arrays are seeds x agents). Per round:

1. exposure: agents that have not seen the news notice it with probability
   attention x salience, and salience rises with buzz and with price moves;
2. opinion dynamics: Hegselmann-Krause bounded confidence over the social
   graph, weighted by each agent's herding;
3. persona rules: momentum chases the last price change, value pulls toward
   fair value, contrarians fade the crowd, macro analysts amplify policy news;
4. demand: each agent moves its position toward capital x tanh(k x belief),
   and the net order flow moves the price by price_impact / depth.

Price is in units of the news' expected magnitude: 1.0 means "moved by as much
as this kind of news usually moves a stock". The forecast is converted back to
a return by multiplying by the magnitude.
"""

from __future__ import annotations

import zlib
from datetime import date

import numpy as np

from .. import universe
from ..config import SwarmConfig
from ..models import SentimentSignal, SwarmForecast
from .graph import KnowledgeGraph
from .population import build_population


class SwarmSimulation:
    def __init__(self, cfg: SwarmConfig | None = None, graph: KnowledgeGraph | None = None):
        self.cfg = cfg or SwarmConfig()
        self.graph = graph or KnowledgeGraph()

    def _rng(self, ticker: str, day: date) -> np.random.Generator:
        return np.random.default_rng(zlib.crc32(f"{ticker}|{day}|{self.cfg.seed}".encode()))

    def run(self, signal: SentimentSignal, observed_move: float | None = None) -> SwarmForecast:
        """Simulate the crowd's reaction to ``signal``.

        ``observed_move`` is the stock's idiosyncratic return on the signal day,
        when known. It anchors the forecast: if the stock has already moved past
        where the swarm ends up, the forecast is a fade.
        """
        cfg = self.cfg
        company = universe.get(signal.ticker)
        if company is None:
            raise KeyError(signal.ticker)
        rng = self._rng(signal.ticker, signal.date)
        macro_items = [a for a in signal.assessments if a.event_type == "macro_policy"]
        macro_share = len(macro_items) / max(1, len(signal.assessments))
        pop = build_population(company, signal.buzz, macro_share, cfg.agents, cfg.neighbours,
                               cfg.rewire, rng)
        S, N = cfg.seeds, pop.size
        depth = 1.0 + 4.0 * pop.liquidity_share

        # Everything is in units of the expected move: fair = 1 means "this news
        # is worth a typical full-size move". The dynamics are close to linear
        # in fair, so the shape of the path (drift vs overshoot) depends on the
        # crowd, not on how strong the headline is.
        fair = float(np.clip(signal.score / cfg.full_move_score, -1.5, 1.5))
        unit = max(abs(fair), 0.3)
        amp = np.where(macro_share > 0, pop.macro_amp, 1.0)
        bias = rng.normal(0.0, 0.5, size=(S, N)) * (1.0 - pop.skill) * unit
        perceived = fair * amp + bias
        radius = cfg.confidence_radius * unit
        k = cfg.demand_gain

        exposed = np.zeros((S, N), dtype=bool)
        belief = np.broadcast_to(pop.initial_belief, (S, N)).copy()
        position = np.zeros((S, N))
        price = np.zeros(S)
        last_dp = np.zeros(S)
        path = np.zeros((cfg.rounds + 1, S))
        salience = min(1.0, 0.5 + 0.35 * signal.buzz) * (0.5 + 0.5 * signal.confidence)
        is_retail = np.array([n == "retail" for n in pop.names])
        vw_base = np.minimum(1.0, 0.5 * pop.value)
        lam = cfg.price_impact / (depth * pop.capital.sum())

        for r in range(1, cfg.rounds + 1):
            # 1. exposure: retail notices faster when the price is moving
            p_see = pop.attention * salience * (1.0 + 2.0 * np.abs(last_dp / unit)[:, None] * is_retail)
            newly = (~exposed) & (rng.random((S, N)) < np.clip(p_see, 0.0, 1.0))
            belief[newly] = perceived[newly]
            exposed |= newly

            # 2. bounded-confidence social update (Hegselmann-Krause)
            nb = belief[:, pop.neighbours]  # (S, N, k)
            nb_exp = exposed[:, pop.neighbours] & (np.abs(nb - belief[:, :, None]) < radius)
            cnt = nb_exp.sum(axis=2)
            nb_mean = np.where(cnt > 0, (nb * nb_exp).sum(axis=2) / np.maximum(cnt, 1), belief)
            social = pop.herding * (nb_mean - belief)
            # Unexposed agents can still be talked into a view by neighbours.
            belief = belief + np.where(exposed, social, 0.5 * social * (cnt > 0))

            # 3. persona rules. Only agents that read the news know fair value;
            # everyone sees the price. Value is proportional (buy below fair,
            # sell above), so it damps the path instead of ringing.
            crowd = (belief * exposed).sum(axis=1, keepdims=True) / np.maximum(
                exposed.sum(axis=1, keepdims=True), 1)
            vw = vw_base * exposed
            anchor = (fair + 1.5 * (fair - price))[:, None]
            belief = belief - pop.fade * (belief - perceived) * exposed
            belief = ((1.0 - vw) * belief + vw * anchor
                      + pop.momentum * 1.5 * last_dp[:, None]
                      - pop.contrarian * 0.3 * (crowd - price[:, None]) * exposed)
            belief = np.clip(belief, -3.0, 3.0)

            # 4. demand and price impact; agents trade halfway to target each round
            active = exposed | (np.abs(belief) > 0.05 * unit)
            target = pop.capital * np.tanh(k * belief) / k * active
            orders = 0.5 * (target - position)
            position += orders
            dp = lam * orders.sum(axis=1)
            price = price + dp
            last_dp = dp
            path[r] = price

        cut = cfg.day_one_rounds
        day_one, total = path[cut], path[-1]
        continuation = total - day_one
        mag = signal.magnitude
        if observed_move is not None and mag > 0:
            gap = total - observed_move / mag
        else:
            gap = np.zeros(S)
        b = cfg.observed_blend if observed_move is not None else 0.0
        move_units = (1.0 - b) * continuation + b * gap
        peak = np.abs(path).max(axis=0)
        overshoot = float(np.median(peak / np.maximum(np.abs(total), 1e-6)))
        maj = np.maximum((belief > 0).sum(axis=1), (belief < 0).sum(axis=1)) / N
        flow = {}
        for persona in sorted(set(pop.names)):
            m = np.array([n == persona for n in pop.names])
            flow[persona] = float(position[:, m].sum(axis=1).mean() / N)
        return SwarmForecast(
            ticker=signal.ticker,
            expected_move=float(move_units.mean() * mag),
            sim_day_one=float(day_one.mean() * mag),
            sim_total=float(total.mean() * mag),
            p_up=float((move_units > 0).mean()),
            dispersion=float(move_units.std() * mag),
            herding=float(maj.mean()),
            overshoot=min(overshoot, 10.0),
            continuation=float(continuation.mean() * mag),
            gap=float(gap.mean() * mag),
            persona_flow=flow,
        )
