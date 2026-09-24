"""Bounded, persistent directory for the unusually large SEC submissions ZIP.

Only directory framing is read here. Python's zipfile parses each individual
central record and performs local-header, decompression, overlap and CRC checks.
The complete corpus of ZipInfo objects and filename strings is never retained.
"""
from __future__ import annotations

import hashlib
import io
import json
import logging
import os
import re
import sqlite3
import struct
import sys
import tempfile
import zipfile
from collections.abc import Iterator
from pathlib import Path
from typing import Any, BinaryIO

LOGGER = logging.getLogger(__name__)
_INDEX_VERSION = 1
_MAIN = re.compile(r"^CIK(\d{10})\.json$")
_HISTORY = re.compile(r"^CIK\d{10}-submissions-\d+\.json$")
_CENTRAL = struct.Struct("<4s6H3L5H2L")
_END = struct.Struct("<4s4H2LH")
_INDEX_CACHE_KIB = 8192


def _stat_identity(stat: os.stat_result) -> tuple[int, int, int, int]:
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def _digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def _one_info(record: bytes) -> zipfile.ZipInfo:
    # A synthetic directory containing exactly one entry delegates all filename,
    # Unicode-extra, version and ZIP64-extra interpretation to the installed
    # stdlib. It contains no payload and is at most 196,673 bytes long.
    ending = _END.pack(b"PK\x05\x06", 0, 0, 1, 1, len(record), 0, 0)
    with zipfile.ZipFile(io.BytesIO(record + ending)) as directory:
        return directory.infolist()[0]


class _PayloadReader(zipfile.ZipFile):
    def _RealGetContents(self) -> None:
        # The bounded directory has already been validated separately. read()
        # receives one genuine ZipInfo, never a name requiring NameToInfo.
        pass


