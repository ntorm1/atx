"""P9 lane D1 (DEC-8): the fitter's prior weight rule table (PRIOR_WEIGHT_RULES, in place of fit_prior's if/elif:
dispatch only), its modifier module list, and its theme tuple read from the alpha registry (the IC runner's theme
table, --theme-registry). Synthetic inputs only; the C++ rule ids come from the K-P9-6 fixture
(atx-impl/tests/fixtures/composition_rules_list.json, CompositionRules.ListRulesJsonPinned)."""
from __future__ import annotations

import json
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_composition_weights as fcw  # noqa: E402
import composition_ic_shrink  # noqa: E402
import composition_resid  # noqa: E402
import composition_rules  # noqa: E402
import composition_theme_erc  # noqa: E402
import composition_theme_tsmom  # noqa: E402
import composition_two_speed  # noqa: E402

LIST_RULES = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "composition_rules_list.json"


# ---------------------------------------------------------------- the prior weight rule table
def test_the_rule_table_partitions_the_prior_compositions():
    seen = [i for rule in fcw.PRIOR_WEIGHT_RULES for i in rule.ids]
    assert len(seen) == len(set(seen))                         # one row per id
    assert set(seen) == set(fcw.PRIOR_COMPOSITIONS)            # every prior composition, nothing else
    assert all(rule.ids and callable(rule.fit) for rule in fcw.PRIOR_WEIGHT_RULES)
    for composition in fcw.PRIOR_COMPOSITIONS:
        assert composition in fcw.prior_weight_rule(composition).ids
    # Only ew-theme-v6 may leave every weight zero (all members in a dropped theme): the admission table alone.
    assert [rule.ids for rule in fcw.PRIOR_WEIGHT_RULES if rule.empty_reason is not None] == [(fcw.V6_RULE_ID,)]
    assert fcw.prior_weight_rule(fcw.V6_RULE_ID).empty_reason == \
        "fit: ew-theme-v6 leaves no member outside the dropped themes"


def test_a_composition_without_a_row_is_refused_by_name():
    for composition in ("ew-theme-new-v9", fcw.RULE_ID, fcw.NETCOST_RULE_ID, ""):
        with pytest.raises(fcw.FitError, match=f"^fit: --composition {composition} has no prior weight rule$"):
            fcw.prior_weight_rule(composition)


def synthetic_inputs(aim: bool) -> fcw.PriorFitInputs:
    """Ten candidates, three to a v4 theme (value, price_momentum, low_risk) plus one options_implied candidate that
    is not active (the rule never reads it); tiers, ICs and gains close enough that no member hits the 1/(2T) cap."""
    ids = [f"c{k}" for k in range(10)]
    themes = ["value"] * 3 + ["price_momentum"] * 3 + ["low_risk"] * 3 + ["options_implied"]
    tiers = ["B", "B+", "B-", "B", "B", "B+", "B-", "B", "B", "A"]
    rows = [{"tau": t, "train_mean": m} for t, m in zip((.02, .09, .03, .05, .01, .12, .04, .06, .03, .02),
                                                        (.011, .009, .010, .008, .009, .010, .007, .008, .009, .02))]
    aims = [{"gain": g} for g in (.5, .6, .55, .5, .45, .6, .5, .55, .6, .9)] if aim else None
    return fcw.PriorFitInputs(list(range(9)), ids, themes, tiers, rows, aims, aim)


def same_fit(got: fcw.PriorFit, weights, theme_table, text: str, series: str) -> bool:
    w = np.asarray(weights, dtype=float)
    return (np.asarray(got.weights, dtype=float).tobytes() == w.tobytes() and got.theme_table == theme_table and
            got.text == text and got.fit_series == series)


