import tempfile
import unittest
from pathlib import Path

from mirofish_trader.models import NewsItem
from mirofish_trader.sentiment.forum import Forum
from mirofish_trader.sentiment.llm import SCHEMA, ClaudeEngine
from datetime import datetime, timezone

ANSWER = {"tickers": [{"ticker": "NVDA", "score": 0.7, "confidence": 0.8},
                      {"ticker": "AMD", "score": 0.3, "confidence": 0.5}],
          "event_type": "earnings_beat", "magnitude_pct": 4.0, "horizon_days": 3,
          "themes": [{"theme": "ai_capex", "shock": 0.5}], "rationale": "Beat on data center."}


class FakeEngine(ClaudeEngine):
    calls = 0

    def _ask(self, text):
        type(self).calls += 1
        return ANSWER


class ClaudeEngineTest(unittest.TestCase):
    def test_parse_cache_and_forum(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "c.jsonl"
            e = FakeEngine(cache_path=cache)
            v = e.views("Nvidia beats estimates")
            self.assertAlmostEqual(v["NVDA"].magnitude, 0.04)
            self.assertEqual(v["NVDA"].themes, {"ai_capex": 0.5})
            e.views("Nvidia beats estimates")
            self.assertEqual(FakeEngine.calls, 1)  # second read from memory
            again = FakeEngine(cache_path=cache)
            again.views("Nvidia beats estimates")
            self.assertEqual(FakeEngine.calls, 1)  # and from disk
            item = NewsItem(datetime(2026, 3, 4, 15, tzinfo=timezone.utc), "reuters", "Nvidia beats estimates")
            got = {a.ticker: a for a in Forum(claude=again).assess(item)}
            self.assertIn("claude", [x.engine for x in got["NVDA"].views])
            self.assertEqual(got["NVDA"].event_type, "earnings_beat")

    def test_schema_is_strict(self):
        self.assertFalse(SCHEMA["additionalProperties"])
        self.assertEqual(set(SCHEMA["required"]), set(SCHEMA["properties"]))


if __name__ == "__main__":
    unittest.main()
