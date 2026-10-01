"""The mined-v1 label-overlap factors (lane MINE-FIX, Ruling E-32a, review MINE-6; tables by lane MINE-STAT).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_mine_overlap_factor.py

Synthetic null draws only (about 45 s). The tables the verb compiles (strategy_mine_rule.hpp kMinedOverlapBands,
kMinedConfirmBands, kMinedMaxBudget) are mine_overlap_factor's and follow from its pinned record of the full run;
three cells of that run are re-derived at their committed seeds; the importance-sampling estimate agrees with plain
null draws; the estimator is the verb's (the same pinned t's as strategy_mine_test.cpp
StrategyMineRule.OverlapFactorIsRegisteredAndItsEstimatorIsTheVerbs).
"""
from __future__ import annotations

import math
import re
from pathlib import Path

import numpy as np
import pytest

import backtest_integrity as BI
import mine_overlap_factor as MOF

ROOT = Path(__file__).resolve().parents[2]
RULE_HPP = ROOT / "atx-impl" / "src" / "strategy_mine_rule.hpp"
FITNESS_HPP = ROOT / "atx-engine" / "include" / "atx" / "engine" / "factory" / "research_ic_fitness.hpp"


def constant(path: Path, name: str) -> str:
    match = re.search(rf"inline constexpr [\w:]+ {name} = ([0-9.]+);", path.read_text(encoding="utf-8"))
    assert match, f"{name} not found in {path}"
    return match.group(1)


def bands(path: Path, name: str) -> tuple[tuple[int, float], ...]:
    """A `std::array<MinedFactorBand, K> name{{{top, factor}, ...}}` table of the header."""
    match = re.search(rf"inline constexpr std::array<MinedFactorBand, (\d+)> {name}\{{\s*\{{(.*?)\}}\}};",
                      path.read_text(encoding="utf-8"), re.S)
    assert match, f"{name} not found in {path}"
    rows = tuple((int(top), float(factor)) for top, factor in re.findall(r"\{(\d+)U, ([0-9.]+)\}", match.group(2)))
    assert len(rows) == int(match.group(1))
    return rows


def pinned_series() -> list[float]:
    """strategy_mine_test.cpp's series: ((37 k) mod 23 - 11) / 100 + .004, k < 60, days 7 and 30 undefined."""
    out = [((k * 37) % 23 - 11) / 100.0 + 0.004 for k in range(60)]
    out[7] = out[30] = float("nan")
    return out


def direct_t(x: list[float], lag: int) -> float:
    """eval::hac::mean_inference(x, BartlettV1, lag, small-sample) written out term by term."""
    n = len(x)
    m = sum(x) / n
    lags = min(lag, n - 1)
    s = sum((v - m) ** 2 for v in x)
    for j in range(1, lags + 1):
        s += 2.0 * (1.0 - j / (lags + 1.0)) * sum((x[t] - m) * (x[t - j] - m) for t in range(j, n))
    return m / math.sqrt(s / (n * n) * (n / (n - 1.0)))


def test_the_rule_header_compiles_the_tables():
    assert bands(RULE_HPP, "kMinedOverlapBands") == MOF.OVERLAP_BANDS
    assert bands(RULE_HPP, "kMinedConfirmBands") == MOF.CONFIRM_BANDS
    # Ruling PM4-13, lifted: the ceiling the verb compiles is the table's, and the one the ledger refuses above.
    assert int(constant(RULE_HPP, "kMinedMaxBudget")) == MOF.MAX_BUDGET == BI.MINED_MAX_BUDGET == 10_000
    assert int(constant(RULE_HPP, "kMinedMinDiscoverRows")) == MOF.ROWS_DISCOVER
    assert int(constant(RULE_HPP, "kMinedMinConfirmRows")) == MOF.ROWS_CONFIRM
    assert float(constant(RULE_HPP, "kMinedFamilyAlpha")) == MOF.FAMILY_ALPHA
    assert float(constant(RULE_HPP, "kMinedConfirmT")) == MOF.CONFIRM_T
    assert float(constant(RULE_HPP, "kMinedConfirmBy")) == MOF.CONFIRM_BY
    assert int(constant(FITNESS_HPP, "kResearchIcHacLag")) == MOF.HAC_LAG
    # The confirm table reaches the configuration's shortlist bound (--max-promotions 1..256).
    assert MOF.CONFIRM_BANDS[-1][0] == 256


