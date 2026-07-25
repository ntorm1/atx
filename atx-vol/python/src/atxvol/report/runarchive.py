"""Pure-Python reader for the RunArchive (ATXRUN01) binary result container.

This is the Python counterpart of ``atx-vol/include/atx/vol/run_archive.hpp``:
a zero-copy reader over the on-disk ABI the C++ ``write_run_archive`` emits. It
depends only on the standard library (``struct`` / ``mmap``) plus ``numpy`` and
the generated ``_schema`` descriptor — never the compiled ``atxvol._core``
extension — so a report can read a run archive without the binding being built.

On-disk layout (little-endian, mirrors the C++ structs)::

    RunArchiveHeader          256 B at offset 0
    RaSectionDescriptor[]      80 B each, name-sorted, at header.section_dir_offset
    64-B-aligned sections; each =
        RaSectionHeader        64 B
        RaColumnDescriptor[]   96 B each
        8-B-aligned column arrays
            DictStr -> u32 code array + aux table (u32 offsets[n+1] + blob)
            U8Enum  -> u8 code array  + aux label table (same shape)

Integrity is layered CRC-32C (Castagnoli): the header CRC (own field zeroed),
the metadata CRC over the section directory, and a per-section payload CRC
mirrored into the directory. ``open`` validates framing + header/metadata CRC +
schema hash eagerly and raises ``ValueError`` on any mismatch; per-section
payload CRCs are checked lazily via ``validate_section`` / ``validate_all``.

    from atxvol.report.runarchive import RunArchive, read_backtest_section

    ar = RunArchive.open("run.atxrun")
    nav = ar.section("backtest").f64("nav")        # zero-copy numpy view
    result, meta, extra = read_backtest_section(ar)
"""

from __future__ import annotations

import mmap
import struct
from dataclasses import dataclass, field
from typing import Iterator

import numpy as np

try:  # normal package import
    from . import _schema
except ImportError:  # pragma: no cover - standalone load without package context
    import _schema  # type: ignore

__all__ = [
    "RunArchive",
    "Section",
    "DictColumn",
    "EnumColumn",
    "BacktestSection",
    "read_backtest_section",
]

# ── Format constants (must match the Task 2 structs byte-for-byte) ────────────

# RaDType codes.
_F64, _I64, _U32, _U8ENUM, _DICTSTR = 0, 1, 2, 3, 4
_DTYPE_SIZE = {_F64: 8, _I64: 8, _U32: 4, _U8ENUM: 1, _DICTSTR: 4}
_NP_DTYPE = {_F64: np.dtype("<f8"), _I64: np.dtype("<i8"), _U32: np.dtype("<u4"),
             _U8ENUM: np.dtype("<u1"), _DICTSTR: np.dtype("<u4")}

_SECTION_ALIGN = 64
_COLUMN_ALIGN = 8
_MAX_ROWS = 1 << 48  # writer-side plausibility cap; makes n_rows*size non-wrapping

_SECTION_MAGIC = b"ATXRSC01"

# struct format strings (little-endian, packed — the structs have no padding).
_HEADER_FMT = struct.Struct("<8sQQQQQQQIIIIHHHHHH164s")   # RunArchiveHeader, 256 B
_SECDESC_FMT = struct.Struct("<QQQIIIB32s11s")            # RaSectionDescriptor, 80 B
_SECHDR_FMT = struct.Struct("<8sQQIIIIIB19s")             # RaSectionHeader, 64 B
_COLDESC_FMT = struct.Struct("<QQQQIBBH40s16s")           # RaColumnDescriptor, 96 B

assert _HEADER_FMT.size == 256
assert _SECDESC_FMT.size == 80
assert _SECHDR_FMT.size == 64
assert _COLDESC_FMT.size == 96


# ── CRC-32C (Castagnoli), bit-identical to atx::vol::detail::crc32c ───────────

def _build_crc32c_table() -> list[int]:
    poly = 0x82F63B78  # reflected Castagnoli polynomial
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ (poly if (c & 1) else 0)
        table.append(c)
    return table


_CRC_TABLE = _build_crc32c_table()


