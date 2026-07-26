"""End-to-end test for ``atxvol.build_surface_db`` (the one-call production build
driver binding) plus a reopen through the ``SurfaceDb`` binding.

The fixture writes a tiny date-partitioned OPRA hive v2 tree with pyarrow,
mirroring the C++ Task-2 synthetic generator (``tests/support/synthetic_opra_hive.hpp``):
2 symbols x 2 dates, 9 strikes 80..120, +28/+56d expiries, put-call-parity C/P
pairs so the loader can imply the spot. Prices are Black-European mids under a
quadratic vol smile, computed at **r = 0** — the rate the fixture's own quotes
embed, so a build at ``r=0.0`` is the one that MATCHES this hive.

REV-R4 (review F-01) corrected what this paragraph used to say. It claimed the
prices were computed at r = 0 "because the build driver has no rate knob"; the
driver always had one (``OpraHiveSpec.r``) and it is the single most important
input to a production build — it was the *binding* that never assigned it, so
every Python build silently ran at 0.0. That is now a keyword argument, and
``test_nonzero_rate_changes_the_fitted_surfaces`` below builds this same fixture
at two different rates and asserts the surfaces come out different. The fixture
stays at r = 0 deliberately: it is the control, and a mismatched rate is exactly
what ``test_failed_cells_carry_the_fitters_own_detail`` induces on purpose.

T7 VERIFICATION NOTE (subprocess-isolated fixture writer). ``atxvol._core``
dynamically links the vcpkg-built ``arrow.dll``/``parquet.dll`` (opra_hive.cpp's
parquet reader), shipped as sibling files next to ``_core.*.pyd``. A `pyarrow`
wheel installed in the same environment bundles its OWN, differently-versioned
copies of the identically-named DLLs. Windows' loader resolves a bare-name DLL
import against whichever same-named module is *already resident* in the
process, regardless of search path or directory — so whichever of
{atxvol._core, pyarrow.lib} imports first wins the process-wide "arrow.dll"
slot and the other fails with "DLL load failed: The specified procedure could
not be found." This is reproducible in both import orders (verified) and is
not a binding or assertion defect. See ``atx-vol/python/README.md`` for the
full user-facing writeup of this limitation. The fixture writer below
therefore shells out to ``_gen_opra_hive.py`` (a separate, real script run as
its own subprocess that only ever imports `pyarrow`, never `atxvol`), keeping
the two incompatible Arrow builds from ever sharing an address space.
"""

from __future__ import annotations

import json
import math
import pathlib
import subprocess
import sys

import pytest

import atxvol

_HIVE_WRITER = pathlib.Path(__file__).parent / "_gen_opra_hive.py"


def _write_hive(root, symbols, dates) -> None:
    # Materialize the synthetic OPRA hive via a child interpreter running
    # _gen_opra_hive.py (see module docstring and atx-vol/python/README.md:
    # pyarrow and atxvol ship incompatible same-named Arrow DLLs, so they must
    # never be imported in the same process).
    payload = json.dumps({"root": str(root), "symbols": list(symbols), "dates": list(dates)})
    subprocess.run(
        [sys.executable, str(_HIVE_WRITER), payload],
        check=True,
    )


def test_build_surface_db_populates_and_reopens(tmp_path) -> None:
    hive_root = tmp_path / "hive"
    db_root = tmp_path / "db"
    symbols = ["AAA", "BBB"]
    dates = ["2026-07-01", "2026-07-02"]
    _write_hive(hive_root, symbols, dates)

    report = atxvol.build_surface_db(
        db_root=str(db_root),
        hive_root=str(hive_root),
        date_lo="2026-07-01",
        date_hi="2026-07-02",
    )

    # Report is a plain nested dict mirroring SurfaceDbBuildReport.
    assert isinstance(report, dict)
    cov = report["coverage"]
    assert cov["cells_loaded"] == 4  # 2 symbols x 2 dates
    assert cov["cells_to_fit"] == 4  # all new this run
    assert cov["cells_ok"] == 4  # every board fit
    assert cov["cells_failed"] == 0
    assert cov["dates_total"] == 2
    assert cov["dates_written"] == 2
    assert report["config"]["n_symbols"] == 2
    assert report["config"]["n_configured"] == 2
    assert report["config"]["n_disabled_failed"] == 0
    assert report["n_dates_loaded"] == 2
    assert report["n_dates_missing"] == 0
    assert report["n_load_errors"] == 0

    # Reopen the built db through the SurfaceDb binding.
    sdb = atxvol.SurfaceDb.open(str(db_root))
    keys = sorted(sdb.partitions())
    assert keys == dates  # one partition per date
    assert set(sdb.symbols()) == set(symbols)

    # Both the owned and zero-copy load paths succeed for a (date, symbol) cell.
    priced = sdb.load_surface(keys[0], "AAA")
    assert priced is not None
    loaded = sdb.map_surface(keys[0], "AAA")
    assert loaded is not None
    # ATM within the fit's expiry span -> a finite implied vol.
    assert math.isfinite(loaded.iv(100.0, 0.10))
    assert math.isfinite(priced.iv(100.0, 0.10))

    # Second build over the unchanged hive re-fits nothing (cell-aware resume).
    report2 = atxvol.build_surface_db(
        db_root=str(db_root),
        hive_root=str(hive_root),
        date_lo="2026-07-01",
        date_hi="2026-07-02",
    )
    assert report2["coverage"]["cells_to_fit"] == 0
    assert report2["coverage"]["cells_already_present"] == 4
    assert report2["coverage"]["dates_written"] == 0
    assert report2["config"]["n_skipped_existing"] == 2

    # REV-R4: the failed-cell list is always present and is EMPTY on a clean
    # build — a consumer never has to guess whether the key exists.
    assert cov["failed_cells"] == []
    assert report2["coverage"]["failed_cells"] == []


