"""LexiconEngine: finance-specific word and phrase sentiment (DESIGN.md 4.2).

The word lists follow the spirit of Loughran-McDonald: words that are neutral
in finance ("liability", "tax", "cost") are left out, and multi-word phrases
are scored before single words so "cuts guidance" is not read as "cuts".
"""

from __future__ import annotations

import math
import re

from ..models import EngineView

PHRASES: dict[str, float] = {
    "beats estimates": 1.5, "beat estimates": 1.5, "tops estimates": 1.5, "beats expectations": 1.5,
    "tops expectations": 1.5, "above expectations": 1.2, "better than expected": 1.3,
    "better-than-expected": 1.3, "misses estimates": -1.5, "missed estimates": -1.5,
    "misses expectations": -1.5, "below expectations": -1.2, "worse than expected": -1.3,
    "worse-than-expected": -1.3, "falls short": -1.2, "raises guidance": 1.6, "raised guidance": 1.6,
    "raises outlook": 1.5, "raises forecast": 1.5, "lifts forecast": 1.5, "boosts outlook": 1.5,
    "cuts guidance": -1.6, "cut guidance": -1.6, "lowers guidance": -1.6, "cuts outlook": -1.5,
    "lowers outlook": -1.5, "cuts forecast": -1.5, "slashes forecast": -1.8, "weak guidance": -1.5,
    "strong guidance": 1.5, "soft guidance": -1.2, "price target raised": 1.0,
    "raises price target": 1.0, "price target cut": -1.0, "cuts price target": -1.0,
    "lowers price target": -1.0, "upgraded to buy": 1.3, "downgraded to sell": -1.3,
    "record revenue": 1.3, "record quarter": 1.2, "share buyback": 0.9, "stock buyback": 0.9,
    "buyback program": 0.9, "dividend increase": 0.8, "raises dividend": 0.8,
    "export controls": -0.8, "export restrictions": -0.9, "antitrust lawsuit": -1.2,
    "class action": -0.8, "data breach": -1.1, "rate cut": 0.7, "rate cuts": 0.7,
    "rate hike": -0.7, "rate hikes": -0.7, "trade deal": 0.9, "trade truce": 0.9,
    "trade war": -1.0, "all-time high": 0.8, "52-week low": -0.8, "going concern": -2.0,
    "accounting irregularities": -2.0, "fda approval": 1.5, "fda approves": 1.5,
    "complete response letter": -1.5, "fda rejects": -1.6, "clinical hold": -1.4,
    "market share gains": 1.0, "loses market share": -1.0, "supply constraints": -0.6,
    "demand surge": 1.2, "demand slowdown": -1.1, "job cuts": -0.4, "cost cuts": 0.3,
}

