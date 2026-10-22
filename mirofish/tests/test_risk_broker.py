import unittest
from datetime import date, datetime, timezone

from mirofish_trader.broker import PaperBroker, Portfolio, Position
from mirofish_trader.config import RiskConfig
from mirofish_trader.models import Decision, Fill, Order
from mirofish_trader.risk import RiskManager

D = date(2026, 3, 4)
TS = datetime(2026, 3, 4, 14, 30, tzinfo=timezone.utc)
LIQ = {t: 1e9 for t in ["NVDA", "AMD", "AVGO", "MU", "QCOM", "INTC", "AAPL", "PEP"]}


def dec(t, w, conv=0.5):
    return Decision(t, w, conv, w)


class BrokerTest(unittest.TestCase):
    def test_accounting(self):
        p = Portfolio(100_000)
        b = PaperBroker(p)
        b.execute([Order("AAPL", 100)], {"AAPL": 200.0}, TS, D)
        self.assertEqual(p.positions["AAPL"].qty, 100)
        self.assertAlmostEqual(p.positions["AAPL"].avg_price, 200.1)  # 5 bps slippage
        self.assertAlmostEqual(p.equity({"AAPL": 200.1}), 100_000 - 1.0)  # min commission
        b.execute([Order("AAPL", -150)], {"AAPL": 210.0}, TS, D)
        self.assertEqual(p.positions["AAPL"].qty, -50)  # flipped short
        b.execute([Order("AAPL", 50)], {"AAPL": 205.0}, TS, D)
        self.assertNotIn("AAPL", p.positions)
        self.assertEqual(len(p.fills), 3)

    def test_round_trip_persistence(self):
        p = Portfolio(5_000.0, {"NVDA": Position(10, 100.0, D, 0.03)},
                      [Fill(TS, "NVDA", 10, 100.0, 1.0, "enter")], 1.0)
        q = Portfolio.from_dict(p.to_dict())
        self.assertEqual(q.positions["NVDA"], p.positions["NVDA"])
        self.assertEqual(q.fills[0].price, 100.0)


class RiskTest(unittest.TestCase):
    def test_caps(self):
        rm = RiskManager(RiskConfig(max_weight=0.1, max_sector=0.25))
        p = Portfolio(1_000_000)
        prices = {t: 100.0 for t in LIQ}
        res = rm.review(D, p, prices, [dec("NVDA", 0.3), dec("AMD", 0.1, 0.4), dec("MU", 0.1, 0.3),
                                       dec("PEP", 0.05, 0.2)], LIQ)
        self.assertEqual(res.targets["NVDA"], 0.1)
        self.assertEqual(res.targets["AMD"], 0.1)
        self.assertAlmostEqual(res.targets["MU"], 0.05)  # sector room left
        self.assertEqual(res.targets["PEP"], 0.05)

    def test_stop_and_max_hold(self):
        rm = RiskManager(RiskConfig(stop_vol_mult=2.0, max_hold_days=5))
        p = Portfolio(900_000, {"AAPL": Position(1000, 100.0, date(2026, 3, 3), 0.02),
                                "PEP": Position(1000, 100.0, date(2026, 2, 20), 0.02)})
        res = rm.review(D, p, {"AAPL": 95.0, "PEP": 100.5}, [], LIQ)
        why = dict(res.exits)
        self.assertIn("stop", why["AAPL"])
        self.assertIn("max hold", why["PEP"])

    def test_daily_loss_halt(self):
        rm = RiskManager(RiskConfig(daily_loss_halt=0.03))
        p = Portfolio(0.0, {"AAPL": Position(10_000, 100.0, D, 0.5)})
        rm.review(D, p, {"AAPL": 100.0}, [], LIQ)
        res = rm.review(date(2026, 3, 5), p, {"AAPL": 96.0}, [dec("NVDA", 0.05)], LIQ)
        self.assertEqual(res.targets, {"AAPL": 0.0})
        self.assertTrue(any("daily loss" in n for n in res.notes))

    def test_illiquid_rejected(self):
        res = RiskManager().review(D, Portfolio(1e6), {"AAPL": 100.0}, [dec("AAPL", 0.05)], {"AAPL": 1e5})
        self.assertEqual(res.targets, {})


if __name__ == "__main__":
    unittest.main()
