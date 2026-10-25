"""Tests for lab_research. Run: python3 -m unittest discover research/tests"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import pandas as pd  # noqa: E402

from lab_research import backtest, pnl, tca  # noqa: E402


def fills(rows):
    return pd.DataFrame(rows, columns=["exchange_ns", "symbol", "side", "qty", "price", "match",
                                       "liquidity", "user_ref", "bid", "ask"])


class PositionsTest(unittest.TestCase):
    def test_round_trip_earns_the_spread(self):
        f = fills([
            (1, "AAA", "B", 100, 100_000, 1, "A", 1, 100_000, 100_200),
            (2, "AAA", "S", 100, 100_200, 2, "A", 2, 100_000, 100_200),
            (3, "BBB", "S", 50, 50_000, 3, "A", 3, 49_900, 50_000),
        ])
        p = pnl.positions(f, {"BBB": 49_000}).set_index("symbol")
        self.assertEqual(p.loc["AAA", "shares"], 0)
        self.assertAlmostEqual(p.loc["AAA", "pnl"], 2.0)  # 100 shares x 2 cents
        self.assertEqual(p.loc["BBB", "shares"], -50)
        self.assertAlmostEqual(p.loc["BBB", "pnl"], 50 * 0.1)  # short 50 marked 10 cents lower


class ReconcileTest(unittest.TestCase):
    def test_finds_missing_and_different_fills(self):
        firm = fills([
            (1, "AAA", "B", 100, 100_000, 7, "A", 1, 0, 0),
            (2, "AAA", "B", 100, 100_000, 8, "A", 2, 0, 0),
            (3, "AAA", "S", 100, 100_100, 9, "A", 3, 0, 0),
        ])
        dc = pd.DataFrame([
            (1, "FIRM01", 1, "B", 100, "AAA", 100_000, 7, "A", 11),
            (3, "FIRM01", 3, "S", 200, "AAA", 100_100, 9, "A", 13),  # qty differs
            (4, "FIRM01", 4, "B", 100, "AAA", 100_000, 10, "A", 14),  # firm missed it
            (5, "FIRM02", 9, "S", 100, "AAA", 100_000, 8, "R", 15),   # other account
        ], columns=["exchange_ns", "account", "user_ref", "side", "qty", "symbol", "price",
                    "match", "liquidity", "order_ref"])
        r = pnl.reconcile(firm, dc, "FIRM01")
        self.assertEqual(r.matched, 1)
        self.assertEqual(list(r.only_firm["match"]), [8])
        self.assertEqual(list(r.only_exchange["match"]), [10])
        self.assertEqual(list(r.mismatched["match"]), [9])
        self.assertFalse(r.clean)


class TcaTest(unittest.TestCase):
    def test_capture_and_markout(self):
        market = pd.DataFrame([
            (0, "AAA", 100_000, 100, 100_200, 100),
            (5_000_000, "AAA", 99_800, 100, 100_000, 100),  # mid drops 2 cents
        ], columns=["feed_ns", "symbol", "bid", "bid_qty", "ask", "ask_qty"])
        f = fills([(1_000, "AAA", "B", 100, 100_000, 1, "A", 1, 100_000, 100_200)])
        # The fill's own market data update, stamped with the fill's time.
        market.loc[len(market)] = (1_000, "AAA", 99_900, 100, 100_200, 100)
        m = tca.markouts(f, market, horizons_ns=(1_000_000, 10_000_000))
        self.assertAlmostEqual(m.loc[0, "capture"], 100)        # bought 1 cent under the mid
        self.assertAlmostEqual(m.loc[0, "markout_1ms"], 50)     # the fill's own impact
        self.assertAlmostEqual(m.loc[0, "markout_10ms"], -100)  # then adverse selection
        s = tca.summary(m).set_index("symbol")
        self.assertAlmostEqual(s.loc["ALL", "capture_cents"], 1.0)

    def test_marks_use_the_last_mid(self):
        market = pd.DataFrame([
            (5, "AAA", 99_800, 100, 100_000, 100),
            (0, "AAA", 100_000, 100, 100_200, 100),  # out of order on purpose
        ], columns=["feed_ns", "symbol", "bid", "bid_qty", "ask", "ask_qty"])
        self.assertEqual(tca.last_mids(market), {"AAA": 99_900})


class BacktestTest(unittest.TestCase):
    def test_quote_matches_the_strategy(self):
        p = backtest.Params()
        self.assertEqual(backtest.quote(p, 10_000, 10_200, 0), (10_000, 100, 10_200, 100))
        self.assertEqual(backtest.quote(p, 10_000, 10_500, 0), (10_100, 100, 10_400, 100))
        self.assertEqual(backtest.quote(p, 10_000, 10_500, 200), (9_900, 100, 10_200, 100))
        self.assertEqual(backtest.quote(p, 10_000, 10_200, 1_000)[1], 0)

    def test_touch_through_fills_and_pnl(self):
        tob = pd.DataFrame([
            (0, 10_000, 100, 10_200, 100),  # we quote 100.00 / 102.00
            (1, 9_800, 100, 10_000, 100),   # ask trades down to our bid: buy at 100.00
            (2, 10_200, 100, 10_400, 100),  # bid comes up to our ask: sell at 102.00
        ], columns=["feed_ns", "bid", "bid_qty", "ask", "ask_qty"])
        # No lean, and a hold wide enough that the ask stays put after the buy.
        r = backtest.run(tob, backtest.Params(skew_ticks=0, hold_ticks=2))
        self.assertEqual([(t[1], t[3]) for t in r.trades], [("B", 10_000), ("S", 10_200)])
        self.assertEqual(r.position, 0)
        self.assertAlmostEqual(r.pnl, 2.0)  # 100 shares x 2 cents

        # With a one-tick hold the ask follows the market down and sells flat.
        r = backtest.run(tob, backtest.Params(skew_ticks=0, hold_ticks=1))
        self.assertEqual([(t[1], t[3]) for t in r.trades], [("B", 10_000), ("S", 10_000)])

    def test_queue_position_decides_the_fill(self):
        cols = ["feed_ns", "bid", "bid_qty", "ask", "ask_qty", "traded_bid", "traded_ask"]
        tob = pd.DataFrame([
            (0, 10_000, 300, 10_200, 300, 0, 0),    # join the bid behind 300 shares
            (1, 10_000, 100, 10_200, 300, 200, 0),  # 200 trade: 100 still ahead
            (2, 10_000, 50, 10_200, 300, 0, 0),     # half the level cancels: 50 ahead
            (3, 10_000, 40, 10_200, 300, 40, 0),    # 40 trade: 10 still ahead, no fill
            (4, 10_000, 0, 10_200, 300, 20, 0),     # 20 trade past the 10: filled
        ], columns=cols)
        r = backtest.run(tob, backtest.Params(skew_ticks=0))
        self.assertEqual([(t[0], t[1], t[3]) for t in r.trades], [(4, "B", 10_000)])

    def test_sweep_returns_every_combination(self):
        tob = pd.DataFrame([(i, 10_000, 1, 10_200, 1) for i in range(5)],
                           columns=["feed_ns", "bid", "bid_qty", "ask", "ask_qty"])
        res = backtest.sweep(tob, {"hold_ticks": [0, 1], "skew_ticks": [0, 1, 2]})
        self.assertEqual(len(res), 6)


if __name__ == "__main__":
    unittest.main()