class SubmissionsArchive:
    """Read-only ZIP access with a disk-backed, last-name-wins directory.

    Cache digests detect accidental corruption or edited index rows. A malicious
    party able to replace both the local cache and its digest receipt is outside
    this local-cache trust model. The source is independently SHA-256 hashed on
    every open; every selected central record is compared with source bytes.
    """

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        # Selected central records must be read from the file again, even when
        # another writer changes bytes and restores the original mtime. A
        # BufferedReader can satisfy an in-buffer seek with stale read-ahead.
        # Directory construction uses its own short-lived buffered handle.
        self._source: BinaryIO = self.path.open("rb", buffering=0)
        self._index: sqlite3.Connection | None = None
        self._reader: _PayloadReader | None = None
        try:
            self._source_stat = _stat_identity(os.fstat(self._source.fileno()))
            self.sha256 = hashlib.file_digest(self._source, "sha256").hexdigest()
            self.assert_unchanged()
            self._identity = {
                "source_sha256": self.sha256,
                "source_stat": list(self._source_stat),
                "index_version": _INDEX_VERSION,
                "python_zipfile_version": list(sys.version_info[:3]),
            }
            self.index_path = self.path.with_name(
                f"{self.path.name}.directory-v{_INDEX_VERSION}-{self.sha256}.sqlite3"
            )
            self._receipt_path = self.index_path.with_suffix(".sqlite3.sha256")
            if not self._open_cached_index():
                self._build_index()
                if not self._open_cached_index():
                    raise ValueError("SEC submissions directory index failed publication validation")
            self._reader = _PayloadReader(self._source)
            self._reader.start_dir = self.start_dir
            self.assert_unchanged()
        except BaseException:
            self.close()
            raise

    def __enter__(self) -> SubmissionsArchive:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def close(self) -> None:
        if self._reader is not None:
            self._reader.close()
            self._reader = None
        if self._index is not None:
            self._index.close()
            self._index = None
        self._source.close()

    def assert_unchanged(self) -> None:
        if (self._source_stat != _stat_identity(os.fstat(self._source.fileno()))
                or self._source_stat != _stat_identity(self.path.stat())):
            raise ValueError("SEC submissions archive changed while open")

    @staticmethod
    def _configure_index(connection: sqlite3.Connection) -> None:
        connection.execute(f"PRAGMA cache_size = -{_INDEX_CACHE_KIB}")
        connection.execute("PRAGMA temp_store = FILE")
        connection.execute("PRAGMA mmap_size = 0")

    def _open_cached_index(self) -> bool:
        connection = None
        try:
            # A finished SQLite file is insufficient: publication requires its
            # full-file digest receipt too. Neither cached row counts nor labels
            # alone authorize reuse after an interrupted build or corruption.
            expected_digest = self._receipt_path.read_text(encoding="ascii").strip()
            if not re.fullmatch(r"[0-9a-f]{64}", expected_digest):
                return False
            before = _stat_identity(self.index_path.stat())
            if _digest(self.index_path) != expected_digest:
                return False
            connection = sqlite3.connect(f"{self.index_path.resolve().as_uri()}?mode=ro", uri=True)
            self._configure_index(connection)
            metadata = dict(connection.execute("SELECT key, value FROM metadata"))
            if json.loads(metadata["identity"]) != self._identity:
                return False
            if before != _stat_identity(self.index_path.stat()):
                return False
            self.start_dir = int(metadata["start_dir"])
            self._concat = int(metadata["concat"])
            self.entry_count = int(metadata["entry_count"])
            self.compressed_bytes = int(metadata["compressed_bytes"])
            self.expanded_bytes = int(metadata["expanded_bytes"])
            self.main_member_count = connection.execute("SELECT count(*) FROM members WHERE kind=1").fetchone()[0]
            self.history_member_count = connection.execute("SELECT count(*) FROM members WHERE kind=2").fetchone()[0]
            self.unique_member_count = connection.execute("SELECT count(*) FROM members").fetchone()[0]
            self._index = connection
            connection = None
            return True
        except (OSError, ValueError, KeyError, sqlite3.DatabaseError):
            return False
        finally:
            if connection is not None:
                connection.close()

    def _directory_layout(self) -> tuple[int, int, int, int]:
        # CPython's bounded EOCD/ZIP64 locator reader consumes at most the ZIP
        # comment window. No ordinary ZipFile is opened on the complete archive.
        end_reader = getattr(zipfile, "_EndRecData", None)
        if end_reader is None:
            raise NotImplementedError("zipfile bounded end-record reader is unavailable")
        try:
            end: list[Any] | None = end_reader(self._source)
        except OSError as exc:
            raise zipfile.BadZipFile("File is not a zip file") from exc
        if end is None:
            raise zipfile.BadZipFile("File is not a zip file")
        signature, disk, directory_disk, disk_entries, entries, size, offset, comment_size, comment, location = end
        if disk or directory_disk or disk_entries != entries:
            raise zipfile.BadZipFile("Multi-disk ZIP archives are not supported")
        if len(comment) != comment_size:
            raise zipfile.BadZipFile("Truncated ZIP archive comment")
        concat = location - size - offset
        if signature == b"PK\x06\x06":
            concat -= 56 + 20  # fixed ZIP64 EOCD and locator; stdlib's contract
        elif signature != b"PK\x05\x06":
            raise zipfile.BadZipFile("Unsupported ZIP end record")
        start = offset + concat
        if start < 0 or size < 0 or start + size > location:
            raise zipfile.BadZipFile("Bad offset for central directory")
        return start, size, concat, entries

    def _central_records(self, start: int, size: int) -> Iterator[tuple[int, bytes]]:
        with self.path.open("rb") as directory:
            if _stat_identity(os.fstat(directory.fileno())) != self._source_stat:
                raise ValueError("SEC submissions archive changed before directory indexing")
            directory.seek(start)
            end = start + size
            while directory.tell() < end:
                position = directory.tell()
                header = directory.read(_CENTRAL.size)
                if len(header) != _CENTRAL.size:
                    raise zipfile.BadZipFile("Truncated central directory")
                fields = _CENTRAL.unpack(header)
                if fields[0] != b"PK\x01\x02":
                    raise zipfile.BadZipFile("Bad magic number for central directory")
                length = sum(fields[10:13])  # filename, extra, comment, each uint16
                if directory.tell() + length > end:
                    raise zipfile.BadZipFile("Truncated central directory entry")
                tail = directory.read(length)
                if len(tail) != length:
                    raise zipfile.BadZipFile("Truncated central directory entry")
                yield position, header + tail
            if directory.tell() != end:
                raise zipfile.BadZipFile("Central directory length mismatch")
            self.assert_unchanged()

    def _build_index(self) -> None:
        start, size, concat, expected_entries = self._directory_layout()
        fd, temporary = tempfile.mkstemp(prefix=".submissions-directory-partial-", dir=self.index_path.parent)
        os.close(fd)
        temporary_path = Path(temporary)
        receipt_temporary: Path | None = None
        connection = sqlite3.connect(temporary_path)
        try:
            self._configure_index(connection)
            connection.executescript("""
                CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
                CREATE TABLE members (
                    name TEXT PRIMARY KEY, ordinal INTEGER NOT NULL,
                    central_offset INTEGER NOT NULL, header_offset INTEGER NOT NULL,
                    record BLOB NOT NULL, kind INTEGER NOT NULL
                ) WITHOUT ROWID;
                CREATE TABLE offsets (header_offset INTEGER NOT NULL, ordinal INTEGER PRIMARY KEY);
            """)
            count = compressed = expanded = 0
            for position, record in self._central_records(start, size):
                info = _one_info(record)
                offset = info.header_offset + concat
                if info.volume or offset < 0 or offset >= start:
                    raise zipfile.BadZipFile("Invalid member volume or local-header offset")
                kind = 1 if _MAIN.match(info.filename) else (2 if _HISTORY.match(info.filename) else 0)
                connection.execute(
                    "INSERT OR REPLACE INTO members VALUES (?, ?, ?, ?, ?, ?)",
                    (info.filename, count, position, offset, record, kind),
                )
                connection.execute("INSERT INTO offsets VALUES (?, ?)", (offset, count))
                count += 1
                compressed += info.compress_size
                expanded += info.file_size
                if count % 10000 == 0:
                    connection.commit()
                if count % 100000 == 0:
                    LOGGER.info("SEC submissions directory indexed: entries=%d/%d", count, expected_entries)
            if count != expected_entries:
                raise zipfile.BadZipFile("ZIP directory entry count mismatch")
            connection.execute("CREATE INDEX offsets_ordered ON offsets(header_offset, ordinal)")
            connection.execute("CREATE INDEX members_kind ON members(kind, name)")
            metadata = {
                "identity": json.dumps(self._identity, sort_keys=True),
                "start_dir": str(start), "concat": str(concat), "entry_count": str(count),
                "compressed_bytes": str(compressed), "expanded_bytes": str(expanded),
            }
            connection.executemany("INSERT INTO metadata VALUES (?, ?)", metadata.items())
            connection.commit()
            connection.close()
            self.assert_unchanged()
            digest = _digest(temporary_path)
            fd, receipt_name = tempfile.mkstemp(prefix=".submissions-receipt-partial-", dir=self.index_path.parent)
            receipt_temporary = Path(receipt_name)
            with os.fdopen(fd, "w", encoding="ascii") as receipt:
                receipt.write(digest + "\n")
                receipt.flush()
                os.fsync(receipt.fileno())
            # If interrupted between replacements the old/missing digest cannot
            # authorize the new file, so the next open builds a fresh index.
            os.replace(temporary_path, self.index_path)
            os.replace(receipt_temporary, self._receipt_path)
        finally:
            connection.close()
            temporary_path.unlink(missing_ok=True)
            if receipt_temporary is not None:
                receipt_temporary.unlink(missing_ok=True)

    def _connection(self) -> sqlite3.Connection:
        if self._index is None:
            raise ValueError("SEC submissions archive is closed")
        return self._index

    def __contains__(self, name: object) -> bool:
        return isinstance(name, str) and self._connection().execute(
            "SELECT 1 FROM members WHERE name=?", (name,),
        ).fetchone() is not None

    def main_members(self, *, after: str | None = None, through: str | None = None) -> Iterator[str]:
        """Replayable sorted unique main-member traversal; bounds are names."""
        cursor = self._connection().execute(
            "SELECT name FROM members WHERE kind=1 AND name > ? AND name <= ? ORDER BY name",
            (after or "", through or "\U0010ffff"),
        )
        try:
            for row in cursor:
                yield row[0]
        finally:
            cursor.close()

    def read(self, name: str) -> bytes:
        self.assert_unchanged()
        connection = self._connection()
        row = connection.execute(
            "SELECT ordinal, central_offset, header_offset, record FROM members WHERE name=?", (name,),
        ).fetchone()
        if row is None:
            raise KeyError(f"There is no item named {name!r} in the archive")
        ordinal, central_offset, header_offset, record = row
        self._source.seek(central_offset)
        if self._source.read(len(record)) != record:
            raise zipfile.BadZipFile("Cached ZIP central record differs from source")
        info = _one_info(record)
        info.header_offset += self._concat
        if info.filename != name or info.header_offset != header_offset:
            raise zipfile.BadZipFile("Cached ZIP member identity differs from source")
        # Keep even overwritten directory entries in this ordering. This is the
        # same boundary assigned by ZipFile's stable descending-offset sort,
        # including the overlap refusal when two entries share a local header.
        same_offset = connection.execute(
            "SELECT 1 FROM offsets WHERE header_offset=? AND ordinal < ? LIMIT 1", (header_offset, ordinal),
        ).fetchone()
        next_offset = connection.execute(
            "SELECT min(header_offset) FROM offsets WHERE header_offset > ?", (header_offset,),
        ).fetchone()[0]
        info._end_offset = header_offset if same_offset else (self.start_dir if next_offset is None else next_offset)
        if self._reader is None:
            raise ValueError("SEC submissions archive is closed")
        data = self._reader.read(info)
        if len(data) != info.file_size:
            raise zipfile.BadZipFile("ZIP member expanded length mismatch")
        self.assert_unchanged()
        return data
