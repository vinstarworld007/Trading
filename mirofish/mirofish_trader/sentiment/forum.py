"""Forum and SignalAggregator (DESIGN.md 4.6, 4.7).

BettaFish's forum lets independent engines state a view and reconciles them.
Here the reconciliation is a confidence-weighted mean, and disagreement
between engines lowers the confidence of the result. The aggregator then
rolls a day's items up into one ``SentimentSignal`` per ticker, weighting each
item by source credibility and novelty (rewrites of the same story count once).
"""

from __future__ import annotations

import math
import re
from collections import defaultdict, deque
from datetime import date

from .. import universe
from ..config import SentimentConfig
from ..models import EngineView, ItemAssessment, NewsItem, SentimentSignal
from ..swarm.graph import KnowledgeGraph
from .events import EventEngine
from .lexicon import LexiconEngine, tokenize
from .linker import EntityLinker
from .macro import MacroPolicyEngine

_WIRE_PREFIX = re.compile(r"^\s*(?:UPDATE\s*\d*|BRIEF|RPT|CORRECTED|REFILE|EXCLUSIVE|BREAKING)\s*[-:]\s*", re.I)
_STOP = {"the", "a", "an", "of", "to", "in", "on", "for", "and", "as", "at", "by", "with", "its",
         "is", "are", "after", "from", "says", "said", "over", "amid"}


def _informative(v: EngineView) -> bool:
    return v.confidence > 0.1 and v.score != 0.0


def reconcile(views: list[EngineView], weights: dict[str, float]) -> tuple[float, float, float]:
    """(score, confidence, disagreement) from engine views."""
    vs = [v for v in views if _informative(v)]
    if not vs:
        return 0.0, 0.0, 0.0
    w = [weights.get(v.engine, 1.0) * v.confidence for v in vs]
    tw = sum(w)
    mean = sum(wi * v.score for wi, v in zip(w, vs)) / tw
    var = sum(wi * (v.score - mean) ** 2 for wi, v in zip(w, vs)) / tw
    disagreement = min(1.0, math.sqrt(var))
    conf = sum(wi * v.confidence for wi, v in zip(w, vs)) / tw
    # More independent engines agreeing is worth more than one engine alone.
    breadth = 1.0 - 0.5 ** len(vs)
    return mean, max(0.0, conf * breadth * (1.0 - disagreement)), disagreement


class Forum:
    def __init__(self, cfg: SentimentConfig | None = None, claude=None,
                 graph: KnowledgeGraph | None = None):
        self.cfg = cfg or SentimentConfig()
        self.linker = EntityLinker()
        self.lexicon = LexiconEngine()
        self.event = EventEngine()
        self.macro = MacroPolicyEngine()
        self.claude = claude
        self.graph = graph or KnowledgeGraph()

    def credibility(self, source: str) -> float:
        s = source.lower().strip()
        for key, c in self.cfg.source_credibility.items():
            if s == key or s.startswith(key + " ") or (len(key) > 3 and key in s):
                return c
        return self.cfg.default_credibility

    def assess(self, item: NewsItem) -> list[ItemAssessment]:
        text = item.text
        links = self.linker.link(item)
        lex = self.lexicon.assess(text)
        ev = self.event.assess(text)
        mac = self.macro.assess(text)
        cl = self.claude.views(text) if self.claude else {}
        for t in cl:
            links.setdefault(t, 0.7)
        cred = self.credibility(item.source)
        weights = self.cfg.engine_weights
        out: dict[str, ItemAssessment] = {}

        for ticker, relevance in links.items():
            company = universe.get(ticker)
            if company is None:
                continue
            views = [lex, ev]
            if mac.themes:
                views.append(self.macro.for_company(mac, company))
            if ticker in cl:
                views.append(cl[ticker])
            score, conf, dis = reconcile(views, weights)
            typed = [v for v in views if v.event_type not in ("general",) and v.confidence > 0.1]
            lead = max(typed, key=lambda v: (v.engine == "claude", v.confidence), default=ev)
            out[ticker] = ItemAssessment(item, ticker, relevance, views, score, conf * relevance, dis,
                                         lead.magnitude, lead.horizon_days, lead.event_type, cred)

        spill: dict[str, ItemAssessment] = {}
        k, kt = self.cfg.spillover_weight, self.cfg.theme_weight
        if mac.themes:
            for ticker in self.graph.exposed(mac.themes):
                if ticker in out:
                    continue
                v = self.macro.for_company(mac, universe.get(ticker))
                spill[ticker] = ItemAssessment(item, ticker, kt, [v], v.score, v.confidence * kt, 0.0,
                                               v.magnitude, v.horizon_days, v.event_type, cred,
                                               spillover=True)
        for src, a in list(out.items()):
            if a.relevance < 1.0 or abs(a.score) < 0.2:
                continue
            for peer, f in self.graph.read_across(src, a.event_type).items():
                if peer in out:
                    continue
                s = a.score * f
                prev = spill.get(peer)
                if prev is None or abs(s) > abs(prev.score):
                    spill[peer] = ItemAssessment(item, peer, k, a.views, s, a.confidence * k, 0.0,
                                                 a.magnitude * f, a.horizon_days, a.event_type, cred,
                                                 spillover=True)
        return list(out.values()) + list(spill.values())