def test_the_tables_follow_from_the_record():
    """The pinned tables are the ceilings of the pinned record's upper bounds; both grow with their count, the
    confirm factor covers the gate, and on the whole of TRAIN every discover ratio is below the floor's."""
    upper = {name: {"upper": ratio + MOF.MARGIN_SE * se} for name, (ratio, se) in MOF.DERIVED.items()}
    table = MOF.tables(upper)
    assert table["overlap_bands"] == [list(b) for b in MOF.OVERLAP_BANDS]
    assert table["confirm_bands"] == [list(b) for b in MOF.CONFIRM_BANDS]
    for rows in (MOF.OVERLAP_BANDS, MOF.CONFIRM_BANDS):
        assert all(a[0] < b[0] and a[1] < b[1] for a, b in zip(rows, rows[1:]))
    gate = upper["confirm-gate:0"]["upper"]
    assert all(factor >= gate for _, factor in MOF.CONFIRM_BANDS)
    for n in MOF.BUDGETS:
        assert MOF.DERIVED[f"four-year:{n}"][0] < MOF.DERIVED[f"discover:{n}"][0]
    # Every tail estimate is tight: the ratio's standard error is under .013 (about one rounding step).
    assert all(se < 0.013 for _, se in MOF.DERIVED.values())


def test_the_levels_are_the_rules():
    assert MOF.cell_level("discover", 1_000) == pytest.approx(0.05 / 2_000)
    assert MOF.cell_level("confirm-gate", 0) == pytest.approx(0.0227501319, rel=1e-8)
    assert MOF.cell_level("confirm-by", 1) == pytest.approx(0.10)
    assert MOF.cell_level("confirm-by", 16) == pytest.approx(0.10 / (16 * 3.380728993), rel=1e-8)


def test_importance_sampling_agrees_with_plain_draws():
    """At the confirm gate on 200 label rows, 20,000 plain null draws and 2,000 proposal draws give the same quantile
    (within three combined standard errors), the proposal's is the tighter, and its weights average 1."""
    level = MOF.cell_level("confirm-gate", 0)
    t, logw = MOF.null_t(np.random.default_rng(7), 20_000, MOF.ROWS_CONFIRM)
    assert not logw.any()
    plain = MOF.tail_quantile(t, logw, level)
    plain_se = MOF.quantile_se(t, logw, level, np.random.default_rng(8))
    t_is, logw_is = MOF.null_t(np.random.default_rng(9), 2_000, MOF.ROWS_CONFIRM, 3.1)
    tilted = MOF.tail_quantile(t_is, logw_is, level)
    tilted_se = MOF.quantile_se(t_is, logw_is, level, np.random.default_rng(10))
    assert abs(plain - tilted) <= 3.0 * math.hypot(plain_se, tilted_se), (plain, plain_se, tilted, tilted_se)
    assert tilted_se < plain_se
    w = np.exp(logw_is)
    assert abs(w.mean() - 1.0) <= 4.0 * w.std(ddof=1) / math.sqrt(w.size)


def test_three_cells_reproduce_the_pinned_derivation():
    """The confirm gate, the confirm band of 16 reads and the 1,000 budget band, re-derived at their committed seeds
    and draws, equal the pinned record, and their bands' factors follow."""
    names = [MOF.cell_name(*MOF.CELLS[i][::2]) for i in (0, 1, 5)]
    assert names == ["confirm-gate:0", "confirm-by:16", "discover:1000"]
    out = MOF.derive([0, 1, 5])["cells"]
    for name in names:
        ratio, se = MOF.DERIVED[name]
        assert out[name]["ratio"] == pytest.approx(ratio, abs=1e-5), name
        assert out[name]["ratio_se"] == pytest.approx(se, abs=1e-5), name
        assert out[name]["draws"] == MOF.DRAWS[out[name]["rows"]]
        assert out[name]["tail_rel_se"] < 0.05, name
    assert MOF.factor_of(out["discover:1000"]["upper"]) == dict(MOF.OVERLAP_BANDS)[1_000]
    confirm = max(out["confirm-gate:0"]["upper"], out["confirm-by:16"]["upper"])
    assert MOF.factor_of(confirm) == dict(MOF.CONFIRM_BANDS)[16]


def test_the_estimator_is_the_verbs():
    daily = pinned_series()
    assert MOF.summarize_t(daily) == pytest.approx(2.096947998340487, rel=1e-12)
    assert MOF.summarize_t(daily[:12]) == pytest.approx(-0.3360738764789554, rel=1e-12)  # lag clamps to 10
    rng = np.random.default_rng(7)
    for n in (5, 22, 23, 80):
        x = rng.standard_normal(n) + 0.1
        assert MOF.hac_t(x[None, :])[0] == pytest.approx(direct_t(list(x), MOF.HAC_LAG), rel=1e-10)


def test_the_daily_statistic_is_the_rank_correlation():
    """null_t's daily IC (sum_j c[order_j] c_j / sum c^2) is Pearson(centred signal rank, label rank)."""
    rng = np.random.default_rng(3)
    labels = rng.standard_normal((40, MOF.NAMES))
    order = np.argsort(labels, axis=-1)
    fast = MOF.CENTRED[order] @ MOF.CENTRED / MOF.CENTRED_SS
    ranks = np.argsort(order, axis=-1).astype(float)
    slow = [np.corrcoef(MOF.CENTRED, r)[0, 1] for r in ranks]
    assert fast == pytest.approx(slow, abs=1e-12)