# ── REV-R4 (review F-01): the carry rate ────────────────────────────────────
#
# `atxvol.build_surface_db` used to build its `SurfaceDbBuildSpec` without ever
# assigning `spec.hive.r`, so EVERY Python build ran at 0.0 and no caller could
# say otherwise. The production database was built at `--r 0.043`.
#
# The assertion that matters is that the rate reaches the FITTER — a test that
# only checked the call signature, or that the kwarg is accepted, would have
# passed against the broken binding just as well as against the fixed one. So
# these tests compare OUTPUT: the same hive, built twice at two different rates,
# must yield different surfaces.

# ATM-ish sample points, inside the fixture's 28/56-day expiry span.
_PROBES = ((90.0, 0.10), (100.0, 0.10), (110.0, 0.10), (100.0, 0.14))


def _sample(db_root, key, symbol):
    """The surface's own numbers at a few (K, T) — the fit's observable output."""
    sdb = atxvol.SurfaceDb.open(str(db_root))
    surface = sdb.load_surface(key, symbol)
    ivs = [surface.iv(k, t) for k, t in _PROBES]
    assert all(math.isfinite(v) for v in ivs), f"non-finite iv in {ivs}"
    return ivs


def test_nonzero_rate_changes_the_fitted_surfaces(tmp_path) -> None:
    """A build at r != 0 produces DIFFERENT surfaces than the same build at r = 0.

    This is the F-01 regression. The fixture's quotes are priced at r = 0, so
    r = 0 is the matching rate and r = 0.043 (the production database's rate) is
    a mismatch small enough to still fit but large enough to move every
    put-call-parity forward — and therefore every implied vol.
    """
    hive_root = tmp_path / "hive"
    symbols = ["AAA", "BBB"]
    date = "2026-07-01"
    _write_hive(hive_root, symbols, [date])

    def build(db_root, **kwargs):
        report = atxvol.build_surface_db(
            db_root=str(db_root),
            hive_root=str(hive_root),
            date_lo=date,
            date_hi=date,
            **kwargs,
        )
        # Both builds must actually FIT, or "different" would only mean "one of
        # them died" and the test would prove nothing about the fitter.
        assert report["coverage"]["cells_ok"] == len(symbols), report["coverage"]
        assert report["coverage"]["cells_failed"] == 0
        return report

    build(tmp_path / "db_zero", r=0.0)
    build(tmp_path / "db_carry", r=0.043)

    for symbol in symbols:
        zero = _sample(tmp_path / "db_zero", date, symbol)
        carry = _sample(tmp_path / "db_carry", date, symbol)
        # Every probe moves. A tolerance far above fit noise but far below the
        # ~1e-4 shift 4.3% of carry actually produces on this fixture.
        assert all(abs(a - b) > 1e-6 for a, b in zip(zero, carry)), (
            f"{symbol}: r reached neither the forward nor the fit — "
            f"r=0 gave {zero}, r=0.043 gave {carry}"
        )

    # And the DEFAULT is the CLI's default: omitting `r` is exactly r = 0.0, not
    # some silently-safer number. The two tools must not disagree about what an
    # unspecified rate means.
    build(tmp_path / "db_default")
    for symbol in symbols:
        assert _sample(tmp_path / "db_default", date, symbol) == _sample(
            tmp_path / "db_zero", date, symbol
        )


