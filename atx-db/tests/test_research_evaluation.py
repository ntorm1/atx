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
from atx_db.research import features as rf
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
        """Verified caps and a point-in-time venue for every name (``venue_pit``)."""
        months = np.repeat(np.array(self.formed, dtype=np.int32), self.n_names)
        return pd.DataFrame({"month_index": months,
                             "security": np.tile(np.arange(self.n_names, dtype=np.int32), len(self.formed)),
                             "market_cap": np.tile(self.caps, len(self.formed)),
                             "venue_pit": True, "is_nyse_pit": np.tile(self.nyse, len(self.formed))})

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
    assert one.ls_nyse_ew10_z > 3.0 and one.venue_basis == f"nyse_pit:{months}"
    # One tested cell per horizon: the cross-trial Sharpe variance, hence the DSR, is undefined.
    assert pd.isna(one.dsr) and pd.isna(one.dsr_sharpe_variance)
    assert one.psr > 0.99  # supporting evidence: the EW decile spread's Sharpe is far from zero
    for h in (3, 6, 12):
        cell = _cell(tables, h)
        assert cell.status == "tested" and cell.ic_z > 3.0 and cell.ic_robust_df >= 1
        assert cell.ic_nw_lags >= h - 1 and cell.dsr_effective_n == cell.sharpe_n // h
        # PSR of overlapping h-month spreads counts floor(n/h) independent returns, not n.
        moments = {"n_obs": int(cell.sharpe_n), "skewness": cell.sharpe_skew, "kurtosis": cell.sharpe_kurt}
        assert cell.psr == pytest.approx(ev.stats.probabilistic_sharpe_ratio(cell.sharpe, 0.0, horizon_periods=h,
                                                                             **moments), abs=1e-15)
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

    tables = ev.evaluate_bases([market.basis(Noise())], _spec(min_names=100, fm_min_obs=100, bootstrap_resamples=199),
                               keep_frames=("cells",))  # the 128k-row series stays in the digest only
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


def test_size_buckets_partition_the_cohort_by_point_in_time_nyse_or_tercile_breakpoints():
    rng = np.random.default_rng(11)
    months, names = 2, 120
    caps = np.exp(rng.normal(7.0, 2.0, names))
    caps[:6] = np.nan                      # no verified cap: unknown size
    nyse = np.zeros(names, dtype=bool)
    nyse[10:50] = True                     # 40 point-in-time NYSE names
    month = np.repeat(np.arange(months), names)
    cap = np.tile(caps, months)
    venue = month == 0                     # month 1: no listing evidence as of the formation -> terciles
    keys = month * names + np.tile(np.arange(names), months)
    spec = ev.validate_spec(_spec(nyse_min_names=20))
    sized = ev._size_buckets(keys, cap, months, names, spec, venue_pit=venue, nyse_pit=np.tile(nyse, months))
    assert sized.venue_basis == ["nyse_pit", "cap_terciles"]
    assert sized.nyse_pit_names.tolist() == [40, 0] and sized.venue_pit_share.tolist() == [1.0, 0.0]
    assert set(np.unique(sized.buckets)) <= {0, 1, 2, 3}
    known = np.isfinite(caps)
    low, high = np.percentile(caps[nyse & known], [20.0, 50.0])
    expected0 = np.where(~known, 0, np.where(caps < low, 1, np.where(caps < high, 2, 3)))
    assert np.array_equal(sized.buckets[:names], expected0)
    t1, t2 = np.percentile(caps[known], [100 / 3, 200 / 3])
    expected1 = np.where(~known, 0, np.where(caps < t1, 1, np.where(caps < t2, 2, 3)))
    assert np.array_equal(sized.buckets[names:], expected1)
    # Through the engine: per breakpoint basis the four slices partition every labeled name, and an
    # NYSE-bucket slice never includes the tercile formation.
    returns = np.expm1(0.05 * rng.standard_normal((months + 1, names)))
    market = Market(returns, months, caps=caps, nyse=nyse, horizons=(1,))
    basis = market.basis({"planted": market.feature("planted", rng.standard_normal((2, names)))})
    basis.context = basis.context.assign(venue_pit=basis.context.month_index == 0)
    tables = ev.evaluate_bases([basis], _spec(min_names=50, min_bucket_names=5, min_formations=2,
                                              horizons_months=(1,), marginal_months=(1,), nyse_min_names=20,
                                              bootstrap_resamples=0, fm_min_obs=50))
    for kind, venue_basis in (("size_bucket", "nyse_pit"), ("size_tercile", "cap_terciles")):
        slices = tables.slices[tables.slices.slice_kind == kind]
        assert sorted(slices.slice_name) == sorted(ev.SIZE_BUCKETS) and set(slices.venue_basis) == {venue_basis}
        assert slices.name_share.sum() == pytest.approx(1.0, abs=1e-12)
        assert slices.set_index("slice_name").loc["unknown", "name_share"] == pytest.approx(6 / names, abs=1e-12)
        assert slices.formations.max() == 1
    assert _cell(tables, 1).venue_basis == "cap_terciles:1,nyse_pit:1"


