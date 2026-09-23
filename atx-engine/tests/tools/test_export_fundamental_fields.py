"""Synthetic Company Facts fixtures for the lane-10 fundamental export; never reads real data."""
import datetime as dt
import math
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import export_fundamental_fields as ex  # noqa: E402

D = dt.date


def fact(concept, start, end, val, filed, form="10-Q", accn=None):
    return {"concept": concept, "start": start, "end": end, "val": val, "filed": filed,
            "form": form, "accn": accn or f"acc-{filed}"}


def doc(rows):
    """Company Facts JSON shape from flat rows."""
    facts = {"us-gaap": {}, "dei": {}}
    for r in rows:
        tax = "dei" if r["concept"] in ex.C_SHARES_DEI else "us-gaap"
        unit = "shares" if "Shares" in r["concept"] else ("USD/shares" if "PerShare" in r["concept"] else "USD")
        body = facts[tax].setdefault(r["concept"], {"units": {}})
        entry = {"end": r["end"], "val": r["val"], "accn": r["accn"], "form": r["form"],
                 "filed": r["filed"]}
        if r["start"]:
            entry["start"] = r["start"]
        body["units"].setdefault(unit, []).append(entry)
    return {"cik": 1, "entityName": "Synthetic", "facts": facts}


class QuarterArithmeticTest(unittest.TestCase):
    def test_ytd_differences_become_discrete_quarters(self):
        durs = {
            (D(2013, 1, 1), D(2013, 3, 31)): 10.0,
            (D(2013, 1, 1), D(2013, 6, 30)): 25.0,   # 6M YTD
            (D(2013, 1, 1), D(2013, 9, 30)): 45.0,   # 9M YTD
            (D(2013, 1, 1), D(2013, 12, 31)): 70.0,  # FY
        }
        q = ex.derive_quarters(durs)
        self.assertEqual(q[D(2013, 3, 31)][1], 10.0)
        self.assertEqual(q[D(2013, 6, 30)][1], 15.0)
        self.assertEqual(q[D(2013, 9, 30)][1], 20.0)
        self.assertEqual(q[D(2013, 12, 31)][1], 25.0)
        self.assertEqual(q[D(2013, 12, 31)][0], D(2013, 10, 1))

    def test_ttm_prefers_annual_and_chains_quarters(self):
        durs = {
            (D(2013, 1, 1), D(2013, 12, 31)): 100.0,
            (D(2013, 4, 1), D(2013, 6, 30)): 20.0,
            (D(2013, 7, 1), D(2013, 9, 30)): 30.0,
            (D(2013, 10, 1), D(2013, 12, 31)): 40.0,
            (D(2014, 1, 1), D(2014, 3, 31)): 50.0,
        }
        ttm = ex.ttm_series(durs)
        self.assertEqual(ttm[D(2013, 12, 31)], 100.0)
        self.assertEqual(ttm[D(2014, 3, 31)], 20.0 + 30.0 + 40.0 + 50.0)
        self.assertNotIn(D(2013, 9, 30), ttm)  # only three quarters available

    def test_sue_requires_history_and_uses_prior_differences(self):
        quarters = {}
        eps = [1.0, 1.0, 1.0, 1.0, 1.1, 1.2, 1.0, 1.1, 1.3, 1.5]
        ends = [D(2011, 3, 31), D(2011, 6, 30), D(2011, 9, 30), D(2011, 12, 31),
                D(2012, 3, 31), D(2012, 6, 30), D(2012, 9, 30), D(2012, 12, 31),
                D(2013, 3, 31), D(2013, 6, 30)]
        for e, v in zip(ends, eps):
            quarters[e] = (e - dt.timedelta(days=89), v)
        got = ex.sue_from_quarters(quarters)
        self.assertIsNotNone(got)
        end, sue = got
        self.assertEqual(end, D(2013, 6, 30))
        prior = [0.1, 0.2, 0.0, 0.1, 0.2]  # seasonal differences before the latest
        expected = (1.5 - 1.2) / __import__("statistics").stdev(prior)
        self.assertAlmostEqual(sue, expected)
        few = {e: quarters[e] for e in ends[:6]}
        self.assertIsNone(ex.sue_from_quarters(few))


