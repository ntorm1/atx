"""R3a statistics core: every estimator is checked against a closed form, a published
reference value, or the prior production implementation - never against itself."""

from __future__ import annotations

import math
from fractions import Fraction
from statistics import NormalDist

import numpy as np
import pandas as pd
import pytest

from atx_db.custom_features import FEATURE_DEFINITIONS, holm_eight
from atx_db.fundamental_signal_evaluation import holm_family
from atx_db.research import stats


# --- oracles: the pre-R3a production implementations, verbatim (HEAD 80878e3e) ---
def _prior_holm_family(p_values):
    ordered = sorted(p_values, key=lambda key: (1 if p_values[key] is None else p_values[key], key))
    result = {}
    running = 0.0
    for index, key in enumerate(ordered):
        p = p_values[key]
        if p is not None and (not math.isfinite(p) or not 0 <= p <= 1):
            raise ValueError("p-values must be finite and within [0,1]")
        running = max(running, (len(ordered) - index) * (1.0 if p is None else p))
        result[key] = None if p is None else min(1.0, running)
    return result


def _prior_holm_eight(p_values):
    ordered = sorted(((p_values.get(feature), feature) for feature in FEATURE_DEFINITIONS),
                     key=lambda item: (1.0 if item[0] is None else item[0], item[1]))
    adjusted = {}
    running = 0.0
    for rank, (p_value, feature) in enumerate(ordered):
        running = max(running, (8-rank)*(1.0 if p_value is None else p_value))
        adjusted[feature] = None if p_value is None else min(1.0, running)
    return adjusted


def _overlapping_sums(rng, n_obs, horizon, rho=0.0):
    """h-period sums of an AR(1) one-period series: the null IC/spread of h-month labels."""
    shocks = rng.standard_normal(n_obs + horizon - 1 + 50)
    series = np.empty_like(shocks)
    series[0] = shocks[0]
    for index in range(1, len(shocks)):
        series[index] = rho * series[index - 1] + shocks[index]
    return np.convolve(series[50:], np.ones(horizon), "valid")


# --- reference distributions -------------------------------------------------------

def test_student_t_matches_closed_forms_and_published_quantiles():
    for t in (0.1, 1.0, 2.0, 7.5, 40.0):
        # df=1 is Cauchy; df=2 has CDF 1/2 + t / (2 sqrt(2 + t^2)).
        assert stats.student_t_two_sided_p(t, 1) == pytest.approx(1 - 2 / math.pi * math.atan(t), rel=1e-12)
        assert stats.student_t_two_sided_p(-t, 2) == pytest.approx(1 - t / math.sqrt(2 + t * t), rel=1e-12)
    # Standard t-table quantiles (two-sided 5% at 10 and 30 df, 1% at 5 df).
    assert stats.student_t_critical(0.05, 10) == pytest.approx(2.228138851964938, abs=1e-9)
    assert stats.student_t_critical(0.05, 30) == pytest.approx(2.042272456301238, abs=1e-9)
    assert stats.student_t_critical(0.01, 5) == pytest.approx(4.032142983557536, abs=1e-9)
    assert stats.student_t_two_sided_p(1.959963984540054, 10**7) == pytest.approx(0.05, abs=1e-6)
    assert stats.normal_equivalent_z(0.05) == pytest.approx(1.959963984540054, abs=1e-12)
    assert math.isclose(stats.HLZ_P_THRESHOLD, 0.0026997960632601866, rel_tol=1e-12)
    assert stats.normal_equivalent_z(stats.HLZ_P_THRESHOLD) == pytest.approx(3.0, abs=1e-9)


# --- HAC ---------------------------------------------------------------------------