def test_size_breakpoints_use_the_point_in_time_nyse_venue_of_the_valid_cohort_never_a_backcast():
    rng = np.random.default_rng(13)
    names = 100
    caps = np.exp(rng.normal(7.0, 2.0, names))
    keys = np.arange(names, dtype=np.int64)
    pit = np.zeros(names, dtype=bool)
    pit[:80] = True                               # listing evidence as of the formation for 80 names
    nyse_pit = np.arange(names) % 4 == 0          # ... which puts every fourth name on NYSE
    universe = np.ones(names, dtype=bool)
    universe[:8] = False                          # invalid or secondary lines never set breakpoints
    caps[:8] = 1e12                               # (they would dominate the percentiles)
    caps[[12, 16, 20]] = np.nan                   # unverified share counts: no cap, no breakpoint vote
    spec = ev.validate_spec(_spec(nyse_min_names=10))
    sized = ev._size_buckets(keys, caps, 1, names, spec, universe=universe, venue_pit=pit, nyse_pit=nyse_pit)
    chosen = universe & pit & nyse_pit & np.isfinite(caps)
    assert sized.venue_basis == ["nyse_pit"] and sized.nyse_pit_names[0] == chosen.sum() == 15
    assert sized.venue_pit_share[0] == pytest.approx(72 / 92)
    low, high = np.percentile(caps[chosen], [20.0, 50.0])
    unknown = ~np.isfinite(caps)
    assert np.array_equal(sized.buckets, np.where(unknown, 0, np.where(caps < low, 1, np.where(caps < high, 2, 3))))
    # Too few point-in-time NYSE names: verified-cap terciles of the valid cohort (never another venue).
    sized = ev._size_buckets(keys, caps, 1, names, ev.validate_spec(_spec(nyse_min_names=30)), universe=universe,
                             venue_pit=pit, nyse_pit=nyse_pit)
    assert sized.venue_basis == ["cap_terciles"]
    t1, t2 = np.percentile(caps[universe & ~unknown], [100 / 3, 200 / 3])
    assert np.array_equal(sized.buckets, np.where(unknown, 0, np.where(caps < t1, 1, np.where(caps < t2, 2, 3))))
    # NYSE-breakpoint deciles: percentiles of the reference rows, ties go to the lower decile.
    values = rng.standard_normal(names)
    deciles = ev.breakpoint_quantiles(np.zeros(names, np.int64), values, nyse_pit, 1, 10, 10)
    breaks = np.percentile(values[nyse_pit], np.arange(1, 10) * 10.0)
    assert np.array_equal(deciles, np.searchsorted(breaks, values, side="left") + 1)
    assert (ev.breakpoint_quantiles(np.zeros(names, np.int64), values, nyse_pit, 1, 10, 30) == 0).all()


def test_ranked_universe_is_valid_primary_lines_and_drops_are_counted_per_formation():
    rng = np.random.default_rng(41)
    names, months = 80, 40
    signal = _ar1(rng, months + 12, names, 0.7)
    returns = np.expm1(0.05 * (0.3 * signal + rng.standard_normal(signal.shape)))
    market = Market(returns, months)
    not_valid, secondary = [0, 1, 2, 3, 4], [5, 6, 7]  # e.g. a preferred line; second lines of dual-class issuers
    basis = market.basis({"planted": market.feature("planted", signal)})
    security = basis.context.security.to_numpy()
    basis.context = basis.context.assign(valid_member=~np.isin(security, not_valid),
                                         primary_line=~np.isin(security, secondary))
    spec = _spec(min_names=30, fm_min_obs=30, min_formations=10, bootstrap_resamples=0)
    tables = ev.evaluate_bases([basis], spec)
    # Oracle: the same feature written only for the ranked universe.
    kept = np.setdiff1d(np.arange(names), not_valid + secondary)
    oracle_feature = market.feature("planted", signal)
    oracle_feature = ev.FeatureData("planted", 1, oracle_feature.values[oracle_feature.values.security.isin(kept)])
    oracle = ev.evaluate_bases([market.basis({"planted": oracle_feature})], spec)
    for h in (1, 3, 6, 12):
        mine, theirs = _cell(tables, h), _cell(oracle, h)
        for column in ("ic_mean", "ls_ew10_mean", "ls_vw10_mean", "fm_slope", "ls_nyse_ew10_mean", "mono_ew10"):
            assert mine[column] == pytest.approx(theirs[column], abs=1e-15, nan_ok=True), column
        assert mine.values_dropped_not_valid == 5 * months and mine.values_dropped_not_primary == 3 * months
        assert mine.mean_universe_names == len(kept) and mine.mean_coverage == pytest.approx(1.0)
        assert theirs.mean_coverage == pytest.approx(len(kept) / names)
        assert mine.universe_rule == ev.UNIVERSE_RULE and not mine.fm_standardized  # signed_raw: raw units
    # The per-formation series carries the cell's inputs and reproduces its means.
    series = tables.series[(tables.series.feature_id == "planted") & (tables.series.horizon_months == 3)]
    assert len(series) == months and (series.dropped_not_valid == 5).all() and (series.dropped_not_primary == 3).all()
    assert (series.universe_names == len(kept)).all() and (series.n_values == len(kept)).all()
    used = series[series.in_selection & series.usable]
    assert used.ic.mean() == pytest.approx(_cell(tables, 3).ic_mean, abs=1e-15)
    assert used.ls_ew10.mean() == pytest.approx(_cell(tables, 3).ls_ew10_mean, abs=1e-15)


