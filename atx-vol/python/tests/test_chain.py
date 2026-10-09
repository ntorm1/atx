"""Unit tests for `atxvol.chain`'s pure frame algebra.

Scope is deliberate: everything here runs on a SYNTHETIC frame built in-process,
with no parquet fixture and no inversion. `add_market_ivs` and `load_chain` are
covered by the export-side tests and by running the tool; what is untested and
easy to break silently is the arithmetic around them -- the expiry selector, the
forward, the normalisation, and the sentinel masking. Those are the parts a
plot quietly lies with when they are wrong.
"""

from __future__ import annotations

# IMPORT ORDER IS LOAD-BEARING AND THIS BLOCK MUST NOT BE ALPHABETISED.
# `import pandas` transitively imports `pyarrow`, and pyarrow ahead of
# `atxvol._core` kills the extension with "DLL load failed ... initialization
# routine failed". So `atxvol` (via `chain`) goes FIRST -- putting numpy/pandas
# above it, which is what an import sorter would do, breaks collection of this
# very file. See `atxvol/chain.py`'s module docstring.
from atxvol import chain

import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
import pytest  # noqa: E402


def _frame(strikes=(90.0, 100.0, 110.0), *, cp="C", year=2026, month=9, day=18,
           years=0.0767, spot=100.0, rate=0.043, sdiv=0.01, vol=0.30):
    """One expiry, one side, flat vol -- enough for every pure function here."""
    n = len(strikes)
    return pd.DataFrame({
        "okey_tk": ["TEST"] * n,
        "okey_yr": [year] * n, "okey_mn": [month] * n, "okey_dy": [day] * n,
        "okey_xx": list(strikes),
        "okey_cp": [cp] * n,
        "uPrc": [spot] * n, "rate": [rate] * n, "sdiv": [sdiv] * n,
        "years": [years] * n,
        "srVol": [vol] * n, "srPrc": [1.0] * n,
        "bidPrc": [0.9] * n, "askPrc": [1.1] * n,
        "bidSz": [10] * n, "askSz": [10] * n,
    })


def test_forward_uses_the_rows_own_recorded_inputs():
    f = chain.forward(_frame(spot=100.0, rate=0.05, sdiv=0.02, years=0.5))
    assert f == pytest.approx(100.0 * np.exp((0.05 - 0.02) * 0.5))


def test_expiries_reports_one_row_per_listed_expiry():
    both = pd.concat([_frame(month=9), _frame(month=10, years=0.16)], ignore_index=True)
    out = chain.expiries(both)
    assert len(out) == 2
    assert list(out["n_contracts"]) == [3, 3]


def test_year_month_selector_raises_rather_than_picking_between_expiries():
    """A silent pick is how a curve ends up mixing tenors -- it must raise."""
    both = pd.concat([_frame(day=4), _frame(day=18)], ignore_index=True)
    with pytest.raises(KeyError, match="2 expiries"):
        chain.slice_expiry(both, "2026-09")
    # Naming the date exactly still works and selects only that expiry.
    assert len(chain.slice_expiry(both, "2026-09-18")) == 3


def test_year_month_selector_accepts_an_unambiguous_month():
    assert len(chain.slice_expiry(_frame(), "2026-09")) == 3


def test_missing_expiry_raises():
    with pytest.raises(KeyError):
        chain.slice_expiry(_frame(), "2026-12")


def test_atm_vol_skips_the_missing_sentinel():
    """A `-99` reaching a plot as a vol is worse than a gap, so it is masked.

    The strike NEAREST the forward carries the sentinel, so a reference vol that
    came back as -99 would prove the mask never ran. Asymmetric strikes make the
    surviving pick unambiguous rather than an artefact of idxmin's tie order.
    """
    f = _frame(strikes=(95.0, 100.0, 130.0))
    f.loc[1, "srVol"] = chain.MISSING          # nearest the forward, and sentinel
    f.loc[0, "srVol"] = 0.44
    f.loc[2, "srVol"] = 0.55
    fwd = chain.forward(f)                     # ~100.25, so 95 beats 130
    assert chain.atm_vol(f, fwd) == pytest.approx(0.44)


def test_atm_vol_raises_when_nothing_is_fitted():
    f = _frame()
    f["srVol"] = chain.MISSING
    with pytest.raises(ValueError, match="no fitted vol"):
        chain.atm_vol(f)


def test_normalized_strike_is_zero_at_the_forward_and_monotone():
    f = _frame(strikes=(80.0, 100.0, 125.0))
    fwd = chain.forward(f)
    z = chain.normalized_strike(f, fwd=fwd, vol=0.30)
    expected = np.log(np.array([80.0, 100.0, 125.0]) / fwd) / (0.30 * np.sqrt(0.0767))
    assert z == pytest.approx(expected)
    assert np.all(np.diff(z) > 0)          # strike order survives normalisation
    # A strike placed exactly at the forward normalises to 0.
    at_f = _frame(strikes=(fwd,))
    assert chain.normalized_strike(at_f, fwd=fwd, vol=0.30)[0] == pytest.approx(0.0)


def test_normalized_strike_refuses_a_degenerate_scale():
    with pytest.raises(ValueError, match="degenerate"):
        chain.normalized_strike(_frame(), vol=0.0)


def test_within_z_keeps_the_window_and_drops_the_wings():
    f = _frame(strikes=(50.0, 95.0, 100.0, 105.0, 200.0))
    kept = chain.within_z(f, 2.0, fwd=100.0, vol=0.30)
    assert list(kept["okey_xx"]) == [95.0, 100.0, 105.0]


def test_otm_only_keeps_puts_below_the_forward_and_calls_above():
    fwd = 100.0
    calls = _frame(strikes=(90.0, 110.0), cp="C")
    puts = _frame(strikes=(90.0, 110.0), cp="P")
    both = pd.concat([calls, puts], ignore_index=True)
    kept = chain.otm_only(both, fwd=fwd)
    assert sorted(zip(kept["okey_cp"], kept["okey_xx"])) == [("C", 110.0), ("P", 90.0)]
