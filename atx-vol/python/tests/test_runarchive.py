"""Minimal gates for the pure-Python RunArchive (ATXRUN01) reader.

This reader has no C++ counterpart — it is an independent implementation of the
container format — so a core of coverage stays here rather than moving to gtest.
It is a core, not a matrix: reading, the typed accessors, integrity detection,
the two defects that were actually fixed (a leaked file handle on close, a raw
UnicodeDecodeError escaping), and the two properties that keep it honest against
the C++ side (the schema hash, and codegen drift versus the C++ header).

Dropped from the earlier comprehensive version: the forged-descriptor matrix
(section magic, out-of-range dict codes, non-monotone aux offsets), the
major/minor version gates, and a hand-built u8enum table. They exercised
branches reachable only by deliberately corrupting bytes in ways the writer
cannot produce, and were most of the file's length.

Reads the committed C++-written fixture `tests/data/runarchive/
wave_a_fixture.atxrun` (a `backtest` series with an appended `atm_iv` signal,
plus a `meta` key/value table). The reader imports only the standard library +
numpy + the generated `_schema` — never the compiled `atxvol._core` — proved by
`test_standalone_import_needs_no_compiled_extension`.
"""

from __future__ import annotations

import gc
import pathlib
import subprocess
import sys

import pytest

from atxvol.report import _schema
from atxvol.report import runarchive as ra

FIXTURE = pathlib.Path(__file__).resolve().parent / "data" / "runarchive" / "wave_a_fixture.atxrun"
GOLDEN_SCHEMA_HASH = 0xDCCE47781AC8390D

# Read once at import: the corruption cases each want their own mutable copy and
# the from_bytes cases want the immutable original.
_FIXTURE_BYTES = FIXTURE.read_bytes()


def _fixture_bytes() -> bytearray:
    return bytearray(_FIXTURE_BYTES)


@pytest.fixture(scope="module")
def archive():
    ar = ra.RunArchive.open(str(FIXTURE))
    yield ar
    ar.close()


@pytest.fixture(scope="module")
def backtest(archive):
    return archive.section("backtest")


# ── framing + typed accessors ────────────────────────────────────────────────

def test_header_and_sections(archive):
    h = archive.header
    assert h.magic == b"ATXRUN01"
    assert h.major == 1 and h.minor == 0
    assert h.endian == 1 and h.pointer_bits == 64
    assert h.created_ts_ns == 123456789
    assert h.run_identity_hash == 0xABCDEF
    assert h.schema_hash == GOLDEN_SCHEMA_HASH
    assert archive.sections == ["backtest", "meta"]  # name-sorted


def test_typed_columns_and_zero_copy(backtest):
    assert backtest.n_rows == 2
    assert backtest.n_cols == 28
    assert list(backtest.i64("ts_ns")) == [10, 20]
    assert list(backtest.f64("pnl_vega")) == [1.25, -0.5]
    assert backtest.dict("date").tolist() == ["2026-07-11", "2026-07-12"]
    # The dynamically-appended per-signal column reads like any other.
    assert list(backtest.f64("atm_iv")) == [0.20, 0.21]

    nav = backtest.f64("nav")
    assert nav.dtype.name == "float64"
    assert nav.base is not None  # zero-copy: views the mmap, does not own its data


def test_meta_section_matched_by_key_name(archive):
    meta = archive.section("meta")
    by_key = {meta.dict("key").at(i): meta.dict("value").at(i) for i in range(meta.n_rows)}
    assert by_key["label"] == "wave-a-fixture"
    assert by_key["date_lo"] == "2026-07-11"


@pytest.mark.parametrize("accessor,name", [
    ("section", "does_not_exist"),   # missing section
    ("f64", "no_such_column"),       # missing column
    ("f64", "date"),                 # `date` is dict-str, not f64
])
def test_lookup_misses_raise_keyerror(archive, backtest, accessor, name):
    target = archive if accessor == "section" else backtest
    with pytest.raises(KeyError):
        getattr(target, accessor)(name)


# ── integrity ────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("mutate,label", [
    (lambda d: d.__setitem__(24, d[24] ^ 1), "schema hash drift"),
    (lambda d: d.__setitem__(0, ord("Z")), "bad magic"),
    # created_ts_ns is covered by the header CRC but not the schema hash.
    (lambda d: d.__setitem__(16, d[16] ^ 0xFF), "header CRC"),
])
def test_framing_corruption_is_rejected_at_open(tmp_path, mutate, label):
    data = _fixture_bytes()
    mutate(data)
    q = tmp_path / "corrupt.atxrun"
    q.write_bytes(data)
    with pytest.raises(ValueError):
        ra.RunArchive.open(str(q))
        pytest.fail(f"{label} was accepted")


def test_truncated_file_raises(tmp_path):
    q = tmp_path / "short.atxrun"
    q.write_bytes(_FIXTURE_BYTES[:128])  # shorter than the 256-B header
    with pytest.raises(ValueError):
        ra.RunArchive.open(str(q))


def test_validate_all_passes_on_clean_fixture(archive):
    archive.validate_all()  # must not raise