def test_newey_west_equals_hand_computed_bartlett_on_ar1_fixture():
    # x_t = 0.5 x_{t-1} + e_t, x_0 = e_0 with e = (1,-1,2,0,-2,1,1,-1): exact binary fractions.
    shocks = [Fraction(v) for v in (1, -1, 2, 0, -2, 1, 1, -1)]
    x = [shocks[0]]
    for shock in shocks[1:]:
        x.append(Fraction(1, 2) * x[-1] + shock)
    assert [float(v) for v in x] == [1.0, -0.5, 1.75, 0.875, -1.5625, 0.21875, 1.109375, -0.4453125]
    n = len(x)
    mean = sum(x) / n
    d = [v - mean for v in x]
    gamma = [sum(d[t] * d[t - k] for t in range(k, n)) / n for k in range(3)]
    # Bartlett, L=2: weights 1 - k/(L+1) = 2/3, 1/3.
    long_run = gamma[0] + 2 * (Fraction(2, 3) * gamma[1] + Fraction(1, 3) * gamma[2])
    expected_se = math.sqrt(long_run / n)
    lags, se, t = stats.newey_west_mean(pd.Series([float(v) for v in x]), lags=2)
    assert lags == 2
    assert se == pytest.approx(expected_se, rel=1e-13)
    assert t == pytest.approx(float(mean) / expected_se, rel=1e-13)
    inference = stats.mean_inference([float(v) for v in x], horizon_periods=1, lags=2)
    assert inference.nw_lags == 2
    assert inference.nw_standard_error == pytest.approx(expected_se, rel=1e-13)
    assert inference.nw_p_value == pytest.approx(math.erfc(abs(float(mean)) / expected_se / math.sqrt(2)))


def test_newey_west_converges_to_ar1_closed_form_bartlett_target():
    phi, lags = 0.5, 40
    rng = np.random.default_rng(2026)
    shocks = rng.standard_normal(200_000)
    x = np.empty_like(shocks)
    x[0] = shocks[0] / math.sqrt(1 - phi * phi)
    for index in range(1, len(x)):
        x[index] = phi * x[index - 1] + shocks[index]
    gamma0 = 1 / (1 - phi * phi)
    target = gamma0 * (1 + 2 * sum((1 - k / (lags + 1)) * phi**k for k in range(1, lags + 1)))
    _, se, _ = stats.newey_west_mean(x, lags=lags)
    assert se * se * len(x) == pytest.approx(target, rel=0.05)
    # The Bartlett target sits below the true long-run variance 1/(1-phi)^2 = 4.
    assert target < 4.0


def test_calendar_positions_keep_gaps_and_match_legacy_calendar_hac():
    rng = np.random.default_rng(5)
    values = rng.normal(0.01, 0.05, 90)
    values[[7, 8, 40, 61]] = np.nan
    legacy = stats.calendar_hac_statistics(
        [(index, value) for index, value in enumerate(values) if np.isfinite(value)], 4)
    positioned = stats.mean_inference(values, horizon_periods=4, lags=3)
    assert legacy["hac_lags"] == positioned.nw_lags == 3
    assert positioned.n_obs == legacy["spread_dates"] == 86
    assert positioned.mean == pytest.approx(legacy["gross_mean"], rel=1e-12)
    assert positioned.nw_standard_error == pytest.approx(legacy["hac_standard_error"], rel=1e-12)
    compressed = stats.mean_inference(values[np.isfinite(values)], horizon_periods=4, lags=3)
    assert compressed.nw_standard_error != pytest.approx(positioned.nw_standard_error, rel=1e-9)


def test_overlap_lag_rule_is_in_formation_units():
    assert stats.newey_west_lags(12, 160) == 11
    assert stats.newey_west_lags(6, 160) == 5
    assert stats.newey_west_lags(1, 160) == 4  # floor(4 * 1.6^(2/9)) = 4
    assert stats.newey_west_lags(1, 2500) == 8
    assert stats.newey_west_lags(12, 8) == 7  # capped at T-1
    month_ends = pd.date_range("2012-01-31", periods=170, freq="BME")
    assert [stats.horizon_in_formation_units(month_ends, h) for h in (63, 126, 252)] == [3, 6, 12]
    assert stats.horizon_in_formation_units(pd.bdate_range("2020-01-01", periods=300), 63) == 63
    weekly = pd.date_range("2020-01-01", periods=40, freq="7D")
    assert stats.horizon_in_formation_units(weekly, 63) == 13
    # Real XNYS 2023-24 sessions: every 63-session window holds a holiday, so the weekday
    # approximation gives 62 units / 61 lags (documented), while 21 sessions stay exact.
    holidays = pd.to_datetime([
        "2023-01-02", "2023-01-16", "2023-02-20", "2023-04-07", "2023-05-29", "2023-06-19",
        "2023-07-04", "2023-09-04", "2023-11-23", "2023-12-25", "2024-01-01", "2024-01-15",
        "2024-02-19", "2024-03-29", "2024-05-27", "2024-06-19", "2024-07-04", "2024-09-02",
        "2024-11-28", "2024-12-25"])
    xnys = pd.bdate_range("2023-01-03", "2024-12-31").difference(holidays)
    assert stats.horizon_in_formation_units(xnys, 21) == 21
    assert stats.horizon_in_formation_units(xnys, 63) == 62
    assert stats.newey_west_lags(62, len(xnys)) == 61
    assert stats.ewc_degrees_of_freedom(160, 12) == 6
    assert stats.ewc_degrees_of_freedom(160, 1) == 11


