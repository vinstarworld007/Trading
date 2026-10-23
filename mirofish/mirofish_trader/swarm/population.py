"""Personas and the social graph (DESIGN.md 5.2).

Each persona is a set of behavioural parameters. ``build_population`` mixes
them for one ticker: retail-heavy, popular names get more retail agents and
denser herding; policy news brings in more macro analysts. Agents sit on a
Watts-Strogatz small-world graph, so opinions spread mostly locally with a few
long-range links.

Extension hook (DESIGN.md 5.6): ``Population.initial_belief`` can be seeded
from LLM persona role-play instead of zeros.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from ..universe import Company


@dataclass(frozen=True)
class Persona:
    name: str
    attention: float  # chance per round to notice the news
    skill: float  # 1 = reads the news without noise
    herding: float  # weight on neighbours' beliefs
    capital: float  # relative size
    momentum: float = 0.0  # chases the simulated price
    value: float = 0.0  # pulls toward fair value
    contrarian: float = 0.0  # fades the crowd
    macro_amp: float = 1.0  # amplifies policy and macro news
    fade: float = 0.05  # per round, how fast excitement beyond its own read of the news wears off
    provides_liquidity: bool = False


PERSONAS: dict[str, Persona] = {
    "retail": Persona("retail", 0.7, 0.3, 0.7, 1.0, momentum=0.9, fade=0.3),
    "institutional": Persona("institutional", 0.15, 0.85, 0.15, 3.0, momentum=0.1, value=0.35),
    "momentum": Persona("momentum", 0.4, 0.5, 0.3, 1.5, momentum=1.4, fade=0.3),
    "value": Persona("value", 0.5, 0.85, 0.05, 2.0, value=1.1),
    "contrarian": Persona("contrarian", 0.5, 0.6, 0.0, 1.0, contrarian=0.8),
    "macro": Persona("macro", 0.35, 0.75, 0.2, 1.5, value=0.2, macro_amp=1.6),
    "market_maker": Persona("market_maker", 1.0, 1.0, 0.0, 0.0, provides_liquidity=True),
}


def persona_mix(company: Company, buzz: float, macro_share: float) -> dict[str, float]:
    attention = min(1.5, 0.6 + 0.4 * buzz)
    mix = {
        "retail": (0.1 + 0.5 * company.retail_share) * (0.6 + 0.4 * company.popularity) * attention,
        "institutional": 0.4 * (1.0 - company.retail_share),
        "momentum": 0.12,
        "value": 0.12,
        "contrarian": 0.05,
        "macro": 0.03 + 0.2 * macro_share,
        "market_maker": 0.08,
    }
    total = sum(mix.values())
    return {k: v / total for k, v in mix.items()}


def small_world(n: int, k: int, rewire: float, rng: np.random.Generator) -> np.ndarray:
    """(n, k) neighbour indices of a Watts-Strogatz graph (directed for speed)."""
    offsets = np.concatenate([np.arange(1, k // 2 + 1), -np.arange(1, k - k // 2 + 1)])
    nbr = (np.arange(n)[:, None] + offsets[None, :]) % n
    mask = rng.random(nbr.shape) < rewire
    rnd = rng.integers(0, n - 1, size=nbr.shape)
    rnd = rnd + (rnd >= np.arange(n)[:, None])  # never self
    nbr[mask] = rnd[mask]
    return nbr


@dataclass
class Population:
    names: list[str]  # persona name per agent
    attention: np.ndarray
    skill: np.ndarray
    herding: np.ndarray
    capital: np.ndarray
    momentum: np.ndarray
    value: np.ndarray
    contrarian: np.ndarray
    macro_amp: np.ndarray
    fade: np.ndarray
    liquidity_share: float
    neighbours: np.ndarray
    initial_belief: np.ndarray

    @property
    def size(self) -> int:
        return len(self.names)


def build_population(company: Company, buzz: float, macro_share: float, n: int, k: int,
                     rewire: float, rng: np.random.Generator) -> Population:
    mix = persona_mix(company, buzz, macro_share)
    liquidity = mix.pop("market_maker")
    names: list[str] = []
    trading = sum(mix.values())
    for p, share in mix.items():
        names += [p] * int(round(n * share / trading))
    names = (names + ["institutional"] * n)[:n]
    rng.shuffle(names)
    # Retail agents cluster: sort them next to each other on the ring so the
    # lattice links retail to retail, then rewiring adds the long links.
    names.sort(key=lambda s: s != "retail")
    get = lambda attr: np.array([getattr(PERSONAS[s], attr) for s in names], dtype=float)  # noqa: E731
    attention = get("attention")
    attention[np.array([s == "retail" for s in names])] *= 0.6 + 0.6 * company.popularity
    return Population(
        names, np.clip(attention, 0.05, 1.0), get("skill"), get("herding"), get("capital"),
        get("momentum"), get("value"), get("contrarian"), get("macro_amp"), get("fade"), liquidity,
        small_world(n, k, rewire, rng), np.zeros(n))
