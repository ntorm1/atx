"""Tests for the pure-Python RunArchive (ATXRUN01) reader.

Reads the committed C++-written fixture
``tests/data/runarchive/wave_a_fixture.atxrun`` (two sections: a ``backtest``
time series with an appended ``atm_iv`` signal, and a ``meta`` key/value table).
The reader under test imports only the standard library + numpy + the generated
``_schema`` module — never the compiled ``atxvol._core`` extension — so it works
whether or not the binding is built (proved explicitly by
``test_standalone_import_needs_no_compiled_extension``).
"""

from __future__ import annotations

import pathlib
import struct
import subprocess
import sys

import pytest

from atxvol.report import _schema
from atxvol.report import runarchive as ra

FIXTURE = pathlib.Path(__file__).resolve().parent / "data" / "runarchive" / "wave_a_fixture.atxrun"
GOLDEN_SCHEMA_HASH = 0xDCCE47781AC8390D


# ── open / header framing ────────────────────────────────────────────────────

def test_open_and_read():
    ar = ra.RunArchive.open(str(FIXTURE))
    nav = ar.section("backtest").f64("nav")
    assert list(nav[:2]) == [100.0, 101.5]


def test_header_fields():
    ar = ra.RunArchive.open(str(FIXTURE))
    h = ar.header
    assert h.magic == b"ATXRUN01"
    assert h.major == 1 and h.minor == 0
    assert h.endian == 1 and h.pointer_bits == 64
    assert h.created_ts_ns == 123456789
    assert h.run_identity_hash == 0xABCDEF
    assert h.section_count == 2
    assert h.schema_hash == GOLDEN_SCHEMA_HASH


def test_sections_are_name_sorted():
    ar = ra.RunArchive.open(str(FIXTURE))
    assert ar.sections == ["backtest", "meta"]


# ── typed column accessors ───────────────────────────────────────────────────

def test_i64_and_f64_columns():
    sec = ra.RunArchive.open(str(FIXTURE)).section("backtest")
    assert sec.n_rows == 2
    assert sec.n_cols == 28
    assert list(sec.i64("ts_ns")) == [10, 20]
    assert list(sec.f64("pnl_vega")) == [1.25, -0.5]


def test_dict_date_column():
    sec = ra.RunArchive.open(str(FIXTURE)).section("backtest")
    dates = sec.dict("date")
    assert len(dates) == 2
    assert dates.at(0) == "2026-07-11"
    assert dates.at(1) == "2026-07-12"
    assert dates.tolist() == ["2026-07-11", "2026-07-12"]


def test_appended_signal_column():
    sec = ra.RunArchive.open(str(FIXTURE)).section("backtest")
    assert list(sec.f64("atm_iv")) == [0.20, 0.21]


def test_meta_section_matched_by_key_name():
    ar = ra.RunArchive.open(str(FIXTURE))
    meta = ar.section("meta")
    keys = meta.dict("key")
    values = meta.dict("value")
    by_key = {keys.at(i): values.at(i) for i in range(meta.n_rows)}
    assert by_key["label"] == "wave-a-fixture"
    assert by_key["date_lo"] == "2026-07-11"
    assert by_key["date_hi"] == "2026-07-12"


def test_zero_copy_view_is_numpy_over_backing():
    sec = ra.RunArchive.open(str(FIXTURE)).section("backtest")
    nav = sec.f64("nav")
    assert nav.dtype.name == "float64"
    # Zero-copy: the array does not own its data (it views the mmap/backing).
    assert nav.base is not None


def test_missing_section_raises_keyerror():
    ar = ra.RunArchive.open(str(FIXTURE))
    with pytest.raises(KeyError):
        ar.section("does_not_exist")


def test_missing_column_raises_keyerror():
    sec = ra.RunArchive.open(str(FIXTURE)).section("backtest")
    with pytest.raises(KeyError):
        sec.f64("no_such_column")


def test_wrong_dtype_accessor_raises_keyerror():
    sec = ra.RunArchive.open(str(FIXTURE)).section("backtest")
    # `date` is a dict-str column, not f64.
    with pytest.raises(KeyError):
        sec.f64("date")


# ── integrity: schema drift, magic, CRCs ─────────────────────────────────────

def test_schema_drift_raises(tmp_path):
    data = bytearray(FIXTURE.read_bytes())
    data[24] ^= 1  # flip a schema_hash byte
    q = tmp_path / "drift.atxrun"
    q.write_bytes(data)
    with pytest.raises(ValueError):
        ra.RunArchive.open(str(q))