def test_failed_cells_carry_the_fitters_own_detail(tmp_path) -> None:
    """`coverage["failed_cells"]` names every lost cell AND why it was lost.

    The report dict used to carry only the `cells_failed` COUNT, which is the
    point at which a notebook operator has to abandon the binding and re-run the
    CLI to read its `failed_cell` lines. A wildly wrong rate is the cheapest way
    to produce a real, fitter-generated failure on every cell.
    """
    hive_root = tmp_path / "hive"
    db_root = tmp_path / "db"
    symbols = ["AAA", "BBB"]
    dates = ["2026-07-01", "2026-07-02"]
    _write_hive(hive_root, symbols, dates)

    report = atxvol.build_surface_db(
        db_root=str(db_root),
        hive_root=str(hive_root),
        date_lo=dates[0],
        date_hi=dates[-1],
        r=0.5,  # nothing on this hive fits at 50% carry
    )
    cov = report["coverage"]
    assert cov["cells_ok"] == 0
    assert cov["cells_failed"] == 4  # 2 symbols x 2 dates

    failed = cov["failed_cells"]
    # The WHOLE list, never the CLI's --max-failures presentation cap.
    assert len(failed) == cov["cells_failed"]

    for cell in failed:
        assert set(cell) == {"date", "symbol", "code", "detail"}
        assert cell["date"] in dates
        assert cell["symbol"] in symbols
        # The error code as its NAME, the same spelling the CLI prints and the
        # --report CSV's `code` column holds.
        assert isinstance(cell["code"], str) and cell["code"]
        # The load-bearing field: the fitter's own message, not a restatement of
        # the code. Without it a failure is un-diagnosable from Python.
        assert isinstance(cell["detail"], str)
        assert "fit_curve_surface" in cell["detail"], cell

    # The C++ side guarantees (date, symbol) ascending for any fit_workers, and
    # the binding copies that order through rather than re-sorting it.
    assert [(c["date"], c["symbol"]) for c in failed] == sorted(
        (d, s) for d in dates for s in symbols
    )


def test_production_knobs_reach_the_hive_spec(tmp_path) -> None:
    """The rest of the CLI surface F-01 added alongside `r`.

    `snapshot_suffix` and the yield-curve pillars are checked by OUTPUT for the
    same reason `r` is: acceptance proves nothing. The three boolean knobs
    (`retry_disabled`, `pin_curve_family`, `allow_coverage_regression`) have no
    observable effect on a first build over a healthy one-date fixture — they are
    covered by the C++ suite, and asserting "the call did not raise" here would
    be the signature test this whole finding is about.
    """
    hive_root = tmp_path / "hive"
    date = "2026-07-01"
    _write_hive(hive_root, ["AAA"], [date])

    def build(db_root, **kwargs):
        report = atxvol.build_surface_db(
            db_root=str(db_root),
            hive_root=str(hive_root),
            date_lo=date,
            date_hi=date,
            **kwargs,
        )
        assert report["coverage"]["cells_ok"] == 1, report["coverage"]
        return _sample(db_root, date, "AAA")

    # The snapshot stamp sets the observation time, which sets every tenor — a
    # different suffix must move the fit.
    default_stamp = build(tmp_path / "db_snap_default")
    open_stamp = build(tmp_path / "db_snap_open", snapshot_suffix="T00:00:00Z")
    assert all(abs(a - b) > 1e-6 for a, b in zip(default_stamp, open_stamp))

    # Two flat pillars at 4.3% must reproduce the flat r = 0.043 build, and both
    # must differ from the r = 0 build above.
    flat = build(tmp_path / "db_flat", r=0.043)
    curve = build(tmp_path / "db_curve", yc_pillar_t=[0.01, 2.0], yc_pillar_r=[0.043, 0.043])
    assert all(abs(a - b) < 1e-9 for a, b in zip(flat, curve))
    assert all(abs(a - b) > 1e-6 for a, b in zip(default_stamp, flat))

    # A pillar-length mismatch is a malformed hive spec, and the binding lets the
    # library say so rather than pre-validating differently from the CLI.
    with pytest.raises(atxvol.AtxError, match="yc_pillar"):
        atxvol.build_surface_db(
            db_root=str(tmp_path / "db_bad"),
            hive_root=str(hive_root),
            date_lo=date,
            date_hi=date,
            yc_pillar_t=[0.1, 1.0],
            yc_pillar_r=[0.043],
        )