def test_family_size_is_invariant_when_expected_cells_are_missing():
    """An expected cell that was not produced enters the family as p = 1: BH/Holm m and the DSR
    n_trials never shrink, q-values can only rise; a two-sided hypothesis (sign 0) is a member."""
    rng = np.random.default_rng(43)
    names, months = 60, 40
    signal = _ar1(rng, months + 12, names, 0.7)
    returns = np.expm1(0.05 * (0.4 * signal + rng.standard_normal(signal.shape)))
    market = Market(returns, months)
    catalog = (ev.CatalogFeature("planted", 1, "momentum", ("signed_raw",)),
               ev.CatalogFeature("mixed", 0, "quality", ("signed_raw",)),  # pre-registered two-sided
               ev.CatalogFeature("omitted", -1, "value", ("signed_raw",)))
    features = {"planted": market.feature("planted", signal),
                "mixed": market.feature("mixed", rng.standard_normal((months, names)), sign=0),
                "omitted": market.feature("omitted", rng.standard_normal((months, names)), sign=-1)}
    spec = _spec(min_names=30, fm_min_obs=30, min_formations=10, bootstrap_resamples=0)
    full = ev.evaluate_bases([market.basis(features)], spec, catalog=catalog)
    partial = ev.evaluate_bases([market.basis({k: v for k, v in features.items() if k != "omitted"})], spec,
                                catalog=catalog)
    for tables in (full, partial):
        cells = tables.cells
        assert tables.family["n_trials"] == 3 * 4 == int(cells.family_member.sum())
        assert (cells.dsr_n_trials.dropna() == 12).all() and cells.dsr_n_trials.notna().any()
        # Not complete: the strict basis was not supplied (R4 must refuse such a run).
        assert tables.family["family_complete"] is False and tables.family["bases_supplied"] == ["reconstructed"]
    missing = partial.cells[partial.cells.feature_id == "omitted"]
    assert set(missing.status) == {"not_produced"} and set(missing.status_reason) == {"not_supplied"}
    assert missing.family_member.all() and missing.bh_q.isna().all()
    assert partial.family["bases"]["reconstructed"]["not_produced"] == 4
    q_full = full.cells.set_index(["feature_id", "horizon_months"]).bh_q
    q_partial = partial.cells.set_index(["feature_id", "horizon_months"]).bh_q
    for h in (1, 3, 6, 12):
        assert q_partial[("planted", h)] >= q_full[("planted", h)]
    two_sided = full.cells[full.cells.feature_id == "mixed"]
    assert set(two_sided.status) == {"tested"} and set(two_sided.expected_sign) == {0}
    assert two_sided.bh_q.notna().all()
    # The value's sign must be the pre-registered one: a two-sided catalog row refuses a signed feature.
    signed = {**features, "mixed": market.feature("mixed", rng.standard_normal((months, names)), sign=1)}
    with pytest.raises(ev.EvaluationInputError, match="differs from the R1a catalog"):
        ev.evaluate_bases([market.basis(signed)], spec, catalog=catalog)


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
    # Strict membership (listing evidence as of the date) exists from 2024 only; it puts every third
    # name on NYSE, while the reconstructed cohort's backcast venue says every odd name.
    con.execute("""
        CREATE TABLE universe_us_listed_membership (universe_id VARCHAR, security_id VARCHAR, valid_from DATE,
            valid_to DATE, available_at TIMESTAMP, as_of_date DATE, exchange_code VARCHAR, source VARCHAR)""")
    _bulk(con, "INSERT INTO universe_us_listed_membership SELECT 'us_listed_v1', security_id, DATE '2024-01-01', "
               "NULL, TIMESTAMP '2024-01-01 22:00:00', DATE '2024-01-01', exchange_code, "
               "'atx-db us-listed universe builder' FROM {source}",
          ["security_id", "exchange_code"],
          [(f"S{i:03d}", "XNYS" if i % 3 == 0 else "XNAS") for i in range(N_SECURITIES)])
    refresh_monthly_forward_labels(store, MonthlyForwardLabelOptions(run_id="r3b_fixture"))
    store.connection.close()
    return signal, month_keys


VERIFIED, UNVERIFIED = "verified_dei_shares", "unverified_vendor_shares"
#: Vendor share counts (unverified size): never a weight, a size bucket or a breakpoint vote.
UNVERIFIED_SIZE = {f"S{i:03d}" for i in range(20, 25)}
#: The R2b version was built from this catalog digest (not the committed catalog's).
FIXTURE_CATALOG_SHA = "c" * 64


