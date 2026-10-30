"""Tests for research/quant_lab.py. Run: python3 -m unittest discover research/tests"""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import quant_lab  # noqa: E402


def run(seed, pnl=1.0, errors=0):
    summary = {"pnl": pnl, "sharpe": 1.5, "max_drawdown": -2.0}
    return {
        "meta": {"seed": seed, "integrity_errors": errors, "book_mismatches": 0, "oms_inconsistent": 0},
        "mm": {"summary": summary}, "pairs": {"summary": summary},
        "trend": {"summary": summary}, "fund": {"summary": summary},
    }


class QuantLabTest(unittest.TestCase):
    def test_parse_seeds(self):
        self.assertEqual(quant_lab.parse_seeds("1-3,7"), [1, 2, 3, 7])
        self.assertEqual(quant_lab.parse_seeds(""), [])

    def test_clean_needs_every_check_at_zero(self):
        self.assertTrue(quant_lab.clean(run(1)))
        self.assertFalse(quant_lab.clean(run(1, errors=1)))

    def test_seed_row_has_each_strategy(self):
        row = quant_lab.seed_row(run(4, pnl=-3.0))
        self.assertEqual(row["seed"], 4)
        self.assertEqual(set(row) - {"seed"}, {"mm", "arb", "trend", "fund"})
        self.assertEqual(row["trend"]["pnl"], -3.0)

    def test_page_embeds_json_that_cannot_close_the_script(self):
        data = run(3)
        data["meta"]["notes"] = ["</script><b>"]
        page = quant_lab.build_page("<x>/*LAB_DATA*/</x><y>/*SEED_DATA*/</y>", data, [])
        self.assertNotIn("</script>", page)
        inner = page.split("<x>")[1].split("</x>")[0]
        self.assertEqual(json.loads(inner)["meta"]["notes"], ["</script><b>"])

    def test_real_runs_have_no_engine_book_check(self):
        data = run(1)
        del data["meta"]["book_mismatches"]
        data["meta"]["mode"] = "real"
        self.assertTrue(quant_lab.clean(data))
        data["meta"]["oms_inconsistent"] = 2
        self.assertFalse(quant_lab.clean(data))

    def test_template_has_both_placeholders(self):
        t = quant_lab.TEMPLATE.read_text()
        self.assertIn("/*LAB_DATA*/", t)
        self.assertIn("/*SEED_DATA*/", t)


if __name__ == "__main__":
    unittest.main()
