"""The ATXVFA fit archive through Python (lane PY, 2026-10-23).

A research user's highest-value read path: write a fit, read it back zero-copy, decode it
bit-identically, and get a typed refusal (``FitArchiveError``, an ``AtxError`` carrying the
library's ``ArchiveStatus``) for every damaged image. The library contracts themselves are
pinned by atx-vol/tests/fit_archive_test.cpp; this file pins what the binding adds: numpy
dtypes, shapes and strides, read-only views that outlive ``close()``, the error mapping and
the series wrapper.
"""

from __future__ import annotations

import gc
import math

import numpy as np
import pytest

import atxvol as av
from test_quote_slice import _wing_chain

SNAPSHOT_NS = 1_766_000_000_000_000_000
EXPIRY_DAY = 20_550


@pytest.fixture(scope="module")
def fit():
    market, quotes = _wing_chain()
    config = av.QuoteSliceConfig()
    config.floor_policy = av.QuoteSliceFloorPolicy.PRICE_ONLY
    result = av.fit_quote_slice(market, quotes, config)
    assert result.fit_status == av.QuoteSliceFitStatus.SUCCESS, result.fit_error
    return result


def _image(fit, archive_id=7, sequence=0):
    writer = av.FitArchiveWriter(created_unix_ns=11, archive_id=archive_id,
                                 segment_sequence=sequence)
    writer.add_fit("SPY", SNAPSHOT_NS, EXPIRY_DAY, fit)
    writer.set_provenance(b"lane-PY test")
    assert writer.fit_count == 1
    return writer.finish()


@pytest.fixture(scope="module")
def image(fit):
    return _image(fit)


def test_a_written_fit_reads_back_and_decodes_bit_identically(fit, image):
    archive = av.FitArchive.from_bytes(image)
    header = archive.header
    assert header["format_major"] == 1
    assert header["archive_id"] == 7 and header["created_unix_ns"] == 11
    assert header["file_size"] == len(image)
    assert header["writer_name"] == f"atx-vol {av.__version__}"
    assert archive.symbols() == ["SPY"]
    assert archive.find_underlier("SPY") == 0 and archive.find_underlier("QQQ") is None
    assert archive.underlier_name(0) == "SPY"
    assert archive.provenance() == b"lane-PY test"
    assert archive.n_fits == 1
    assert archive.find_fit("SPY", SNAPSHOT_NS, EXPIRY_DAY) == 0
    assert archive.find_fit("SPY", SNAPSHOT_NS, EXPIRY_DAY + 1) is None

    record = archive.fit(0)
    assert (record.underlier, record.snapshot_ns, record.expiry_day) == (
        "SPY", SNAPSHOT_NS, EXPIRY_DAY)
    assert record.fit_status == av.QuoteSliceFitStatus.SUCCESS
    assert record.n_rows == len(fit.rows)
    assert record.has_prices and record.n_price_nodes == len(fit.node_strikes)
    assert [r.index for r in archive.fits()] == [0]

    decoded = archive.decode_fit(0)
    assert decoded.fit_status == fit.fit_status
    assert decoded.forward == fit.forward
    assert list(decoded.node_strikes) == list(fit.node_strikes)
    assert len(decoded.rows) == len(fit.rows)
    for a, b in zip(decoded.rows, fit.rows):
        assert (a.fitted_price == b.fitted_price) or (
            math.isnan(a.fitted_price) and math.isnan(b.fitted_price))
        assert a.evaluation_status == b.evaluation_status
    curve = archive.decode_curve(0)
    assert curve.kind == record.curve_kind
    assert curve.dof > 0
    k = np.array([-0.1, 0.0, 0.1])
    iv = curve.iv(k)
    assert iv.dtype == np.float64 and iv.shape == (3,)
    np.testing.assert_allclose(iv, np.sqrt(curve.w(k) / record.curve_T), rtol=1e-12)


def test_zero_copy_views_have_their_documented_dtype_shape_and_strides(fit, image):
    archive = av.FitArchive.from_bytes(image)
    rows = archive.quote_rows(0)
    assert rows.shape == (len(fit.rows),)
    assert rows.dtype.itemsize == 72 and rows.strides == (72,)
    assert rows.dtype["fitted_price"] == np.dtype("<f8")
    assert rows.dtype["evaluation_status"] == np.dtype("u1")
    assert not rows.flags.writeable
    with pytest.raises(ValueError):
        rows["fitted_price"][0] = 0.0
    prices = np.array([r.fitted_price for r in fit.rows])
    np.testing.assert_array_equal(rows["fitted_price"], prices)

    nodes = archive.price_nodes(0)
    assert nodes.dtype.names == ("strike", "call", "put")
    assert nodes.shape == (len(fit.node_strikes),)
    np.testing.assert_array_equal(nodes["strike"], np.asarray(fit.node_strikes))
    payload = archive.curve_payload(0)
    assert payload.dtype == np.float64 and payload.ndim == 1 and payload.size > 0
    for view in (archive.moved_nodes(0), archive.price_only_nodes(0), archive.dividends(0),
                 archive.carry_curve(0)):
        assert view.ndim == 1 and not view.flags.writeable


