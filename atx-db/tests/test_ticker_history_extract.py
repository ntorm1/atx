"""Streaming extraction of the SpiderRock ticker-history archive member."""

from __future__ import annotations

import hashlib
import zipfile
from pathlib import Path

import pytest

from atx_db.ticker_history import DEFAULT_TICKER_HISTORY_ZIP, SOURCE_COLUMNS
from atx_db.ticker_history_extract import (
    TICKER_HISTORY_UNCOMPRESSED_BYTES,
    extract_ticker_history_tsv,
    sidecar_path,
)

_HEADER = "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tclosePr\tvolume\tshares\treturnFactor"
_ROWS = [
    "2024-01-02\t32952\tAAA\tAAA\t10.0\t10.5\t9.9\t10.2\t10.2\t1000\t1000000\t1.0",
    "2024-01-02\t32953\tBBB\tBBB\t20.0\t20.5\t19.9\t20.2\t20.2\t2000\t2000000\t1.0",
    "2024-01-02\t32954\tCCC\tCCC\t30.0\t30.5\t29.9\t30.2\t30.2\t3000\t3000000\t1.0",
]
_MEMBER = "tbltickerhistory3_10y.txt"


def _payload() -> bytes:
    return ("\r\n".join([_HEADER, *_ROWS]) + "\r\n").encode("utf-8")


@pytest.fixture
def synthetic_zip(tmp_path: Path) -> Path:
    zip_path = tmp_path / "tbltickerhistory3_10y.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(_MEMBER, _payload())
    return zip_path


def test_expected_uncompressed_size_is_the_audited_constant():
    assert TICKER_HISTORY_UNCOMPRESSED_BYTES == 11_084_562_320


def test_extraction_writes_the_member_and_records_its_sha256(synthetic_zip, tmp_path):
    tsv_path = tmp_path / "staging" / _MEMBER
    result = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert result.skipped is False
    assert result.member_name == _MEMBER
    assert result.written_bytes == len(_payload())
    assert tsv_path.read_bytes() == _payload()
    assert result.sha256 == hashlib.sha256(_payload()).hexdigest()
    assert sidecar_path(tsv_path).read_text(encoding="ascii").strip() == result.sha256


def test_extraction_is_resumable_and_skips_a_complete_staging_file(synthetic_zip, tmp_path):
    tsv_path = tmp_path / "staging" / _MEMBER
    first = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    mtime = tsv_path.stat().st_mtime_ns
    second = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert second.skipped is True
    assert second.sha256 == first.sha256
    assert tsv_path.stat().st_mtime_ns == mtime


def test_a_truncated_staging_file_is_re_extracted(synthetic_zip, tmp_path):
    tsv_path = tmp_path / "staging" / _MEMBER
    extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    tsv_path.write_bytes(_payload()[:10])
    result = extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert result.skipped is False
    assert tsv_path.read_bytes() == _payload()


def test_a_size_mismatch_against_the_expected_constant_fails_fast(synthetic_zip, tmp_path):
    with pytest.raises(ValueError, match="uncompressed bytes"):
        extract_ticker_history_tsv(synthetic_zip, tmp_path / _MEMBER)


def test_progress_callback_reports_bytes_and_total(synthetic_zip, tmp_path):
    seen: list[tuple[int, int]] = []
    extract_ticker_history_tsv(
        synthetic_zip,
        tmp_path / _MEMBER,
        expected_bytes=len(_payload()),
        progress=lambda written, total: seen.append((written, total)),
        progress_every_bytes=1,
    )
    assert seen
    assert seen[-1] == (len(_payload()), len(_payload()))


def test_a_multi_member_archive_is_rejected(tmp_path):
    zip_path = tmp_path / "two.zip"
    with zipfile.ZipFile(zip_path, "w") as archive:
        archive.writestr("a.txt", b"a")
        archive.writestr("b.txt", b"b")
    with pytest.raises(RuntimeError, match="exactly one ticker-history member"):
        extract_ticker_history_tsv(zip_path, tmp_path / "out.txt", expected_bytes=None)


def test_a_missing_archive_raises_file_not_found(tmp_path):
    with pytest.raises(FileNotFoundError):
        extract_ticker_history_tsv(tmp_path / "nope.zip", tmp_path / "out.txt", expected_bytes=None)


def test_no_part_file_survives_a_successful_extraction(synthetic_zip, tmp_path):
    tsv_path = tmp_path / _MEMBER
    extract_ticker_history_tsv(synthetic_zip, tsv_path, expected_bytes=len(_payload()))
    assert not tsv_path.with_suffix(tsv_path.suffix + ".part").exists()


@pytest.mark.skipif(
    not DEFAULT_TICKER_HISTORY_ZIP.is_file(),
    reason="real tbltickerhistory3_10y.zip archive not present on this machine",
)
def test_real_archive_header_matches_source_columns():
    """Settle the 70-vs-71-column audit discrepancy against the real archive.

    Reads ONLY the first line of the sole member via the streaming zip reader
    (never extracts the 10.32 GiB member) and compares the tab-split header to
    ``ticker_history.SOURCE_COLUMNS`` verbatim, in order.
    """
    with zipfile.ZipFile(DEFAULT_TICKER_HISTORY_ZIP) as archive:
        members = [info for info in archive.infolist() if not info.is_dir()]
        assert len(members) == 1, f"expected exactly one member, found {[m.filename for m in members]}"
        with archive.open(members[0]) as source:
            header_line = source.readline()
    header = header_line.decode("utf-8").rstrip("\r\n")
    assert header.split("\t") == SOURCE_COLUMNS
