"""DEC-5 freeze (P9 lane A1): no new Python field builder.

A new research field is a row of ``field_registry.json`` naming a C++ builder kind (``kind: engine``), never a new
Python module. This test fails on
* a ``research_fields_*.py`` or ``prepare_research_fields_*.py`` anywhere in the repository (tracked or untracked, not
  ignored) that is not a row of ``ALLOWLIST``;
* an ``ALLOWLIST`` row whose file no longer exists (delete the row with the file: the list only shrinks);
* a ``kind: python`` registry row outside ``FROZEN_PYTHON_FIELDS`` (the 92 fields of the base commit d7c1c520): a
  field may move from python to engine, or be deleted, but no python field is added.
Each row names the slice or lane that retires the file (platform core migration section 4, P9 plan section 2).
"""
from __future__ import annotations

from pathlib import Path
import re
import subprocess
import unittest

import field_registry as fr

REPO = Path(__file__).resolve().parents[2]
PATTERN = re.compile(r"(^|/)(prepare_)?research_fields_[^/]*\.py$")
SLICE4 = "migration slice 4 (A3: TickerHistory3 vendor panel in C++; Python deleted one slice after TRAIN identity)"
SLICE5 = "migration slice 5 (A4: SEC / holdings sources in C++; Python deleted one slice after TRAIN identity)"
SHIM = "deprecated shim: deleted once every caller uses prepare_research_fields.py --registry (E2's field kind, wave 2)"
# (path, why it exists, what retires it); sorted by path
ALLOWLIST = (
    ("atx-engine/tools/prepare_research_fields_draft.py", "v13 draft modules' registration", SHIM),
    ("atx-engine/tools/prepare_research_fields_engine.py", "engine path of si_shares, si_dtc, vol_126",
     "A2 (kind: engine rows); deleted after root's TRAIN identity of the three engine fields (migration 3.3)"),
    ("atx-engine/tools/prepare_research_fields_ohlc.py", "session-bar module registration", SHIM),
    ("atx-engine/tools/prepare_research_fields_xdata.py", "XDATA and gold modules' registration", SHIM),
    ("atx-engine/tools/prepare_research_fields_ydata.py", "YDATA modules' registration", SHIM),
    ("atx-engine/tools/research_fields_connected.py", "13F common-ownership field", SLICE5),
    ("atx-engine/tools/research_fields_deals.py", "SEC pending-merger field", SLICE5),
    ("atx-engine/tools/research_fields_divevent.py", "TickerHistory3 dividend events field", SLICE4),
    ("atx-engine/tools/research_fields_gold.py", "atx-db gold-panel reader fields", SLICE4),
    ("atx-engine/tools/research_fields_holdings.py", "13F / FTD / Reg SHO / short-volume-ext fields", SLICE5),
    ("atx-engine/tools/research_fields_ivshape.py", "TickerHistory3 option-smile field", SLICE4),
    ("atx-engine/tools/research_fields_mgr13f.py", "13F manager-horizon field", SLICE5),
    ("atx-engine/tools/research_fields_ohlc.py", "TickerHistory3 session-bar fields", SLICE4),
    ("atx-engine/tools/research_fields_price.py", "price and long-lookback fields", SLICE4),
    ("atx-engine/tools/research_fields_sec.py", "SEC earnings / Form 4 / 8-K fields", SLICE5),
    ("atx-engine/tools/research_fields_v8.py", "v8 issuer fields", SLICE5),
    ("atx-engine/tools/research_fields_v9.py", "v9 draft SEC / issuer fields", SLICE5),
    ("atx-engine/tools/research_fields_xdata.py", "TickerHistory3 expansion-X fields", SLICE4),
)
FROZEN_PYTHON_FIELDS = frozenset((
    "si_shares", "si_dtc", "iv_atm_21d", "iv_atm_63d", "iv_atm_126d", "earn_recent", "shares_out", "mktcap_lagged",
    "size_grp", "is_common", "mkt_ret", "be", "at", "at_lag4", "lt", "che", "debt", "sale_ttm", "gp_ttm", "oi_ttm",
    "ni_ttm", "ni_q", "ni_q_lag4", "be_lag1q", "be_lag1q_lag4", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm",
    "prstkc_ttm", "sstk_ttm", "txt_q", "txt_q_lag4", "shrs_q", "shrs_q_lag4", "noa", "noa_lag4", "sue", "fscore",
    "me_company", "grp_sic2", "grp_ff12", "grp_ff49", "sv_ratio126", "ea_days_to_expected", "ea_days_since",
    "ea_window_pre5", "ea_window_post3", "ea_delay_days", "ea_time_of_day", "ins_net_buy_ratio", "ins_n_buyers",
    "ins_n_sellers", "ins_opportunistic_net", "ins_cluster_buy", "k8_count_63", "k8_item_material_21",
    "k8_days_since_any", "k8_item402_63", "ret_overnight", "ret_intraday", "ceq_iss_5y", "coskew_60m", "vol_126",
    "xrd0_ttm", "grp_ff12f49", "gscore7_lowbm", "eps_consist_4y", "nt_first_126", "earn_season_rank",
    "div_month_pred", "beta_dvol_21", "season_y2_5", "gp_hl_spread_21", "gp_iv_term_slope", "open_adj", "high_adj",
    "low_adj", "iv_skew_21", "stio_chg_q", "div_init_omit", "deal_pending", "conn_ret63", "inst_own_share",
    "inst_breadth_chg", "inst_own_chg_q", "inst_best_ideas", "inst_n_holders", "ftd_shares_ratio21",
    "regsho_threshold_days63", "sv_offexchange_share126", "exch_up_365d"))