def test_views_outlive_the_archive_and_a_closed_archive_refuses(image):
    archive = av.FitArchive.from_bytes(image)
    rows = archive.quote_rows(0)
    before = rows["fitted_iv"].copy()
    archive.close()
    assert archive.closed
    with pytest.raises(ValueError, match="closed"):
        archive.fit(0)
    del archive
    gc.collect()
    np.testing.assert_array_equal(rows["fitted_iv"], before)
    with pytest.raises(IndexError):
        av.FitArchive.from_bytes(image).quote_rows(5)


def test_a_file_round_trip_verifies_and_closes_as_a_context_manager(fit, tmp_path):
    path = tmp_path / "one.atxvfa"
    writer = av.FitArchiveWriter(archive_id=3)
    writer.add_fit("SPY", SNAPSHOT_NS, EXPIRY_DAY, fit)
    writer.write_file(str(path))
    for mode in (av.FitArchiveOpenMode.MAPPED, av.FitArchiveOpenMode.READ):
        with av.FitArchive.open(str(path), mode=mode) as archive:
            assert archive.verify_file_checksum()
            assert all(archive.verify_section(i) for i in range(len(archive.sections())))
            rows = archive.quote_rows(0)
        assert archive.closed
        assert rows.shape == (len(fit.rows),)


def test_every_damaged_image_is_a_typed_fit_archive_error(image, tmp_path):
    with pytest.raises(av.FitArchiveError) as raised:
        av.FitArchive.from_bytes(image[:100])
    assert isinstance(raised.value, av.AtxError)
    assert raised.value.status == av.ArchiveStatus.TRUNCATED
    assert raised.value.code == av.ErrorCode.PARSE_ERROR

    archive = av.FitArchive.from_bytes(image)
    rows_section = next(s for s in archive.sections() if s["name"] == "quote-rows")
    damaged = bytearray(image)
    damaged[rows_section["offset"] + 8] ^= 0xFF
    with pytest.raises(av.FitArchiveError) as raised:
        av.FitArchive.from_bytes(bytes(damaged))
    assert raised.value.status == av.ArchiveStatus.SECTION_CHECKSUM
    assert raised.value.section_name == "quote-rows"

    with pytest.raises(av.FitArchiveError) as raised:
        av.FitArchive.open(str(tmp_path / "absent.atxvfa"))
    assert raised.value.code == av.ErrorCode.NOT_FOUND


def test_a_series_publishes_segments_and_finds_fits_across_them(fit, tmp_path):
    directory = str(tmp_path / "series")
    # Sequences start at 1: next_sequence() is the manifest's latest + 1 (latest 0 if empty).
    for offset, day in enumerate((EXPIRY_DAY, EXPIRY_DAY + 7)):
        writer = av.FitArchiveWriter()
        writer.add_fit("SPY", SNAPSHOT_NS + offset, day, fit)
        assert av.append_fit_archive_segment(directory, writer, archive_id=40) == offset + 1
    series = av.FitArchiveSeries.open(directory)
    assert series.segment_count == 2 and len(series.manifest()) == 2
    assert series.find_fit("SPY", SNAPSHOT_NS + 1, EXPIRY_DAY + 7) == (1, 0)
    assert series.find_fit("SPY", SNAPSHOT_NS, EXPIRY_DAY + 7) is None
    segment = series.segment(1)
    del series
    gc.collect()
    assert segment.fit(0).expiry_day == EXPIRY_DAY + 7
    compacted = av.compact_fit_archive_series(directory)
    assert isinstance(compacted, dict)
    assert av.FitArchiveSeries.open(directory).find_fit("SPY", SNAPSHOT_NS, EXPIRY_DAY) is not None


def test_the_archive_status_and_section_enums_name_every_library_value():
    for name, enum in (("ArchiveStatus", av.ArchiveStatus),
                       ("FitArchiveSectionType", av.FitArchiveSectionType)):
        library = av._core._enum_names(name)
        bound = {int(v) for v in enum.__members__.values()}
        assert set(library) == bound, name
    assert av.fit_archive_crc32c(b"123456789") == 0xE3069283
