"""The catalog's typed columns hold no statistic (P9 SQL2; blind rule, ruling SQL-7; synthetic).

A catalog row is queryable by column (``atx-research-store query``, ``dump``); a statistic of the research window in a
typed column would turn every query into a look at results. Statistics stay inside the ``doc`` / ``extra`` JSON of the
documents that carry them (verdicts, wave results), which only root opens. This test reads the committed group fixture
``schema/catalog_records.json`` (the gtest ``ResearchCatalog.RecordsSchemaJsonEqualsFixture`` pins it to the C++
descriptors byte for byte) and fails when a column name, or a name the ``pin_status`` view selects, holds a statistic
token: return, IC, Sharpe, turnover, drawdown, DSR / PSR / PBO, NAV and their kin.

Run: python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store_blind.py
"""
from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "atx-engine" / "tests" / "fixtures" / "research_store" / "schema" / "catalog_records.json"

# Whole tokens of a snake_case name (split on "_" and digits).
STAT_TOKENS = frozenset({
    "ret", "rets", "return", "returns", "ic", "ics", "icir", "ir", "rankic", "sharpe", "sortino", "calmar", "turnover",
    "drawdown", "dd", "mdd", "maxdd", "dsr", "psr", "pbo", "nav", "pnl", "cagr", "vol", "volatility", "alpha", "beta",
    "tstat", "hac", "rho", "corr", "excess", "hit", "hitrate", "mean", "std", "stdev", "var", "skew", "kurt", "pvalue",
    "pval", "lw", "cbb", "ci", "se", "score", "marginal", "spread",
})
# Substrings that mark a statistic inside any name.
STAT_SUBSTRINGS = ("sharpe", "sortino", "turnover", "drawdown", "return", "volatil", "pnl", "_ic", "ic_")


def tokens(name: str) -> list:
    return [t for t in re.split(r"[_0-9]+", name.lower()) if t]


def statistic_names(names) -> list:
    return sorted({n for n in names if set(tokens(n)) & STAT_TOKENS or any(s in n.lower() for s in STAT_SUBSTRINGS)})


def records_group() -> dict:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


class TypedColumnsHoldNoStatistic(unittest.TestCase):
    def test_no_typed_column_is_a_statistic(self):
        group = records_group()
        self.assertEqual(group["name"], "catalog_records")
        columns = [f"{t['name']}.{c['name']}" for t in group["tables"] for c in t["columns"]]
        self.assertGreater(len(columns), 150)
        self.assertEqual(statistic_names(c.split(".", 1)[1] for c in columns), [])
        self.assertEqual(statistic_names(t["name"] for t in group["tables"]), [])

    def test_no_view_selects_a_statistic(self):
        for view in records_group()["views"]:
            identifiers = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", view["sql"])
            self.assertEqual(statistic_names(identifiers), [], view["name"])

    def test_the_token_list_catches_statistics(self):
        planted = ["sharpe", "ic_mean", "mean_ic", "rank_ic21", "nav_end", "max_drawdown", "turnover", "dsr", "psr",
                   "pbo", "excess_return", "ann_vol", "hac_t", "paired_lw_p"]
        self.assertEqual(statistic_names(planted), sorted(planted))
        self.assertEqual(statistic_names(["run_dir", "wall_seconds", "peak_mib", "n_before", "accepted", "ledger_head",
                                          "trial_id", "count", "exit_code", "spec_sha256"]), [])


if __name__ == "__main__":
    unittest.main()