def _research_fixture(tmp_path):
    """R2a-shaped panel tables and an R2b version in R2b's own schema (``features.ensure_feature_schema``:
    wide matrix + the ``research_feature_values`` contract view), over real R3a monthly labels."""
    warehouse = tmp_path / "warehouse.duckdb"
    signal, month_keys = _warehouse(warehouse)
    research = ResearchStore(tmp_path / "research.duckdb", warehouse_path=warehouse, memory_limit="256MB")
    research.open()
    con = research.con
    panel_sha = "a" * 64
    panel_spec = json.dumps({"size_policy": {"verified_status": VERIFIED, "unverified_status": UNVERIFIED,
                                             "size_features": ["market_cap"]}})
    for run_id, basis, status in (("panel_recon", "reconstructed", "complete"),
                                  ("panel_strict", "strict", "untestable_strict")):
        con.execute("""
            INSERT INTO research_panel_runs (run_id, status, basis, universe_id, identity_basis, universe_basis,
                fundamental_availability_basis, market_availability_basis, query_version, spec_json, spec_sha256,
                definitions_json, definitions_sha256, code_sha256, source_ids_json, calendar_sha256, start_month,
                end_month, as_of_date, run_at, warehouse_path, blockers_json, panel_sha256, created_at)
            VALUES (?, ?, ?, 'u', ?, 'u', 'conservative_filing_46h', 'modeled_trade_date_22h', 'q', ?, 'x', '[]',
                    'x', 'x', '{}', 'x', '2023-01-01', '2024-12-01', '2024-12-31', '2025-01-02', 'w',
                    '["research_only_not_release_eligible"]', ?, '2025-01-02')
        """, [run_id, status, basis, f"{basis}_identity", panel_spec, panel_sha])
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
    cohort, caps, matrix, dates = [], [], [], []
    produced = {"alpha_one": ("signed_raw", "rank_normal"), "book_to_market": ("rank_normal",),
                "momentum_12_1": ("rank_normal",)}
    formed = [row for row in calendar if row[5] == "formed"]
    for month_position, (_, _, formation, cutoff, _, _) in enumerate(formed):
        for i in range(N_SECURITIES):
            security = f"S{i:03d}"
            if security == "S007" and formation >= DELIST_LAST_TRADE:
                continue
            # S011 is the second line of S010's issuer; S012 is not common stock: R2b writes neither.
            owner = f"{10 if i == 11 else i:010d}"
            reason = "not_common" if security == "S012" else "valid"
            primary = None if reason != "valid" else security != "S011"
            # The cohort's exchange code is a current-directory backcast (every odd name on NYSE): never read.
            cohort.append((formation, security, "XNYS" if i % 2 else "XNAS", owner, reason, primary))
            caps.append((formation, security, float(1e8 * (i + 1)),
                         UNVERIFIED if security in UNVERIFIED_SIZE else VERIFIED))
            if security in ("S011", "S012"):
                continue
            alpha, value_b, value_m = signal[month_position, i], math.sin(i + month_position), \
                math.cos(3 * i + month_position)
            for feature, raw, variants in (("alpha_one", alpha, {"signed_raw": alpha, "rank_normal": alpha / 2}),
                                           ("book_to_market", value_b, {"rank_normal": value_b / 2}),
                                           ("momentum_12_1", value_m, {"rank_normal": value_m / 2})):
                matrix.append(("fv_recon", formation, security, feature, 1, cutoff, raw,
                               *(variants.get(name) for name in rf.VARIANTS)))
        for feature, variants in produced.items():
            dates.extend(("fv_recon", formation, feature, variant, rf.DATE_FORMED, N_SECURITIES, N_SECURITIES - 3,
                          1.0, N_SECURITIES, N_SECURITIES - 3, "{}") for variant in variants)
    _bulk(con, """
        INSERT INTO research_panel_cohort (run_id, formation_date, security_id, exchange_code, identity_basis,
                                           owner_cik, cohort_reason, eligible, primary_line)
        SELECT 'panel_recon', CAST(formation_date AS DATE), security_id, exchange_code,
               'current_ticker_unverified', owner_cik, cohort_reason, true, primary_line FROM {source}
    """, ["formation_date", "security_id", "exchange_code", "owner_cik", "cohort_reason", "primary_line"], cohort)
    _bulk(con, """
        INSERT INTO research_panel_values (run_id, formation_date, security_id, feature_id, metric_code,
            metric_window, raw_value, reason, available_at, identity_basis, universe_basis, availability_basis,
            size_status)
        SELECT 'panel_recon', CAST(formation_date AS DATE), security_id, 'market_cap', 'market_cap', 'daily',
               raw_value, 'valid', TIMESTAMP '2023-01-01', 'x', 'u', 'm', size_status FROM {source}
    """, ["formation_date", "security_id", "raw_value", "size_status"], caps)
    # The R2b version in R2b's own schema: wide matrix (only rows that passed every gate), date rows,
    # the catalog snapshot, and the research_feature_values contract view over the matrix.
    rf.ensure_feature_schema(con)
    now = dt.datetime(2025, 1, 3)
    con.executemany(f"""
        INSERT INTO research_feature_versions (feature_version, status, basis, panel_run_id, panel_sha256,
            classification_basis, values_sha256, query_version, universe_rule, spec_json, spec_sha256, code_sha256,
            catalog_sha256, inputs_json, inputs_sha256, blockers_json, created_at, finished_at)
        VALUES (?, ?, ?, ?, ?, 'current_sic_backcast', 'v', '{rf.QUERY_VERSION}', '{rf.UNIVERSE_RULE}', '{{}}',
                'x', 'x', '{FIXTURE_CATALOG_SHA}', '{{}}', 'x', ?, ?, ?)
    """, [("fv_recon", "sealed", "reconstructed", "panel_recon", panel_sha,
           '["classification_current_sic_backcast"]', now, now),
          ("fv_strict", "untestable_strict", "strict", "panel_strict", panel_sha, "[]", now, now)])
    snapshot = [("alpha_one", "value", 1, rf.FEATURE_BUILT, None), ("book_to_market", "value", 1, rf.FEATURE_BUILT, None),
                ("momentum_12_1", "momentum", 1, rf.FEATURE_BUILT, None),
                ("gamma_absent", "growth", -1, rf.FEATURE_INPUT_MISSING, "metric:gamma/ttm"),
                ("zeta_blocked", "quality", 1, rf.FEATURE_BLOCKED, "blocked_incomparable_origin")]
    con.executemany("""
        INSERT INTO research_feature_catalog (feature_version, feature_id, source_kind, anomaly_class,
            hypothesis_family, expected_sign, orientation_sign, preferred_transform, domain, caveat_codes,
            admission, is_control, inputs_json, status, status_reason, variants_json)
        VALUES (?, ?, 'seed_metric', ?, ?, ?, ?, 'rank_normal', 'none', '[]', 'eligible', false, '{}', ?, ?, ?)
    """, [(version, feature, anomaly_class, f"{feature}_family", sign, sign or 1,
           status if version == "fv_recon" or status == rf.FEATURE_BLOCKED else rf.FEATURE_UNTESTABLE,
           reason, json.dumps(list(produced.get(feature, ()))))
          for version in ("fv_recon", "fv_strict") for feature, anomaly_class, sign, status, reason in snapshot])
    variant_values = ", ".join(f"CASE WHEN isnan(CAST({v} AS DOUBLE)) THEN NULL ELSE CAST({v} AS DOUBLE) END"
                               for v in rf.VARIANTS)  # a variant R2b does not produce is NULL, not NaN
    _bulk(con, f"""
        INSERT INTO research_feature_matrix (feature_version, formation_date, security_id, feature_id, expected_sign,
            available_at, raw_value, domain_status, {', '.join(rf.VARIANTS)})
        SELECT feature_version, CAST(formation_date AS DATE), security_id, feature_id, expected_sign, available_at,
               raw_value, '{rf.IN_DOMAIN}', {variant_values} FROM {{source}}
    """, ["feature_version", "formation_date", "security_id", "feature_id", "expected_sign", "available_at",
          "raw_value", *rf.VARIANTS], matrix)
    _bulk(con, "INSERT INTO research_feature_dates ({columns}) SELECT feature_version, CAST(formation_date AS DATE), "
               "feature_id, variant, date_status, eligible_members, valid_names, coverage_fraction, candidate_names, "
               "in_domain_names, reasons_json FROM {source}",
          ["feature_version", "formation_date", "feature_id", "variant", "date_status", "eligible_members",
           "valid_names", "coverage_fraction", "candidate_names", "in_domain_names", "reasons_json"], dates)
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


