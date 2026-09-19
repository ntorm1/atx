"""Stream the ticker-history ZIP member to a staging TSV.

``ticker_history_bulk.publish_bulk_ticker_history`` scans an *extracted* TSV twice
with DuckDB projection pushdown, so the 3.3 GiB archive has to be materialised as
its 10.32 GiB member first. That extraction is the longest single step of a
from-scratch activation, so it is resumable: a staging file whose size matches the
member's uncompressed size and which has a recorded sha256 sidecar is accepted
as-is. The sidecar keeps the checksum with the artifact, so a resumed run can
report the same digest without re-reading 10 GiB.

Measured archive facts (audit-ticker-zip.md): one DEFLATE member
``tbltickerhistory3_10y.txt``, 11,084,562,320 uncompressed bytes, tab-delimited,
CRLF, 31,464,423 data rows, 2012-03-26 .. 2026-06-15.
"""

from __future__ import annotations

import hashlib
import logging
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

LOGGER = logging.getLogger(__name__)

TICKER_HISTORY_MEMBER = "tbltickerhistory3_10y.txt"
TICKER_HISTORY_UNCOMPRESSED_BYTES = 11_084_562_320
_READ_CHUNK_BYTES = 1 << 24  # 16 MiB


@dataclass(frozen=True)
class TickerHistoryExtractResult:
    tsv_path: Path
    member_name: str
    expected_bytes: int
    written_bytes: int
    sha256: str
    skipped: bool


def sidecar_path(tsv_path: Path) -> Path:
    """Return the sha256 sidecar path for an extracted staging TSV."""
    return tsv_path.with_suffix(tsv_path.suffix + ".sha256")


def _sole_member(archive: zipfile.ZipFile) -> zipfile.ZipInfo:
    members = [info for info in archive.infolist() if not info.is_dir()]
    if len(members) != 1:
        raise RuntimeError(
            "expected exactly one ticker-history member, found "
            f"{len(members)}: {[info.filename for info in members]}"
        )
    return members[0]


def extract_ticker_history_tsv(
    zip_path: Path,
    tsv_path: Path,
    *,
    expected_bytes: int | None = TICKER_HISTORY_UNCOMPRESSED_BYTES,
    progress: Callable[[int, int], None] | None = None,
    progress_every_bytes: int = 1 << 30,
) -> TickerHistoryExtractResult:
    """Extract the sole ZIP member to ``tsv_path``, resuming when already complete.

    ``expected_bytes`` gates against a silently different archive; pass ``None``
    only for synthetic fixtures. Writes through a ``.part`` file and renames on
    success so a killed run never leaves a truncated file that the resume check
    would accept.
    """
    if not zip_path.is_file():
        raise FileNotFoundError(zip_path)
    if progress_every_bytes < 1:
        raise ValueError("progress_every_bytes must be positive")

    sidecar = sidecar_path(tsv_path)
    partial = tsv_path.with_suffix(tsv_path.suffix + ".part")
    digest = hashlib.sha256()
    written = 0

    with zipfile.ZipFile(zip_path) as archive:
        info = _sole_member(archive)
        member_bytes = int(info.file_size)
        if expected_bytes is not None and member_bytes != expected_bytes:
            raise ValueError(
                f"{zip_path} member {info.filename} is {member_bytes:,} uncompressed bytes; "
                f"expected {expected_bytes:,}"
            )
        if tsv_path.is_file() and tsv_path.stat().st_size == member_bytes and sidecar.is_file():
            LOGGER.info("reusing extracted ticker-history TSV at %s", tsv_path)
            return TickerHistoryExtractResult(
                tsv_path=tsv_path,
                member_name=info.filename,
                expected_bytes=member_bytes,
                written_bytes=member_bytes,
                sha256=sidecar.read_text(encoding="ascii").strip(),
                skipped=True,
            )

        tsv_path.parent.mkdir(parents=True, exist_ok=True)
        next_report = progress_every_bytes
        with archive.open(info) as source, open(partial, "wb") as target:
            while True:
                chunk = source.read(_READ_CHUNK_BYTES)
                if not chunk:
                    break
                target.write(chunk)
                digest.update(chunk)
                written += len(chunk)
                if progress is not None and written >= next_report:
                    progress(written, member_bytes)
                    while next_report <= written:
                        next_report += progress_every_bytes

    if written != member_bytes:
        partial.unlink(missing_ok=True)
        raise RuntimeError(f"extracted {written:,} bytes from {zip_path}, expected {member_bytes:,}")

    partial.replace(tsv_path)
    checksum = digest.hexdigest()
    sidecar.write_text(checksum + "\n", encoding="ascii")
    if progress is not None:
        progress(written, member_bytes)
    LOGGER.info("extracted %s (%s bytes, sha256 %s)", tsv_path, f"{written:,}", checksum)
    return TickerHistoryExtractResult(
        tsv_path=tsv_path,
        member_name=info.filename,
        expected_bytes=member_bytes,
        written_bytes=written,
        sha256=checksum,
        skipped=False,
    )
