"""R3b monthly evaluation harness: planted-signal recovery, null-family size, look-ahead
refusal, decay, turnover by hand, size-bucket partition, definition parity with
signal_eval, frozen splits and a reproducible store manifest over real R3a labels."""

from __future__ import annotations

import datetime as dt
import json
import math
from typing import Any

import duckdb
import numpy as np
import pandas as pd
import pytest

from atx_db import signal_eval
from atx_db.connection import DuckDBStore
from atx_db.research import evaluation as ev
from atx_db.research.labels import MonthlyForwardLabelOptions, refresh_monthly_forward_labels
from atx_db.research.store import ResearchStore

TRUE_IC = 0.05
BETA = 2.0 * math.sin(TRUE_IC * math.pi / 6.0)  # Pearson that gives Spearman 0.05 for Gaussians


# ---------------------------------------------------------------------------
# Synthetic month-end market (pure engine inputs)
# ---------------------------------------------------------------------------

def _month(start: dt.date, offset: int) -> dt.date:
    total = start.month - 1 + offset
    return dt.date(start.year + total // 12, total % 12 + 1, 1)


class Market:
    """``n_months`` formed month-ends; ``returns[t]`` is realized over the month after formation t."""

    def __init__(self, returns: np.ndarray, n_months: int, *, start=dt.date(2012, 1, 1), caps=None, nyse=None,
                 horizons=(1, 3, 6, 12), missing=()):
        self.n_months, self.n_names = n_months, returns.shape[1]
        self.returns, self.horizons = returns, horizons
        rows = []
        for m in range(n_months):
            month_start = _month(start, m)
            formation = _month(start, m + 1) - dt.timedelta(days=1)
            formed = m not in missing
            rows.append({"month_index": m, "month_start": month_start,
                         "formation_date": formation if formed else None,
                         "cutoff": dt.datetime.combine(formation, dt.time(22)) if formed else None,
                         "entry_date": formation + dt.timedelta(days=1) if formed else None,
                         "status": "formed" if formed else "missing_month_end", "eligible_members": self.n_names})
        self.calendar = pd.DataFrame(rows)
        self.formed = [m for m in range(n_months) if m not in missing]
        self.entry = {m: rows[m]["entry_date"] for m in self.formed}
        self.caps = caps if caps is not None else np.exp(np.linspace(0.0, 6.0, self.n_names))
        self.nyse = nyse if nyse is not None else (np.arange(self.n_names) % 3 == 0)

    def labels(self) -> pd.DataFrame:
        parts = []
        growth = np.log1p(self.returns)
        cumulative = np.vstack([np.zeros(self.n_names), np.cumsum(growth, axis=0)])
        for h in self.horizons:
            months = np.array([m for m in self.formed if m + h <= len(self.returns)], dtype=np.int64)
            forward = np.expm1(cumulative[months + h] - cumulative[months])
            entries = np.array([self.entry[m] for m in months], dtype="datetime64[D]")
            parts.append(pd.DataFrame({
                "month_index": np.repeat(months, self.n_names).astype(np.int32),
                "security": np.tile(np.arange(self.n_names, dtype=np.int32), len(months)),
                "horizon_months": np.int8(h), "forward_return": forward.ravel(), "status": np.int8(0),
                "terminal": np.int8(0), "anchor_date": np.repeat(entries, self.n_names)}))
        return pd.concat(parts, ignore_index=True)

    def maturity(self) -> pd.DataFrame:
        rows = []
        for h in self.horizons:
            for m in self.formed:
                end = _month(dt.date(2012, 1, 1), m + h + 1) - dt.timedelta(days=1)
                rows.append({"month_index": m, "horizon_months": h, "expected_end": end,
                             "matured": m + h <= len(self.returns)})
        return pd.DataFrame(rows)

    def context(self) -> pd.DataFrame:
        months = np.repeat(np.array(self.formed, dtype=np.int32), self.n_names)
        return pd.DataFrame({"month_index": months,
                             "security": np.tile(np.arange(self.n_names, dtype=np.int32), len(self.formed)),
                             "market_cap": np.tile(self.caps, len(self.formed)),
                             "is_nyse": np.tile(self.nyse, len(self.formed))})

    def basis(self, features: dict, *, labels=None, basis="reconstructed", variants=("signed_raw",),
              controls=None) -> ev.BasisInputs:
        loader = features.__getitem__ if isinstance(features, dict) else features
        names = list(features) if isinstance(features, dict) else features.names
        return ev.BasisInputs(
            basis=basis, status=ev.BASIS_AVAILABLE, security_count=self.n_names, calendar=self.calendar,
            labels=self.labels() if labels is None else labels, maturity=self.maturity(), context=self.context(),
            controls=controls if controls is not None else pd.DataFrame(columns=["month_index", "security",
                                                                                 "control", "value"]),
            variants_by_feature={name: tuple(variants) for name in names}, load_feature=loader,
            meta={"identity_basis": "synthetic", "universe_basis": "synthetic"}, release_frames=True)

    def feature(self, feature_id: str, values: np.ndarray, *, variant="signed_raw", sign=1,
                available_at=None) -> ev.FeatureData:
        months = np.repeat(np.array(self.formed, dtype=np.int32), self.n_names)
        frame = pd.DataFrame({"month_index": months,
                              "security": np.tile(np.arange(self.n_names, dtype=np.int32), len(self.formed)),
                              "variant": variant, "value": values[self.formed].ravel()})
        if available_at is not None:
            frame["available_at"] = available_at
        return ev.FeatureData(feature_id, sign, frame)


def _ar1(rng: np.random.Generator, periods: int, names: int, rho: float) -> np.ndarray:
    values = np.empty((periods, names))
    values[0] = rng.standard_normal(names)
    scale = math.sqrt(1.0 - rho * rho)
    for t in range(1, periods):
        values[t] = rho * values[t - 1] + scale * rng.standard_normal(names)
    return values


def _spec(**overrides) -> ev.EvaluationSpec:
    base: dict[str, Any] = {"run_id": "r3b_test", "min_names": 30, "min_bucket_names": 10, "fm_min_obs": 30, "min_formations": 24,
            "nyse_min_names": 10, "bootstrap_resamples": 499}
    base.update(overrides)
    return ev.EvaluationSpec(**base)


def _cell(tables: ev.EvaluationTables, h: int, feature: str = "planted", variant: str = "signed_raw") -> pd.Series:
    cells = tables.cells
    chosen = cells[(cells.feature_id == feature) & (cells.variant == variant) & (cells.horizon_months == h)]
    assert len(chosen) == 1
    return chosen.iloc[0]


# ---------------------------------------------------------------------------
# Acceptance: planted signal, decay
# ---------------------------------------------------------------------------

def test_planted_signal_is_recovered_inside_its_bootstrap_ci_with_hac_t_above_three():
    """IC 0.05, 2,000 names, 160 months, AR(1) feature (rho 0.9)."""
    rng = np.random.default_rng(20260925)
    names, months = 2000, 160
    signal = _ar1(rng, months + 12, names, 0.9)
    shocks = BETA * signal + math.sqrt(1.0 - BETA ** 2) * rng.standard_normal(signal.shape)
    returns = np.expm1(0.08 * shocks + rng.normal(0.005, 0.04, (months + 12, 1)))
    market = Market(returns, months)
    tables = ev.evaluate_bases([market.basis({"planted": market.feature("planted", signal)})],
                               _spec(min_names=200, fm_min_obs=200, min_bucket_names=50, nyse_min_names=20))
    one = _cell(tables, 1)
    assert one.status == "tested" and one.ic_n == months
    assert one.ic_boot_low < TRUE_IC < one.ic_boot_high  # the planted IC lies in its studentized CI
    assert one.ic_boot_low < one.ic_mean < one.ic_boot_high
    assert abs(one.ic_mean - TRUE_IC) < 0.004
    assert one.ic_nw_t > 3.0 and one.ic_z > 3.0 and one.ic_hlz_pass and one.hlz_pass
    assert one.ls_ew10_z > 3.0 and one.ls_vw10_mean > 0 and one.fm_slope > 0 and one.fm_z > 3.0
    assert one.mono_ew10 > 0.9 and one.bh_discovery and one.dsr_n_trials == 4
    # One tested cell per horizon: the cross-trial Sharpe variance, hence the DSR, is undefined.
    assert pd.isna(one.dsr) and pd.isna(one.dsr_sharpe_variance)
    for h in (3, 6, 12):
        cell = _cell(tables, h)
        assert cell.status == "tested" and cell.ic_z > 3.0 and cell.ic_robust_df >= 1
        assert cell.ic_nw_lags >= h - 1 and cell.dsr_effective_n == cell.sharpe_n // h
    decay = tables.decay
    marginal = decay[decay.kind == "marginal_month"].sort_values("horizon_months")
    assert list(marginal.horizon_months) == [1, 3, 6, 12]
    assert np.all(np.diff(marginal.ic_mean.to_numpy()) < 0)  # month-k predictability decays like rho^(k-1)
    expected = [TRUE_IC * 0.9 ** (k - 1) for k in (1, 3, 6, 12)]
    assert np.allclose(marginal.ic_mean.to_numpy(), expected, atol=0.006)
    cumulative = decay[decay.kind == "cumulative"].sort_values("horizon_months")
    assert np.all(np.diff(cumulative.ic_mean.to_numpy()) > 0)  # a persistent signal accumulates
    lag = decay[decay.kind == "availability_lag1"].set_index("horizon_months")
    assert 0.0 < lag.loc[1, "ic_mean"] < one.ic_mean  # one month stale still predicts, but less


@pytest.mark.slow  # 130-240 s (800 cells x 199 bootstrap resamples); runs in the gate with --run-slow
def test_pure_noise_family_controls_false_discoveries():
    """200 AR(1) noise features x 4 horizons: BH discoveries <= 15 at q=0.05, ~0 HLZ passes."""
    rng = np.random.default_rng(99)
    names, months, n_features = 300, 160, 200
    loadings = rng.standard_normal(names)
    factor = rng.normal(0.0, 0.03, (months + 12, 1))
    returns = np.expm1(0.08 * rng.standard_normal((months + 12, names)) + factor * loadings
                       + rng.normal(0.005, 0.04, (months + 12, 1)))
    market = Market(returns, months)

    class Noise:
        names = tuple(f"noise_{i:03d}" for i in range(n_features))

        def __call__(self, feature_id):
            seed = int(feature_id.split("_")[1])
            return market.feature(feature_id, _ar1(np.random.default_rng(1000 + seed), months, names, 0.8))

    tables = ev.evaluate_bases([market.basis(Noise())], _spec(min_names=100, fm_min_obs=100,
                                                              bootstrap_resamples=199))
    family = tables.family
    cells = tables.cells
    assert family["n_trials"] == n_features * 4 == int(cells.family_member.sum())
    assert family["bh_discoveries"] <= 15
    assert family["hlz_passes"] <= 4
    rejections = float((cells.ic_robust_p <= 0.05).mean())
    assert 0.01 <= rejections <= 0.10  # EWC size near nominal under overlap
    tested = cells[cells.status == "tested"]
    assert (tested.dsr_n_trials == n_features * 4).all()
    assert (tested.dsr_effective_n == tested.sharpe_n // tested.horizon_months).all()
    assert int(tested.family_best.sum()) == 4  # one best-of-N pick per horizon, deflated by the family
    assert (tested[tested.family_best].dsr < 0.95).all()


# ---------------------------------------------------------------------------
# Point-in-time guards
# ---------------------------------------------------------------------------

def _small_market(names=40, months=30, seed=3):
    rng = np.random.default_rng(seed)
    returns = np.expm1(0.05 * rng.standard_normal((months + 12, names)))
    return Market(returns, months), rng


def test_labels_shifted_one_period_earlier_are_refused_as_lookahead():
    market, rng = _small_market()
    feature = market.feature("planted", rng.standard_normal((market.n_months, market.n_names)))
    labels = market.labels()
    shifted = labels.assign(month_index=labels.month_index + 1)  # formation m now sees the label of m-1
    shifted = shifted[shifted.month_index < market.n_months]
    with pytest.raises(ev.LookaheadError, match="look-ahead"):
        ev.evaluate_bases([market.basis({"planted": feature}, labels=shifted)], _spec(min_formations=5))
    lagged = labels.assign(month_index=labels.month_index - 1)
    lagged = lagged[lagged.month_index >= 0]
    with pytest.raises(ev.LookaheadError, match="not anchored"):
        ev.evaluate_bases([market.basis({"planted": feature}, labels=lagged)], _spec(min_formations=5))
    ev.evaluate_bases([market.basis({"planted": feature})], _spec(min_formations=5))  # aligned labels pass


def test_feature_available_after_its_formation_cutoff_is_refused():
    market, rng = _small_market()
    values = rng.standard_normal((market.n_months, market.n_names))
    cutoffs = np.repeat([market.calendar.cutoff[m] for m in market.formed], market.n_names)
    late = pd.Series(pd.to_datetime(cutoffs)).copy()
    late.iloc[7] = late.iloc[7] + pd.Timedelta(minutes=1)
    feature = market.feature("planted", values, available_at=late)
    with pytest.raises(ev.LookaheadError, match="after their formation cutoff"):
        ev.evaluate_bases([market.basis({"planted": feature})], _spec(min_formations=5))
    on_time = market.feature("planted", values, available_at=pd.to_datetime(cutoffs))
    ev.evaluate_bases([market.basis({"planted": on_time})], _spec(min_formations=5))


def test_missing_month_end_is_a_gap_not_a_substitution():
    rng = np.random.default_rng(5)
    returns = np.expm1(0.05 * rng.standard_normal((42, 40)))
    market = Market(returns, 30, missing=(10,))
    values = rng.standard_normal((30, 40))
    tables = ev.evaluate_bases([market.basis({"planted": market.feature("planted", values)})],
                               _spec(min_formations=5, horizons_months=(1,), marginal_months=(1,)))
    cell = _cell(tables, 1)
    assert cell.formations_formed == 29 and cell.ic_n == 29
    with pytest.raises(ev.EvaluationInputError, match="not a formed formation"):
        bad = market.feature("planted", values)
        extra = bad.values.iloc[:3].assign(month_index=10)
        ev.evaluate_bases([market.basis({"planted": ev.FeatureData("planted", 1, pd.concat([bad.values, extra]))})],
                          _spec(min_formations=5, horizons_months=(1,), marginal_months=(1,)))


# ---------------------------------------------------------------------------
# Turnover, size buckets, definition parity
# ---------------------------------------------------------------------------

def test_turnover_net_spread_and_rank_autocorrelation_match_hand_calculation():
    names, months = 20, 3
    values = np.tile(np.arange(names, dtype=float), (months, 1))
    values[1, [19, 17]] = [100.0, 99.0]    # top decile {17, 19}: one kept, one new
    values[1, 18] = -1.0                   # 18 falls to the bottom decile with 0
    values[2] = np.arange(names)[::-1]     # full reversal: top {0, 1}, bottom {18, 19}
    returns = np.zeros((months + 1, names))
    returns[:months] = 0.001 * values[:months] / 10.0
    market = Market(returns, months, horizons=(1,))
    tables = ev.evaluate_bases([market.basis({"planted": market.feature("planted", values)})],
                               _spec(min_names=10, min_formations=2, horizons_months=(1,), marginal_months=(1,),
                                     bootstrap_resamples=0, fm_min_obs=10))
    cell = _cell(tables, 1)
    # Hand: top {18,19} -> {17,19} -> {0,1}: traded 1.0 then 2.0 (one-way 0.5, 1.0).
    #       bottom {0,1} -> {0,18} -> {18,19}: traded 1.0 then 1.0 (one-way 0.5, 0.5).
    assert cell.top_turnover_1m == pytest.approx(0.75, abs=1e-12)
    assert cell.bottom_turnover_1m == pytest.approx(0.5, abs=1e-12)
    top = {0: [18, 19], 1: [17, 19], 2: [0, 1]}
    bottom = {0: [0, 1], 1: [0, 18], 2: [18, 19]}
    spreads = [returns[m, top[m]].mean() - returns[m, bottom[m]].mean() for m in range(months)]
    traded = {1: 1.0 + 1.0, 2: 2.0 + 1.0}
    assert cell.ls_ew10_mean == pytest.approx(np.mean(spreads), abs=1e-15)
    for bp, column in ((10, "net10_mean"), (25, "net25_mean"), (50, "net50_mean")):
        expected = np.mean([spreads[m] - bp / 10_000.0 * traded[m] for m in (1, 2)])
        assert cell[column] == pytest.approx(expected, abs=1e-15)
    autocorr = [pd.Series(values[m]).rank().corr(pd.Series(values[m - 1]).rank()) for m in (1, 2)]
    assert cell.rank_autocorr_1m == pytest.approx(np.mean(autocorr), abs=1e-12)


def test_traded_fraction_handles_unequal_leg_sizes():
    # Old leg {0,1,2} (1/3 each), new leg {0,3} (1/2 each): |1/2-1/3| + 1/3 + 1/3 + 1/2 = 4/3.
    traded = ev.traded_fraction(np.array([0, 0, 0, 1, 1]), np.array([0, 1, 2, 0, 3]), 10, 2,
                                np.array([True, True]), 1)
    assert math.isnan(traded[0]) and traded[1] == pytest.approx(4.0 / 3.0, abs=1e-15)


def test_size_buckets_partition_the_cohort_with_nyse_or_tercile_breakpoints():
    rng = np.random.default_rng(11)
    months, names = 2, 120
    caps = np.exp(rng.normal(7.0, 2.0, names))
    caps[:6] = np.nan                      # unknown size
    nyse = np.zeros(names, dtype=bool)
    nyse[10:50] = True                     # 40 identifiable NYSE names
    month = np.repeat(np.arange(months), names)
    cap = np.tile(caps, months)
    is_nyse = np.tile(nyse, months)
    is_nyse[month == 1] = False            # month 1: no venue -> terciles
    keys = month * names + np.tile(np.arange(names), months)
    spec = ev.validate_spec(_spec(nyse_min_names=20))
    buckets, methods = ev._size_buckets(keys, cap, is_nyse, months, names, spec)
    assert methods == ["nyse_20_50", "cap_terciles"]
    assert set(np.unique(buckets)) <= {0, 1, 2, 3}
    known = np.isfinite(caps)
    low, high = np.percentile(caps[nyse & known], [20.0, 50.0])
    expected0 = np.where(~known, 0, np.where(caps < low, 1, np.where(caps < high, 2, 3)))
    assert np.array_equal(buckets[:names], expected0)
    t1, t2 = np.percentile(caps[known], [100 / 3, 200 / 3])
    expected1 = np.where(~known, 0, np.where(caps < t1, 1, np.where(caps < t2, 2, 3)))
    assert np.array_equal(buckets[names:], expected1)
    # Through the engine: the four slices partition every labeled name.
    returns = np.expm1(0.05 * rng.standard_normal((months + 1, names)))
    market = Market(returns, months, caps=caps, nyse=nyse, horizons=(1,))
    tables = ev.evaluate_bases([market.basis({"planted": market.feature("planted", rng.standard_normal((2, names)))})],
                               _spec(min_names=50, min_bucket_names=5, min_formations=2, horizons_months=(1,),
                                     marginal_months=(1,), nyse_min_names=20, bootstrap_resamples=0, fm_min_obs=50))
    slices = tables.slices[tables.slices.slice_kind == "size_bucket"]
    assert sorted(slices.slice_name) == sorted(ev.SIZE_BUCKETS)
    assert slices.name_share.sum() == pytest.approx(1.0, abs=1e-12)
    assert slices.set_index("slice_name").loc["unknown", "name_share"] == pytest.approx(6 / names, abs=1e-12)


def test_ic_decile_spread_and_autocorrelation_equal_signal_eval_definitions():
    rng = np.random.default_rng(17)
    names, months = 60, 12
    values = np.round(rng.standard_normal((months, names)), 1)  # heavy ties
    returns = np.round(0.03 * rng.standard_normal((months + 3, names)), 3)
    market = Market(returns, months, horizons=(1, 3))
    tables = ev.evaluate_bases([market.basis({"planted": market.feature("planted", values)})],
                               _spec(min_names=10, min_formations=2, horizons_months=(1, 3), marginal_months=(1,),
                                     bootstrap_resamples=0, fm_min_obs=10))
    panel = pd.DataFrame({"security_id": np.tile(np.arange(names), months),
                          "as_of_date": pd.to_datetime(np.repeat([market.calendar.formation_date[m] for m in range(months)],
                                                                 names)),
                          "factor_id": "planted", "value": values.ravel()})
    labels = market.labels()
    forward = pd.DataFrame({"security_id": labels.security, "horizon": labels.horizon_months,
                            "as_of_date": pd.to_datetime([market.calendar.formation_date[m] for m in labels.month_index]),
                            "forward_return": labels.forward_return})
    legacy = signal_eval.compute_information_coefficient(panel, forward, horizons=(1, 3)).per_date
    for h in (1, 3):
        mine, _ = ev.grouped_rank_correlation(labels.month_index[labels.horizon_months == h],
                                              values[labels.month_index[labels.horizon_months == h],
                                                     labels.security[labels.horizon_months == h]],
                                              labels.forward_return[labels.horizon_months == h], months)
        theirs = legacy[legacy.horizon == h].sort_values("as_of_date").rank_ic.to_numpy()
        assert np.allclose(mine[np.isfinite(mine)], theirs, rtol=0, atol=1e-12)
        spread = signal_eval.compute_quantile_spread(panel, forward, n_quantiles=10, horizons=(h,))
        cell = _cell(tables, h)
        assert cell.ls_ew10_mean == pytest.approx(spread.long_short_spread.iloc[0], abs=1e-14)
        assert cell.mono_ew10 == pytest.approx(spread.decile_monotonicity.iloc[0], abs=1e-12)
    turnover = signal_eval.compute_turnover(panel, n_quantiles=10)
    assert _cell(tables, 1).rank_autocorr_1m == pytest.approx(turnover.mean_rank_autocorrelation.iloc[0], abs=1e-12)


def test_vectorized_cross_sectional_ols_equals_r3a_fama_macbeth_per_date():
    rng = np.random.default_rng(29)
    months, names = 9, 80
    month = np.repeat(np.arange(months), names)
    x = rng.standard_normal(len(month))
    size = 20.0 + 2.0 * rng.standard_normal(len(month))  # unscaled level: conditioning must hold
    control = rng.standard_normal(len(month))
    y = 0.02 * x - 0.001 * size + 0.01 * control + 0.05 * rng.standard_normal(len(month))
    x[month == 3] = np.nan                    # a formation without the regressor
    keep = ~((month == 5) & (np.arange(len(month)) % names >= 12))  # a thin formation (12 names)
    control[month == 7] = 2.0 * size[month == 7] - 1.0  # rank deficient: control collinear with size
    frame = pd.DataFrame({"month": month, "y": y, "x": x, "size": size, "control": control})[keep]
    oracle = ev.stats.fama_macbeth(frame, date="month", y="y", x=["x", "size", "control"], horizon_periods=1,
                                   min_obs=30, formation_dates=range(months)).per_date
    slopes, status, counts = ev.cross_sectional_ols(
        frame.month.to_numpy(), frame.y.to_numpy(), [frame.x.to_numpy(), frame["size"].to_numpy(),
                                                     frame.control.to_numpy()], months, min_obs=30)
    labels = {ev.FM_ESTIMATED: "estimated", ev.FM_INSUFFICIENT: "insufficient_obs",
              ev.FM_RANK_DEFICIENT: "rank_deficient"}
    assert [labels[int(s)] for s in status] == oracle.status.tolist()
    assert list(oracle.status[[3, 5, 7]]) == ["insufficient_obs", "insufficient_obs", "rank_deficient"]
    assert np.allclose(slopes, oracle[["x", "size", "control"]].to_numpy(), rtol=1e-9, atol=1e-12, equal_nan=True)
    assert counts.tolist() == oracle.n_obs.tolist()


# ---------------------------------------------------------------------------
# Frozen split (RX7)
# ---------------------------------------------------------------------------

def test_frozen_split_purges_label_overlap_and_keeps_holdout_out_of_selection(tmp_path):
    split = ev.freeze_split("split_v1", [("train", "2012-01-01", "2019-12-31"),
                                         ("validation", "2020-01-01", "2022-12-31"),
                                         ("holdout", "2023-01-01", "2025-12-31")], embargo_months=2)
    path = tmp_path / "split.json"
    path.write_text(json.dumps({**split.content(), "sha256": split.sha256}), encoding="utf-8")
    assert ev.load_frozen_split(path) == split
    tampered = {**split.content(), "sha256": split.sha256}
    tampered["segments"][2]["start"] = "2022-07-01"
    tampered["segments"][1]["end"] = "2022-06-30"
    with pytest.raises(ev.EvaluationInputError, match="not frozen"):
        ev.load_frozen_split(tampered)
    rng = np.random.default_rng(23)
    names, months = 60, 160
    signal = _ar1(rng, months + 12, names, 0.5)
    returns = np.expm1(0.05 * (0.3 * signal + rng.standard_normal(signal.shape)))
    market = Market(returns, months)
    tables = ev.evaluate_bases([market.basis({"planted": market.feature("planted", signal)})],
                               _spec(split=split, bootstrap_resamples=0))
    dates = pd.to_datetime(market.calendar.formation_date)
    selection_months = int(((dates >= "2012-01-01") & (dates <= "2022-12-31")).sum())
    holdout_months = int(((dates >= "2023-01-01") & (dates <= "2025-12-31")).sum())
    for h in (1, 3, 6, 12):
        cell = _cell(tables, h)
        assert cell["sample"] == "selection"
        # Label windows that end in the holdout are purged from selection: the last h formations.
        assert cell.ic_n == selection_months - h
        slices = tables.slices[(tables.slices.horizon_months == h) & (tables.slices.slice_kind == "split")]
        by_name = slices.set_index("slice_name")
        assert by_name.loc["train", "purged"] == h and by_name.loc["validation", "purged"] == h
        assert by_name.loc["validation", "embargoed"] == 2 and by_name.loc["holdout", "embargoed"] == 2
        assert by_name.loc["holdout", "formations"] == holdout_months - 2
        assert by_name.loc["selection", "formations"] == selection_months - h
        assert by_name.loc["full", "formations"] == months
    subperiods = tables.slices[tables.slices.slice_kind == "subperiod"]
    assert set(subperiods.slice_name) == {"sub_2013_2016", "sub_2017_2020", "sub_2021_2025"}
    late = subperiods[(subperiods.slice_name == "sub_2021_2025") & (subperiods.horizon_months == 1)].iloc[0]
    assert late.formations == 23  # 2021-01..2022-11: the holdout never enters a selection slice


# ---------------------------------------------------------------------------
# Store path: R2b contract adapter, R2a context, real R3a labels, sealed manifest
# ---------------------------------------------------------------------------

SESSIONS = [day.date() for day in pd.bdate_range("2023-01-02", "2024-12-31")]
N_SECURITIES = 60
DELIST_LAST_TRADE = dt.date(2023, 11, 14)  # S007 delists 2023-11-15 at -40% (observed)


def _bulk(con, sql, columns, rows):
    """``INSERT ... SELECT <columns> FROM`` a registered frame (row-wise executemany is too slow)."""
    con.register("_fixture_rows", pd.DataFrame(rows, columns=columns))
    try:
        con.execute(sql.format(columns=", ".join(columns), source="_fixture_rows"))
    finally:
        con.unregister("_fixture_rows")


def _warehouse(path):
    """Bars with a planted month-ahead drift, the observed calendar, one delisting; real monthly labels."""
    rng = np.random.default_rng(31)
    store = DuckDBStore(path)
    store.connection = duckdb.connect(str(path), config={"threads": 1, "memory_limit": "256MB"})
    store._configure_session(store.con)
    store.analytical_memory_limit = "256MB"
    store.analytical_threads = 1
    store._initialized = True
    con = store.con
    con.execute("""
        CREATE TABLE equity_daily_bars (source VARCHAR, security_id VARCHAR, symbol VARCHAR, trade_date DATE,
            close DOUBLE, adjusted_close DOUBLE, available_at TIMESTAMP, vendor_security_id VARCHAR,
            source_loaded_at TIMESTAMP);
        CREATE TABLE trading_calendar (calendar_id VARCHAR, trade_date DATE, is_open BOOLEAN, source VARCHAR);
        CREATE TABLE delisting_terminal_returns (terminal_return_id VARCHAR, security_id VARCHAR, delist_date DATE,
            terminal_return DOUBLE, terminal_return_source VARCHAR, return_observation_id VARCHAR,
            available_at TIMESTAMP, source_loaded_at TIMESTAMP);
        CREATE TABLE delisting_events (security_id VARCHAR, delist_date DATE, available_at TIMESTAMP);
        CREATE TABLE forward_returns_survivorship_safe (
            forward_return_id VARCHAR PRIMARY KEY, source VARCHAR NOT NULL, security_id VARCHAR NOT NULL,
            symbol VARCHAR, as_of_date DATE NOT NULL, horizon_days INTEGER NOT NULL, forward_end_date DATE,
            raw_forward_return DOUBLE, terminal_return DOUBLE, forward_return DOUBLE NOT NULL,
            is_delisted_in_horizon BOOLEAN NOT NULL, is_stitched BOOLEAN NOT NULL, delist_date DATE,
            terminal_return_source VARCHAR, return_observation_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true, available_at TIMESTAMP NOT NULL, run_id VARCHAR,
            price_basis VARCHAR, calculation_version VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now());
    """)
    _bulk(con, "INSERT INTO trading_calendar SELECT 'XNYS', CAST(day AS DATE), true, 'equity_daily_bars calendar' "
               "FROM {source}", ["day"], [(day,) for day in SESSIONS])
    month_keys = sorted({(d.year, d.month) for d in SESSIONS})
    signal = rng.standard_normal((len(month_keys), N_SECURITIES))
    last_trade = {f"S{i:03d}": SESSIONS[-1] for i in range(N_SECURITIES)}
    last_trade["S007"] = DELIST_LAST_TRADE  # delists inside validation: observed terminal
    rows = []
    for i in range(N_SECURITIES):
        security = f"S{i:03d}"
        price = 20.0 + i
        for day in SESSIONS:
            if day > last_trade[security]:
                break
            month = month_keys.index((day.year, day.month))
            drift = 0.002 * signal[month - 1, i] if month else 0.0  # the month-end value predicts next month
            price *= math.exp(drift + 0.01 * rng.standard_normal())
            rows.append((security, security, day, price, price, dt.datetime.combine(day, dt.time(22))))
    _bulk(con, "INSERT INTO equity_daily_bars SELECT 'prices', security_id, symbol, CAST(trade_date AS DATE), close, "
               "adjusted_close, available_at, 'v', TIMESTAMP '2025-01-01' FROM {source}",
          ["security_id", "symbol", "trade_date", "close", "adjusted_close", "available_at"], rows)
    con.execute("INSERT INTO delisting_terminal_returns VALUES ('t7', 'S007', '2023-11-15', -0.4, 'observed', "
                "'obs-7', '2023-11-17', '2023-11-17')")
    con.execute("INSERT INTO delisting_events VALUES ('S007', '2023-11-15', '2023-11-17')")
    refresh_monthly_forward_labels(store, MonthlyForwardLabelOptions(run_id="r3b_fixture"))
    store.connection.close()
    return signal, month_keys


def _research_fixture(tmp_path):
    warehouse = tmp_path / "warehouse.duckdb"
    signal, month_keys = _warehouse(warehouse)
    research = ResearchStore(tmp_path / "research.duckdb", warehouse_path=warehouse, memory_limit="256MB")
    research.open()
    con = research.con
    panel_sha = "a" * 64
    for run_id, basis, status in (("panel_recon", "reconstructed", "complete"),
                                  ("panel_strict", "strict", "untestable_strict")):
        con.execute("""
            INSERT INTO research_panel_runs (run_id, status, basis, universe_id, identity_basis, universe_basis,
                fundamental_availability_basis, market_availability_basis, query_version, spec_json, spec_sha256,
                definitions_json, definitions_sha256, code_sha256, source_ids_json, calendar_sha256, start_month,
                end_month, as_of_date, run_at, warehouse_path, blockers_json, panel_sha256, created_at)
            VALUES (?, ?, ?, 'u', ?, 'u', 'conservative_filing_46h', 'modeled_trade_date_22h', 'q', '{}', 'x', '[]',
                    'x', 'x', '{}', 'x', '2023-01-01', '2024-12-01', '2024-12-31', '2025-01-02', 'w',
                    '["research_only_not_release_eligible"]', ?, '2025-01-02')
        """, [run_id, status, basis, f"{basis}_identity", panel_sha])
    calendar = []
    for year, month in month_keys:
        start = dt.date(year, month, 1)
        end = max(day for day in SESSIONS if (day.year, day.month) == (year, month))
        later = [day for day in SESSIONS if day > end]
        status = "formed" if later else "missing_next_session"
        calendar.append((start, end, end if later else None,
                         dt.datetime.combine(end, dt.time(22)) if later else None,
                         later[0] if later else None, status))
    con.executemany("""
        INSERT INTO research_panel_calendar (run_id, month_start, expected_session, last_observed_session,
            formation_date, cutoff, entry_date, status, eligible_members)
        VALUES ('panel_recon', ?, ?, ?, ?, ?, ?, ?, ?)
    """, [(s, e, e, f, c, n, st, N_SECURITIES) for s, e, f, c, n, st in calendar])
    cohort, caps, values, dates = [], [], [], []
    formed = [row for row in calendar if row[5] == "formed"]
    for month_position, (_, _, formation, cutoff, _, _) in enumerate(formed):
        for i in range(N_SECURITIES):
            security = f"S{i:03d}"
            if security == "S007" and formation >= DELIST_LAST_TRADE:
                continue
            cohort.append((formation, security, "XNYS" if i % 2 else "XNAS"))
            caps.append((formation, security, float(1e8 * (i + 1))))
            for feature, variants, value in (
                    ("alpha_one", ("signed_raw", "rank_normal"), signal[month_position, i]),
                    ("book_to_market", ("rank_normal",), math.sin(i + month_position)),
                    ("momentum_12_1", ("rank_normal",), math.cos(3 * i + month_position))):
                for variant in variants:
                    values.append(("fv_recon", formation, security, feature, variant,
                                   value if variant == "signed_raw" else value / 2.0, 1, cutoff))
        for feature, variants in (("alpha_one", ("signed_raw", "rank_normal")), ("book_to_market", ("rank_normal",)),
                                  ("momentum_12_1", ("rank_normal",))):
            dates.extend(("fv_recon", formation, feature, variant, "formed", N_SECURITIES, N_SECURITIES, 1.0)
                         for variant in variants)
    _bulk(con, """
        INSERT INTO research_panel_cohort (run_id, formation_date, security_id, exchange_code, identity_basis,
                                           cohort_reason)
        SELECT 'panel_recon', CAST(formation_date AS DATE), security_id, exchange_code,
               'current_ticker_unverified', 'valid' FROM {source}
    """, ["formation_date", "security_id", "exchange_code"], cohort)
    _bulk(con, """
        INSERT INTO research_panel_values (run_id, formation_date, security_id, feature_id, metric_code,
            metric_window, raw_value, reason, available_at, identity_basis, universe_basis, availability_basis)
        SELECT 'panel_recon', CAST(formation_date AS DATE), security_id, 'market_cap', 'market_cap', 'daily',
               raw_value, 'valid', TIMESTAMP '2023-01-01', 'x', 'u', 'm' FROM {source}
    """, ["formation_date", "security_id", "raw_value"], caps)
    con.execute("""
        CREATE TABLE research_feature_versions (feature_version VARCHAR, status VARCHAR, basis VARCHAR,
            panel_run_id VARCHAR, panel_sha256 VARCHAR, classification_basis VARCHAR, values_sha256 VARCHAR);
        CREATE TABLE research_feature_values (feature_version VARCHAR, formation_date DATE, security_id VARCHAR,
            feature_id VARCHAR, variant VARCHAR, value DOUBLE, expected_sign INTEGER, available_at TIMESTAMP);
        CREATE TABLE research_feature_dates (feature_version VARCHAR, formation_date DATE, feature_id VARCHAR,
            variant VARCHAR, date_status VARCHAR, eligible_members BIGINT, valid_names BIGINT,
            coverage_fraction DOUBLE);
    """)
    con.executemany("INSERT INTO research_feature_versions VALUES (?, ?, ?, ?, ?, 'current_sic_backcast', 'v')", [
        ("fv_recon", "sealed", "reconstructed", "panel_recon", panel_sha),
        ("fv_strict", "untestable_strict", "strict", "panel_strict", panel_sha)])
    value_columns = ["feature_version", "formation_date", "security_id", "feature_id", "variant", "value",
                     "expected_sign", "available_at"]
    _bulk(con, "INSERT INTO research_feature_values SELECT feature_version, CAST(formation_date AS DATE), "
               "security_id, feature_id, variant, value, expected_sign, available_at FROM {source}",
          value_columns, values)
    _bulk(con, "INSERT INTO research_feature_dates SELECT feature_version, CAST(formation_date AS DATE), feature_id, "
               "variant, date_status, eligible_members, valid_names, coverage_fraction FROM {source}",
          ["feature_version", "formation_date", "feature_id", "variant", "date_status", "eligible_members",
           "valid_names", "coverage_fraction"], dates)
    return research


def _store_spec(run_id, **overrides):
    split = ev.freeze_split("fixture_split", [("train", "2023-01-01", "2023-09-30"),
                                              ("validation", "2023-10-01", "2024-03-31"),
                                              ("holdout", "2024-04-01", "2024-12-31")])
    base: dict[str, Any] = {"run_id": run_id, "feature_versions": ("fv_recon", "fv_strict"),
            "label_cutoff": dt.datetime(2025, 1, 10, tzinfo=dt.UTC), "split": split, "min_names": 30,
            "min_bucket_names": 5, "fm_min_obs": 30, "min_formations": 3, "nyse_min_names": 5,
            "bootstrap_resamples": 99, "verify_panels": False}
    base.update(overrides)
    return ev.EvaluationSpec(**base)


def test_store_run_seals_a_manifest_that_reproduces_byte_identical_results(tmp_path):
    research = _research_fixture(tmp_path)
    try:
        con = research.con
        with pytest.raises(ev.EvaluationInputError, match="RX7"):
            ev.run_evaluation(research, _store_spec("r3b_unsplit", split=None))
        result = ev.run_evaluation(research, _store_spec("r3b_store"))
        assert result.status == "complete"
        cells = con.execute("SELECT * FROM research_eval_cells WHERE run_id='r3b_store'").df()
        recon = cells[cells.basis == "reconstructed"]
        strict = cells[cells.basis == "strict"]
        # Every reconstructed cell has a matching strict cell reported as untestable, outside the family.
        assert len(recon) == len(strict) == 4 * 4  # alpha_one x2, book_to_market, momentum_12_1 x 4 horizons
        assert set(strict.status) == {"untestable_strict"} and not strict.family_member.any()
        alpha = recon[(recon.feature_id == "alpha_one") & (recon.variant == "signed_raw")].set_index("horizon_months")
        assert alpha.loc[1, "status"] == "tested" and alpha.loc[1, "sample"] == "selection"
        assert alpha.loc[1, "ic_mean"] > 0.1  # planted month-ahead drift
        assert alpha.loc[1, "fmc_controls"] == "size,book_to_market,momentum_12_1"
        assert alpha.loc[1, "identity_basis"] == "reconstructed_identity"
        assert alpha.loc[1, "classification_basis"] == "current_sic_backcast"
        assert (recon.dsr_n_trials.dropna() == int(recon.family_member.sum())).all()
        # The delisting of S007 is stitched into the labels the harness used (observed terminal).
        assert alpha.loc[1, "stitched_observed"] >= 1
        attrition = con.execute("SELECT scope, horizon_months, measure, value FROM research_eval_attrition "
                                "WHERE run_id='r3b_store'").df()
        source = attrition[attrition.scope == "label_source"]
        assert {"survivorship_attrition", "unlabeled_matured", "observed_terminals"} <= set(source.measure)
        assert set(source.horizon_months) >= {1, 3, 6, 12}
        run = con.execute("SELECT status, results_sha256, blockers_json, family_json FROM research_eval_runs "
                          "WHERE run_id='r3b_store'").fetchone()
        blockers = json.loads(run[2])
        assert "strict_basis_untestable" in blockers
        assert "feature_table_contract_declared_by_r3b_pending_r2b" in blockers
        assert json.loads(run[3])["n_trials"] == int(recon.family_member.sum())
        # The manifest reproduces the results byte for byte, and a second run agrees.
        check = ev.verify_evaluation_run(research, "r3b_store")
        assert check["reproduced"] and check["stored_rows_match"] and check["inputs_match"]
        again = ev.run_evaluation(research, _store_spec("r3b_again"))
        assert again.results_sha256 == result.results_sha256 == run[1]
        with pytest.raises(ev.EvaluationInputError, match="already exists"):
            ev.run_evaluation(research, _store_spec("r3b_store"))
        con.execute("UPDATE research_eval_cells SET ic_mean = ic_mean + 1e-9 "
                    "WHERE run_id='r3b_store' AND status='tested'")
        tampered = ev.verify_evaluation_run(research, "r3b_store")
        assert not tampered["stored_rows_match"] and not tampered["reproduced"]
    finally:
        research.close()


def test_interrupted_run_resumes_to_the_clean_digest(tmp_path, monkeypatch):
    research = _research_fixture(tmp_path)
    try:
        clean = ev.run_evaluation(research, _store_spec("r3b_clean", bootstrap_resamples=0))
        original = ev._evaluate_feature
        calls = {"n": 0}

        def flaky(prep, inputs, spec, feature_id):
            calls["n"] += 1
            if calls["n"] == 2:
                raise RuntimeError("injected interruption")
            return original(prep, inputs, spec, feature_id)

        monkeypatch.setattr(ev, "_evaluate_feature", flaky)
        with pytest.raises(RuntimeError, match="injected"):
            ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=0))
        con = research.con
        assert con.execute("SELECT status FROM research_eval_runs WHERE run_id='r3b_resume'").fetchone() == ("failed",)
        assert con.execute("SELECT count(*) FROM research_eval_feature_inputs WHERE run_id='r3b_resume'"
                           ).fetchone() == (1,)  # the first feature committed atomically, the second did not
        with pytest.raises(ev.EvaluationInputError, match="already exists"):
            ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=0))
        with pytest.raises(ev.EvaluationInputError, match="spec or code changed"):
            ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=9), resume=True)
        resumed = ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=0), resume=True)
        assert resumed.results_sha256 == clean.results_sha256
        assert ev.verify_evaluation_run(research, "r3b_resume")["reproduced"]
    finally:
        research.close()