#: The fixture's pre-registered family: one catalog hypothesis (gamma_absent) R2b never produced.
CATALOG = (ev.CatalogFeature("alpha_one", 1, "value", ("signed_raw", "rank_normal")),
           ev.CatalogFeature("book_to_market", 1, "value", ("rank_normal",)),
           ev.CatalogFeature("gamma_absent", -1, "growth", ("rank_normal",)),
           ev.CatalogFeature("momentum_12_1", 1, "momentum", ("rank_normal",)))
PRODUCED_CATALOG = tuple(item for item in CATALOG if item.feature_id != "gamma_absent")


def test_store_run_seals_a_manifest_that_reproduces_byte_identical_results(tmp_path):
    research = _research_fixture(tmp_path)
    try:
        con = research.con
        with pytest.raises(ev.EvaluationInputError, match="RX7"):
            ev.run_evaluation(research, _store_spec("r3b_unsplit", split=None), catalog=CATALOG)
        result = ev.run_evaluation(research, _store_spec("r3b_store"), catalog=CATALOG)
        assert result.status == "complete" and not result.family_complete
        cells = con.execute("SELECT * FROM research_eval_cells WHERE run_id='r3b_store'").df()
        recon = cells[cells.basis == "reconstructed"]
        strict = cells[cells.basis == "strict"]
        # Every expected catalog cell has a row on both bases: 5 (feature, variant) x 4 horizons.
        assert len(recon) == len(strict) == 5 * 4
        assert set(strict.status) == {"untestable_strict"} and not strict.family_member.any()
        absent = recon[recon.feature_id == "gamma_absent"]
        assert set(absent.status) == {"not_produced"} and absent.family_member.all() and absent.bh_q.isna().all()
        assert set(absent.status_reason) == {"r2b:input_not_in_panel:metric:gamma/ttm"}  # R2b says why
        assert set(strict.status_reason) == {"basis_untestable"}
        assert int(recon.family_member.sum()) == 20  # the not-produced hypothesis still counts (p = 1)
        alpha = recon[(recon.feature_id == "alpha_one") & (recon.variant == "signed_raw")].set_index("horizon_months")
        assert alpha.loc[1, "status"] == "tested" and alpha.loc[1, "sample"] == "selection"
        assert alpha.loc[1, "ic_mean"] > 0.1  # planted month-ahead drift
        assert alpha.loc[1, "fmc_controls"] == "size,book_to_market,momentum_12_1"
        assert alpha.loc[1, "identity_basis"] == "reconstructed_identity"
        assert alpha.loc[1, "classification_basis"] == "current_sic_backcast"
        assert alpha.loc[1, "anomaly_class"] == "value"
        assert (recon.dsr_n_trials.dropna() == 20).all()
        assert alpha.loc[1, "hypothesis_family"] is None  # injected rows carry none; snapshot rows do
        # The ranked universe excludes the non-common S012 and the secondary line S011 (R2b wrote no rows).
        assert alpha.loc[1, "values_dropped_not_valid"] == 0 and alpha.loc[1, "values_dropped_not_primary"] == 0
        assert alpha.loc[1, "mean_coverage"] == pytest.approx(1.0)
        assert alpha.loc[1, "mean_universe_names"] < N_SECURITIES - 1
        assert alpha.loc[1, "universe_scope"] == ev.UNIVERSE_SCOPE_LINKED and alpha.loc[1, "values_unlinked_lines"] == 0
        assert 0.0 <= alpha.loc[1, "psr"] <= 1.0
        # The delisting of S007 is stitched into the labels the harness used (observed terminal).
        assert alpha.loc[1, "stitched_observed"] >= 1
        series = con.execute("SELECT * FROM research_eval_series WHERE run_id='r3b_store' AND feature_id='alpha_one' "
                             "AND variant='signed_raw' AND horizon_months=1 ORDER BY formation_date").df()
        assert len(series) == int(alpha.loc[1, "formations_formed"])
        assert (series.coverage.dropna() == 1.0).all() and set(series.segment) == {"train", "validation", "holdout"}
        chosen = series[series.in_selection & series.usable]
        assert chosen.ic.mean() == pytest.approx(alpha.loc[1, "ic_mean"], abs=1e-12)
        # Size buckets: point-in-time listing evidence exists from 2024 only. 2023 formations are bucketed by
        # verified-cap terciles, never by the cohort's backcast exchange code; 2024 by PIT NYSE breakpoints
        # over the valid cohort's verified caps (every third name on NYSE, minus S012 and unverified S021/S024).
        years = pd.to_datetime(series.formation_date).dt.year
        assert set(series.venue_basis[years == 2023]) == {"cap_terciles"}
        assert set(series.venue_basis[years == 2024]) == {"nyse_pit"} and (series.nyse_pit_names[years == 2024] == 17).all()
        selected = series[series.in_selection].venue_basis.value_counts()
        assert alpha.loc[1, "venue_basis"] == ",".join(f"{k}:{v}" for k, v in sorted(selected.items()))
        assert 0.0 < alpha.loc[1, "venue_pit_share"] < 1.0
        kinds = con.execute("SELECT DISTINCT slice_kind, venue_basis FROM research_eval_slices WHERE run_id='r3b_store' "
                            "AND slice_kind LIKE 'size_%' ORDER BY 1").fetchall()
        assert kinds == [("size_bucket", "nyse_pit"), ("size_tercile", "cap_terciles")]
        large = con.execute("SELECT slice_kind, formations FROM research_eval_slices WHERE run_id='r3b_store' "
                            "AND feature_id='alpha_one' AND variant='signed_raw' AND horizon_months=1 "
                            "AND slice_name='large' ORDER BY 1").fetchall()
        pit_used = int((chosen.venue_basis == "nyse_pit").sum())
        assert large == [("size_bucket", pit_used), ("size_tercile", len(chosen) - pit_used)] and pit_used >= 1
        attrition = con.execute("SELECT scope, horizon_months, measure, value FROM research_eval_attrition "
                                "WHERE run_id='r3b_store'").df()
        source = attrition[attrition.scope == "label_source"]
        assert {"survivorship_attrition", "unlabeled_matured", "observed_terminals"} <= set(source.measure)
        assert set(source.horizon_months) >= {1, 3, 6, 12}
        run = con.execute("SELECT status, results_sha256, blockers_json, family_json, family_complete "
                          "FROM research_eval_runs WHERE run_id='r3b_store'").fetchone()
        blockers = json.loads(run[2])
        assert "strict_basis_untestable" in blockers
        assert "reconstructed_catalog_cells_not_produced:4" in blockers
        assert "reconstructed_size_buckets_without_pit_nyse_venue:12" in blockers  # the 12 formations of 2023
        assert "reconstructed_features:classification_current_sic_backcast" in blockers  # R2b's own blockers
        assert "reconstructed_feature_catalog_differs_from_committed_catalog" in blockers
        assert "family_catalog_injected_not_the_feature_version_snapshot" in blockers
        assert not any("declared_by_r3b" in b for b in blockers)
        assert "partial_family_subset" not in blockers and run[4] is False
        family = json.loads(run[3])
        assert family["n_trials"] == 20 and family["bases"]["reconstructed"]["not_produced"] == 4
        assert family["composition"]["strict"] == {"untestable_strict": 20}
        # Size is verified (DEI-share) market cap only: vendor-share caps never weight, bucket or break.
        probe = ev.open_basis_inputs(research, "fv_recon", ev.validate_spec(_store_spec("r3b_probe")))
        unverified = probe.context.security.isin([int(s[1:]) for s in UNVERIFIED_SIZE])  # codes follow S000..S059
        assert probe.context.market_cap[unverified].isna().all() and probe.context.market_cap[~unverified].notna().all()
        formed_count = int(alpha.loc[1, "formations_formed"])
        assert probe.digests["size"]["verified_status"] == VERIFIED
        assert probe.digests["size"]["unverified_cap_rows_excluded"] == len(UNVERIFIED_SIZE) * formed_count
        # The manifest reproduces the results byte for byte, and a second run agrees.
        check = ev.verify_evaluation_run(research, "r3b_store", catalog=CATALOG)
        assert check["reproduced"] and check["stored_rows_match"] and check["inputs_match"]
        again = ev.run_evaluation(research, _store_spec("r3b_again"), catalog=CATALOG)
        assert again.results_sha256 == result.results_sha256 == run[1]
        with pytest.raises(ev.EvaluationInputError, match="already exists"):
            ev.run_evaluation(research, _store_spec("r3b_store"), catalog=CATALOG)
        con.execute("UPDATE research_eval_series SET ic = ic + 1e-9 WHERE run_id='r3b_store' AND ic IS NOT NULL")
        tampered = ev.verify_evaluation_run(research, "r3b_store", catalog=CATALOG)
        assert not tampered["stored_rows_match"] and not tampered["reproduced"]
    finally:
        research.close()