def test_robust_test_holds_size_under_overlap_where_newey_west_over_rejects():
    rng = np.random.default_rng(160)
    sims, n_obs = 1000, 160
    for horizon in (6, 12):
        nw_rejects = robust_rejects = robust_hlz = 0
        for _ in range(sims):
            inference = stats.mean_inference(_overlapping_sums(rng, n_obs, horizon), horizon_periods=horizon)
            assert inference.nw_lags == horizon - 1
            nw_rejects += inference.nw_p_value < 0.05
            robust_rejects += inference.robust_p_value < 0.05
            robust_hlz += inference.hlz_pass
        # Bartlett L=h-1 keeps ~2/3 of the overlap variance: the plan rule over-rejects.
        assert nw_rejects / sims > 0.10
        assert 0.03 <= robust_rejects / sims <= 0.08
        assert robust_hlz / sims <= 0.01


def test_robust_test_detects_a_real_mean():
    rng = np.random.default_rng(3)
    inference = stats.mean_inference(0.05 + rng.normal(0, 0.1, 160), horizon_periods=1)
    assert inference.robust_p_value < 1e-3 and inference.hlz_pass and inference.robust_df == 11
    assert inference.robust_ci95_low < 0.05 < inference.robust_ci95_high
    empty = stats.mean_inference([np.nan, 1.0], horizon_periods=1)
    assert empty.n_obs == 1 and math.isnan(empty.nw_t) and math.isnan(empty.robust_t) and not empty.hlz_pass


# --- multiple testing ----------------------------------------------------------------

_BH_1995 = [0.0001, 0.0004, 0.0019, 0.0095, 0.0201, 0.0278, 0.0298, 0.0344, 0.0459,
            0.3240, 0.4262, 0.5719, 0.6528, 0.7590, 1.000]


def test_benjamini_hochberg_reproduces_the_1995_fifteen_p_value_example():
    q = stats.benjamini_hochberg({f"h{i:02d}": p for i, p in enumerate(_BH_1995)})
    # q_(i) = min_{j>=i} 15 p_(j) / j, worked by hand.
    expected = [0.0015, 0.003, 0.0095, 0.035625, 0.0603, 15 * 0.0298 / 7, 15 * 0.0298 / 7,
                0.0645, 0.0765, 0.486, 15 * 0.4262 / 11, 0.714875, 15 * 0.6528 / 13,
                15 * 0.7590 / 14, 1.0]
    assert [q[f"h{i:02d}"] for i in range(15)] == pytest.approx(expected, abs=1e-12)
    # BH (1995): the step-up rule at q=0.05 rejects exactly the four smallest.
    assert sorted(key for key, value in q.items() if value <= 0.05) == ["h00", "h01", "h02", "h03"]
    by = stats.benjamini_hochberg({i: p for i, p in enumerate(_BH_1995)}, arbitrary_dependence=True)
    harmonic = sum(1 / k for k in range(1, 16))
    assert by[3] == pytest.approx(min(1.0, 0.035625 * harmonic))
    with_missing = stats.benjamini_hochberg({"a": 0.01, "b": None, "c": 0.02})
    assert with_missing == {"a": pytest.approx(0.03), "b": None, "c": pytest.approx(0.03)}
    with pytest.raises(ValueError):
        stats.benjamini_hochberg({"a": float("nan")})