def test_each_row_calls_its_rule_with_the_v8_arguments():
    """Dispatch only: each row's result is its rule's own call on the members the v8 chain passed, bit for bit."""
    x, xa = synthetic_inputs(False), synthetic_inputs(True)
    act = x.active
    ids, themes, tiers = [x.ids[k] for k in act], [x.themes[k] for k in act], [x.tiers[k] for k in act]
    gains = [xa.aims[k]["gain"] for k in act]
    # ew-theme-v1
    w, table = fcw.ew_theme_weights(themes)
    got = fcw.prior_weight_rule(fcw.EW_THEME_RULE_ID).fit(x)
    assert same_fit(got, w, table, "w_k=1/(T*n_theme(k)) over admitted non-degenerate k; T=themes with >=1 such "
                    "member; no mean or covariance estimation", "none (equal theme weights); diagnostic uses s_k*f "
                    "over ALL TRAIN scored decisions, flat decisions 0")
    assert got.parent is None and got.v6_detail is None
    # ew-theme-aim-v1
    w, table = fcw.ew_theme_aim_weights(themes, gains)
    got = fcw.prior_weight_rule(fcw.AIM_RULE_ID).fit(xa)
    assert np.asarray(got.weights).tobytes() == np.asarray(w).tobytes() and got.theme_table == table
    assert got.fit_series == fcw.AIM_FIT_SERIES and got.text.startswith("w_k=(g_k/(T*n_theme(k)))")
    # ew-theme-v6
    w, table, detail = fcw.ew_theme_v6_weights(ids, themes, [x.rows[k]["tau"] for k in act])
    got = fcw.prior_weight_rule(fcw.V6_RULE_ID).fit(x)
    assert np.asarray(got.weights).tobytes() == np.asarray(w).tobytes() and got.theme_table == table
    assert got.v6_detail == detail and got.text.startswith("ew-theme-v6: theme'=options_implied->short_interest")
    # ew-theme-std-v1 and ew-theme-std-aim-v1
    for composition, inputs, g in ((composition_rules.STD_RULE_ID, x, None),
                                   (composition_rules.STD_AIM_RULE_ID, xa, gains)):
        std = composition_rules.ew_theme_std(ids, themes, tiers, error=fcw.FitError, gains=g)
        got = fcw.prior_weight_rule(composition).fit(inputs)
        assert same_fit(got, std.weights, std.theme_table, std.text, std.fit_series), composition
        assert got.parent is not None and got.parent.block == std.block
    # ew-theme-aim-v2
    w, table = composition_rules.ew_theme_aim_v2(ids, themes, gains, error=fcw.FitError)
    got = fcw.prior_weight_rule(fcw.AIM_V2_RULE_ID).fit(xa)
    assert same_fit(got, w, table, composition_rules.AIM_V2_TEXT, composition_rules.AIM_V2_FIT_SERIES)
    # ic-shrink-v1 and ic-shrink-aim-v1: the admission rows' train_mean are the ICs
    ics = [x.rows[k]["train_mean"] for k in act]
    for composition, inputs, g in ((composition_ic_shrink.RULE_ID, x, None),
                                   (composition_ic_shrink.AIM_RULE_ID, xa, gains)):
        fit = composition_ic_shrink.ic_shrink(ids, themes, ics, error=fcw.FitError, gains=g)
        got = fcw.prior_weight_rule(composition).fit(inputs)
        assert same_fit(got, fit.weights, fit.theme_table, fit.text, fit.fit_series), composition
        assert got.parent is not None and got.parent.block == fit.block


def test_the_rule_ids_the_fitter_writes_are_rows_of_the_runner_table():
    """The weights blocks the fitter writes name rules of the IC runner's table (K-P9-6 --list-rules), under the block
    key the runner parses them by."""
    listed = {row["id"]: row["block_key"] for row in json.loads(LIST_RULES.read_text(encoding="utf-8"))["rules"]}
    written = {fcw.V6_REDISTRIBUTION: "theme_redistribution", composition_rules.STD_RULE_ID: "theme_standardise",
               composition_ic_shrink.RULE_ID: "theme_standardise",
               composition_ic_shrink.AIM_RULE_ID: "theme_standardise",
               composition_theme_erc.RULE_ID: "theme_standardise", composition_resid.RULE_ID: composition_resid.BLOCK,
               composition_theme_tsmom.RULE_ID: composition_theme_tsmom.BLOCK,
               composition_two_speed.RULE_ID: composition_two_speed.BLOCK}
    assert written == listed


