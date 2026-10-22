"""KnowledgeGraph: tickers, sectors, themes and peer edges (DESIGN.md 5.1).

MiroFish seeds a graph from the source material and lets agents reason over
it. Here the graph is the market's structure: which names share a sector,
which are peers or supply-chain links, and how exposed each sector is to each
policy or macro theme. The sentiment layer uses it for spillover; the swarm
uses it to set how far "fair value" moves for a name.
"""

from __future__ import annotations

from collections import defaultdict

from .. import universe

# How much of a direct shock reads across to a peer, by event type.
READ_ACROSS = {
    "earnings_beat": 0.35, "earnings_miss": 0.35, "guidance_raise": 0.4, "guidance_cut": 0.4,
    "earnings": 0.2, "regulatory_action": 0.3, "product_launch": 0.1, "product_issue": 0.15,
    "upgrade": 0.1, "downgrade": 0.1, "mna_target": 0.25, "fda_approval": 0.1,
    "fda_rejection": 0.15, "fraud": 0.1, "macro_policy": 0.0,
}
DEFAULT_READ_ACROSS = 0.1


class KnowledgeGraph:
    def __init__(self, companies: dict[str, universe.Company] | None = None):
        self.companies = companies or universe.UNIVERSE
        self.edges: dict[str, dict[str, float]] = defaultdict(dict)
        for c in self.companies.values():
            for p in c.peers:
                if p in self.companies:
                    self._link(c.ticker, p, 1.0)
        by_sector: dict[str, list[str]] = defaultdict(list)
        for c in self.companies.values():
            by_sector[c.sector].append(c.ticker)
        for names in by_sector.values():
            for a in names:
                for b in names:
                    if a != b:
                        self._link(a, b, 0.6)

    def _link(self, a: str, b: str, w: float) -> None:
        self.edges[a][b] = max(self.edges[a].get(b, 0.0), w)
        self.edges[b][a] = max(self.edges[b].get(a, 0.0), w)

    def neighbours(self, ticker: str) -> dict[str, float]:
        return dict(self.edges.get(ticker, {}))

    def read_across(self, ticker: str, event_type: str) -> dict[str, float]:
        """peer -> fraction of ``ticker``'s shock that reaches it."""
        k = READ_ACROSS.get(event_type, DEFAULT_READ_ACROSS)
        if k <= 0:
            return {}
        return {p: k * w for p, w in self.neighbours(ticker).items()}

    def theme_exposure(self, ticker: str, themes: dict[str, float]) -> float:
        c = self.companies.get(ticker)
        if c is None or not themes:
            return 0.0
        return sum(shock * c.exposure(t) for t, shock in themes.items())

    def exposed(self, themes: dict[str, float], min_abs: float = 0.2) -> dict[str, float]:
        """ticker -> signed theme shock, for every name exposed to ``themes``."""
        out = {}
        for t in self.companies:
            s = self.theme_exposure(t, themes)
            if abs(s) >= min_abs:
                out[t] = s
        return out