class NewsMemory:
    """Recent headlines (for novelty) and daily mention counts (for buzz)."""

    def __init__(self, cfg: SentimentConfig):
        self.cfg = cfg
        self.recent: deque[tuple[date, str, frozenset[str]]] = deque()
        self.counts: dict[str, deque[tuple[date, int]]] = defaultdict(deque)

    @staticmethod
    def shingles(headline: str) -> frozenset[str]:
        return frozenset(t for t in tokenize(_WIRE_PREFIX.sub("", headline)) if t not in _STOP)

    def novelty(self, day: date, ticker: str, headline: str) -> float:
        sh = self.shingles(headline)
        if not sh:
            return 1.0
        best = 0.0
        for d, t, other in self.recent:
            if t != ticker or (day - d).days > self.cfg.novelty_lookback_days:
                continue
            j = len(sh & other) / len(sh | other)
            best = max(best, j)
        if best > self.cfg.novelty_jaccard:
            return 0.2
        return 1.0 - 0.5 * best

    def remember(self, day: date, ticker: str, headline: str) -> None:
        self.recent.append((day, ticker, self.shingles(headline)))
        while self.recent and (day - self.recent[0][0]).days > self.cfg.novelty_lookback_days + 7:
            self.recent.popleft()

    def buzz(self, day: date, ticker: str, today: int) -> float:
        hist = [c for d, c in self.counts[ticker] if d < day]
        base = sum(hist) / self.cfg.buzz_lookback_days if hist else 0.0
        return math.log1p(today / (base + 0.25))

    def record_counts(self, day: date, counts: dict[str, int]) -> None:
        for t, c in counts.items():
            q = self.counts[t]
            q.append((day, c))
            while q and (day - q[0][0]).days > self.cfg.buzz_lookback_days * 7 // 5 + 2:
                q.popleft()


class SignalAggregator:
    def __init__(self, cfg: SentimentConfig | None = None):
        self.cfg = cfg or SentimentConfig()
        self.memory = NewsMemory(self.cfg)

    def aggregate(self, day: date, assessments: list[ItemAssessment]) -> dict[str, SentimentSignal]:
        by_ticker: dict[str, list[ItemAssessment]] = defaultdict(list)
        for a in sorted(assessments, key=lambda a: a.item.ts):
            if not a.spillover:
                a.novelty = self.memory.novelty(day, a.ticker, a.item.headline)
                self.memory.remember(day, a.ticker, a.item.headline)
            by_ticker[a.ticker].append(a)

        counts = {t: sum(1 for a in v if not a.spillover) for t, v in by_ticker.items()}
        out: dict[str, SentimentSignal] = {}
        for ticker, items in by_ticker.items():
            raw, noisy_or, mag_w, mag, horizon = 0.0, 1.0, 0.0, 0.0, 1
            for a in items:
                w = a.confidence * a.credibility * a.novelty
                raw += a.score * w
                noisy_or *= 1.0 - min(0.95, w)
                mag += a.magnitude * w * abs(a.score)
                mag_w += w * abs(a.score)
                horizon = max(horizon, a.horizon_days)
            score = math.tanh(1.5 * raw)
            if abs(score) < self.cfg.min_abs_score:
                continue
            out[ticker] = SentimentSignal(
                ticker, day, score, 1.0 - noisy_or, mag / mag_w if mag_w else 0.01,
                self.memory.buzz(day, ticker, counts.get(ticker, 0)), horizon, items)
        self.memory.record_counts(day, counts)
        return out
