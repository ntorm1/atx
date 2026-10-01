"""Synthetic checks for composition_resid (rule theme-resid-v1, platform v8 R-11; no real data).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_resid.py

The fixture of ThemeResid.ThreeThemesEqualTheRegisteredRule (atx-impl/tests/strategy_ic_theme_resid_test.cpp) is built
here identically; both sides compare against the same exact values of the rule (EXPECTED_BLEND), which is the "Python
equals C++" check of the brief.
"""
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import composition_ic_shrink as cis  # noqa: E402
import composition_resid as cres  # noqa: E402
import composition_rules as cr  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as tfw  # noqa: E402  (its synthetic fitter fixture, not its tests)

REPO = Path(__file__).resolve().parents[2]
STD_ARGS = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
NAN = float("nan")
DAYS, WIDTH = 3, 8
WEIGHTS = [0.2, 0.15, 0.2, 0.2, 0.25]           # a1, a2 (theme a); b1, b2 (theme b); c1 (theme c)
SIGNS = [1, -1, 1, 1, 1]
POSITIONS = [0, 0, 1, 1, 2]                     # registered order a, b, c
# The rule's exact blend on the fixture (rational arithmetic; date 2: theme b equals theme a, so it adds nothing).
# Theme b's composite ties names 2 / 4 and 5 / 6 on dates 0 and 1 (b2's ranks tie them): Ruling PM4-12's tie step
# applies there (the registered text without it gave -337/840, -27/280, 9/56, 1/10, 17/70, 1/60, -13/168, 23/420 and
# -53/140, 9/70, 1/140, -1/140, 11/60, -97/420, 17/210, 13/60); date 2 has no tie.
EXPECTED_BLEND = [
    [Fraction(-11, 24), Fraction(-27, 280), Fraction(53, 280), Fraction(1, 10), Fraction(3, 14), Fraction(43, 420),
     Fraction(-89, 840), Fraction(23, 420)],
    [Fraction(-53, 140), Fraction(9, 70), Fraction(-11, 420), Fraction(-1, 140), Fraction(13, 60), Fraction(-37, 140),
     Fraction(4, 35), Fraction(13, 60)],
    [Fraction(1, 15), Fraction(3, 40), Fraction(-7, 60), Fraction(-1, 15), Fraction(-31, 120), Fraction(1, 12),
     Fraction(13, 60), None]]
# Ruling PM4-12 kernel fixture (strategy_ic_theme_resid_test.cpp TiedCompositeStaysTiedAfterResidualisation): two
# themes, two dates, six names, W = 1/2 each; theme 1 is a sparse flag on date 0 and three levels on date 1.
TIE_NAMES = 6
TIE_PLANES = [[12, 10, 19, 18, 1, 14, 7, 1, 15, 2, 19, 11], [0, 0, 0, 1, 1, 0, 0, 2, 1, 2, 0, 1]]
TIE_EXPECTED = [Fraction(-3, 20), Fraction(-1, 4), Fraction(3, 20), Fraction(7, 20), Fraction(-1, 20), Fraction(-1, 20),
                Fraction(-1, 4), Fraction(-1, 4), Fraction(7, 20), Fraction(-3, 20), Fraction(1, 20), Fraction(1, 4)]
TIE_SPREAD = [Fraction(-1, 5), Fraction(-2, 5), Fraction(3, 10), Fraction(2, 5), Fraction(-1, 10), Fraction(0),
              Fraction(-3, 10), Fraction(-3, 10), Fraction(2, 5), Fraction(0), Fraction(1, 10), Fraction(1, 10)]
# Finding R6C-3 (Ruling PM5-12, the Python half; the C++ kernel case is deferred to integration 8): one date, eight
# names, W = 1/2 each; theme 1's composite has a tie block of three names (2, 4, 6) and one of two (5, 7) beside three
# singletons (0, 1, 3). The block means order the theme's add name 0 < name 1 < {5, 7} < {2, 4, 6} < name 3; a block
# sum (no division by the block size) lifts both blocks above name 3; averaging the residual's ranks per block (then
# re-ranking) puts {5, 7} level with name 3, below {2, 4, 6}. Exact values (rational arithmetic) of the rule and of
# the two wrong tie steps.
UNEQUAL_NAMES = 8
UNEQUAL_PLANES = [[5, 3, 6, 0, 4, 2, 7, 1], [0, 2, 1, 4, 1, 3, 1, 3]]
UNEQUAL_EXPECTED = [Fraction(-1, 7), Fraction(-3, 14), Fraction(2, 7), Fraction(0), Fraction(1, 7), Fraction(-5, 28),
                    Fraction(5, 14), Fraction(-1, 4)]
UNEQUAL_WRONG = {
    "block sum": [Fraction(-1, 7), Fraction(-3, 14), Fraction(5, 14), Fraction(-5, 14), Fraction(3, 14),
                  Fraction(-3, 28), Fraction(3, 7), Fraction(-5, 28)],
    "mean of ranks": [Fraction(-1, 7), Fraction(-3, 14), Fraction(5, 14), Fraction(-2, 7), Fraction(3, 14),
                      Fraction(-1, 7), Fraction(3, 7), Fraction(-3, 14)]}
# Finding R6B-O-7 kernel fixture (strategy_ic_theme_resid_test.cpp SmallCaseSeparatesTheRegisteredRegressors): one
# date, five names, three themes without a tie, W = .3 / .45 / .25; theme 1 is absent on name 4, theme 2 on name 2.
# Exact values (rational arithmetic) of the rule and of the plausible wrong rules (a) regressing on the earlier themes'
# pre-rank residuals, (b) on their re-ranked residuals, (c) on the earlier composites without the intercept.
SMALL_NAMES = 5
SMALL_PLANES = [[3, 9, 19, 15, 2], [19, 11, 8, 13, NAN], [2, 11, NAN, 18, 13]]
SMALL_MASS = [0.3, 0.45, 0.25]
SMALL_EXPECTED = [Fraction(-1, 24), Fraction(-7, 20), Fraction(3, 40), Fraction(17, 40), Fraction(-13, 120)]
_SMALL_ON_RESIDUALS = [Fraction(-1, 8), Fraction(-4, 15), Fraction(3, 40), Fraction(41, 120), Fraction(-1, 40)]
SMALL_WRONG = {"a": _SMALL_ON_RESIDUALS, "b": _SMALL_ON_RESIDUALS,
               "c": [Fraction(-1, 8), Fraction(-4, 15), Fraction(3, 40), Fraction(17, 40), Fraction(-13, 120)]}
