"""Tests for the option C colo model. Run: python3 -m unittest discover research/tests"""

import random
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import colo as colo_cli  # noqa: E402
from lab_research import colo  # noqa: E402
from lab_research.colo.components import Switch  # noqa: E402
from lab_research.colo.engine import Latency, Packet, Sim  # noqa: E402

TRADER_OUT = """trader FIRM01 ran 10.00 s on 5 symbols (500 listed on the feed)
Tick to wire             n=1834       min=2100 p50=4800 p90=9200 p99=31000 p99.9=88000 p99.99=140000 max=152000 ns
Decide to wire           n=1834       min=900 p50=2100 p90=4000 p99=12000 p99.9=40000 p99.99=60000 max=61000 ns
"""
SHORT = {"duration_ms": 3}


class Sink:
    def __init__(self, sim):
        self.sim, self.times = sim, []

    def send(self, pkt):
        self.times.append(self.sim.now)


def two_frames_through(switch_name):
    sim = Sim()
    sw = Switch(sim, "sw", colo.SWITCHES[switch_name], random.Random(1))
    sink = Sink(sim)
    sw.add_route("order", sink)
    for seq in range(2):
        sim.at(0.0, sw.receive, Packet("order", 125, 0, seq))  # 100 ns at 10 Gb/s
    sim.run()
    return sink.times


class SwitchTest(unittest.TestCase):
    def test_layer1_never_queues(self):
        a, b = two_frames_through("arista_7130_l1")
        self.assertLess(a, 6)
        self.assertLess(abs(a - b), 2)

    def test_metamux_serialises_simultaneous_orders(self):
        a, b = sorted(two_frames_through("arista_7130_metamux"))
        self.assertAlmostEqual(b - a, 100, delta=5)


class EmpiricalTest(unittest.TestCase):
    def test_draws_follow_the_percentiles(self):
        lat = Latency.of({"dist": "empirical", "mean": 50,
                          "points": [[0, 10], [50, 50], [99, 100], [100, 500]]})
        rng = random.Random(3)
        xs = sorted(lat.sample(rng) for _ in range(20000))
        self.assertGreaterEqual(xs[0], 10)
        self.assertLessEqual(xs[-1], 500)
        self.assertAlmostEqual(xs[len(xs) // 2], 50, delta=2)


class TraderLogTest(unittest.TestCase):
    def test_parses_tick_to_wire(self):
        t = colo.parse_tick_to_wire(TRADER_OUT)
        self.assertEqual((t["n"], t["p50"], t["p99.9"], t["max"]), (1834, 4800, 88000, 152000))

    def test_rejects_missing_or_empty(self):
        with self.assertRaises(ValueError):
            colo.parse_tick_to_wire("trader ran\n")
        with self.assertRaises(ValueError):
            colo.parse_tick_to_wire(TRADER_OUT.replace("n=1834", "n=0", 1))


class OptionCTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        t2w = colo.parse_tick_to_wire(TRADER_OUT)
        cls.c, cls.lab = (colo.run_scenario(s, SHORT) for s in colo.option_c_scenarios(t2w))

    def stage(self, r, name):
        return next(b["mean_ns"] for b in r["breakdown"] if b["stage"] == name)

    def test_option_c_path(self):
        self.assertGreater(self.c["orders"], 0)
        self.assertLess(self.stage(self.c, "MD switch"), 6)
        self.assertEqual(self.stage(self.c, "MD switch queue"), 0)
        self.assertAlmostEqual(self.stage(self.c, "Order switch"), 39, delta=3)
        # two 50 m cross-connects bound wire-to-wire from below
        self.assertGreater(self.c["wire_to_wire"]["min"], 490)
        self.assertLess(self.c["wire_to_wire"]["p50"], 5000)

    def test_lab_host_uses_measured_latency(self):
        measured = self.stage(self.lab, "lab trader: feed to order (measured)")
        self.assertTrue(4000 < measured < 12000, measured)  # mean of the measured percentiles
        self.assertGreater(self.lab["tick_to_trade"]["p50"], self.c["tick_to_trade"]["p50"] + 4000)
        self.assertEqual(self.lab["md_switch"], self.c["md_switch"])

    def test_same_ticks_in_both_runs(self):
        self.assertEqual(self.c["md_frames"], self.lab["md_frames"])


class CliTest(unittest.TestCase):
    def test_writes_json_and_page(self):
        with tempfile.TemporaryDirectory() as d:
            log, page, js = Path(d, "trader.out"), Path(d, "colo.html"), Path(d, "colo.json")
            log.write_text(TRADER_OUT)
            rc = colo_cli.main(["--trader-log", str(log), "--duration-ms", "2", "--no-sweep",
                                "--page", str(page), "--json", str(js)])
            self.assertEqual(rc, 0)
            self.assertIn('"id":"C-lab"', page.read_text())
            self.assertNotIn("__DATA__", page.read_text())
            self.assertIn('"trader_tick_to_wire"', js.read_text())


if __name__ == "__main__":
    unittest.main()
