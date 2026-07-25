"""End-to-end test for ``atxvol.build_surface_db`` (the one-call production build
driver binding) plus a reopen through the ``SurfaceDb`` binding.

The fixture writes a tiny date-partitioned OPRA hive v2 tree with pyarrow,
mirroring the C++ Task-2 synthetic generator (``tests/support/synthetic_opra_hive.hpp``):
2 symbols x 2 dates, 9 strikes 80..120, +28/+56d expiries, put-call-parity C/P
pairs so the loader can imply the spot. Prices are Black-European mids under a
quadratic vol smile, computed at r = 0 because the build driver has no rate knob
(``OpraHiveSpec.r`` defaults to 0.0) — a nonzero-r fixture would break the fits.

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