def _crc32c(data) -> int:
    """One-shot CRC-32C with the standard init/final XOR (0xFFFFFFFF)."""
    crc = 0xFFFFFFFF
    tab = _CRC_TABLE
    for byte in data:
        crc = (crc >> 8) ^ tab[(crc ^ byte) & 0xFF]
    return crc ^ 0xFFFFFFFF


# ── Header / descriptor records ──────────────────────────────────────────────

@dataclass(frozen=True)
class Header:
    magic: bytes
    file_size: int
    created_ts_ns: int
    schema_hash: int
    writer_version_hash: int
    run_identity_hash: int
    section_dir_offset: int
    data_offset: int
    section_count: int
    header_crc32c: int
    metadata_crc32c: int
    flags: int
    major: int
    minor: int
    header_size: int
    endian: int
    pointer_bits: int


@dataclass(frozen=True)
class _SectionDescriptor:
    section_offset: int
    section_size: int
    n_rows: int
    n_cols: int
    col_desc_offset: int
    payload_crc32c: int
    kind: int
    name: str


@dataclass(frozen=True)
class _ColumnDescriptor:
    data_offset: int
    data_size: int
    aux_offset: int
    aux_size: int
    aux_count: int
    dtype: int
    name_len: int
    name: str


# ── Decoded string columns ───────────────────────────────────────────────────

class _StringColumn:
    """Common base for dict-str / u8-enum columns: per-row codes + a string table."""

    __slots__ = ("codes", "table")

    def __init__(self, codes: np.ndarray, table: list[str]):
        self.codes = codes      # numpy view over the on-disk code array (zero-copy)
        self.table = table      # decoded string / label table

    def __len__(self) -> int:
        return int(self.codes.shape[0])

    def at(self, row: int) -> str:
        """Row ``row`` decoded through the table, or "" if out of range."""
        if row < 0 or row >= len(self):
            return ""
        return self.table[int(self.codes[row])]

    def __getitem__(self, row: int) -> str:
        return self.at(row)

    def __iter__(self) -> Iterator[str]:
        table = self.table
        return (table[int(c)] for c in self.codes)

    def tolist(self) -> list[str]:
        table = self.table
        return [table[int(c)] for c in self.codes]


class DictColumn(_StringColumn):
    """A ``DictStr`` column: u32 codes decoded through a first-appearance dict."""


class EnumColumn(_StringColumn):
    """A ``U8Enum`` column: u8 codes decoded through a fixed label vocabulary.

    ``labels`` is an alias of ``table`` for readability at the call site.
    """

    @property
    def labels(self) -> list[str]:
        return self.table


# ── Section view ─────────────────────────────────────────────────────────────