def test_bad_magic_raises(tmp_path):
    data = bytearray(FIXTURE.read_bytes())
    data[0] = ord("Z")
    q = tmp_path / "badmagic.atxrun"
    q.write_bytes(data)
    with pytest.raises(ValueError):
        ra.RunArchive.open(str(q))


def test_header_crc_corruption_raises(tmp_path):
    data = bytearray(FIXTURE.read_bytes())
    data[16] ^= 0xFF  # flip a created_ts_ns byte (covered by header CRC, not schema)
    q = tmp_path / "hdrcrc.atxrun"
    q.write_bytes(data)
    with pytest.raises(ValueError):
        ra.RunArchive.open(str(q))


def test_truncated_file_raises(tmp_path):
    data = FIXTURE.read_bytes()[:128]  # shorter than the 256-B header
    q = tmp_path / "short.atxrun"
    q.write_bytes(data)
    with pytest.raises(ValueError):
        ra.RunArchive.open(str(q))


def test_validate_all_passes_on_clean_fixture():
    ar = ra.RunArchive.open(str(FIXTURE))
    ar.validate_all()  # must not raise
    ar.validate_section("backtest")
    ar.validate_section("meta")


def test_payload_crc_corruption_is_lazy_but_detected(tmp_path):
    data = bytearray(FIXTURE.read_bytes())
    # Flip a byte well inside the backtest section payload (offset 448, size 3224):
    # not covered by the header or metadata CRC, so open() still succeeds, but the
    # per-section payload CRC no longer matches.
    data[448 + 200] ^= 0xFF
    q = tmp_path / "payload.atxrun"
    q.write_bytes(data)
    ar = ra.RunArchive.open(str(q))  # framing intact -> opens
    with pytest.raises(ValueError):
        ar.validate_section("backtest")
    with pytest.raises(ValueError):
        ar.validate_all()


# ── high-level shim ──────────────────────────────────────────────────────────

def test_read_backtest_section_shim():
    ar = ra.RunArchive.open(str(FIXTURE))
    result, meta, extra = ra.read_backtest_section(ar)
    assert result.size() == 2
    assert result.date == ["2026-07-11", "2026-07-12"]
    assert list(result.ts_ns) == [10, 20]
    assert list(result.nav) == [100.0, 101.5]
    assert list(result.pnl_vega) == [1.25, -0.5]
    # meta echoed as a {key: value} dict, matched by key name.
    assert meta["label"] == "wave-a-fixture"
    assert meta["date_lo"] == "2026-07-11"
    assert meta["date_hi"] == "2026-07-12"
    # the dynamically-appended signal column is not a registry column -> extra.
    assert "atm_iv" in extra
    assert list(extra["atm_iv"]) == [0.20, 0.21]
    assert "nav" not in extra  # registry column, lives on the result


# ── from_bytes (in-memory backing, not an mmap) ──────────────────────────────

def test_from_bytes_decodes_string_columns_and_shim():
    """Regression: via ``from_bytes`` the backing is a memoryview whose slices
    are memoryviews (no ``.decode``), so every string column and the backtest
    shim used to raise ``AttributeError``. Exercise the fixture bytes end-to-end."""
    ar = ra.RunArchive.from_bytes(FIXTURE.read_bytes())

    # DictStr decode over the memoryview backing (the broken path).
    sec = ar.section("backtest")
    dates = sec.dict("date")
    assert dates.tolist() == ["2026-07-11", "2026-07-12"]
    assert dates.at(1) == "2026-07-12"

    # meta key/value dict columns decode too.
    meta_sec = ar.section("meta")
    by_key = {meta_sec.dict("key").at(i): meta_sec.dict("value").at(i)
              for i in range(meta_sec.n_rows)}
    assert by_key["label"] == "wave-a-fixture"

    # The high-level shim (which calls dict() internally) works via from_bytes.
    result, meta, extra = ra.read_backtest_section(ar)
    assert result.date == ["2026-07-11", "2026-07-12"]
    assert list(result.nav) == [100.0, 101.5]
    assert meta["label"] == "wave-a-fixture"
    assert list(extra["atm_iv"]) == [0.20, 0.21]


def test_from_bytes_numeric_views_stay_zero_copy_and_readonly():
    """The from_bytes fix must not turn numeric columns into owning/writable
    arrays: they must remain zero-copy views that are read-only (the memoryview
    is over immutable bytes)."""
    sec = ra.RunArchive.from_bytes(FIXTURE.read_bytes()).section("backtest")
    nav = sec.f64("nav")
    assert nav.base is not None            # zero-copy: does not own its data
    assert nav.flags.writeable is False    # read-only view over the backing
    with pytest.raises(ValueError):
        nav[0] = 1.0