def test_holm_equals_prior_holm_family_and_holm_eight_outputs():
    holm = stats.holm({i: p for i, p in enumerate(_BH_1995)})
    assert [holm[i] for i in range(4)] == pytest.approx([0.0015, 14 * 0.0004, 13 * 0.0019, 12 * 0.0095])
    assert sum(value <= 0.05 for value in holm.values()) == 3
    rng = np.random.default_rng(11)
    for _ in range(200):
        size = int(rng.integers(1, 12))
        family = {f"s{i}": (None if rng.random() < 0.2 else float(rng.choice([rng.random(), 0.01, 0.5])))
                  for i in range(size)}
        prior = _prior_holm_family(family)
        assert holm_family(family) == prior
        assert list(holm_family(family)) == list(prior)
        features = {feature: (None if rng.random() < 0.3 else float(rng.random()))
                    for feature in FEATURE_DEFINITIONS if rng.random() < 0.8}
        assert holm_eight(features) == _prior_holm_eight(features)
    assert holm_family({"a": .01, "b": None, "c": .04}) == {"a": .03, "c": .08, "b": None}
    with pytest.raises(ValueError):
        holm_family({"a": 1.5})


def test_harvey_liu_zhu_hurdle_accepts_only_normal_equivalent_z_or_p():
    assert stats.hlz_pass(z_equivalent=3.0) and stats.hlz_pass(z_equivalent=-3.2)
    assert stats.hlz_pass(z_equivalent=math.inf) and stats.hlz_pass(p_value=0.0026)
    assert not stats.hlz_pass(z_equivalent=2.99) and not stats.hlz_pass(p_value=0.0028)
    assert not stats.hlz_pass(z_equivalent=float("nan")) and not stats.hlz_pass(p_value=float("nan"))
    # A raw t (NW or small-df EWC) cannot be fed positionally, and exactly one input is required.
    with pytest.raises(TypeError):
        stats.hlz_pass(3.5)  # type: ignore[misc]
    with pytest.raises(TypeError):
        stats.hlz_pass()
    with pytest.raises(TypeError):
        stats.hlz_pass(z_equivalent=3.1, p_value=0.001)
    # The p threshold and the z threshold are the same hurdle.
    assert stats.hlz_pass(p_value=stats.HLZ_P_THRESHOLD)
    assert stats.normal_equivalent_z(5e-324) == pytest.approx(38.4674, abs=1e-3)  # no underflow


# --- Fama-MacBeth ---------------------------------------------------------------------

def _fm_panel(rng, dates=120, names=400, beta=0.02):
    rows = []
    for day in pd.date_range("2013-01-31", periods=dates, freq="BME"):
        feature = rng.standard_normal(names)
        control = 0.6 * feature + 0.8 * rng.standard_normal(names)
        size = rng.lognormal(0, 1, names)
        slope = beta + rng.normal(0, 0.03)  # date-level slope noise: what FM averages over
        target = 0.01 + rng.normal(0, 0.02) + slope * feature + 0.01 * control + rng.normal(0, 0.1, names)
        rows.append(pd.DataFrame({"date": day, "feature": feature, "control": control,
                                  "size": size, "ret": target}))
    return pd.concat(rows, ignore_index=True)


