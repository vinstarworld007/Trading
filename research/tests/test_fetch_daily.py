"""Tests for research/fetch_daily.py. Run: python3 -m unittest discover research/tests"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import fetch_daily  # noqa: E402

PAYLOAD = {"data": {"symbol": "AAPL", "tradesTable": {"rows": [
    {"date": "01/31/2019", "close": "$41.61", "volume": "40,739,649", "open": "$41.53",
     "high": "$42.25", "low": "$41.14"},
    {"date": "01/30/2019", "close": "$41.3125", "volume": "244,439,200", "open": "$40.8125",
     "high": "$41.5375", "low": "$40.0575"},
]}}}


class FetchDailyTest(unittest.TestCase):
    def test_parse_sorts_oldest_first_and_strips_formatting(self):
        rows = fetch_daily.parse_nasdaq(PAYLOAD)
        self.assertEqual([r["date"] for r in rows], ["2019-01-30", "2019-01-31"])
        self.assertEqual(rows[0]["close"], "41.3125")
        self.assertEqual(rows[0]["volume"], "244439200")

    def test_unsplit_restores_the_traded_prices(self):
        rows = fetch_daily.unsplit(fetch_daily.parse_nasdaq(PAYLOAD), 4, "2020-08-31")
        self.assertEqual(rows[0]["close"], "165.25")
        self.assertEqual(rows[0]["open"], "163.25")
        self.assertEqual(rows[0]["volume"], "61109800")

    def test_unsplit_keeps_plain_decimals(self):
        rows = fetch_daily.unsplit([{"date": "2019-01-02", "open": "40.00", "high": "40",
                                     "low": "40", "close": "40", "volume": "8"}], 4, "2020-01-01")
        self.assertEqual(rows[0]["open"], "160")

    def test_unsplit_leaves_later_bars(self):
        rows = fetch_daily.unsplit(fetch_daily.parse_nasdaq(PAYLOAD), 4, "2019-01-31")
        self.assertEqual(rows[1]["close"], "41.61")

    def test_empty_payload(self):
        self.assertEqual(fetch_daily.parse_nasdaq({"data": None}), [])

    def test_csv(self):
        csv = fetch_daily.to_csv(fetch_daily.parse_nasdaq(PAYLOAD))
        self.assertTrue(csv.startswith("date,open,high,low,close,volume\n2019-01-30,40.8125,"))


if __name__ == "__main__":
    unittest.main()