def test_u8enum_decode_over_memoryview_backing():
    """The fixture has no U8Enum column, but ``.u8enum()`` shares the fixed
    ``_string_table`` decode path with ``.dict()``. Drive that path directly over
    a memoryview backing (the from_bytes shape) with a hand-built aux table."""
    labels = ["ALPHA", "BB", "c"]
    codes = [0, 2, 1, 0]
    offsets = [0]
    for lab in labels:
        offsets.append(offsets[-1] + len(lab))
    blob = b"".join(lab.encode("ascii") for lab in labels)
    code_bytes = bytes(codes)
    aux_bytes = struct.pack(f"<{len(offsets)}I", *offsets) + blob
    buf = memoryview(code_bytes + aux_bytes)  # memoryview == the from_bytes backing

    cd = ra._ColumnDescriptor(
        data_offset=0, data_size=len(code_bytes), aux_offset=len(code_bytes),
        aux_size=len(aux_bytes), aux_count=len(labels), dtype=ra._U8ENUM,
        name_len=1, name="e",
    )
    sec = ra.Section(buf, base=0, name="t", kind=1, n_rows=len(codes),
                     cols={"e": cd})
    col = sec.u8enum("e")
    assert col.labels == labels
    assert col.tolist() == ["ALPHA", "c", "BB", "ALPHA"]


# ── generated schema descriptor ──────────────────────────────────────────────

def test_generated_schema_hash_matches_golden():
    # Guards the _schema.py codegen: it must fold to the same value the C++
    # ra_schema_hash() pins, or every archive would be rejected at open.
    assert _schema.ra_schema_hash() == GOLDEN_SCHEMA_HASH
    assert _schema.RA_SCHEMA_HASH == GOLDEN_SCHEMA_HASH


def test_generated_schema_registry_shape():
    names = [name for name, _kind, _cols in _schema.SECTIONS]
    assert names[:2] == ["meta", "backtest"]
    assert len(_schema.SECTIONS) == 10


def test_schema_py_not_stale_vs_cpp_header():
    """Catch C++ registry drift: re-parse ``run_archive_schema.hpp`` via the
    generator's ``--check`` and fail if the committed ``_schema.py`` no longer
    matches the header. ``test_generated_schema_hash_matches_golden`` only pins
    ``_schema.py`` to a constant — an unmirrored header change would stay green
    without this. Skip only if the header/generator are genuinely absent (e.g. a
    sdist install); in this repo they are present and this MUST run."""
    atx_vol = pathlib.Path(__file__).resolve().parents[2]  # .../atx-vol
    header = atx_vol / "include" / "atx" / "vol" / "run_archive_schema.hpp"
    generator = atx_vol / "tools" / "gen_runarchive_schema.py"
    if not header.exists() or not generator.exists():
        pytest.skip(f"header/generator not present (sdist install?): {header}")
    proc = subprocess.run(
        [sys.executable, str(generator), "--check"],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, (
        "_schema.py is stale vs run_archive_schema.hpp (rerun "
        f"gen_runarchive_schema.py):\nstdout: {proc.stdout}\nstderr: {proc.stderr}"
    )


# ── standalone import (no compiled binding) ──────────────────────────────────

def test_standalone_import_needs_no_compiled_extension():
    """The reader must load and run in a fresh interpreter that never imports
    the atxvol package (hence never the compiled _core extension)."""
    report_dir = pathlib.Path(ra.__file__).resolve().parent
    script = (
        "import sys, pathlib, importlib.util\n"
        f"rd = r'{report_dir}'\n"
        "sys.path.insert(0, rd)\n"
        "import _schema\n"
        "spec = importlib.util.spec_from_file_location('ra_standalone', rd + '/runarchive.py')\n"
        "m = importlib.util.module_from_spec(spec)\n"
        "sys.modules['ra_standalone'] = m  # dataclasses need the module registered\n"
        "spec.loader.exec_module(m)\n"
        "assert 'atxvol' not in sys.modules, 'imported atxvol package'\n"
        f"ar = m.RunArchive.open(r'{FIXTURE}')\n"
        "assert list(ar.section('backtest').f64('nav')) == [100.0, 101.5]\n"
        "print('OK')\n"
    )
    proc = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert "OK" in proc.stdout