def test_fama_macbeth_recovers_planted_beta_and_matches_closed_form_ols():
    rng = np.random.default_rng(42)
    panel = _fm_panel(rng)
    result = stats.fama_macbeth(panel, date="date", y="ret", x=["feature", "control"], horizon_periods=1)
    row = result.summary.set_index("term").loc["feature"]
    assert abs(row["mean"] - 0.02) < 2 * row["nw_standard_error"]
    assert abs(row["mean"] - 0.02) < 2 * row["robust_standard_error"]
    assert row["nw_t"] > 3 and row["n_obs"] == 120 and row["nw_lags"] == 4
    # Each per-date slope is the closed-form OLS (X'X)^-1 X'y.
    first = panel[panel["date"] == panel["date"].min()]
    design = np.column_stack([np.ones(len(first)), first["feature"], first["control"]])
    closed = np.linalg.solve(design.T @ design, design.T @ first["ret"].to_numpy())
    got = result.per_date.iloc[0]
    assert [got["intercept"], got["feature"], got["control"]] == pytest.approx(list(closed), rel=1e-10)
    # Univariate slope = cov/var; WLS slope = weighted cov/var.
    univariate = stats.fama_macbeth(panel, date="date", y="ret", x=["feature"], horizon_periods=1)
    slope = np.cov(first["feature"], first["ret"], ddof=0)[0, 1] / np.var(first["feature"])
    assert univariate.per_date.iloc[0]["feature"] == pytest.approx(slope, rel=1e-10)
    weighted = stats.fama_macbeth(panel, date="date", y="ret", x=["feature"], horizon_periods=1, weights="size")
    w = first["size"].to_numpy()
    fx, fy = first["feature"].to_numpy(), first["ret"].to_numpy()
    mx, my = np.average(fx, weights=w), np.average(fy, weights=w)
    wslope = np.sum(w * (fx - mx) * (fy - my)) / np.sum(w * (fx - mx) ** 2)
    assert weighted.per_date.iloc[0]["feature"] == pytest.approx(wslope, rel=1e-10)


def test_fama_macbeth_marks_thin_and_degenerate_dates_and_keeps_calendar_positions():
    rng = np.random.default_rng(1)
    panel = _fm_panel(rng, dates=6, names=50)
    days = sorted(panel["date"].unique())
    panel.loc[panel["date"] == days[1], "feature"] = 1.0  # collinear with the intercept
    panel = panel[~((panel["date"] == days[2]) & (panel.index % 50 >= 3))]  # 3 names left
    gap = pd.Timestamp("2013-05-15")  # a formation with no rows at all
    calendar = [*days[:4], gap, *days[4:]]
    result = stats.fama_macbeth(panel, date="date", y="ret", x=["feature"], horizon_periods=3,
                                formation_dates=calendar)
    statuses = dict(zip(result.per_date["date"], result.per_date["status"], strict=True))
    assert statuses[days[1]] == "rank_deficient"
    assert statuses[days[2]] == "insufficient_obs"
    assert statuses[gap] == "insufficient_obs"
    assert result.summary.set_index("term").loc["feature", "n_obs"] == 4
    assert result.summary.set_index("term").loc["feature", "span"] == 7
    with pytest.raises(ValueError, match="outside formation_dates"):
        stats.fama_macbeth(panel, date="date", y="ret", x=["feature"], horizon_periods=1, formation_dates=days[:2])


# --- deflated Sharpe ------------------------------------------------------------------

def test_deflated_sharpe_matches_bailey_lopez_de_prado_worked_example():
    # JPM 2014 "The Deflated Sharpe Ratio", numerical example: annualized SR 2.5 over
    # 5 years of daily returns (T=1250), N=100 trials, V[SR_n]=1/2 annualized,
    # skew -3, kurtosis 10. Paper: SR0 ~ 0.1132, SR ~ 0.1581, DSR ~ 0.9004.
    sharpe = 2.5 / math.sqrt(250)
    result = stats.deflated_sharpe_ratio(sharpe, n_obs=1250, skewness=-3.0, kurtosis=10.0,
                                         n_trials=100, sharpe_variance=0.5 / 250)
    assert result.benchmark_sharpe == pytest.approx(0.1132, abs=5e-5)
    assert result.deflated_sharpe_ratio == pytest.approx(0.9004, abs=5e-5)
    # Hand evaluation of the same closed form with the stdlib normal.
    z = NormalDist().inv_cdf
    gamma = 0.5772156649015329
    sr0 = math.sqrt(0.5 / 250) * ((1 - gamma) * z(1 - 1 / 100) + gamma * z(1 - 1 / (100 * math.e)))
    hand = NormalDist().cdf((sharpe - sr0) * math.sqrt(1249) / math.sqrt(1 + 3 * sharpe + 9 / 4 * sharpe**2))
    assert result.deflated_sharpe_ratio == pytest.approx(hand, abs=1e-6)
    assert result.deflated_sharpe_ratio == pytest.approx(0.9003968344, abs=1e-6)
    # One trial has no selection: DSR = PSR against zero.
    assert stats.expected_maximum_sharpe(1, 0.5) == 0.0
    psr = stats.probabilistic_sharpe_ratio(0.1, 0.0, n_obs=101, skewness=0.0, kurtosis=3.0)
    assert psr == pytest.approx(NormalDist().cdf(0.1 * 10 / math.sqrt(1 + 0.5 * 0.01)), rel=1e-12)
    returns = [0.01, -0.02, 0.03, 0.0, 0.02, -0.01]
    sr, skew, kurt, n = stats.sharpe_moments(returns)
    array = np.array(returns)
    assert (sr, n) == (pytest.approx(array.mean() / array.std(ddof=1)), 6)
    assert kurt == pytest.approx(np.mean((array - array.mean()) ** 4) / np.var(array) ** 2)
    assert skew == pytest.approx(np.mean((array - array.mean()) ** 3) / np.var(array) ** 1.5)