def test_family_is_anchored_to_the_catalog_and_subset_runs_are_flagged(tmp_path):
    research = _research_fixture(tmp_path)
    try:
        con = research.con
        full = ev.run_evaluation(research, _store_spec("r3b_full", bootstrap_resamples=0), catalog=PRODUCED_CATALOG)
        assert full.family_complete and "partial_family_subset" not in full.blockers
        assert full.family["n_trials"] == 16
        subset = ev.run_evaluation(research, _store_spec("r3b_subset", bootstrap_resamples=0, features=("alpha_one",),
                                                         horizons_months=(1, 3)), catalog=PRODUCED_CATALOG)
        assert not subset.family_complete and "partial_family_subset" in subset.blockers
        cells = con.execute("SELECT feature_id, variant, horizon_months, status, family_member, dsr_n_trials, bh_q "
                            "FROM research_eval_cells WHERE run_id='r3b_subset' AND basis='reconstructed'").df()
        # A subset run still corrects for the whole expected family: excluded cells enter as p = 1.
        assert len(cells) == 16 and cells.family_member.all() and subset.family["n_trials"] == 16
        excluded = cells[cells.status == "excluded_by_subset"]
        assert len(excluded) == 16 - 4 and excluded.bh_q.isna().all()
        tested = cells[cells.status == "tested"]
        assert set(tested.horizon_months) == {1, 3} and (tested.dsr_n_trials == 16).all()
        full_q = con.execute("SELECT bh_q FROM research_eval_cells WHERE run_id='r3b_full' AND basis='reconstructed' "
                             "AND feature_id='alpha_one' AND variant='signed_raw' AND horizon_months=1").fetchone()[0]
        subset_q = tested[(tested.variant == "signed_raw") & (tested.horizon_months == 1)].bh_q.iloc[0]
        assert subset_q >= full_q  # never a smaller correction than the full run
        flags = dict(con.execute("SELECT run_id, family_complete FROM research_eval_runs").fetchall())
        assert flags == {"r3b_full": True, "r3b_subset": False}
        # Leaving a basis out is not a complete family either (strict joins m once it is testable).
        solo = ev.run_evaluation(research, _store_spec("r3b_solo", bootstrap_resamples=0, feature_versions=("fv_recon",)),
                                 catalog=PRODUCED_CATALOG)
        assert not solo.family_complete and "strict_basis_not_supplied" in solo.blockers
        assert "partial_family_subset" not in solo.blockers and solo.family["bases_supplied"] == ["reconstructed"]
        # Formations whose entry session is after the label vintage are not yet matured, not misaligned.
        early = ev.run_evaluation(research, _store_spec("r3b_early", bootstrap_resamples=0, horizons_months=(1,),
                                                        label_cutoff=dt.datetime(2024, 10, 31, 23, tzinfo=dt.UTC),
                                                        label_diagnostics=False), catalog=PRODUCED_CATALOG)
        manifest = json.loads(con.execute("SELECT inputs_json FROM research_eval_runs WHERE run_id='r3b_early'"
                                          ).fetchone()[0])["bases"]["reconstructed"]
        assert manifest["digests"]["label"]["alignment"]["entry_after_label_cutoff"] == 2  # Oct and Nov 2024
        assert not any("entry_after_label_cutoff" in b or "label_entry" in b or "label_calendar" in b
                       for b in early.blockers)
    finally:
        research.close()