def test_cli_runs_and_verifies_a_sealed_evaluation(tmp_path, capsys):
    import importlib.util
    from pathlib import Path

    path = Path(__file__).resolve().parents[1] / "scripts" / "research_evaluate.py"
    loaded = importlib.util.spec_from_file_location("research_evaluate_cli", path)
    assert loaded is not None and loaded.loader is not None
    cli = importlib.util.module_from_spec(loaded)
    loaded.loader.exec_module(cli)
    research = _research_fixture(tmp_path)
    research.close()
    common = ["--run-id", "r3b_cli", "--research-db", str(tmp_path / "research.duckdb"),
              "--warehouse", str(tmp_path / "warehouse.duckdb"), "--memory-limit", "256MB"]
    assert cli.main(["run", *common, "--feature-version", "fv_recon", "--feature-version", "fv_strict",
                     "--label-cutoff", "2025-01-10T00:00:00Z", "--allow-unsplit", "--min-names", "30",
                     "--min-formations", "3", "--bootstrap-resamples", "0", "--skip-panel-validation"]) == 0
    summary = json.loads(capsys.readouterr().out)
    assert summary["status"] == "complete" and "no_frozen_split_selection_uses_all_formations" in summary["blockers"]
    assert cli.main(["verify", *common]) == 0
    assert json.loads(capsys.readouterr().out)["reproduced"] is True


def test_adapter_refuses_contract_violations(tmp_path):
    research = _research_fixture(tmp_path)
    try:
        con = research.con
        con.execute("UPDATE research_feature_values SET available_at = available_at + INTERVAL 1 HOUR "
                    "WHERE feature_id='alpha_one' AND variant='signed_raw' AND security_id='S001'")
        with pytest.raises(ev.LookaheadError):
            ev.run_evaluation(research, _store_spec("r3b_late"))
        status = con.execute("SELECT status FROM research_eval_runs WHERE run_id='r3b_late'").fetchone()
        assert status == ("failed",)
        con.execute("UPDATE research_feature_versions SET panel_sha256='b' WHERE feature_version='fv_recon'")
        with pytest.raises(ev.EvaluationInputError, match="panel digest"):
            ev.run_evaluation(research, _store_spec("r3b_digest"))
        con.execute("ALTER TABLE research_feature_values DROP COLUMN available_at")
        with pytest.raises(ev.EvaluationInputError, match="R2b feature contract"):
            ev.run_evaluation(research, _store_spec("r3b_contract"))
    finally:
        research.close()
