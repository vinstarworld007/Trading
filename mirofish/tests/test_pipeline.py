import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from mirofish_trader.backtest import run_backtest
from mirofish_trader.config import Config
from mirofish_trader.ingest.news import bucket_by_session, save_jsonl, session_date
from mirofish_trader.ingest.prices import MarketData
from mirofish_trader.ingest.synthetic import generate
from mirofish_trader.paper import run_cycle


def small_config() -> Config:
    cfg = Config()
    cfg.swarm.agents, cfg.swarm.seeds = 120, 4
    return cfg


WORLD = generate(days=70, seed=3)
MARKET = MarketData(WORLD.bars)


class TimingTest(unittest.TestCase):
    def test_session_date(self):
        s = MARKET.sessions
        d = s[5]
        before = datetime(d.year, d.month, d.day, 19, 0, tzinfo=timezone.utc)  # 15:00 ET
        after = datetime(d.year, d.month, d.day, 21, 30, tzinfo=timezone.utc)  # 17:30 ET
        self.assertEqual(session_date(before, s), d)
        self.assertEqual(session_date(after, s), s[6])


class BacktestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.res = run_backtest(WORLD.news, MARKET, small_config())

    def test_trades_and_reconciles(self):
        res = self.res
        self.assertGreater(len(res.portfolio.fills), 10)
        last = MARKET.sessions[-1]
        closes = {t: MARKET.px_close(t, last) for t in res.portfolio.positions}
        self.assertAlmostEqual(res.portfolio.equity(closes), res.equity.iloc[-1], places=4)
        cash = small_config().initial_cash - sum(f.qty * f.price + f.fees for f in res.portfolio.fills)
        self.assertAlmostEqual(cash, res.portfolio.cash, places=4)
        ordered = {(b.as_of, o.ticker) for b in res.batches for o in b.orders}
        for f in res.portfolio.fills:
            prev = MARKET.sessions[MARKET.index(f.ts.date()) - 1]
            self.assertIn((prev, f.ticker), ordered)

    def test_no_lookahead(self):
        cut = MARKET.sessions[50]
        news = [n for n in WORLD.news if (s := session_date(n.ts, MARKET.sessions)) and s <= cut]
        trunc = run_backtest(news, MARKET.history_until(cut), small_config())
        full = {b.as_of: [(o.ticker, o.qty) for o in b.orders] for b in self.res.batches}
        part = {b.as_of: [(o.ticker, o.qty) for o in b.orders] for b in trunc.batches}
        self.assertTrue(part)
        for d, orders in part.items():
            self.assertEqual(orders, full[d], d)


class PaperTest(unittest.TestCase):
    def test_two_cycles_fill_pending_orders(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            news = tmp / "news.jsonl"
            save_jsonl(WORLD.news, news)
            buckets = bucket_by_session(WORLD.news, MARKET.sessions)
            busy = [d for d in MARKET.sessions[25:-2] if len(buckets.get(d, [])) >= 4]
            day = busy[0]
            nxt = MARKET.next_session(day)
            for d in (day, nxt):
                csv = tmp / f"prices-{d}.csv"
                MARKET.history_until(d).bars.to_csv(csv, index=False)
                report = run_cycle(tmp / "state", small_config(), str(csv), str(news))
                self.assertIn(f"Paper cycle {d}", report)
            self.assertTrue((tmp / "state" / "portfolio.json").exists())
            self.assertTrue((tmp / "state" / "reports" / f"paper-{nxt}.md").exists())
            again = run_cycle(tmp / "state", small_config(), str(csv), str(news))
            self.assertIn("Already ran", again)


if __name__ == "__main__":
    unittest.main()
