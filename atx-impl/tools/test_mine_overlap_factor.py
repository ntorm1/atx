"""The mined-v1 label-overlap factor (platform v8 lane MINE-FIX; Ruling E-32a, review MINE-6).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_mine_overlap_factor.py

mine_overlap_factor.derive() reproduces the pinned factor 1.55 at its committed seed (about 15 s, synthetic null
draws only); the C++ constants it was derived for are the ones the verb compiles (strategy_mine_rule.hpp,
research_ic_fitness.hpp); its estimator is the verb's (the same pinned t's as strategy_mine_test.cpp
StrategyMineRule.OverlapFactorIsRegisteredAndItsEstimatorIsTheVerbs).
"""
from __future__ import annotations

import math
import re
from pathlib import Path

import numpy as np
import pytest

import mine_overlap_factor as MOF

ROOT = Path(__file__).resolve().parents[2]
RULE_HPP = ROOT / "atx-impl" / "src" / "strategy_mine_rule.hpp"
FITNESS_HPP = ROOT / "atx-engine" / "include" / "atx" / "engine" / "factory" / "research_ic_fitness.hpp"


def constant(path: Path, name: str) -> str:
    match = re.search(rf"inline constexpr [\w:]+ {name} = ([0-9.]+);", path.read_text(encoding="utf-8"))
    assert match, f"{name} not found in {path}"
    return match.group(1)


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


@pytest.fixture(scope="module")
def derivation() -> dict:
    return MOF.derive()


def test_the_derivation_reproduces_the_pinned_factor(derivation):
    assert derivation["seed"] == 20260930
    assert derivation["confirm"]["ratio"] == pytest.approx(1.5422, abs=5e-5)
    assert derivation["discover"]["ratio"] == pytest.approx(1.3931, abs=5e-5)
    assert derivation["factor"] == MOF.FACTOR == 1.55
    # The factor covers both terms: t / F at the confirm gate and the discover tail is conservative.
    assert MOF.FACTOR >= max(derivation["confirm"]["ratio"], derivation["discover"]["ratio"])


def test_the_constants_are_the_verbs():
    assert float(constant(RULE_HPP, "kMinedOverlapFactor")) == MOF.FACTOR
    assert int(constant(RULE_HPP, "kMinedMinDiscoverRows")) == MOF.ROWS_DISCOVER
    assert int(constant(RULE_HPP, "kMinedMinConfirmRows")) == MOF.ROWS_CONFIRM
    assert int(constant(FITNESS_HPP, "kResearchIcHacLag")) == MOF.HAC_LAG


def test_the_estimator_is_the_verbs():
    daily = pinned_series()
    assert MOF.summarize_t(daily) == pytest.approx(2.096947998340487, rel=1e-12)
    assert MOF.summarize_t(daily[:12]) == pytest.approx(-0.3360738764789554, rel=1e-12)  # lag clamps to 10
    rng = np.random.default_rng(7)
    for n in (5, 22, 23, 80):
        x = rng.standard_normal(n) + 0.1
        assert MOF.hac_t(x[None, :])[0] == pytest.approx(direct_t(list(x), MOF.HAC_LAG), rel=1e-10)