WORDS: dict[str, float] = {
    # positive
    "beat": 1.0, "beats": 1.0, "surge": 1.2, "surges": 1.2, "soar": 1.3, "soars": 1.3,
    "jump": 1.0, "jumps": 1.0, "rally": 1.0, "rallies": 1.0, "gain": 0.7, "gains": 0.7,
    "record": 0.6, "strong": 0.8, "stronger": 0.8, "robust": 0.8, "upgrade": 1.0,
    "upgrades": 1.0, "upgraded": 1.0, "outperform": 0.9, "raises": 0.5, "growth": 0.5,
    "profit": 0.4, "profitable": 0.6, "exceeds": 0.9, "exceeded": 0.9, "approval": 0.9,
    "approves": 0.9, "approved": 0.9, "wins": 0.8, "win": 0.6, "boost": 0.7, "boosts": 0.7,
    "expands": 0.5, "expansion": 0.5, "breakthrough": 1.0, "accelerates": 0.7,
    "accelerating": 0.7, "partnership": 0.5, "acquire": 0.3, "acquires": 0.3,
    "buyout": 0.9, "optimistic": 0.7, "bullish": 0.9, "rebound": 0.7, "rebounds": 0.7,
    "climbs": 0.8, "rises": 0.6, "tops": 0.8, "launches": 0.4, "unveils": 0.4,
    "eases": 0.4, "easing": 0.4, "relief": 0.6, "exemption": 0.7, "exempt": 0.6,
    "settles": 0.3, "dismissed": 0.7, "cleared": 0.6, "momentum": 0.4,
    # negative
    "miss": -1.0, "misses": -1.0, "missed": -1.0, "plunge": -1.4, "plunges": -1.4,
    "plummet": -1.4, "plummets": -1.4, "tumble": -1.2, "tumbles": -1.2, "sink": -1.1,
    "sinks": -1.1, "slump": -1.1, "slumps": -1.1, "drop": -0.8, "drops": -0.8, "fall": -0.7,
    "falls": -0.7, "decline": -0.7, "declines": -0.7, "weak": -0.9, "weaker": -0.9,
    "weakness": -0.9, "downgrade": -1.0, "downgrades": -1.0, "downgraded": -1.0,
    "underperform": -0.9, "loss": -0.7, "losses": -0.8, "lawsuit": -0.9, "sued": -0.9,
    "sues": -0.8, "probe": -0.9, "investigation": -0.9, "investigates": -0.9,
    "subpoena": -1.0, "recall": -1.0, "recalls": -1.0, "fine": -0.7, "fined": -0.9,
    "penalty": -0.8, "ban": -1.0, "bans": -1.0, "banned": -1.0, "tariff": -0.6,
    "tariffs": -0.6, "sanctions": -0.8, "restrict": -0.7, "restricts": -0.7,
    "restrictions": -0.7, "warns": -1.0, "warning": -0.9, "halts": -0.9, "halted": -0.9,
    "layoffs": -0.5, "delay": -0.7, "delays": -0.7, "delayed": -0.7, "fraud": -1.6,
    "bankruptcy": -2.0, "default": -1.4, "slowdown": -0.9, "slows": -0.7, "concern": -0.5,
    "concerns": -0.5, "uncertainty": -0.5, "bearish": -0.9, "outage": -0.9,
    "breach": -1.0, "resigns": -0.7, "ousted": -0.8, "departure": -0.4, "shortage": -0.6,
    "crackdown": -1.0, "blocks": -0.8, "blocked": -0.8, "rejects": -1.0, "rejected": -1.0,
    "disappointing": -1.1, "disappoints": -1.1, "pressure": -0.4, "headwinds": -0.7,
    "antitrust": -0.6, "monopoly": -0.7, "slashes": -1.0, "cuts": -0.4, "hike": -0.4,
}

NEGATORS = {"not", "no", "never", "without", "fails", "failed", "unlikely", "avoids",
            "avoid", "denies"}
INTENSIFIERS = {"sharply": 1.5, "massive": 1.5, "huge": 1.4, "significantly": 1.3, "record": 1.2,
                "steep": 1.4, "deep": 1.3, "big": 1.2, "biggest": 1.4, "strongly": 1.3}
HEDGES = {"may", "might", "could", "reportedly", "rumor", "rumored", "considering", "weighs",
          "mulls", "possible", "potential", "sources"}

_TOKEN = re.compile(r"[a-z0-9][a-z0-9\-']*")


def tokenize(text: str) -> list[str]:
    return _TOKEN.findall(text.lower())


class LexiconEngine:
    name = "lexicon"

    def __init__(self, phrases: dict[str, float] | None = None, words: dict[str, float] | None = None):
        self.phrases = phrases or PHRASES
        self.words = words or WORDS
        self._max_phrase = max(len(p.split()) for p in self.phrases)

    def score_text(self, text: str) -> tuple[float, int, list[str]]:
        """(raw score, number of hits, matched terms)."""
        toks = tokenize(text)
        i, total, hits, matched = 0, 0.0, 0, []
        hedged = any(t in HEDGES for t in toks)
        while i < len(toks):
            w, span = None, 1
            for n in range(min(self._max_phrase, len(toks) - i), 1, -1):
                p = " ".join(toks[i:i + n])
                if p in self.phrases:
                    w, span = self.phrases[p], n
                    break
            if w is None:
                w = self.words.get(toks[i])
            if w is not None:
                window = toks[max(0, i - 3):i]
                if any(t in NEGATORS for t in window):
                    w = -0.7 * w
                if i > 0 and toks[i - 1] in INTENSIFIERS:
                    w *= INTENSIFIERS[toks[i - 1]]
                total += w
                hits += 1
                matched.append(" ".join(toks[i:i + span]))
            i += span
        if hedged:
            total *= 0.6
        return total, hits, matched

    def assess(self, text: str) -> EngineView:
        raw, hits, matched = self.score_text(text)
        if hits == 0:
            return EngineView(self.name, 0.0, 0.1, rationale="no sentiment terms")
        score = math.tanh(raw / math.sqrt(hits))
        confidence = min(0.9, 0.35 + 0.15 * hits) * (0.6 + 0.4 * abs(score))
        return EngineView(self.name, score, confidence, rationale=", ".join(matched[:6]))