# Finding R6B-O-7 runner fixture (strategy_ic_runner_test.cpp ThemeResidRunner.ThreeThemeCycleIsTheRegisteredRule):
# eight names, volume = 1e8 (i + 1); one-member themes resid_a = 1 / (volume - 1e8) (absent on name 0),
# resid_b = (volume - 1.5e8)(volume - 6e8), resid_c = (volume - .5e8)(volume - 4e8), weights .3 / .45 / .25, at
# registered positions 1, 2, 0 (a 3-cycle of their library order). Exact values of the rule and of (a)-(c) and (d) the
# inverted position map (2, 0, 1).
CYCLE_X = [1e8 * (i + 1) for i in range(WIDTH)]
CYCLE_SIGNALS = [[NAN] + [1 / (x - 1e8) for x in CYCLE_X[1:]], [(x - 1.5e8) * (x - 6e8) for x in CYCLE_X],
                 [(x - 0.5e8) * (x - 4e8) for x in CYCLE_X]]
CYCLE_WEIGHTS = [0.3, 0.45, 0.25]
CYCLE_EXPECTED = [Fraction(-3, 35), Fraction(1, 20), Fraction(-2, 5), Fraction(-13, 140), Fraction(3, 140),
                  Fraction(19, 140), Fraction(1, 4), Fraction(17, 140)]
_CYCLE_ON_RESIDUALS = [Fraction(6, 35), Fraction(-1, 70), Fraction(-13, 28), Fraction(-1, 35), Fraction(3, 140),
                       Fraction(19, 140), Fraction(13, 70), Fraction(-1, 140)]
CYCLE_WRONG = {"a": _CYCLE_ON_RESIDUALS, "b": _CYCLE_ON_RESIDUALS,
               "c": [Fraction(-3, 35), Fraction(1, 10), Fraction(-7, 20), Fraction(-13, 140), Fraction(3, 140),
                     Fraction(19, 140), Fraction(1, 5), Fraction(1, 14)],
               "d": [Fraction(1, 140), Fraction(-1, 140), Fraction(-51, 140), Fraction(0), Fraction(3, 70),
                     Fraction(1, 20), Fraction(13, 140), Fraction(5, 28)]}


