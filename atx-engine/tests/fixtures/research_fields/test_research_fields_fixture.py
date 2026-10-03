"""The Python side of the research field-builder identity fixture (platform core migration, slice 1).

Re-runs make_research_fields_fixture.py (the EXISTING Python builder on the synthetic inputs) into a temporary
directory: every committed input and every committed expected output must come back byte for byte. The C++ side is
the gtest ResearchFieldsFixture.* (atx-engine-research-fields-tests), which reads the same committed files.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_research_fields_fixture as fx  # noqa: E402

COMMITTED = sorted(p.relative_to(HERE).as_posix() for p in HERE.rglob("*")
                   if p.is_file() and p.parent.name != "__pycache__" and p.suffix != ".py" and p.name != ".gitattributes")


def test_the_fixture_is_the_python_builders_output(tmp_path):
    fx.build(tmp_path)
    produced = sorted(p.relative_to(tmp_path).as_posix() for p in tmp_path.rglob("*") if p.is_file())
    assert produced == COMMITTED
    for rel in COMMITTED:
        assert (tmp_path / rel).read_bytes() == (HERE / rel).read_bytes(), rel


def test_the_fixture_exercises_every_rule():
    m = json.loads((HERE / "expected" / "manifest.normalized.json").read_text(encoding="utf-8"))
    checks = m["source_checks"]
    assert checks["si_shares"]["rows_available_on_or_after_2025_dropped"] == 1      # the seal probe row
    assert checks["si_shares"]["rows_ignored_unknown_id"] == 1
    assert (HERE / "finra" / "asof" / "si_dtc.csv").read_bytes().count(b"\r\n") == len(fx.SI_DTC) + 1
    shape = (fx.N_SESSIONS, len(fx.IDS))
    vol = np.frombuffer((HERE / "expected" / "vol_126.f64").read_bytes(), dtype="<f8").reshape(shape)
    assert np.isnan(vol[:126]).all()                     # t < 126 -> NaN
    assert np.isnan(vol[:, fx.IDS.index(55)]).all()      # never present
    assert np.isfinite(vol[126:, fx.IDS.index(11)]).all()
    si = np.frombuffer((HERE / "expected" / "si_shares.f64").read_bytes(), dtype="<f8").reshape(shape)
    assert np.isnan(si[:, fx.IDS.index(55)]).all() and np.isfinite(si).any()