class Section:
    """Zero-copy typed view over one section record.

    Column accessors return numpy views straight over the archive's backing
    bytes (``f64`` / ``i64`` / ``u32``) or decoded string columns (``dict`` /
    ``u8enum``). The views borrow the owning :class:`RunArchive`'s buffer and
    must not be used after it is closed. A missing name (or a name whose stored
    dtype differs from the accessor) raises ``KeyError``.
    """

    __slots__ = ("_buf", "_base", "name", "kind", "n_rows", "n_cols", "_cols")

    def __init__(self, buf, base: int, name: str, kind: int, n_rows: int,
                 cols: dict[str, _ColumnDescriptor]):
        self._buf = buf
        self._base = base
        self.name = name
        self.kind = kind
        self.n_rows = n_rows
        self.n_cols = len(cols)
        self._cols = cols

    @property
    def columns(self) -> list[str]:
        return list(self._cols)

    def has(self, name: str) -> bool:
        return name in self._cols

    def _find(self, name: str, dtype: int) -> _ColumnDescriptor:
        cd = self._cols.get(name)
        if cd is None:
            raise KeyError(f"section {self.name!r}: no column {name!r}")
        if cd.dtype != dtype:
            raise KeyError(
                f"section {self.name!r}: column {name!r} has dtype {cd.dtype}, "
                f"not {dtype}"
            )
        return cd

    def _typed(self, name: str, dtype: int) -> np.ndarray:
        cd = self._find(name, dtype)
        return np.frombuffer(self._buf, dtype=_NP_DTYPE[dtype], count=self.n_rows,
                             offset=self._base + cd.data_offset)

    def f64(self, name: str) -> np.ndarray:
        """Zero-copy ``float64`` view of an ``F64`` column."""
        return self._typed(name, _F64)

    def i64(self, name: str) -> np.ndarray:
        """Zero-copy ``int64`` view of an ``I64`` column."""
        return self._typed(name, _I64)

    def u32(self, name: str) -> np.ndarray:
        """Zero-copy ``uint32`` view of a ``U32`` column."""
        return self._typed(name, _U32)

    def dict(self, name: str) -> DictColumn:
        """A ``DictStr`` column decoded to a :class:`DictColumn`."""
        cd = self._find(name, _DICTSTR)
        codes = np.frombuffer(self._buf, dtype=_NP_DTYPE[_DICTSTR], count=self.n_rows,
                              offset=self._base + cd.data_offset)
        return DictColumn(codes, self._string_table(cd))

    def u8enum(self, name: str) -> EnumColumn:
        """A ``U8Enum`` column decoded to an :class:`EnumColumn`."""
        cd = self._find(name, _U8ENUM)
        codes = np.frombuffer(self._buf, dtype=_NP_DTYPE[_U8ENUM], count=self.n_rows,
                              offset=self._base + cd.data_offset)
        return EnumColumn(codes, self._string_table(cd))

    def _string_table(self, cd: _ColumnDescriptor) -> list[str]:
        # Aux table: u32 offsets[aux_count + 1] then the concatenated blob. Framing
        # (offset monotonicity / bounds) was validated when the section was built.
        n = cd.aux_count
        off_base = self._base + cd.aux_offset
        offsets = struct.unpack_from(f"<{n + 1}I", self._buf, off_base)
        blob_base = off_base + 4 * (n + 1)
        # Materialize bytes before slicing/decoding: on the mmap path a slice is
        # already ``bytes``, but via ``from_bytes`` the backing is a memoryview
        # whose slices are memoryviews (no ``.decode``). ``bytes(...)`` normalizes
        # both paths; the numeric column views stay zero-copy (they never route
        # through here).
        blob = bytes(self._buf[blob_base:blob_base + offsets[n]])
        try:
            return [blob[offsets[i]:offsets[i + 1]].decode("utf-8")
                    for i in range(n)]
        except UnicodeDecodeError as exc:
            # A forged/corrupt string table can hold non-utf8 bytes. Surface the
            # documented ValueError the rest of the reader raises on hostile input
            # rather than letting a raw UnicodeDecodeError escape.
            raise ValueError(
                f"RunArchive: string table is not valid UTF-8 ({exc})"
            ) from exc


# ── Archive ──────────────────────────────────────────────────────────────────