# ---------------------------------------------------------------- the modifier modules
def test_the_modifier_modules_register_and_check_in_order():
    assert fcw.MODIFIER_MODULES == (composition_resid, composition_theme_erc, composition_theme_tsmom,
                                    composition_two_speed)
    assert fcw.MODIFIER_MODULES == (composition_resid,) + fcw.PARENT_CHECKED_MODIFIERS
    assert all(callable(m.add_argument) for m in fcw.MODIFIER_MODULES)
    assert all(callable(m.check_args) for m in fcw.PARENT_CHECKED_MODIFIERS)
    argv = ["--library", "l", "--library-sha256", "0" * 64, "--train", "t", "--train-sha256", "0" * 64,
            "--orientations", "o", "--orientations-sha256", "0" * 64, "--runner-summary", "s",
            "--runner-summary-sha256", "0" * 64, "--screen", "none", "--output", "out"]
    args = fcw.parse_args(argv)
    for name in ("theme_resid", "theme_erc", "theme_tsmom", "two_speed"):
        assert getattr(args, name) is None  # flag absent: no modifier
    flagged = fcw.parse_args(argv + ["--theme-resid", composition_resid.RULE_ID, "--theme-erc",
                                     composition_theme_erc.RULE_ID, "--theme-tsmom", composition_theme_tsmom.RULE_ID,
                                     "--two-speed", composition_two_speed.RULE_ID])
    assert (flagged.theme_resid, flagged.theme_erc, flagged.theme_tsmom, flagged.two_speed) == (
        composition_resid.RULE_ID, composition_theme_erc.RULE_ID, composition_theme_tsmom.RULE_ID,
        composition_two_speed.RULE_ID)


# ---------------------------------------------------------------- the theme tuple from the registry
def write_registry(path: Path, themes, schema: str = fcw.REGISTRY_SCHEMA) -> Path:
    path.write_text(json.dumps({"schema": schema, "alphas": [], "themes": {t: f"{t} text" for t in themes}}),
                    encoding="utf-8")
    return path


def test_the_theme_tuple_is_the_registry_table_in_file_order():
    themes = tuple(json.loads(fcw.REGISTRY_PATH.read_text(encoding="utf-8"))["themes"])
    assert fcw.PRIOR_THEMES == themes
    assert fcw.PRIOR_THEMES[:len(fcw.V4_THEMES)] == fcw.V4_THEMES
    assert fcw.V7_APPENDED_THEMES == fcw.PRIOR_THEMES[len(fcw.V4_THEMES):]
    assert fcw.registered_prior_themes() == fcw.PRIOR_THEMES
    assert fcw.registry_themes(fcw.REGISTRY_PATH) == fcw.PRIOR_THEMES
    # At the P9 base the registry is the list of record.
    assert fcw.PRIOR_THEMES == fcw.V4_THEMES + fcw.V7_APPENDED_THEMES_OF_RECORD


def test_a_theme_registered_once_reaches_the_tuple(tmp_path, monkeypatch):
    later = fcw.PRIOR_THEMES + ("another_theme",)
    monkeypatch.setattr(fcw, "REGISTRY_PATH", write_registry(tmp_path / "later.json", later))
    assert fcw.registered_prior_themes() == later
    assert fcw.prior_themes() == (later, "registry later.json")
    monkeypatch.setattr(fcw, "REGISTRY_PATH", tmp_path / "absent.json")
    assert fcw.registered_prior_themes() == fcw.V4_THEMES + fcw.V7_APPENDED_THEMES_OF_RECORD


def test_a_registry_that_breaks_the_frozen_prefix_or_its_schema_is_refused(tmp_path, monkeypatch):
    swapped = ("profitability_quality", "value") + fcw.PRIOR_THEMES[2:]
    cases = {"swapped.json": (swapped, fcw.REGISTRY_SCHEMA, "must begin with the frozen v4 list"),
             "short.json": (fcw.V4_THEMES[:5], fcw.REGISTRY_SCHEMA, "must begin with the frozen v4 list"),
             "schema.json": (fcw.PRIOR_THEMES, "atx.alpha-registry/v2", "is not an atx.alpha-registry/v1 document"),
             "empty.json": ((), fcw.REGISTRY_SCHEMA, "is not an atx.alpha-registry/v1 document")}
    for name, (themes, schema, reason) in cases.items():
        monkeypatch.setattr(fcw, "REGISTRY_PATH", write_registry(tmp_path / name, themes, schema))
        with pytest.raises(fcw.FitError, match=reason):
            fcw.registered_prior_themes()
    duplicate = tmp_path / "duplicate.json"
    duplicate.write_text('{"schema": "atx.alpha-registry/v1", "themes": {"value": 1, "value": 2}}', encoding="utf-8")
    monkeypatch.setattr(fcw, "REGISTRY_PATH", duplicate)
    with pytest.raises(fcw.FitError, match="duplicate JSON key"):
        fcw.registered_prior_themes()
