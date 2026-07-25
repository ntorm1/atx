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
    matches the header.

    FIX-5/I5 — this test used to take an unconditional ``pytest.skip`` on this
    tree, against its own docstring ("in this repo they are present and this
    MUST run"), because neither the header nor the generator is tracked here:
    the ATXRUN01 C++ writer is out-of-tree. A guard that always skips is worse
    than no guard, because it reads as coverage.

    It now asserts in BOTH worlds rather than skipping in one:

    * header + generator present  -> run ``--check`` exactly as before, so the
      day the C++ side lands in this repo the guard starts working with no edit;
    * header + generator absent   -> assert ``_schema.py`` does not ADVERTISE a
      provenance it does not have. The silent skip was only possible because the
      module claimed to be generated from files that do not exist; forbidding
      that claim is the property that actually holds on this tree, and it is
      what stops the misleading header from coming back.
    """
    atx_vol = pathlib.Path(__file__).resolve().parents[2]  # .../atx-vol
    header = atx_vol / "include" / "atx" / "vol" / "run_archive_schema.hpp"
    generator = atx_vol / "tools" / "gen_runarchive_schema.py"
    if header.exists() and generator.exists():
        proc = subprocess.run(
            [sys.executable, str(generator), "--check"],
            capture_output=True, text=True,
        )
        assert proc.returncode == 0, (
            "_schema.py is stale vs run_archive_schema.hpp (rerun "
            f"gen_runarchive_schema.py):\nstdout: {proc.stdout}\nstderr: {proc.stderr}"
        )
        return

    # The checks are LINE-ANCHORED on the declaration FORM, not on a substring
    # search: the corrected docstring quotes the old claim in order to explain that
    # it was wrong, and a bare `"GENERATED, do not edit by hand" not in doc` would
    # trip on that quotation. What must not come back is the module *declaring*
    # itself generated — the header line ending in that phrase, and the
    # ``Source of truth:`` / ``Regenerate:`` fields naming files that do not exist.
    doc = (_schema.__doc__ or "")
    lines = [ln.strip() for ln in doc.splitlines()]
    assert not any(ln.endswith("GENERATED, do not edit by hand.") for ln in lines), (
        f"_schema.py declares itself generated, but {header} is absent — the "
        "declared source of truth does not exist in this repo. Either land the "
        "header and generator, or state the real provenance."
    )
    assert not any(ln.startswith("Source of truth:") for ln in lines), (
        f"_schema.py names a source of truth, but {header} is absent."
    )
    assert not any(ln.startswith("Regenerate:") for ln in lines), (
        f"_schema.py advertises a regeneration path, but {generator} is absent."
    )
    # And the provenance it DOES state must be the honest one: the module has to
    # say, in words, that the C++ writer's header is not in this repository.
    flat = " ".join(doc.split())
    assert "Neither file exists in this repository" in flat, (
        "_schema.py must record that its C++ source of truth is out-of-tree"
    )


def test_gross_vega_unit_collision_is_documented():
    """FIX-5/I5 — ``gross_vega`` carries the SAME name over two units 100x apart
    plus a gross/net flip, and the registry's unit annotation is blank exactly
    where it matters:

      trade_schedule / projected_schedule -> 'usd_per_volpt', produced by
        listed_dispersion_schedule.cpp:310,360 (sum of |achieved leg vega| per
        VOL POINT — genuinely gross);
      backtest / projected_cold / projected_nodiv -> '', produced by
        backtest.cpp:1603,1862 (`out.gross_vega.push_back(g.vega)` — the pricer's
        dP/dsigma per UNIT vol, and per 2a7321c the NET book figure).

    The units cannot be corrected in ``SECTIONS`` without moving
    ``RA_SCHEMA_HASH`` — ``ra_schema_hash`` folds the unit string of every
    column, so relabelling would reject every existing ``.atxrun`` at open. The
    semantics therefore live in the non-hashed ``COLUMN_NOTES``, and this test
    pins that they are recorded for every blank-unit ``gross_vega`` entry AND
    that recording them did not perturb the format identity."""
    blank = []
    labelled = []
    for section, _kind, cols in _schema.SECTIONS:
        for cname, _dtype, unit in cols:
            if cname != "gross_vega":
                continue
            (blank if unit == "" else labelled).append((section, unit))

    # The collision is real and still present (this test is not vacuous).
    assert [s for s, _ in blank] == ["backtest", "projected_cold", "projected_nodiv"]
    assert labelled == [
        ("trade_schedule", "usd_per_volpt"),
        ("projected_schedule", "usd_per_volpt"),
    ]

    # Every blank one is documented, and says both the unit and the gross/net flip.
    for section, _unit in blank:
        note = _schema.column_note(section, "gross_vega")
        assert "usd_per_unitvol" in note, section
        assert "NET" in note, section
    # The labelled ones are documented as gross, so the two are distinguishable.
    for section, _unit in labelled:
        note = _schema.column_note(section, "gross_vega")
        assert "usd_per_volpt" in note and "GROSS" in note, section

    # COLUMN_NOTES is documentation, NOT format identity: adding it must not have
    # moved the hash the reader pins archives against.
    assert _schema.RA_SCHEMA_HASH == GOLDEN_SCHEMA_HASH


# ── reader hardening: forged descriptors, version gate, close(), utf-8 ───────
#
# These byte-patch a *temp-dir copy* of the committed fixture (never the
# original) to prove the reader's documented failure taxonomy: forged/corrupt
# descriptors and a non-utf8 string table raise a documented ``ValueError``
# (not a raw ``UnicodeDecodeError`` or an unhandled crash), a version bump is
# rejected at ``open()``, and ``close()`` never leaks the OS file handle when
# numpy views are still exported over the mmap (``mmap.close()`` -> BufferError).

# Header field byte offsets (fmt "<8sQQQQQQQIIIIHHHHHH164s").
_HDR_CRC_OFF = 68
_HDR_MAJOR_OFF = 80
_HDR_MINOR_OFF = 82


def _fixture_bytes() -> bytearray:
    return bytearray(FIXTURE.read_bytes())


def _recompute_header_crc(data: bytearray) -> None:
    """Restamp the header CRC (own field zeroed) after patching a header field,
    so a version/field test exercises the intended gate rather than tripping the
    header CRC check first."""
    head = bytearray(data[: ra._HEADER_FMT.size])
    head[_HDR_CRC_OFF:_HDR_CRC_OFF + 4] = b"\x00\x00\x00\x00"
    struct.pack_into("<I", data, _HDR_CRC_OFF, ra._crc32c(head))


def _section_base(data: bytearray, section_name: str) -> int:
    h = ra.Header(*ra._HEADER_FMT.unpack_from(data, 0)[:17])
    for i in range(h.section_count):
        d = ra._SECDESC_FMT.unpack_from(
            data, h.section_dir_offset + i * ra._SECDESC_FMT.size)
        if d[7].split(b"\x00", 1)[0].decode() == section_name:
            return d[0]
    raise AssertionError(f"section {section_name!r} not in fixture")


def _dict_column_offsets(data: bytearray, section_name: str, col_name: str):
    """Absolute (data, aux, aux_count, blob) offsets of a dict/enum column."""
    h = ra.Header(*ra._HEADER_FMT.unpack_from(data, 0)[:17])
    for i in range(h.section_count):
        d = ra._SECDESC_FMT.unpack_from(
            data, h.section_dir_offset + i * ra._SECDESC_FMT.size)
        if d[7].split(b"\x00", 1)[0].decode() != section_name:
            continue
        base, cdo, ncols = d[0], d[4], d[3]
        for c in range(ncols):
            cr = ra._COLDESC_FMT.unpack_from(
                data, base + cdo + c * ra._COLDESC_FMT.size)
            if cr[8][: cr[7]].decode() == col_name:
                data_off, aux_off, aux_count = cr[0], cr[2], cr[4]
                return (base + data_off, base + aux_off, aux_count,
                        base + aux_off + 4 * (aux_count + 1))
    raise AssertionError(f"column {col_name!r} not in section {section_name!r}")


def _write(tmp_path, name: str, data: bytearray) -> str:
    q = tmp_path / name
    q.write_bytes(data)
    return str(q)


# #10 — negative section()-framing: forged descriptors raise documented ValueError.

def test_forged_section_magic_raises_valueerror(tmp_path):
    # The section magic lives in the section payload (not covered by the header
    # or metadata CRC), so open() still succeeds; section() must reject it.
    data = _fixture_bytes()
    data[_section_base(data, "backtest")] = ord("X")  # ATXRSC01 -> XTXRSC01
    ar = ra.RunArchive.open(_write(tmp_path, "badsecmagic.atxrun", data))
    with pytest.raises(ValueError, match="bad section magic"):
        ar.section("backtest")


def test_forged_dict_code_out_of_range_raises_valueerror(tmp_path):
    data = _fixture_bytes()
    data_abs, _aux_abs, aux_count, _blob_abs = _dict_column_offsets(
        data, "backtest", "date")
    struct.pack_into("<I", data, data_abs, aux_count)  # code == aux_count -> OOR
    ar = ra.RunArchive.open(_write(tmp_path, "codeoob.atxrun", data))
    with pytest.raises(ValueError, match="code out of range"):
        ar.section("backtest")


def test_forged_nonmonotone_aux_offsets_raises_valueerror(tmp_path):
    data = _fixture_bytes()
    _data_abs, aux_abs, _aux_count, _blob_abs = _dict_column_offsets(
        data, "backtest", "date")
    struct.pack_into("<I", data, aux_abs + 4, 0xFFFF)  # offsets[1] > offsets[2]
    ar = ra.RunArchive.open(_write(tmp_path, "nonmono.atxrun", data))
    with pytest.raises(ValueError, match="not monotone"):
        ar.section("backtest")


# #11 — version-mismatch: a bumped major/minor is rejected at open().

def test_version_major_mismatch_raises_valueerror(tmp_path):
    data = _fixture_bytes()
    struct.pack_into("<H", data, _HDR_MAJOR_OFF, 2)  # major 1 -> 2
    struct.pack_into("<H", data, _HDR_MINOR_OFF, 1)  # minor 0 -> 1
    _recompute_header_crc(data)  # isolate the version gate from the CRC gate
    with pytest.raises(ValueError, match="unsupported major version"):
        ra.RunArchive.open(_write(tmp_path, "major2.atxrun", data))


def test_version_minor_too_new_raises_valueerror(tmp_path):
    data = _fixture_bytes()
    struct.pack_into("<H", data, _HDR_MINOR_OFF, _schema.RA_MINOR + 1)
    _recompute_header_crc(data)
    with pytest.raises(ValueError, match="unsupported minor version"):
        ra.RunArchive.open(_write(tmp_path, "minornew.atxrun", data))


# #13 — forged non-utf8 string table: documented ValueError, not raw UnicodeDecodeError.

def test_forged_non_utf8_string_table_raises_documented_valueerror(tmp_path):
    data = _fixture_bytes()
    _data_abs, _aux_abs, _aux_count, blob_abs = _dict_column_offsets(
        data, "backtest", "date")
    data[blob_abs] = 0xFF  # invalid utf-8 lead byte in the first table string
    ar = ra.RunArchive.open(_write(tmp_path, "nonutf8.atxrun", data))
    sec = ar.section("backtest")  # framing (offsets/bounds) intact -> section() ok
    with pytest.raises(ValueError) as ei:
        sec.dict("date")  # decode happens here (in _string_table)
    # A documented ValueError must surface, NOT a raw UnicodeDecodeError bubbling up.
    assert not isinstance(ei.value, UnicodeDecodeError)
    assert "utf-8" in str(ei.value).lower()


# #12 — close() must not leak the file handle on BufferError (live numpy views).

def test_close_releases_file_handle_even_with_live_view(tmp_path):
    ar = ra.RunArchive.open(_write(tmp_path, "live.atxrun", _fixture_bytes()))
    nav = ar.section("backtest").f64("nav")  # exported view keeps the mmap open
    fh = ar._fh
    ar.close()  # mmap.close() raises BufferError internally; handle must still close
    assert ar._fh is None, "file handle leaked on BufferError path"
    assert fh.closed, "underlying OS file handle left open"
    # The mapping is retained so the live view still resolves correctly.
    assert list(nav[:2]) == [100.0, 101.5]
    # Dropping the view lets a later close() release the retained mapping.
    del nav
    import gc
    gc.collect()
    ar.close()
    assert ar._mm is None


def test_close_is_idempotent_after_views_dropped(tmp_path):
    ar = ra.RunArchive.open(_write(tmp_path, "idem.atxrun", _fixture_bytes()))
    _ = ar.section("backtest").f64("nav")
    del _
    import gc
    gc.collect()
    ar.close()
    assert ar._mm is None and ar._fh is None
    ar.close()  # second close is a no-op and must not raise


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