class RunArchive:
    """An opened ATXRUN01 archive, memory-mapped read-only.

    ``open`` validates framing, the header/metadata CRC-32C, and the schema hash
    (raising ``ValueError`` on any mismatch); ``section`` validates one section
    record's framing lazily; ``validate_section`` / ``validate_all`` verify the
    per-section payload CRCs. Column views borrow the mapping, so keep the
    archive alive while they are in use.
    """

    def __init__(self, buf, header: Header, directory: list[_SectionDescriptor],
                 mm=None, fh=None):
        self._buf = buf
        self.header = header
        self._dir = directory
        self._by_name = {d.name: d for d in directory}
        self._mm = mm
        self._fh = fh

    # -- construction ---------------------------------------------------------

    @classmethod
    def open(cls, path: str) -> "RunArchive":
        """Memory-map ``path`` and validate its framing. Raises ``ValueError``."""
        fh = open(path, "rb")
        try:
            size = fh.seek(0, 2)
            if size == 0:
                raise ValueError(f"{path}: empty file")
            mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        except Exception:
            fh.close()
            raise
        try:
            header, directory = cls._parse_and_validate(mm)
        except Exception:
            mm.close()
            fh.close()
            raise
        return cls(mm, header, directory, mm=mm, fh=fh)

    @classmethod
    def from_bytes(cls, data: bytes) -> "RunArchive":
        """Open an in-memory archive (a resident buffer rather than a mapping)."""
        buf = memoryview(data)
        header, directory = cls._parse_and_validate(buf)
        return cls(buf, header, directory)

    @staticmethod
    def _parse_and_validate(buf):
        n = len(buf)
        if n < _HEADER_FMT.size:
            raise ValueError("RunArchive: shorter than header")
        raw = _HEADER_FMT.unpack_from(buf, 0)
        header = Header(*raw[:17])  # trailing reserved[164] dropped

        if header.magic != _schema.RA_MAGIC:
            raise ValueError("RunArchive: bad magic")
        if header.major != _schema.RA_MAJOR:
            raise ValueError(f"RunArchive: unsupported major version {header.major}")
        if header.minor > _schema.RA_MINOR:
            raise ValueError(f"RunArchive: unsupported minor version {header.minor}")
        if header.endian != 1:
            raise ValueError("RunArchive: non-little-endian archive")
        if header.pointer_bits != 64:
            raise ValueError("RunArchive: unsupported pointer width")
        if header.header_size != _HEADER_FMT.size:
            raise ValueError("RunArchive: header size mismatch")
        # Schema pin: reject a drifted registry before any bytes are trusted.
        expected = _schema.ra_schema_hash()
        if header.schema_hash != expected:
            raise ValueError(
                f"RunArchive: schema hash mismatch "
                f"(file 0x{header.schema_hash:016x}, reader 0x{expected:016x})"
            )
        if header.file_size != n:
            raise ValueError("RunArchive: file size mismatch")

        dir_off = header.section_dir_offset
        dir_bytes = header.section_count * _SECDESC_FMT.size
        if dir_off < _HEADER_FMT.size:
            raise ValueError("RunArchive: directory overlaps header")
        if dir_off > n or dir_bytes > n - dir_off:
            raise ValueError("RunArchive: directory out of bounds")
        if dir_off + dir_bytes > header.data_offset:
            raise ValueError("RunArchive: directory overlaps data")
        if header.data_offset > n:
            raise ValueError("RunArchive: data offset out of bounds")

        # Header CRC (own field zeroed) then metadata CRC over the directory.
        head = bytearray(buf[:_HEADER_FMT.size])
        head[68:72] = b"\x00\x00\x00\x00"  # header_crc32c field
        if _crc32c(head) != header.header_crc32c:
            raise ValueError("RunArchive: header checksum mismatch")
        if _crc32c(buf[dir_off:dir_off + dir_bytes]) != header.metadata_crc32c:
            raise ValueError("RunArchive: metadata checksum mismatch")

        directory = []
        for i in range(header.section_count):
            d = _SECDESC_FMT.unpack_from(buf, dir_off + i * _SECDESC_FMT.size)
            name = d[7].split(b"\x00", 1)[0].decode("utf-8")
            de = _SectionDescriptor(
                section_offset=d[0], section_size=d[1], n_rows=d[2], n_cols=d[3],
                col_desc_offset=d[4], payload_crc32c=d[5], kind=d[6], name=name,
            )
            if not de.name:
                raise ValueError("RunArchive: empty section name")
            if de.kind > 2:
                raise ValueError("RunArchive: invalid section kind")
            if de.n_rows > _MAX_ROWS:
                raise ValueError("RunArchive: implausible row count")
            if de.section_offset < header.data_offset:
                raise ValueError("RunArchive: section precedes data")
            if de.section_offset > n or de.section_size > n - de.section_offset:
                raise ValueError("RunArchive: section out of bounds")
            if de.section_size < _SECHDR_FMT.size:
                raise ValueError("RunArchive: section smaller than header")
            if de.section_offset % _COLUMN_ALIGN != 0:
                raise ValueError("RunArchive: section offset misaligned")
            directory.append(de)
        return header, directory

    # -- queries --------------------------------------------------------------

    @property
    def sections(self) -> list[str]:
        """Section names in directory (name-sorted) order."""
        return [d.name for d in self._dir]

    def __contains__(self, name: str) -> bool:
        return name in self._by_name

    def section(self, name: str) -> Section:
        """Build a validated zero-copy view over section ``name``.

        Raises ``KeyError`` if absent, ``ValueError`` on any framing failure.
        """
        de = self._by_name.get(name)
        if de is None:
            raise KeyError(f"RunArchive: no section {name!r}")
        buf = self._buf
        base = de.section_offset
        sh = _SECHDR_FMT.unpack_from(buf, base)
        (magic, section_size, n_rows, n_cols, col_desc_offset, data_offset,
         payload_crc32c, flags, kind) = sh[:9]
        if magic != _SECTION_MAGIC:
            raise ValueError(f"section {name!r}: bad section magic")
        if (section_size != de.section_size or n_rows != de.n_rows or
                n_cols != de.n_cols or kind != de.kind or
                col_desc_offset != de.col_desc_offset):
            raise ValueError(f"section {name!r}: descriptor disagreement")
        if n_cols == 0:
            raise ValueError(f"section {name!r}: section has no columns")
        desc_bytes = n_cols * _COLDESC_FMT.size
        if (col_desc_offset < _SECHDR_FMT.size or col_desc_offset > section_size or
                desc_bytes > section_size - col_desc_offset):
            raise ValueError(f"section {name!r}: column descriptors out of bounds")

        cols: dict[str, _ColumnDescriptor] = {}
        for c in range(n_cols):
            raw = _COLDESC_FMT.unpack_from(buf, base + col_desc_offset + c * _COLDESC_FMT.size)
            (data_off, data_size, aux_off, aux_size, aux_count, dtype,
             _reserved_u8, name_len, name_bytes, _unit) = raw
            if name_len == 0 or name_len > 40:
                raise ValueError(f"section {name!r}: column name length out of bounds")
            if dtype > _DICTSTR:
                raise ValueError(f"section {name!r}: invalid column dtype")
            if data_off % _COLUMN_ALIGN != 0:
                raise ValueError(f"section {name!r}: column data misaligned")
            if data_off > section_size or data_size > section_size - data_off:
                raise ValueError(f"section {name!r}: column data out of bounds")
            elem = _DTYPE_SIZE[dtype]
            if data_size % elem != 0 or data_size // elem != n_rows:
                raise ValueError(f"section {name!r}: column size disagrees with n_rows")
            col_name = name_bytes[:name_len].decode("utf-8")

            if dtype in (_DICTSTR, _U8ENUM):
                if aux_off == 0 or aux_off % 4 != 0:
                    raise ValueError(f"section {name!r}: aux table missing or misaligned")
                if aux_off > section_size or aux_size > section_size - aux_off:
                    raise ValueError(f"section {name!r}: aux table out of bounds")
                offsets_bytes = (aux_count + 1) * 4
                if offsets_bytes > aux_size:
                    raise ValueError(f"section {name!r}: aux offsets out of bounds")
                blob_bytes = aux_size - offsets_bytes
                offsets = struct.unpack_from(f"<{aux_count + 1}I", buf, base + aux_off)
                if offsets[0] != 0:
                    raise ValueError(f"section {name!r}: aux table does not start at 0")
                prev = 0
                for k in range(1, aux_count + 1):
                    if offsets[k] < prev:
                        raise ValueError(f"section {name!r}: aux offsets not monotone")
                    prev = offsets[k]
                if prev != blob_bytes:
                    raise ValueError(f"section {name!r}: aux blob size disagreement")
                # Every code must index the table (accessors are then unchecked).
                if n_rows:
                    code_dt = _NP_DTYPE[dtype]
                    codes = np.frombuffer(buf, dtype=code_dt, count=n_rows,
                                          offset=base + data_off)
                    if int(codes.max()) >= aux_count:
                        raise ValueError(f"section {name!r}: code out of range")
            elif aux_off != 0 or aux_size != 0 or aux_count != 0:
                raise ValueError(f"section {name!r}: unexpected aux table")

            cols[col_name] = _ColumnDescriptor(
                data_offset=data_off, data_size=data_size, aux_offset=aux_off,
                aux_size=aux_size, aux_count=aux_count, dtype=dtype,
                name_len=name_len, name=col_name,
            )
        return Section(buf, base, name, kind, n_rows, cols)

    # -- lazy integrity -------------------------------------------------------

    def validate_section(self, name: str) -> None:
        """Verify one section's payload CRC against the header and directory.

        Raises ``KeyError`` if absent, ``ValueError`` on mismatch.
        """
        de = self._by_name.get(name)
        if de is None:
            raise KeyError(f"RunArchive: no section {name!r}")
        base = de.section_offset
        stored = struct.unpack_from("<I", self._buf, base + 36)[0]  # payload_crc32c
        if stored != de.payload_crc32c:
            raise ValueError(
                f"section {name!r}: section/directory checksum disagreement"
            )
        sec = bytearray(self._buf[base:base + de.section_size])
        sec[36:40] = b"\x00\x00\x00\x00"  # zero payload_crc32c before recompute
        if _crc32c(sec) != stored:
            raise ValueError(f"section {name!r}: payload checksum mismatch")

    def validate_all(self) -> None:
        """Verify every section's payload CRC. Raises ``ValueError`` on any miss."""
        for de in self._dir:
            self.validate_section(de.name)

    # -- lifecycle ------------------------------------------------------------

    def close(self) -> None:
        """Release the mapping and the file handle.

        Closing the mapping invalidates outstanding zero-copy views. If numpy
        views are still exported over the mmap, ``mmap.close()`` raises
        ``BufferError`` and the mapping is *retained* for them — ``_mm`` stays
        set so a later ``close()`` (after the views are dropped) releases it. The
        OS file handle, however, is released unconditionally in a ``finally``:
        the mapping stays valid independently of the descriptor (on both POSIX
        and Windows), so the handle is never leaked, even on the BufferError
        path. ``close()`` is idempotent.
        """
        try:
            if self._mm is not None:
                self._mm.close()
                self._mm = None
        except BufferError:
            # numpy views still export the mapping; keep it mapped for them and
            # let a later close() release it once they are gone. Fall through to
            # release the file handle regardless — do not leak it.
            pass
        finally:
            if self._fh is not None:
                self._fh.close()
                self._fh = None

    def __enter__(self) -> "RunArchive":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# ── High-level shim: backtest section -> BacktestResult-like ─────────────────

