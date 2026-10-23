import unittest
from datetime import date

from mirofish_trader.config import SwarmConfig
from mirofish_trader.models import SentimentSignal
from mirofish_trader.swarm.calibration import Calibrator
from mirofish_trader.swarm.simulation import SwarmSimulation

D = date(2026, 3, 4)


def sig(ticker, score=0.6, mag=0.035):
    return SentimentSignal(ticker, D, score, 0.6, mag, 1.0, 4)


class SwarmTest(unittest.TestCase):
    def setUp(self):
        self.sim = SwarmSimulation(SwarmConfig(agents=200, seeds=8))

    def test_deterministic(self):
        a, b = self.sim.run(sig("NVDA")), self.sim.run(sig("NVDA"))
        self.assertEqual(a.sim_total, b.sim_total)

    def test_sign_follows_news(self):
        self.assertGreater(self.sim.run(sig("AAPL", 0.6)).sim_total, 0)
        self.assertLess(self.sim.run(sig("AAPL", -0.6)).sim_total, 0)

    def test_retail_names_overshoot_institutional_names_drift(self):
        tsla, pep = self.sim.run(sig("TSLA")), self.sim.run(sig("PEP"))
        self.assertGreater(tsla.sim_day_one, pep.sim_day_one)
        self.assertGreater(pep.continuation, tsla.continuation)
        self.assertGreater(tsla.overshoot, pep.overshoot)

    def test_gap_fades_an_overreaction(self):
        calm = self.sim.run(sig("AAPL"), observed_move=0.0)
        hot = self.sim.run(sig("AAPL"), observed_move=0.08)
        self.assertGreater(calm.gap, 0)
        self.assertLess(hot.gap, 0)


class CalibratorTest(unittest.TestCase):
    def test_prior_until_samples(self):
        c = Calibrator()
        self.assertEqual(c.weights, c.prior)

    def test_only_matured_samples(self):
        c = Calibrator(prior_weight=1.0)
        c.add_forecast(date(2026, 1, 2), date(2026, 1, 6), "AAPL", (0.01, 0.0))
        c.mature(date(2026, 1, 6), lambda *a: 0.03)  # window closes on the 6th: not yet
        self.assertEqual(c.n, 0)
        c.mature(date(2026, 1, 7), lambda *a: 0.03)
        self.assertEqual(c.n, 1)

    def test_learns_and_clamps(self):
        c = Calibrator(prior_weight=1.0)
        pairs = [(0.01 * ((i % 7) - 3), 0.01 * ((i % 5) - 2)) for i in range(200)]
        for f in pairs:
            c.add_forecast(date(2026, 1, 1), date(2026, 1, 2), "X", f)
        it = iter(pairs)

        def realized(ticker, start, matures):
            cont, gap = next(it)
            return 2.0 * cont - 1.0 * gap

        c.mature(date(2026, 2, 1), realized)
        a, b = c.weights
        self.assertAlmostEqual(a, 2.0, delta=0.2)
        self.assertEqual(b, 0.0)  # a negative weight clamps to zero, never inverts


if __name__ == "__main__":
    unittest.main()
