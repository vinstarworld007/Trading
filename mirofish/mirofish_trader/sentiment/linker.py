"""EntityLinker: which universe tickers a news item is about (DESIGN.md 4.1).

Names and aliases match case-sensitively on word boundaries (``Meta`` the
company, not "meta" the word); bare symbols match only in upper case, and
``$NVDA`` / ``(NASDAQ: NVDA)`` cashtags always match. The first company named
in the headline is the subject (relevance 1.0); others are secondary.
"""

from __future__ import annotations

import re

from .. import universe
from ..models import NewsItem

_CASHTAG = re.compile(r"\$([A-Z]{1,5})\b|\((?:NASDAQ|Nasdaq|NasdaqGS|NYSE)\s*:\s*([A-Z]{1,5})\)")


class EntityLinker:
    def __init__(self, companies: dict[str, universe.Company] | None = None):
        self.companies = companies or universe.UNIVERSE
        self._patterns: list[tuple[str, re.Pattern]] = []
        for c in self.companies.values():
            names = sorted({c.name, *c.aliases}, key=len, reverse=True)
            alt = "|".join(re.escape(n) for n in names)
            self._patterns.append((c.ticker, re.compile(rf"(?<![\w$])(?:{alt})(?:'s)?(?!\w)")))
            self._patterns.append((c.ticker, re.compile(rf"(?<![\w$]){re.escape(c.ticker)}(?![\w])")))

    def _positions(self, text: str) -> dict[str, int]:
        first: dict[str, int] = {}
        for m in _CASHTAG.finditer(text):
            t = m.group(1) or m.group(2)
            if t in self.companies:
                first[t] = min(first.get(t, m.start()), m.start())
        for ticker, pat in self._patterns:
            m = pat.search(text)
            if m:
                first[ticker] = min(first.get(ticker, m.start()), m.start())
        return first

    def link(self, item: NewsItem) -> dict[str, float]:
        """ticker -> relevance in (0, 1]."""
        head = self._positions(item.headline)
        body = self._positions(item.body) if item.body else {}
        out: dict[str, float] = {}
        if head:
            subject = min(head, key=head.get)
            for t in head:
                out[t] = 1.0 if t == subject else 0.6
        for t in body:
            out.setdefault(t, 0.35)
        for t in item.tickers:
            t = t.upper()
            if t in self.companies:
                out[t] = max(out.get(t, 0.0), 0.8)
        return out