class SnapshotPitTest(unittest.TestCase):
    def rows(self):
        return [
            fact("Assets", None, "2012-12-31", 100.0, "2013-02-20", "10-K"),
            fact("StockholdersEquity", None, "2012-12-31", 40.0, "2013-02-20", "10-K"),
            fact("NetIncomeLoss", "2012-01-01", "2012-12-31", 12.0, "2013-02-20", "10-K"),
            fact("Assets", None, "2013-03-31", 110.0, "2013-05-01", "10-Q"),
            fact("StockholdersEquity", None, "2013-03-31", 44.0, "2013-05-01", "10-Q"),
            # The 10-K/A restating FY2012 equity lands AFTER the Q1 10-Q.
            fact("StockholdersEquity", None, "2012-12-31", 38.0, "2013-06-15", "10-K/A"),
            fact("WeightedAverageNumberOfDilutedSharesOutstanding", "2013-01-01", "2013-03-31", 10.0,
                 "2013-05-01", "10-Q"),
            # A fact available only after the seal must never be exported.
            fact("Assets", None, "2019-12-31", 999.0, "2020-02-01", "10-K"),
        ]

    def test_snapshots_are_point_in_time_and_sealed(self):
        facts = ex.parse_company_facts(doc(self.rows()))
        snaps = ex.company_snapshots(facts, seal=D(2020, 1, 1))
        self.assertEqual([s["filed"] for s in snaps], [D(2013, 2, 20), D(2013, 5, 1), D(2013, 6, 15)])
        first, second, third = snaps
        self.assertEqual(first["available_date"], D(2013, 2, 21))
        self.assertEqual(first["period_end"], D(2012, 12, 31))
        self.assertEqual(first["book_equity"], 40.0)
        self.assertEqual(first["net_income_ttm"], 12.0)
        self.assertTrue(math.isnan(first["sue"]))
        self.assertEqual(second["period_end"], D(2013, 3, 31))
        self.assertEqual(second["total_assets"], 110.0)
        self.assertEqual(second["shares_outstanding"], 10.0)
        # The older-period amendment never regresses the newer balance sheet.
        self.assertEqual(third["book_equity"], 44.0)
        self.assertEqual(third["period_end"], D(2013, 3, 31))
        self.assertTrue(all(s["available_date"] < D(2020, 1, 1) for s in snaps))

    def test_stale_statement_fields_are_not_relabelled_current(self):
        rows = [
            fact("NetIncomeLoss", "2011-01-01", "2011-12-31", 5.0, "2012-02-20", "10-K"),
            fact("Assets", None, "2012-09-30", 90.0, "2012-11-01", "10-Q"),
        ]
        snaps = ex.company_snapshots(ex.parse_company_facts(doc(rows)), seal=D(2020, 1, 1))
        latest = snaps[-1]
        self.assertEqual(latest["period_end"], D(2012, 9, 30))
        self.assertTrue(math.isnan(latest["net_income_ttm"]))  # 273 days older than the period

    def test_split_restated_comparative_keeps_issuance_split_consistent(self):
        w = "WeightedAverageNumberOfDilutedSharesOutstanding"
        rows = [
            fact(w, "2013-04-01", "2013-06-30", 100.0, "2013-08-01", "10-Q"),
            # 7:1 split; the 2014 10-Q restates the 2013 comparative to 700.
            fact(w, "2014-04-01", "2014-06-30", 690.0, "2014-08-01", "10-Q"),
            fact(w, "2013-04-01", "2013-06-30", 700.0, "2014-08-01", "10-Q"),
        ]
        snaps = ex.company_snapshots(ex.parse_company_facts(doc(rows)), seal=D(2020, 1, 1))
        latest = snaps[-1]
        self.assertEqual(latest["shares_outstanding"], 690.0)
        self.assertEqual(latest["shares_lag1y"], 700.0)

    def test_future_period_and_non_event_forms_are_ignored(self):
        rows = [
            fact("Assets", None, "2013-12-31", 1.0, "2013-06-01", "10-Q"),  # ends after filing
            fact("Assets", None, "2013-03-31", 2.0, "2013-05-01", "8-K"),
        ]
        self.assertEqual(ex.parse_company_facts(doc(rows)), [])


if __name__ == "__main__":
    unittest.main()