@dataclass
class BacktestSection:
    """A ``BacktestResult``-like view of one backtest time-series section.

    Exposes ``date`` (list[str]), ``ts_ns`` (list[int]) and every registry F64
    series as an attribute (e.g. ``.nav``, ``.pnl_vega``) — the shape the report
    layer consumes — without importing the compiled binding. Dynamically
    appended per-signal columns are returned separately by
    :func:`read_backtest_section`.
    """

    date: list[str]
    ts_ns: list[int]
    series: dict[str, np.ndarray] = field(default_factory=dict)

    def __post_init__(self) -> None:
        for name, arr in self.series.items():
            setattr(self, name, arr)

    def size(self) -> int:
        return len(self.date)

    def __len__(self) -> int:
        return len(self.date)


def read_backtest_section(archive: RunArchive, name: str = "backtest"):
    """Read a backtest section as ``(result, meta, extra)``.

    The drop-in shape of ``io.read_backtest_tsv``:

    * ``result`` — a :class:`BacktestSection` carrying ``date`` / ``ts_ns`` and
      every registry F64 series as an attribute;
    * ``meta``   — the ``meta`` key/value section as ``dict[str, str]`` (empty if
      the archive has no ``meta`` section), matched by key name;
    * ``extra``  — any non-registry columns (the per-signal series appended at
      write time) as ``{name: numpy.ndarray}``.
    """
    sec = archive.section(name)

    # Registry column names for this section (date, ts_ns, then the F64 series).
    reg = next((cols for sname, _kind, cols in _schema.SECTIONS if sname == name), None)
    if reg is None:
        raise KeyError(f"read_backtest_section: {name!r} is not a registry section")
    reg_names = {cname for cname, _dt, _unit in reg}

    date = sec.dict("date").tolist()
    ts_ns = [int(v) for v in sec.i64("ts_ns")]
    series: dict[str, np.ndarray] = {}
    for cname, dtype, _unit in reg:
        if dtype == _F64:
            series[cname] = sec.f64(cname)

    result = BacktestSection(date=date, ts_ns=ts_ns, series=series)

    meta: dict[str, str] = {}
    if "meta" in archive:
        mv = archive.section("meta")
        keys = mv.dict("key")
        values = mv.dict("value")
        meta = {keys.at(i): values.at(i) for i in range(mv.n_rows)}

    extra: dict[str, np.ndarray] = {}
    for cname in sec.columns:
        if cname in reg_names:
            continue
        # Appended signal series are F64 like the registry series.
        extra[cname] = sec.f64(cname)

    return result, meta, extra