def registered_without_tie_step(planes, mass, names: int) -> np.ndarray:
    """The registered text before Ruling PM4-12 (no tie step), on the reference's own functions: the contrast for the
    tie fixture and the bit-for-bit reference for a composite without ties."""
    planes = np.asarray(planes, dtype=np.float64)
    out = np.zeros(planes.shape[1])
    for d in range(planes.shape[1] // names):
        total = planes[:, d * names:(d + 1) * names]
        z = cres.standardise(np.nan_to_num(total, nan=0.0), ~np.isnan(total))
        row = out[d * names:(d + 1) * names]
        for t in range(len(planes)):
            names_t = np.flatnonzero(~np.isnan(z[t]))
            if len(names_t) < 2:
                continue
            if t == 0:
                row[names_t] += mass[0] * z[0, names_t]
                continue
            e, spanned = cres.residual(z[t, names_t], np.nan_to_num(z[:t][:, names_t], nan=0.0).T)
            if spanned:
                continue
            full, keep = np.zeros(names), np.zeros(names, dtype=bool)
            full[names_t], keep[names_t] = e, True
            row[names_t] += mass[t] * cres.centred_tied_ranks(full, keep)[names_t]
    return out


def _tie_blocks(z) -> list[list[int]]:
    """The names of each run of exactly equal ``z`` of two or more (the rule's tie blocks)."""
    by: dict = {}
    for k, v in enumerate(z):
        by.setdefault(float(v), []).append(k)
    return [b for b in by.values() if len(b) > 1]


def tie_block_sums(e, z) -> np.ndarray:
    """Wrong tie step (finding R6C-3): each tie block's residuals replaced by their sum, not divided by the block size."""
    out = np.array(e, dtype=np.float64)
    for b in _tie_blocks(z):
        out[b] = float(np.sum(out[b]))
    return out


def tie_block_rank_means(e, z) -> np.ndarray:
    """Wrong tie step (finding R6C-3): the raw residual's centred ranks, each tie block's replaced by their mean (the
    rule's next step re-ranks them with the singletons' ranks)."""
    out = cres.centred_tied_ranks(np.asarray(e, dtype=np.float64), np.ones(len(e), dtype=bool))
    for b in _tie_blocks(z):
        out[b] = float(np.mean(out[b]))
    return out


WRONG_TIE_STEPS = {"block sum": tie_block_sums, "mean of ranks": tie_block_rank_means}


def plausible_wrong_rule(planes, mass, names: int, variant: str) -> np.ndarray:
    """Finding R6B-O-7's plausible wrong rules on the reference's functions (no tie step: their fixtures have no tie).
    Theme t > 0 is regressed on, per earlier theme: "a" its pre-rank residual (the first theme: its composite), "b" its
    re-ranked residual (the first: its composite), "c" its composite without the intercept; absent = 0."""
    planes = np.asarray(planes, dtype=np.float64)
    out = np.zeros(planes.shape[1])
    for d in range(planes.shape[1] // names):
        total = planes[:, d * names:(d + 1) * names]
        z = cres.standardise(np.nan_to_num(total, nan=0.0), ~np.isnan(total))
        row = out[d * names:(d + 1) * names]
        regressors = []                                  # per earlier theme, a names-vector (absent 0)
        for t in range(len(planes)):
            names_t = np.flatnonzero(~np.isnan(z[t]))
            composite = np.nan_to_num(z[t], nan=0.0)
            if len(names_t) < 2:
                regressors.append(np.zeros(names))
                continue
            if t == 0:
                row[names_t] += mass[0] * z[0, names_t]
                regressors.append(composite)
                continue
            x = np.column_stack([r[names_t] for r in regressors])
            y = z[t, names_t]
            if variant == "c":
                e, spanned = y - x @ np.linalg.lstsq(x, y, rcond=None)[0], False
            else:
                e, spanned = cres.residual(y, x)
            residual, reranked = np.zeros(names), np.zeros(names)
            if not spanned:
                keep = np.zeros(names, dtype=bool)
                residual[names_t], keep[names_t] = e, True
                reranked[names_t] = cres.centred_tied_ranks(residual, keep)[names_t]
                row[names_t] += mass[t] * reranked[names_t]
            regressors.append({"a": residual, "b": reranked, "c": composite}[variant])
    return out


def fixture():
    """strategy_ic_theme_resid_test.cpp ResidFixture: 3 dates x 8 names, name 7 leaves on date 2; a2 (sign -1) misses
    name 0 on date 0 and every name on date 2; c1 misses name 2 on date 0; theme b misses name 1 on date 1; on date 2
    b1 = b2 = a1."""
    member = np.ones((DAYS, WIDTH), dtype=np.uint8)
    member[2, 7] = 0
    s = [np.zeros((DAYS, WIDTH)) for _ in range(5)]
    for d in range(DAYS):
        for i in range(WIDTH):
            s[0][d, i] = float((3 * i + 2 * d) % 8) + 0.25 * i
            s[1][d, i] = float((5 * i + 3 * d + 1) % 8)
            s[2][d, i] = float((7 * i + d) % 8) - 0.5 * (i % 3)
            s[3][d, i] = float((i * i + 3 * d) % 11)
            s[4][d, i] = float((3 * i * i + 5 * d + 2) % 13) + 0.125 * i
    s[1][0, 0] = NAN
    s[4][0, 2] = NAN
    s[2][1, 1] = s[3][1, 1] = NAN
    s[1][2, :] = NAN
    s[2][2, :] = s[0][2, :]
    s[3][2, :] = s[0][2, :]
    return member, s


def std_blend(member, signals, weights, signs, positions, themes):
    """ew-theme-std-v1 on the same planes: sum over themes of W_t z_t (no residualisation)."""
    out = np.where(member == 1, 0.0, np.nan)
    mass = np.zeros(themes)
    for k, w in enumerate(weights):
        if w > 0:
            mass[positions[k]] += w
    for d in range(member.shape[0]):
        total = np.zeros((themes, member.shape[1]))
        present = np.zeros((themes, member.shape[1]), dtype=bool)
        for k, sig in enumerate(signals):
            if not weights[k] > 0:
                continue
            r = cres.centred_tied_ranks(sig[d], (member[d] == 1) & np.isfinite(sig[d]))
            ok = ~np.isnan(r)
            total[positions[k], ok] += signs[k] * weights[k] * r[ok]
            present[positions[k], ok] = True
        for t in range(themes):
            z = cres.centred_tied_ranks(total[t], present[t])
            ok = ~np.isnan(z)
            out[d, ok] += mass[t] * z[ok]
    return out


class DeclaredRule(unittest.TestCase):
    def test_declared_constants_and_their_cpp_pins(self):
        self.assertEqual((cres.RULE_ID, cres.BLOCK, cres.SPAN_TOLERANCE),
                         ("theme-resid-v1", "theme_residualise", 1e-10))
        # rule 1 (Ruling PM4-11): the order is PRIOR_THEMES; its first ten are the frozen v4 list + v7 appended theme
        self.assertEqual(cres.FROZEN_PREFIX, fcw.V4_THEMES + fcw.V7_APPENDED_THEMES)
        self.assertEqual(cres.registered_order(fcw.PRIOR_THEMES), fcw.PRIOR_THEMES)
        self.assertEqual(cres.STD_RULE_ID, cr.STD_RULE_ID)
        # rule 6 (E-44, E-45): every rerank-true theme_standardise rule the fitter writes (R-1 / R-3, then R-10)
        self.assertEqual(cres.STANDARDISE_RULES, (cr.STD_RULE_ID,) + cis.RULES)
        self.assertEqual(len(cres.RULE_TEXT), 5)
        registry = REPO / "atx-impl" / "strategies" / "alphas" / "registry.json"
        if registry.is_file():   # the registry's themes table in file order is PRIOR_THEMES (a new theme edits both)
            self.assertEqual(tuple(json.loads(registry.read_text(encoding="utf-8"))["themes"]), fcw.PRIOR_THEMES)
        kernel = (REPO / "atx-engine" / "include" / "atx" / "engine" / "combine" / "group_residualise.hpp").read_text(
            encoding="utf-8")
        self.assertIn("kResidualSpanTolerance = 1e-10;", kernel)
        detail = (REPO / "atx-impl" / "src" / "strategy_ic_detail.hpp").read_text(encoding="utf-8")
        self.assertIn('theme_residualise_rule="theme-resid-v1";', detail)
        self.assertIn('"theme_residualise"', (REPO / "atx-impl" / "src" / "strategy_ic_theme_resid.cpp").read_text(
            encoding="utf-8"))
        # finding R6B-O-4: the runner's copy of the registered order (strategy_ic_theme_resid.hpp theme_resid_order)
        # extends the fitter's PRIOR_THEMES and places filing_events after the frozen ten (Ruling PM4-11)
        header = (REPO / "atx-impl" / "src" / "strategy_ic_theme_resid.hpp").read_text(encoding="utf-8")
        listed = re.search(r"theme_resid_order\{([^}]*)\}", header)
        self.assertIsNotNone(listed)
        cpp = tuple(re.findall(r'"([a-z0-9_]+)"', listed.group(1)))
        self.assertEqual(cpp[:len(fcw.PRIOR_THEMES)], fcw.PRIOR_THEMES)
        self.assertEqual(cpp, cres.FROZEN_PREFIX + ("filing_events",))
        self.assertEqual(cres.registered_order(cpp), cpp)

    def test_theme_order_is_the_registered_order_restricted(self):
        self.assertEqual(cres.theme_order(["reversal_seasonality", "value", "ownership_flow", "value"], fcw.PRIOR_THEMES),
                         ["value", "reversal_seasonality", "ownership_flow"])
        with self.assertRaises(cres.ResidError):
            cres.theme_order(["value", "liquidity"], fcw.PRIOR_THEMES)
        with self.assertRaises(cres.ResidError):
            cres.theme_order([], fcw.PRIOR_THEMES)

    def test_a_later_registered_theme_follows_the_frozen_ten(self):
        """Ruling PM4-11 (finding R6B-O-2): a theme registered after the frozen ten (v8.1's filing_events, E7) is
        appended in registration order, so filing_events is last, after ownership_flow; before it is registered it is
        refused as outside the order. The frozen ten may not be reordered, dropped or interleaved."""
        later = fcw.PRIOR_THEMES + ("filing_events",)
        self.assertEqual(cres.registered_order(later), later)
        self.assertEqual(cres.theme_order(["filing_events", "ownership_flow", "value"], later),
                         ["value", "ownership_flow", "filing_events"])
        self.assertEqual(cres.theme_order(["filing_events", "low_risk"], later + ("another_theme",)),
                         ["low_risk", "filing_events"])
        with self.assertRaises(cres.ResidError):
            cres.theme_order(["filing_events", "value"], fcw.PRIOR_THEMES)
        swapped = ("profitability_quality", "value") + fcw.PRIOR_THEMES[2:]
        interleaved = fcw.PRIOR_THEMES[:9] + ("filing_events",) + fcw.PRIOR_THEMES[9:]
        for bad in (swapped, interleaved, fcw.PRIOR_THEMES[1:], later + ("value",), ()):
            with self.subTest(bad=bad), self.assertRaises(cres.ResidError):
                cres.registered_order(bad)


class RunnerReference(unittest.TestCase):
    def test_three_themes_equal_the_registered_rule(self):
        """Python equals C++: the numpy reference (lstsq) gives the exact values that strategy_ic_theme_resid_test.cpp
        pins."""
        member, signals = fixture()
        out = cres.blend(member, signals, WEIGHTS, SIGNS, POSITIONS, 3)
        for d in range(DAYS):
            for i in range(WIDTH):
                want = EXPECTED_BLEND[d][i]
                if want is None:
                    self.assertTrue(math.isnan(out[d, i]))
                else:
                    self.assertAlmostEqual(out[d, i], float(want), delta=1e-12, msg=(d, i))
        std = std_blend(member, signals, WEIGHTS, SIGNS, POSITIONS, 3)
        self.assertGreater(np.nanmax(np.abs(out - std)), 0.05)                  # the rule changes the blend
        # date 2: theme b is theme a, so it adds nothing; theme a is the standardised composite
        a_only = std_blend(member, signals, [0.2, 0.15, 0, 0, 0], SIGNS, POSITIONS, 1)
        b_equal_a = cres.blend(member, signals, [0.2, 0.15, 0.2, 0.2, 0], SIGNS, POSITIONS, 2)
        np.testing.assert_allclose(b_equal_a[2, :7], a_only[2, :7], rtol=0, atol=1e-15)
        # the order is the rule: b first changes the blend (by exactly 1/40 at most, on date 2, with PM4-12's tie step;
        # the three-theme kernel case below and ThemeResidRunner's 3-cycle are the stronger order checks)
        swapped = cres.blend(member, signals, WEIGHTS, SIGNS, [1, 1, 0, 0, 2], 3)
        self.assertGreater(np.nanmax(np.abs(out - swapped)), 0.02)

    def test_first_theme_is_its_standardised_composite(self):
        member, signals = fixture()
        one = [0.2, 0.15, 0, 0, 0]
        np.testing.assert_array_equal(cres.blend(member, signals, one, SIGNS, POSITIONS, 1),
                                      std_blend(member, signals, one, SIGNS, POSITIONS, 1))

    def test_two_theme_residual_is_the_closed_form_and_orthogonal(self):
        rng = np.random.default_rng(11)
        y, x = rng.normal(size=9), rng.normal(size=9)
        e, spanned = cres.residual(y, x.reshape(-1, 1))
        xc, yc = x - x.mean(), y - y.mean()
        closed = yc - (xc @ yc / (xc @ xc)) * xc
        self.assertFalse(spanned)
        np.testing.assert_allclose(e, closed, rtol=0, atol=1e-14)
        self.assertLess(abs(e.sum()), 1e-12)
        self.assertLess(abs(e @ x), 1e-12)

    def test_tied_composite_stays_tied_after_residualisation(self):
        """Ruling PM4-12: names tied in a theme's own composite get one value of it, the re-ranked mean of their
        residuals (exact fractions; the C++ kernel test pins the same); on date 1 the block means reorder the levels;
        the registered text without the tie step spreads each block (TIE_SPREAD)."""
        got = cres.kernel(TIE_PLANES, [0.5, 0.5], TIE_NAMES)
        np.testing.assert_allclose(got, [float(x) for x in TIE_EXPECTED], rtol=0, atol=1e-15)
        spread = registered_without_tie_step(TIE_PLANES, [0.5, 0.5], TIE_NAMES)
        np.testing.assert_allclose(spread, [float(x) for x in TIE_SPREAD], rtol=0, atol=1e-15)
        self.assertGreater(np.max(np.abs(got - spread)), 0.05)
        for d in range(2):
            cells = slice(d * TIE_NAMES, (d + 1) * TIE_NAMES)
            first = cres.centred_tied_ranks(np.array(TIE_PLANES[0][cells], dtype=float), np.ones(TIE_NAMES, bool))
            second = got[cells] - 0.5 * first                                   # theme 1's add per name
            flag = np.array(TIE_PLANES[1][cells])
            for level in set(flag.tolist()):
                block = second[flag == level]
                self.assertLess(np.ptp(block), 1e-15, (d, level))           # one value per tie block
        levels = {lv: float(second[np.array(TIE_PLANES[1][TIE_NAMES:]) == lv][0]) for lv in (0, 1, 2)}
        self.assertEqual(sorted(levels, key=levels.get), [0, 2, 1])            # residualisation moved whole blocks

    def test_tie_block_means_sum_in_ascending_name_order(self):
        """The block mean's summation order (PM4-12 as coded on both sides): from the block's first name, ascending; a
        block of one name is untouched (its bits kept)."""
        e = np.array([1e16, 0.3, 1.0, -1e16])
        z = np.array([0.5, -0.5, 0.5, 0.5])
        got = cres.tie_block_means(e, z)
        self.assertEqual(got[1], 0.3)                                          # a block of one: untouched
        self.assertEqual(list(got[[0, 2, 3]]), [0.0, 0.0, 0.0])               # ((1e16 + 1) - 1e16) / 3 in this order
        self.assertNotEqual((1e16 + -1e16 + 1.0) / 3, 0.0)                    # another order gives 1/3
        np.testing.assert_array_equal(cres.tie_block_means(e, np.arange(4.0)), e)

    def test_unequal_tie_blocks_beside_singletons_pin_the_block_mean(self):
        """Finding R6C-3 (Ruling PM5-12, the Python half): tie blocks of three and two names beside three singletons,
        where the order of the block means differs from the block sums' and from the per-block mean of the residual's
        ranks (UNEQUAL_*). The rule gives UNEQUAL_EXPECTED, one value per block; a block sum or rank averaging gives
        UNEQUAL_WRONG, each at least 2/7 away on some name; without the tie step the blocks spread."""
        mass = [0.5, 0.5]
        got = cres.kernel(UNEQUAL_PLANES, mass, UNEQUAL_NAMES)
        np.testing.assert_allclose(got, [float(x) for x in UNEQUAL_EXPECTED], rtol=0, atol=1e-15)
        first = cres.centred_tied_ranks(np.array(UNEQUAL_PLANES[0], dtype=float), np.ones(UNEQUAL_NAMES, dtype=bool))
        add = got - mass[0] * first                                            # theme 1's add per name
        blocks = _tie_blocks(UNEQUAL_PLANES[1])
        self.assertEqual(sorted(map(len, blocks)), [2, 3])                    # unequal blocks beside 3 singletons
        for b in blocks:
            self.assertLess(np.ptp(add[b]), 1e-15, b)                          # one value per block
        self.assertEqual(sorted(range(UNEQUAL_NAMES), key=lambda k: (add[k], k)), [0, 1, 5, 7, 2, 4, 6, 3])
        for variant, values in UNEQUAL_WRONG.items():
            with unittest.mock.patch.object(cres, "tie_block_means", WRONG_TIE_STEPS[variant]):
                wrong = cres.kernel(UNEQUAL_PLANES, mass, UNEQUAL_NAMES)
            np.testing.assert_allclose(wrong, [float(x) for x in values], rtol=0, atol=1e-15, err_msg=variant)
            self.assertGreater(np.max(np.abs(wrong - got)), 2 / 7 - 1e-12, variant)
        spread = registered_without_tie_step(UNEQUAL_PLANES, mass, UNEQUAL_NAMES)
        self.assertGreater(np.max(np.abs(spread - got)), 0.1)

    def test_no_tie_composite_is_the_registered_rule_bit_for_bit(self):
        """Ruling PM4-12: a composite without ties gives the registered text bit for bit (strategy_ic_theme_resid_test.cpp
        NoTieCompositeIsTheRegisteredRuleBitForBit, same planes)."""
        names = 7
        planes = np.zeros((3, 2 * names))
        for d in range(2):
            for i in range(names):
                planes[0, d * names + i] = float((3 * i + 2 * d) % 7) + 0.5
                planes[1, d * names + i] = float((5 * i + d) % 7) * 0.25
                planes[2, d * names + i] = float((2 * i + 3 * d + 1) % 7) - 3.0
        planes[1, names + 2] = NAN
        planes[2, 5] = NAN
        got = cres.kernel(planes, [0.3, 0.45, 0.25], names)
        want = registered_without_tie_step(planes, [0.3, 0.45, 0.25], names)
        self.assertEqual(got.view(np.uint64).tolist(), want.view(np.uint64).tolist())
        self.assertGreater(np.max(np.abs(got)), 0.05)

    def test_small_case_separates_the_registered_regressors(self):
        """Finding R6B-O-7 (strategy_ic_theme_resid_test.cpp SmallCaseSeparatesTheRegisteredRegressors, same planes):
        the registered rule (theme t on an intercept and the earlier composites) gives SMALL_EXPECTED; regressing on the
        earlier pre-rank residuals (a), on their re-ranked residuals (b) or without the intercept (c) gives SMALL_WRONG,
        each at least 1/12 away. No composite has a tie, so the tie step leaves the registered text bit for bit."""
        got = cres.kernel(SMALL_PLANES, SMALL_MASS, SMALL_NAMES)
        np.testing.assert_allclose(got, [float(x) for x in SMALL_EXPECTED], rtol=0, atol=1e-15)
        want = registered_without_tie_step(SMALL_PLANES, SMALL_MASS, SMALL_NAMES)
        self.assertEqual(got.view(np.uint64).tolist(), want.view(np.uint64).tolist())
        for variant, values in SMALL_WRONG.items():
            wrong = plausible_wrong_rule(SMALL_PLANES, SMALL_MASS, SMALL_NAMES, variant)
            np.testing.assert_allclose(wrong, [float(x) for x in values], rtol=0, atol=1e-12, err_msg=variant)
            self.assertGreater(np.max(np.abs(wrong - got)), 0.08, variant)

    def test_three_theme_cycle_is_the_registered_rule(self):
        """Finding R6B-O-7 (strategy_ic_runner_test.cpp ThemeResidRunner.ThreeThemeCycleIsTheRegisteredRule, same
        signals): the blend with the members at registered positions 1, 2, 0 is CYCLE_EXPECTED; (d) the inverted
        position map (2, 0, 1) and the plausible wrong rules (a)-(c) give CYCLE_WRONG, each at least 1/20 away."""
        member = np.ones((1, WIDTH), dtype=np.uint8)
        signals = [np.array([s]) for s in CYCLE_SIGNALS]
        got = cres.blend(member, signals, CYCLE_WEIGHTS, [1, 1, 1], [1, 2, 0], 3)[0]
        np.testing.assert_allclose(got, [float(x) for x in CYCLE_EXPECTED], rtol=0, atol=1e-15)
        order = [2, 0, 1]                                  # the registered order: resid_c, resid_a, resid_b
        planes, mass = [CYCLE_SIGNALS[k] for k in order], [CYCLE_WEIGHTS[k] for k in order]
        np.testing.assert_allclose(cres.kernel(planes, mass, WIDTH), got, rtol=0, atol=1e-15)
        wrong = {v: plausible_wrong_rule(planes, mass, WIDTH, v) for v in "abc"}
        wrong["d"] = cres.blend(member, signals, CYCLE_WEIGHTS, [1, 1, 1], [2, 0, 1], 3)[0]
        for variant, values in CYCLE_WRONG.items():
            np.testing.assert_allclose(wrong[variant], [float(x) for x in values], rtol=0, atol=1e-12,
                                       err_msg=variant)
            self.assertGreater(np.max(np.abs(wrong[variant] - got)), 0.05 - 1e-12, variant)

    def test_spanned_and_dependent_regressors(self):
        x = np.array([0.5, -0.25, 1 / 6, -1 / 3, 0.125, 0.0])
        e, spanned = cres.residual(2 * x - 1, x.reshape(-1, 1))
        self.assertTrue(spanned)
        self.assertEqual(e.tolist(), [0.0] * 6)
        y = np.array([0.1, 0.4, -0.3, 0.2, -0.5, 0.05])
        one, _ = cres.residual(y, x.reshape(-1, 1))
        dup, _ = cres.residual(y, np.column_stack([x, 3 * x + 2, np.zeros(6)]))
        np.testing.assert_allclose(dup, one, rtol=0, atol=1e-14)            # a dependent column changes nothing
        e, spanned = cres.residual(np.full(4, 0.25), np.empty((4, 0)))
        self.assertTrue(spanned)                                            # a constant composite adds nothing


class FitterEndToEnd(unittest.TestCase):
    """fit_composition_weights --theme-resid theme-resid-v1 on the fitter's synthetic world (registry absent)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        panel, signals, ids, extra = tfw.aim_world()
        extra = dict(extra)
        extra["flip"] = dict(extra["flip"], theme="value")           # two themes, five admitted members
        extra["medium_half"] = dict(extra["medium_half"], tier="C+")  # a scored grade
        cls.ids = ids
        cls.fx = tfw.Fixture(cls.root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                             candidate_extra=extra)
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", cls.root / "no-registry.json"):
            cls.std_code, cls.std_summary = fcw.fit(cls.fx.args(cls.root / "std", **STD_ARGS))
            cls.code, cls.summary = fcw.fit(cls.fx.args(cls.root / "resid", theme_resid=cres.RULE_ID, **STD_ARGS))
            # R-11 on an accepted R-10 parent (Rulings E-44, E-45, finding R6B-O-1): the rerank-true block of either
            # rule of the runner's table that R-10 writes, ic-shrink-v1 and its aim variant ic-shrink-aim-v1
            cls.shrink_codes = {}
            for comp in cis.RULES:
                shrink = dict(STD_ARGS, composition=comp)
                cls.shrink_codes[comp] = (fcw.fit(cls.fx.args(cls.root / comp, **shrink))[0],
                                          fcw.fit(cls.fx.args(cls.root / f"{comp}-resid", theme_resid=cres.RULE_ID,
                                                              **shrink))[0])
            # R-11 on an accepted R-3 parent (Ruling E-44, finding R6B-O-6): ew-theme-std-aim-v1
            aim = dict(STD_ARGS, composition=cr.STD_AIM_RULE_ID)
            cls.aim_codes = (fcw.fit(cls.fx.args(cls.root / "std-aim", **aim))[0],
                             fcw.fit(cls.fx.args(cls.root / "std-aim-resid", theme_resid=cres.RULE_ID, **aim))[0])
        cls.std_bytes = {p.name: p.read_bytes() for p in (cls.root / "std").iterdir()}
        cls.bytes = {p.name: p.read_bytes() for p in (cls.root / "resid").iterdir()}
        cls.std = json.loads(cls.std_bytes[fcw.OUTPUT_WEIGHTS])
        cls.doc = json.loads(cls.bytes[fcw.OUTPUT_WEIGHTS])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_resid_is_the_parent_weights_plus_the_block(self):
        self.assertEqual((self.code, self.std_code), (fcw.EXIT_OK, fcw.EXIT_OK))
        self.assertEqual(self.bytes[fcw.OUTPUT_ADMISSION], self.std_bytes[fcw.OUTPUT_ADMISSION])   # screen unchanged
        self.assertEqual(self.doc[cres.BLOCK], {"rule": "theme-resid-v1", "order": ["value", "reversal_seasonality"]})
        self.assertEqual(self.summary[cres.BLOCK], self.doc[cres.BLOCK])
        resid = self.doc["provenance"]["resid"]
        self.assertEqual((resid["rule"], resid["parent_composition"], resid["span_tolerance_relative"]),
                         ("theme-resid-v1", "ew-theme-std-v1", 1e-10))
        self.assertEqual(resid["registered_order"], list(fcw.PRIOR_THEMES))
        self.assertEqual(resid["frozen_prefix"], list(cres.FROZEN_PREFIX))
        # flag absent: no key; the resid file minus the block and provenance.resid is the parent file, byte for byte
        self.assertNotIn(cres.BLOCK, self.std)
        self.assertNotIn("resid", self.std["provenance"])
        self.assertNotIn(cres.BLOCK, self.std_summary)
        stripped = json.loads(self.bytes[fcw.OUTPUT_WEIGHTS])
        del stripped[cres.BLOCK]
        del stripped["provenance"]["resid"]
        self.assertEqual(fcw.canonical_bytes(stripped), self.std_bytes[fcw.OUTPUT_WEIGHTS])
        self.assertEqual(self.bytes[fcw.OUTPUT_WEIGHTS], fcw.canonical_bytes(self.doc))

    def test_document_is_what_the_runner_reads(self):
        # strategy_ic_theme_resid.cpp composition_residualise: theme_residualise {rule, order} beside a rerank-true
        # theme_standardise, order naming each weighted theme exactly once
        self.assertEqual(self.doc["schema"], "atx.dsl-composition-weights/v2")
        std = self.doc["theme_standardise"]
        self.assertEqual((std["rule"], std["rerank"]), ("ew-theme-std-v1", True))
        weighted = {t for i, t in std["themes"].items() if self.doc["weights"][i] > 0}
        self.assertEqual(sorted(self.doc[cres.BLOCK]["order"]), sorted(weighted))

    def test_refused_without_a_standardised_parent(self):
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"):
            with self.assertRaises(fcw.FitError) as caught:                      # ew-theme-v1: no theme_standardise
                fcw.fit(self.fx.args(self.root / "v1-resid", theme_resid=cres.RULE_ID, **tfw.V4_ARGS))
            self.assertIn("theme_standardise", str(caught.exception))
            with self.assertRaises(fcw.FitError) as caught:                      # the mv path: not a prior fit
                fcw.fit(self.fx.args(self.root / "mv-resid", theme_resid=cres.RULE_ID))
            self.assertIn("--theme-resid", str(caught.exception))
        self.assertFalse((self.root / "v1-resid").exists())
        self.assertFalse((self.root / "mv-resid").exists())

    def test_an_ic_shrink_parent_is_a_standardised_parent(self):
        """Rulings E-44, E-45 (finding R6B-O-1): R-10's ic-shrink-v1 and ic-shrink-aim-v1 files carry a rerank-true
        theme_standardise of their own rule (the runner's per-date standardisation unchanged), so R-11 attaches its
        block on either and records the parent's rule; the file minus the block and provenance.resid is the plain parent
        file byte for byte (composition_resid.apply runs after composition_ic_shrink.attach: before it, the block would
        be missing and the fit refused)."""
        for comp in cis.RULES:
            with self.subTest(parent=comp):
                self.assertEqual(self.shrink_codes[comp], (fcw.EXIT_OK, fcw.EXIT_OK))
                plain = (self.root / comp / fcw.OUTPUT_WEIGHTS).read_bytes()
                doc = json.loads((self.root / f"{comp}-resid" / fcw.OUTPUT_WEIGHTS).read_bytes())
                self.assertEqual((doc["theme_standardise"]["rule"], doc["theme_standardise"]["rerank"]), (comp, True))
                self.assertEqual(doc[cres.BLOCK], {"rule": "theme-resid-v1", "order": ["value", "reversal_seasonality"]})
                self.assertEqual(doc["provenance"]["rule"], comp)
                self.assertEqual(doc["provenance"]["resid"]["parent_composition"], comp)
                self.assertEqual("aim" in doc["provenance"], comp == cis.AIM_RULE_ID)   # the aim parent's gains stay
                del doc[cres.BLOCK]
                del doc["provenance"]["resid"]
                self.assertEqual(fcw.canonical_bytes(doc), plain)

    def test_an_aim_parent_keeps_its_gains_and_records_its_rule(self):
        """Ruling E-44 (finding R6B-O-6): R-11 on R-3's ew-theme-std-aim-v1 parent, end to end. The parent writes the
        ew-theme-std-v1 block (rerank true), so the fitter attaches theme_residualise in the registered order and
        provenance.resid records the aim parent's rule; the aim gains stay in the weights: the file minus the block and
        provenance.resid is the plain aim parent file byte for byte. A future change of the aim variant's block rule
        that R-11 does not accept fails here."""
        self.assertEqual(self.aim_codes, (fcw.EXIT_OK, fcw.EXIT_OK))
        plain = (self.root / "std-aim" / fcw.OUTPUT_WEIGHTS).read_bytes()
        doc = json.loads((self.root / "std-aim-resid" / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual((doc["theme_standardise"]["rule"], doc["theme_standardise"]["rerank"]), (cr.STD_RULE_ID, True))
        self.assertEqual(doc[cres.BLOCK], {"rule": "theme-resid-v1", "order": ["value", "reversal_seasonality"]})
        self.assertEqual(doc["provenance"]["rule"], cr.STD_AIM_RULE_ID)
        self.assertEqual(doc["provenance"]["resid"]["parent_composition"], "ew-theme-std-aim-v1")
        self.assertIn("aim", doc["provenance"])
        self.assertNotEqual(doc["weights"], self.std["weights"])                 # the gains moved weight
        del doc[cres.BLOCK]
        del doc["provenance"]["resid"]
        self.assertEqual(fcw.canonical_bytes(doc), plain)

    def test_attach_refuses_rerank_off_and_unregistered_themes(self):
        off = json.loads(json.dumps(self.std))
        off["theme_standardise"]["rerank"] = False
        with self.assertRaises(cres.ResidError):
            cres.attach(off, fcw.PRIOR_THEMES)
        unknown = json.loads(json.dumps(self.std))                             # a rule outside the runner's table
        unknown["theme_standardise"]["rule"] = "ew-theme-std-v9"
        with self.assertRaises(cres.ResidError):
            cres.attach(unknown, fcw.PRIOR_THEMES)
        alien = json.loads(json.dumps(self.std))
        first = next(i for i, w in alien["weights"].items() if w > 0)
        alien["theme_standardise"]["themes"][first] = "liquidity"
        with self.assertRaises(cres.ResidError):
            cres.attach(alien, fcw.PRIOR_THEMES)

    def resid_fit(self, name: str, parent: Path | None, sha: str | None = None, **override):
        """--theme-resid on the std parent's argv into ``name``, checked against ``parent`` (finding R6B-O-5)."""
        args = dict(STD_ARGS, theme_resid=cres.RULE_ID)
        args.update(override)
        if parent is not None:
            args.update(theme_resid_parent=str(parent),
                        theme_resid_parent_sha256=sha or hashlib.sha256(parent.read_bytes()).hexdigest())
        with unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"):
            return fcw.fit(self.fx.args(self.root / name, **args))

    def test_the_re_fit_is_checked_against_the_parent_cells_file(self):
        """Finding R6B-O-5: with --theme-resid-parent the re-fit must be the parent cell's weights file plus the block;
        provenance.resid and the summary record the parent's SHA-256. A changed fitter file (script_sha256, and the
        admission SHA it moves) is the one tolerated difference."""
        parent = self.root / "std" / fcw.OUTPUT_WEIGHTS
        sha = hashlib.sha256(parent.read_bytes()).hexdigest()
        code, summary = self.resid_fit("checked", parent)
        self.assertEqual(code, fcw.EXIT_OK)
        doc = json.loads((self.root / "checked" / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual(doc["provenance"]["resid"]["parent_weights_sha256"], sha)
        self.assertEqual(doc["provenance"]["resid"]["parent_check"], cres.PARENT_CHECK)
        self.assertEqual(summary["theme_residualise_parent_sha256"], sha)
        self.assertEqual(summary[cres.BLOCK], doc[cres.BLOCK])
        del doc[cres.BLOCK]
        del doc["provenance"]["resid"]
        self.assertEqual(fcw.canonical_bytes(doc), parent.read_bytes())
        with unittest.mock.patch.object(fcw, "SCRIPT_SHA256", "0" * 64):       # the fitter file changed in between
            code, _ = self.resid_fit("new-script", parent)
        self.assertEqual(code, fcw.EXIT_OK)
        moved = json.loads((self.root / "new-script" / fcw.OUTPUT_WEIGHTS).read_bytes())["provenance"]
        before = json.loads(parent.read_bytes())["provenance"]
        self.assertEqual(moved["script_sha256"], "0" * 64)
        self.assertNotEqual(moved["admission_sha256"], before["admission_sha256"])

    def test_a_re_fit_that_is_not_the_parent_cells_file_is_refused(self):
        """Finding R6B-O-5: another composition's file, a parent whose weights or admission were changed, a parent with
        a key the re-fit lacks (even a null one), a parent that already carries the block, a wrong pin, a pooled (era)
        fit and incomplete flags are refused before any output."""
        parent = self.root / "std" / fcw.OUTPUT_WEIGHTS
        tampered = self.root / "tampered"
        tampered.mkdir()
        doc = json.loads(parent.read_bytes())
        first = next(i for i, w in doc["weights"].items() if w > 0)
        doc["weights"][first] += 1e-12
        (tampered / fcw.OUTPUT_WEIGHTS).write_bytes(fcw.canonical_bytes(doc))
        (tampered / fcw.OUTPUT_ADMISSION).write_bytes((self.root / "std" / fcw.OUTPUT_ADMISSION).read_bytes())
        moved = self.root / "moved-admission"                              # an admission changed beyond the script SHA
        moved.mkdir()
        adm = json.loads((self.root / "std" / fcw.OUTPUT_ADMISSION).read_bytes())
        adm["rules"]["note"] = "edited"
        (moved / fcw.OUTPUT_ADMISSION).write_bytes(fcw.canonical_bytes(adm))
        doc = json.loads(parent.read_bytes())
        doc["provenance"]["admission_sha256"] = hashlib.sha256(fcw.canonical_bytes(adm)).hexdigest()
        (moved / fcw.OUTPUT_WEIGHTS).write_bytes(fcw.canonical_bytes(doc))
        extra = self.root / "extra-null"                                   # a key the re-fit lacks, even as null
        extra.mkdir()
        doc = json.loads(parent.read_bytes())
        doc["provenance"]["note"] = None
        (extra / fcw.OUTPUT_WEIGHTS).write_bytes(fcw.canonical_bytes(doc))
        (extra / fcw.OUTPUT_ADMISSION).write_bytes((self.root / "std" / fcw.OUTPUT_ADMISSION).read_bytes())
        cases = [("other-rule", self.root / cis.RULE_ID / fcw.OUTPUT_WEIGHTS, {}, "is not the parent cell's"),
                 ("tampered", tampered / fcw.OUTPUT_WEIGHTS, {}, r"differ: \['weights'"),
                 ("moved", moved / fcw.OUTPUT_WEIGHTS, {}, r"admission.json differs .*\['rules'\]"),
                 ("extra-null", extra / fcw.OUTPUT_WEIGHTS, {}, r"differ: \['provenance.note'\]"),
                 ("resid-parent", self.root / "resid" / fcw.OUTPUT_WEIGHTS, {}, "already carries theme_residualise"),
                 ("bad-pin", parent, {"sha": "0" * 64}, "SHA-256 pin differs"),
                 ("pooled", parent, {"era_id": "E3"}, "the pooled .era. fit never takes it")]
        for name, path, extra, needle in cases:
            with self.subTest(name), self.assertRaisesRegex(fcw.FitError, needle):
                self.resid_fit(f"refused-{name}", path, **extra)
            self.assertFalse((self.root / f"refused-{name}").exists())
        with self.assertRaisesRegex(fcw.FitError, "go together"):
            self.resid_fit("refused-half", None, theme_resid_parent=str(parent))
        with self.assertRaisesRegex(fcw.FitError, "needs --theme-resid"):
            self.resid_fit("refused-no-flag", parent, theme_resid=None)
        self.assertFalse((self.root / "refused-half").exists() or (self.root / "refused-no-flag").exists())

    def registry(self, name: str, themes) -> Path:
        """An alpha registry (task A-1 schema) whose themes table lists ``themes`` in order."""
        path = self.root / name
        path.write_text(json.dumps({"schema": fcw.REGISTRY_SCHEMA, "alphas": [], "fields": {},
                                    "themes": {t: f"{t} text" for t in themes}}), encoding="utf-8")
        return path

    def test_the_fitter_derives_the_order_from_prior_themes(self):
        """Ruling PM4-11 (finding R6B-O-2): the fitter passes its PRIOR_THEMES, so a theme registered later (E7's
        filing_events, in the registry and in V7_APPENDED_THEMES: finding R6C-4) is placed last once it is registered,
        and a weighted member of it is refused before."""
        later = fcw.PRIOR_THEMES + ("filing_events",)
        doc = json.loads(json.dumps(self.std))
        weighted = [i for i, w in doc["weights"].items() if w > 0]
        doc["theme_standardise"]["themes"][weighted[0]] = "filing_events"
        self.assertEqual(cres.attach(json.loads(json.dumps(doc)), later)[cres.BLOCK]["order"][-1], "filing_events")
        with self.assertRaises(cres.ResidError):
            cres.attach(doc, fcw.PRIOR_THEMES)
        with unittest.mock.patch.object(fcw, "PRIOR_THEMES", later), \
                unittest.mock.patch.object(fcw, "REGISTRY_PATH", self.registry("later.json", later)), \
                unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"):
            code, _ = fcw.fit(self.fx.args(self.root / "later-resid", theme_resid=cres.RULE_ID, **STD_ARGS))
        self.assertEqual(code, fcw.EXIT_OK)
        resid = json.loads((self.root / "later-resid" / fcw.OUTPUT_WEIGHTS).read_bytes())["provenance"]["resid"]
        self.assertEqual(resid["registered_order"], list(later))
        self.assertEqual(resid["order"], ["value", "reversal_seasonality"])

    def test_the_registry_themes_must_be_prior_themes_under_theme_resid(self):
        """Finding R6C-4 (Ruling PM5-12): under --theme-resid the registry's theme tuple must equal PRIOR_THEMES, the
        registered order, before anything is computed (here before the parent flags are even read): a theme registered
        without extending V7_APPENDED_THEMES (E7's filing_events), PRIOR_THEMES extended without the registry, or the
        same themes in another order are refused naming both tuples, with no output. Flag absent: not checked."""
        later = fcw.PRIOR_THEMES + ("filing_events",)
        reordered = fcw.PRIOR_THEMES[1:] + fcw.PRIOR_THEMES[:1]
        bogus_parent = {"theme_resid_parent": str(self.root / "no-parent.json"), "theme_resid_parent_sha256": "0" * 64}
        for name, registered, constant in (("registered-only", later, fcw.PRIOR_THEMES),
                                           ("constant-only", fcw.PRIOR_THEMES, later),
                                           ("reordered", reordered, fcw.PRIOR_THEMES)):
            with self.subTest(name), unittest.mock.patch.object(fcw, "PRIOR_THEMES", constant), \
                    unittest.mock.patch.object(fcw, "REGISTRY_PATH", self.registry(f"{name}.json", registered)), \
                    unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"):
                with self.assertRaises(fcw.FitError) as caught:
                    fcw.fit(self.fx.args(self.root / f"order-{name}", theme_resid=cres.RULE_ID, **bogus_parent,
                                         **STD_ARGS))
                msg = str(caught.exception)
                self.assertIn(f"the admissible themes (registry {name}.json: {', '.join(registered)}) are not "
                              f"PRIOR_THEMES", msg)
                self.assertIn(f"(Ruling PM4-11: {', '.join(constant)})", msg)
                self.assertFalse((self.root / f"order-{name}").exists())
        with unittest.mock.patch.object(fcw, "REGISTRY_PATH", self.registry("plain.json", later)), \
                unittest.mock.patch.object(cr, "REGISTRY_PATH", self.root / "no-registry.json"):
            code, _ = fcw.fit(self.fx.args(self.root / "order-plain", **STD_ARGS))   # no --theme-resid: no check
        self.assertEqual(code, fcw.EXIT_OK)


if __name__ == "__main__":
    unittest.main()