def repository_files() -> list:
    """Every tracked or untracked, not ignored, file of the repository (posix paths relative to its root)."""
    done = subprocess.run(["git", "-C", str(REPO), "ls-files", "-co", "--exclude-standard"], capture_output=True,
                          text=True, check=True, timeout=300)
    return sorted({line.strip() for line in done.stdout.splitlines() if line.strip()})


class NoNewPythonBuilder(unittest.TestCase):
    def test_every_python_builder_file_is_allowlisted(self):
        found = [p for p in repository_files() if PATTERN.search(p)]
        listed = [row[0] for row in ALLOWLIST]
        new = sorted(set(found) - set(listed))
        self.assertEqual(new, [], "DEC-5: a new field is a field_registry.json row naming a C++ builder kind, not a "
                                  "new Python builder module")

    def test_the_allowlist_only_shrinks(self):
        paths = [row[0] for row in ALLOWLIST]
        self.assertEqual(paths, sorted(set(paths)))
        stale = [p for p in paths if not (REPO / p).is_file()]
        self.assertEqual(stale, [], "a deleted builder's row must leave the allowlist with it")
        for path, why, retired_by in ALLOWLIST:
            self.assertTrue(PATTERN.search(path) and why and retired_by, path)
            self.assertTrue(path.startswith("atx-engine/tools/"), path)

    def test_no_python_field_is_added_to_the_registry(self):
        python = {row["name"] for row in fr.load()["fields"] if row["kind"] == "python"}
        self.assertEqual(sorted(python - FROZEN_PYTHON_FIELDS), [],
                         "DEC-5: a new registry row is kind engine (a C++ builder kind)")
        self.assertEqual(len(FROZEN_PYTHON_FIELDS), 92)

    def test_the_guard_has_teeth(self):
        self.assertTrue(PATTERN.search("atx-engine/tools/research_fields_new.py"))
        self.assertTrue(PATTERN.search("scripts/prepare_research_fields_v16.py"))
        self.assertFalse(PATTERN.search("atx-engine/tools/test_research_fields_price.py"))
        self.assertFalse(PATTERN.search("atx-engine/tools/prepare_research_fields.py"))
        self.assertFalse(PATTERN.search("atx-engine/tools/field_registry.py"))


if __name__ == "__main__":
    unittest.main()
