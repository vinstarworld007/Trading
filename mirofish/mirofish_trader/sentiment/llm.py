"""ClaudeEngine: an optional LLM analyst in the forum (DESIGN.md 4.5).

Claude reads each item and answers in a fixed JSON schema (structured
outputs), so every response parses. Answers are cached on disk by a hash of
the text, which keeps backtests reproducible and makes a re-run free. Any API
failure returns ``None`` and the deterministic engines carry the signal.

Needs ``pip install anthropic`` and credentials (``ANTHROPIC_API_KEY`` or an
``ant auth login`` profile).
"""

from __future__ import annotations

import hashlib
import json
import logging
import threading
from pathlib import Path

from ..models import EngineView
from ..universe import THEMES, UNIVERSE

log = logging.getLogger(__name__)

MODEL = "claude-opus-5-5"

EVENT_TYPES = [
    "earnings_beat", "earnings_miss", "earnings", "guidance_raise", "guidance_cut", "upgrade",
    "downgrade", "mna_target", "mna_acquirer", "buyback", "product_launch", "product_issue",
    "litigation", "regulatory_action", "fda_approval", "fda_rejection", "exec_change", "layoffs",
    "fraud", "macro_policy", "general",
]

SCHEMA = {
    "type": "object",
    "properties": {
        "tickers": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "ticker": {"type": "string", "enum": sorted(UNIVERSE)},
                    "score": {"type": "number"},
                    "confidence": {"type": "number"},
                },
                "required": ["ticker", "score", "confidence"],
                "additionalProperties": False,
            },
        },
        "event_type": {"type": "string", "enum": EVENT_TYPES},
        "magnitude_pct": {"type": "number"},
        "horizon_days": {"type": "integer"},
        "themes": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "theme": {"type": "string", "enum": list(THEMES)},
                    "shock": {"type": "number"},
                },
                "required": ["theme", "shock"],
                "additionalProperties": False,
            },
        },
        "rationale": {"type": "string"},
    },
    "required": ["tickers", "event_type", "magnitude_pct", "horizon_days", "themes", "rationale"],
    "additionalProperties": False,
}

SYSTEM = f"""You are an equity news analyst on a Nasdaq desk. For each news item, judge how it
moves the stocks in this universe over the next few sessions: {", ".join(sorted(UNIVERSE))}.

- tickers: only names the item materially affects, directly or through its sector. score is
  -1 (very bearish) to 1 (very bullish) for that stock; confidence 0 to 1 is how sure you are
  the market reads it that way. Leave the list empty if the item is noise.
- event_type: the single best label.
- magnitude_pct: the typical absolute move in percent for a stock directly hit by this kind of
  news (for example 3.5 for an earnings beat at a large cap).
- horizon_days: sessions until the news is fully priced, 1 to 10.
- themes: policy or macro themes the item shocks, with shock -1 to 1 (positive = good for
  exposed companies, such as tariff relief or rate cuts).
- rationale: one sentence.

Judge the news as written; do not speculate beyond it. Headlines that only restate an old
story deserve low confidence."""


class ClaudeEngine:
    name = "claude"

    def __init__(self, cache_path: str | Path = ".cache/claude_sentiment.jsonl", client=None,
                 model: str = MODEL):
        self.cache_path = Path(cache_path)
        self.model = model
        self._client = client
        self._cache: dict[str, dict] = {}
        self._lock = threading.Lock()
        if self.cache_path.exists():
            for line in self.cache_path.read_text().splitlines():
                if line.strip():
                    row = json.loads(line)
                    self._cache[row["key"]] = row["answer"]

    @property
    def client(self):
        if self._client is None:
            import anthropic  # optional dependency

            self._client = anthropic.Anthropic()
        return self._client

    def _key(self, text: str) -> str:
        return hashlib.sha256(f"{self.model}|{text}".encode()).hexdigest()[:24]

    def _ask(self, text: str) -> dict | None:
        import anthropic

        try:
            response = self.client.beta.messages.create(
                model=self.model,
                max_tokens=2048,
                system=SYSTEM,
                messages=[{"role": "user", "content": f"News item:\n{text}"}],
                output_config={"effort": "low", "format": {"type": "json_schema", "schema": SCHEMA}},
                betas=["server-side-fallback-2026-07-01"],
                fallbacks="default",
            )
        except anthropic.RateLimitError as e:
            log.warning("Claude rate limited after retries: %s", e)
            return None
        except anthropic.APIStatusError as e:
            log.warning("Claude API error %s: %s", e.status_code, e.message)
            return None
        except anthropic.APIConnectionError as e:
            log.warning("Claude unreachable: %s", e)
            return None
        if response.stop_reason in ("refusal", "max_tokens"):
            log.warning("Claude stopped with %s; skipping item", response.stop_reason)
            return None
        body = next((b.text for b in response.content if b.type == "text"), None)
        return json.loads(body) if body else None

    def analyse(self, text: str) -> dict | None:
        key = self._key(text)
        with self._lock:
            if key in self._cache:
                return self._cache[key]
        answer = self._ask(text)
        if answer is None:
            return None
        with self._lock:
            self._cache[key] = answer
            self.cache_path.parent.mkdir(parents=True, exist_ok=True)
            with self.cache_path.open("a") as f:
                f.write(json.dumps({"key": key, "answer": answer}) + "\n")
        return answer

    def views(self, text: str) -> dict[str, EngineView]:
        """ticker -> view; empty when Claude is unavailable or sees no effect."""
        a = self.analyse(text)
        if not a:
            return {}
        themes = {t["theme"]: max(-1.0, min(1.0, t["shock"])) for t in a.get("themes", [])}
        mag = max(0.002, min(0.25, a.get("magnitude_pct", 1.0) / 100))
        horizon = max(1, min(10, int(a.get("horizon_days", 3))))
        out = {}
        for t in a.get("tickers", []):
            out[t["ticker"]] = EngineView(
                self.name, max(-1.0, min(1.0, t["score"])), max(0.0, min(1.0, t["confidence"])),
                a.get("event_type", "general"), mag, horizon, themes, a.get("rationale", ""))
        return out
