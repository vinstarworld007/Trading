import unittest
from datetime import date, datetime, timezone

from mirofish_trader.models import NewsItem
from mirofish_trader.sentiment.events import EventEngine
from mirofish_trader.sentiment.forum import Forum, SignalAggregator, reconcile
from mirofish_trader.sentiment.lexicon import LexiconEngine
from mirofish_trader.sentiment.linker import EntityLinker
from mirofish_trader.sentiment.macro import MacroPolicyEngine
from mirofish_trader.models import EngineView
from mirofish_trader import universe

TS = datetime(2026, 3, 4, 15, 0, tzinfo=timezone.utc)


def item(headline, source="reuters", body=""):
    return NewsItem(TS, source, headline, body)


class LinkerTest(unittest.TestCase):
    def setUp(self):
        self.l = EntityLinker()

    def test_subject_and_secondary(self):
        links = self.l.link(item("Nvidia supplier deal lifts Micron shares"))
        self.assertEqual(links["NVDA"], 1.0)
        self.assertEqual(links["MU"], 0.6)

    def test_alias_and_cashtag(self):
        self.assertIn("AAPL", self.l.link(item("Tim Cook says iPhone demand is solid")))
        self.assertIn("AMD", self.l.link(item("Chip stocks rally; $AMD leads")))

    def test_case_sensitive_names(self):
        self.assertNotIn("META", self.l.link(item("A meta analysis of chip demand")))
        self.assertIn("META", self.l.link(item("Meta unveils new glasses")))


class LexiconTest(unittest.TestCase):
    def setUp(self):
        self.e = LexiconEngine()

    def test_phrases_beat_words(self):
        self.assertLess(self.e.assess("Apple cuts guidance").score, 0)
        self.assertGreater(self.e.assess("Apple raises guidance").score, 0)

    def test_negation(self):
        self.assertLess(self.e.assess("Intel fails to beat estimates").score, 0)

    def test_hedge_dampens(self):
        plain = self.e.assess("Tesla shares surge").score
        hedged = self.e.assess("Tesla shares could surge, sources say").score
        self.assertGreater(plain, hedged)

    def test_no_terms(self):
        v = self.e.assess("Apple to hold event on Tuesday")
        self.assertEqual(v.score, 0.0)


class EventTest(unittest.TestCase):
    def test_types(self):
        e = EventEngine()
        self.assertEqual(e.classify("Nvidia beats estimates on data center").event_type, "earnings_beat")
        self.assertEqual(e.classify("Adobe cuts full-year guidance").event_type, "guidance_cut")
        self.assertEqual(e.classify("FDA approves Moderna vaccine").event_type, "fda_approval")
        self.assertEqual(e.classify("Analyst downgrades Netflix to sell").event_type, "downgrade")
        self.assertIsNone(e.classify("Apple to hold event on Tuesday"))


class MacroTest(unittest.TestCase):
    def test_export_controls_hit_semis_not_staples(self):
        m = MacroPolicyEngine()
        v = m.assess("US to tighten export controls on advanced chips to China")
        self.assertLess(v.themes["export_controls"], 0)
        nvda = m.for_company(v, universe.get("NVDA"))
        pep = m.for_company(v, universe.get("PEP"))
        self.assertLess(nvda.score, -0.5)
        self.assertEqual(pep.score, 0.0)

    def test_rate_cut_is_positive(self):
        v = MacroPolicyEngine().assess("Fed cuts rates by 25 basis points")
        self.assertGreater(v.themes["rates"], 0)


class ForumTest(unittest.TestCase):
    def test_disagreement_lowers_confidence(self):
        agree = reconcile([EngineView("a", 0.8, 0.8), EngineView("b", 0.7, 0.8)], {})
        split = reconcile([EngineView("a", 0.8, 0.8), EngineView("b", -0.7, 0.8)], {})
        self.assertGreater(agree[1], split[1])
        self.assertGreater(split[2], agree[2])

    def test_macro_spills_to_exposed_names(self):
        f = Forum()
        got = {a.ticker: a for a in f.assess(item("US to tighten export controls on advanced chips to China"))}
        self.assertIn("NVDA", got)
        self.assertTrue(got["NVDA"].spillover)
        self.assertNotIn("PEP", got)

    def test_peer_read_across(self):
        got = {a.ticker: a for a in Forum().assess(item("Nvidia beats estimates as revenue surges"))}
        self.assertFalse(got["NVDA"].spillover)
        self.assertTrue(got["AMD"].spillover)
        self.assertLess(got["AMD"].score, got["NVDA"].score)

    def test_rewrites_count_once(self):
        f, agg = Forum(), SignalAggregator()
        a = f.assess(item("Nvidia beats estimates as revenue surges"))
        once = agg.aggregate(date(2026, 3, 4), a)["NVDA"].score
        agg2 = SignalAggregator()
        b = a + f.assess(item("UPDATE 1-Nvidia beats estimates as revenue surges", "cnbc"))
        twice = agg2.aggregate(date(2026, 3, 4), b)["NVDA"].score
        self.assertLess(twice - once, 0.1)

    def test_low_credibility_source(self):
        f = Forum()
        self.assertLess(f.credibility("reddit"), f.credibility("reuters"))
        self.assertEqual(f.credibility("foxbusiness"), f.cfg.default_credibility)


if __name__ == "__main__":
    unittest.main()