def test_deflated_sharpe_uses_non_overlapping_count_for_overlapping_returns():
    # Review I1 probe: monthly-sampled 12-month spread returns (MA(11)), T=160.
    rng = np.random.default_rng(12)
    spread = np.convolve(rng.normal(0.004, 0.03, 171), np.ones(12), "valid")
    sharpe, skew, kurt, n = stats.sharpe_moments(spread)
    assert n == 160
    naive = stats.deflated_sharpe_ratio(sharpe, n_obs=n, skewness=skew, kurtosis=kurt,
                                        n_trials=1, sharpe_variance=0.0)
    honest = stats.deflated_sharpe_ratio(sharpe, n_obs=n, skewness=skew, kurtosis=kurt,
                                         n_trials=1, sharpe_variance=0.0, horizon_periods=12)
    assert (honest.effective_n_obs, honest.horizon_periods) == (13, 12)
    # Treating 160 overlapping returns as independent inflates z by sqrt(159/12) ~ 3.6.
    assert naive.z / honest.z == pytest.approx(math.sqrt(159 / 12), rel=1e-12)
    assert naive.z > 3 > honest.z
    assert honest.deflated_sharpe_ratio == pytest.approx(stats.probabilistic_sharpe_ratio(
        sharpe, 0.0, n_obs=13, skewness=skew, kurtosis=kurt), rel=1e-12)
    assert stats.probabilistic_sharpe_ratio(sharpe, 0.0, n_obs=n, skewness=skew, kurtosis=kurt,
                                            horizon_periods=12) == honest.deflated_sharpe_ratio
    with pytest.raises(ValueError, match="independent returns"):
        stats.deflated_sharpe_ratio(sharpe, n_obs=20, skewness=skew, kurtosis=kurt,
                                    n_trials=10, sharpe_variance=0.01, horizon_periods=12)


# --- block bootstrap ------------------------------------------------------------------

def test_block_bootstrap_ci_covers_95_percent_under_the_null():
    for horizon in (1, 6):
        rng = np.random.default_rng(99)
        sims = 2000
        covered = 0
        for sim in range(sims):
            interval = stats.circular_block_bootstrap_ci(
                _overlapping_sums(rng, 160, horizon), horizon_periods=horizon, n_resamples=499, seed=sim)
            assert interval.block_length >= horizon
            covered += interval.low <= 0.0 <= interval.high
        assert 0.93 <= covered / sims <= 0.97, (horizon, covered / sims)


def test_block_bootstrap_is_reproducible_and_guards_block_length():
    values = np.random.default_rng(8).normal(0.02, 0.05, 150)
    first = stats.circular_block_bootstrap_ci(values, horizon_periods=3)
    assert first == stats.circular_block_bootstrap_ci(values, horizon_periods=3)
    assert first.seed == stats.DEFAULT_BOOTSTRAP_SEED and first.block_length == 5
    assert first.low < first.estimate < first.high
    other = stats.circular_block_bootstrap_ci(values, horizon_periods=3, seed=1)
    assert other.low != first.low
    percentile = stats.circular_block_bootstrap_ci(values, horizon_periods=3, method="percentile")
    assert (percentile.low, percentile.high) == (first.percentile_low, first.percentile_high)
    with pytest.raises(ValueError, match="at least horizon_periods"):
        stats.circular_block_bootstrap_ci(values, horizon_periods=6, block_length=3)