def test_interrupted_run_resumes_to_the_clean_digest(tmp_path, monkeypatch):
    research = _research_fixture(tmp_path)
    try:
        clean = ev.run_evaluation(research, _store_spec("r3b_clean", bootstrap_resamples=0), catalog=CATALOG)
        original = ev._evaluate_feature
        calls = {"n": 0}

        def flaky(*args):
            calls["n"] += 1
            if calls["n"] == 2:
                raise RuntimeError("injected interruption")
            return original(*args)

        monkeypatch.setattr(ev, "_evaluate_feature", flaky)
        with pytest.raises(RuntimeError, match="injected"):
            ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=0), catalog=CATALOG)
        con = research.con
        assert con.execute("SELECT status FROM research_eval_runs WHERE run_id='r3b_resume'").fetchone() == ("failed",)
        assert con.execute("SELECT count(*) FROM research_eval_feature_inputs WHERE run_id='r3b_resume'"
                           ).fetchone() == (1,)  # the first feature committed atomically, the second did not
        with pytest.raises(ev.EvaluationInputError, match="already exists"):
            ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=0), catalog=CATALOG)
        with pytest.raises(ev.EvaluationInputError, match="spec or code changed"):
            ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=9), resume=True, catalog=CATALOG)
        resumed = ev.run_evaluation(research, _store_spec("r3b_resume", bootstrap_resamples=0), resume=True,
                                    catalog=CATALOG)
        assert resumed.results_sha256 == clean.results_sha256
        assert ev.verify_evaluation_run(research, "r3b_resume", catalog=CATALOG)["reproduced"]
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
    # By default the family is the catalog snapshot R2b stored with the versions (not the live seed):
    # 4 hypotheses (the blocked row is not one) x 6 expected variants x 4 horizons; the fixture
    # produced 4 (feature, variant) pairs, so 80 reconstructed cells are not produced.
    family = summary["family"]
    assert family["catalog_source"] == "feature_version_snapshot" and family["feature_catalog_sha256"] == "c" * 64
    assert family["n_trials"] == 4 * 6 * 4 and summary["family_complete"] is False
    assert "reconstructed_catalog_cells_not_produced:80" in summary["blockers"]
    assert not any("features_outside_catalog" in b for b in summary["blockers"])
    # The snapshot differs from today's committed catalog: flagged, never silently mixed.
    assert "reconstructed_feature_catalog_differs_from_committed_catalog" in summary["blockers"]
    assert cli.main(["verify", *common]) == 0
    assert json.loads(capsys.readouterr().out)["reproduced"] is True


def _value_line(con, security, *, feature="alpha_one", like="S010", owner_basis=None):
    """Give ``security`` an R2b matrix row copied from ``like`` (R2b itself writes none off the universe)."""
    extra = "" if owner_basis is None else ", owner_basis"
    chosen = "" if owner_basis is None else f", '{owner_basis}'"
    con.execute(f"""
        INSERT INTO research_feature_matrix (feature_version, formation_date, security_id, feature_id, expected_sign,
            available_at, raw_value, domain_status, signed_raw, rank_normal{extra})
        SELECT feature_version, formation_date, '{security}', feature_id, expected_sign, available_at, raw_value,
               domain_status, signed_raw, rank_normal{chosen}
        FROM research_feature_matrix
        WHERE security_id='{like}' AND feature_id='{feature}'
          AND formation_date IN (SELECT formation_date FROM research_panel_cohort WHERE security_id='{security}')
    """)