def test_payload_crc_corruption_is_lazy_but_detected(tmp_path):
    # A byte well inside the backtest payload is covered by neither the header nor
    # the metadata CRC, so open() still succeeds and only validation catches it.
    data = _fixture_bytes()
    data[448 + 200] ^= 0xFF
    q = tmp_path / "payload.atxrun"
    q.write_bytes(data)
    ar = ra.RunArchive.open(str(q))  # framing intact -> opens
    with pytest.raises(ValueError):
        ar.validate_section("backtest")


# ── defect regressions (both fixed RED-first; keep the anchors) ──────────────

def test_forged_non_utf8_string_table_raises_documented_valueerror(tmp_path):
    # A documented ValueError must surface, NOT a raw UnicodeDecodeError.
    data = _fixture_bytes()
    header = ra.Header(*ra._HEADER_FMT.unpack_from(data, 0)[:17])
    blob_abs = None
    for i in range(header.section_count):
        d = ra._SECDESC_FMT.unpack_from(data, header.section_dir_offset + i * ra._SECDESC_FMT.size)
        if d[7].split(b"\x00", 1)[0].decode() != "backtest":
            continue
        base, cdo, ncols = d[0], d[4], d[3]
        for c in range(ncols):
            cr = ra._COLDESC_FMT.unpack_from(data, base + cdo + c * ra._COLDESC_FMT.size)
            if cr[8][: cr[7]].decode() == "date":
                blob_abs = base + cr[2] + 4 * (cr[4] + 1)
    assert blob_abs is not None, "date column not found in fixture"

    data[blob_abs] = 0xFF  # invalid utf-8 lead byte in the first table string
    q = tmp_path / "nonutf8.atxrun"
    q.write_bytes(data)
    sec = ra.RunArchive.open(str(q)).section("backtest")  # framing intact
    with pytest.raises(ValueError) as ei:
        sec.dict("date")  # decode happens here
    assert not isinstance(ei.value, UnicodeDecodeError)
    assert "utf-8" in str(ei.value).lower()


def test_close_releases_file_handle_even_with_live_view(tmp_path):
    # mmap.close() raises BufferError while numpy views are exported; the OS file
    # handle must still be released, and the mapping retained so the view resolves.
    q = tmp_path / "live.atxrun"
    q.write_bytes(_fixture_bytes())
    ar = ra.RunArchive.open(str(q))
    nav = ar.section("backtest").f64("nav")
    fh = ar._fh
    ar.close()
    assert ar._fh is None, "file handle leaked on BufferError path"
    assert fh.closed, "underlying OS file handle left open"
    assert list(nav[:2]) == [100.0, 101.5]

    del nav
    gc.collect()
    ar.close()  # second close releases the retained mapping and must not raise
    assert ar._mm is None


# ── high-level shim + in-memory backing ─────────────────────────────────────

def test_read_backtest_section_shim(archive):
    result, meta, extra = ra.read_backtest_section(archive)
    assert result.size() == 2
    assert result.date == ["2026-07-11", "2026-07-12"]
    assert list(result.nav) == [100.0, 101.5]
    assert meta["label"] == "wave-a-fixture"
    # A dynamically-appended signal is not a registry column -> extra.
    assert list(extra["atm_iv"]) == [0.20, 0.21]
    assert "nav" not in extra


def test_from_bytes_decodes_strings_and_keeps_views_readonly():
    # Regression: via from_bytes the backing is a memoryview whose slices are
    # memoryviews (no .decode), so every string column used to raise AttributeError.
    ar = ra.RunArchive.from_bytes(_FIXTURE_BYTES)
    sec = ar.section("backtest")
    assert sec.dict("date").tolist() == ["2026-07-11", "2026-07-12"]

    nav = sec.f64("nav")
    assert nav.base is not None            # still zero-copy
    assert nav.flags.writeable is False    # read-only view over immutable bytes
    with pytest.raises(ValueError):
        nav[0] = 1.0


# ── staying honest against the C++ side ─────────────────────────────────────

def test_generated_schema_hash_matches_golden():
    # The codegen must fold to the value C++ ra_schema_hash() pins, or every
    # archive would be rejected at open.
    assert _schema.ra_schema_hash() == GOLDEN_SCHEMA_HASH
    assert _schema.RA_SCHEMA_HASH == GOLDEN_SCHEMA_HASH


def test_schema_py_not_stale_vs_cpp_header():
    """Catch C++ registry drift: re-parse `run_archive_schema.hpp` via the
    generator's `--check`. The golden-hash test above only pins `_schema.py` to a
    constant — an unmirrored header change would stay green without this."""
    atx_vol = pathlib.Path(__file__).resolve().parents[2]
    header = atx_vol / "include" / "atx" / "vol" / "run_archive_schema.hpp"
    generator = atx_vol / "tools" / "gen_runarchive_schema.py"
    if not header.exists() or not generator.exists():
        pytest.skip(f"header/generator not present (sdist install?): {header}")
    proc = subprocess.run([sys.executable, str(generator), "--check"],
                          capture_output=True, text=True)
    assert proc.returncode == 0, (
        "_schema.py is stale vs run_archive_schema.hpp (rerun "
        f"gen_runarchive_schema.py):\nstdout: {proc.stdout}\nstderr: {proc.stderr}")


def test_standalone_import_needs_no_compiled_extension():
    """The reader must load and run in a fresh interpreter that never imports the
    atxvol package (hence never the compiled _core extension)."""
    report_dir = pathlib.Path(ra.__file__).resolve().parent
    script = (
        "import sys, importlib.util\n"
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