def test_adapter_refuses_contract_violations(tmp_path):
    research = _research_fixture(tmp_path)
    try:
        con = research.con
        # Universe clause: a value on a secondary line, on a non-valid row, or two lines of one owner.
        for security, reason in (("S011", r"not_primary_line=[1-9]"), ("S012", r"not_valid=[1-9]")):
            _value_line(con, security)
            with pytest.raises(ev.EvaluationInputError, match=reason):
                ev.run_evaluation(research, _store_spec(f"r3b_line_{security.lower()}"), catalog=CATALOG)
            con.execute(f"DELETE FROM research_feature_matrix WHERE security_id='{security}'")
        con.execute("UPDATE research_panel_cohort SET primary_line = true WHERE security_id='S011'")
        _value_line(con, "S011")
        with pytest.raises(ev.EvaluationInputError, match=r"multi_line_owner_formations=[1-9]"):
            ev.run_evaluation(research, _store_spec("r3b_two_lines"), catalog=CATALOG)
        con.execute("UPDATE research_panel_cohort SET primary_line = false WHERE security_id='S011'")
        con.execute("DELETE FROM research_feature_matrix WHERE security_id='S011'")
        # expected_sign must equal the pre-registered catalog sign.
        flipped = tuple(ev.CatalogFeature(item.feature_id, -item.expected_sign, item.anomaly_class, item.variants)
                        if item.feature_id == "alpha_one" else item for item in CATALOG)
        with pytest.raises(ev.EvaluationInputError, match="differs from the R1a catalog"):
            ev.run_evaluation(research, _store_spec("r3b_sign"), catalog=flipped)
        # One run tests one pre-registered family: versions built from different catalog snapshots are refused.
        con.execute("UPDATE research_feature_versions SET catalog_sha256='d' WHERE feature_version='fv_strict'")
        with pytest.raises(ev.EvaluationInputError, match="different catalog snapshots"):
            ev.run_evaluation(research, _store_spec("r3b_snapshots"))
        con.execute(f"UPDATE research_feature_versions SET catalog_sha256='{FIXTURE_CATALOG_SHA}'")
        con.execute("UPDATE research_feature_matrix SET available_at = available_at + INTERVAL 1 HOUR "
                    "WHERE feature_id='alpha_one' AND security_id='S001'")
        with pytest.raises(ev.LookaheadError):
            ev.run_evaluation(research, _store_spec("r3b_late"), catalog=CATALOG)
        status = con.execute("SELECT status FROM research_eval_runs WHERE run_id='r3b_late'").fetchone()
        assert status == ("failed",)
        # Only an accepted R2b query version and universe rule are read.
        for column, bad, good in (("query_version", "research-feature-store-v0", rf.QUERY_VERSION),
                                  ("universe_rule", "all_lines", rf.UNIVERSE_RULE)):
            con.execute(f"UPDATE research_feature_versions SET {column}='{bad}' WHERE feature_version='fv_recon'")
            with pytest.raises(ev.EvaluationInputError, match=f"R2b feature contract.*{bad}"):
                ev.run_evaluation(research, _store_spec(f"r3b_{column}"), catalog=CATALOG)
            con.execute(f"UPDATE research_feature_versions SET {column}='{good}'")
        con.execute("UPDATE research_feature_versions SET panel_sha256='b' WHERE feature_version='fv_recon'")
        with pytest.raises(ev.EvaluationInputError, match="panel digest"):
            ev.run_evaluation(research, _store_spec("r3b_digest"), catalog=CATALOG)
        con.execute("CREATE OR REPLACE VIEW research_feature_values AS SELECT feature_version, formation_date, "
                    "security_id, feature_id, 'signed_raw' AS variant, signed_raw AS value, expected_sign "
                    "FROM research_feature_matrix")
        with pytest.raises(ev.EvaluationInputError, match=r"R2b feature contract.*available_at"):
            ev.run_evaluation(research, _store_spec("r3b_contract"), catalog=CATALOG)
    finally:
        research.close()


def test_unlinked_price_line_rows_are_ranked_as_their_own_names_when_r2b_flags_them(tmp_path):
    """Controller ruling on R2b I1: an identity-free price-line feature ranks eligible lines without an owner
    link as their own names (``owner_basis='unlinked_line'``); fundamentals stay on linked primary lines."""
    research = _research_fixture(tmp_path)
    try:
        con = research.con
        # S013 loses its owner link (e.g. a delisted line with no current ticker): no fundamentals, but R2b's
        # price-line feature alpha_one keeps it as its own name.
        con.execute("UPDATE research_panel_cohort SET cohort_reason='missing_owner_link', owner_cik=NULL, "
                    "primary_line=NULL WHERE security_id='S013'")
        con.execute("DELETE FROM research_feature_matrix WHERE security_id='S013' AND feature_id <> 'alpha_one'")
        with pytest.raises(ev.EvaluationInputError, match=r"not_valid=[1-9]"):  # unflagged: outside the universe
            ev.run_evaluation(research, _store_spec("r3b_unflagged", bootstrap_resamples=0), catalog=CATALOG)
        # R2b schema v2 carries owner_basis on the matrix and the view; on a v1 schema, add it the same way.
        if "owner_basis" not in ev._research_columns(con, "research_feature_values"):
            con.execute("DROP VIEW research_feature_values")
            con.execute("ALTER TABLE research_feature_matrix ADD COLUMN owner_basis VARCHAR")
            con.execute(f"""CREATE VIEW research_feature_values AS
                SELECT feature_version, formation_date, security_id, feature_id, variant, value, expected_sign,
                       available_at, domain_status, owner_basis
                FROM research_feature_matrix UNPIVOT INCLUDE NULLS (value FOR variant IN ({', '.join(rf.VARIANTS)}))""")
        con.execute("UPDATE research_feature_matrix SET owner_basis='linked_primary'")
        con.execute("UPDATE research_feature_matrix SET owner_basis='unlinked_line' WHERE security_id='S013'")
        result = ev.run_evaluation(research, _store_spec("r3b_unlinked", bootstrap_resamples=0), catalog=CATALOG)
        cells = con.execute("SELECT feature_id, variant, universe_scope, values_unlinked_lines, mean_universe_names, "
                            "mean_coverage, formations_usable FROM research_eval_cells WHERE run_id='r3b_unlinked' "
                            "AND basis='reconstructed' AND horizon_months=1 AND status='tested'").df().set_index(
                                ["feature_id", "variant"])
        alpha, value = cells.loc[("alpha_one", "signed_raw")], cells.loc[("book_to_market", "rank_normal")]
        assert alpha.universe_scope == ev.UNIVERSE_SCOPE_WITH_UNLINKED and value.universe_scope == ev.UNIVERSE_SCOPE_LINKED
        assert alpha.mean_universe_names == value.mean_universe_names + 1  # the unlinked line is one more name
        assert alpha.values_unlinked_lines > 0 and value.values_unlinked_lines == 0
        assert alpha.mean_coverage == pytest.approx(1.0) and value.mean_coverage == pytest.approx(1.0)
        series = con.execute("SELECT n_unlinked FROM research_eval_series WHERE run_id='r3b_unlinked' "
                             "AND feature_id='alpha_one' AND variant='signed_raw' AND horizon_months=1").df()
        assert set(series.n_unlinked) == {1}
        assert result.status == "complete"
        # A linked, valid line cannot be passed off as an unlinked one.
        con.execute("UPDATE research_feature_matrix SET owner_basis='unlinked_line' WHERE security_id='S001'")
        with pytest.raises(ev.EvaluationInputError, match=r"unlinked_line_not_an_unlinked_member=[1-9]"):
            ev.run_evaluation(research, _store_spec("r3b_fake_unlinked", bootstrap_resamples=0), catalog=CATALOG)
    finally:
        research.close()
