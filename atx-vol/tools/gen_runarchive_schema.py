#!/usr/bin/env python3
"""Generate ``atxvol/report/_schema.py`` from ``run_archive_schema.hpp``.

The C++ header ``atx-vol/include/atx/vol/run_archive_schema.hpp`` is the single
source of truth for the RunArchive (ATXRUN01) column registry and its
``ra_schema_hash()``. The pure-Python reader must recompute the same schema hash
to reject a drifted archive at open, so this script parses that header and emits
a small ``_schema.py`` carrying:

  * the format identity (magic / major / minor / schema salt),
  * the registry as plain data (section name, kind code, column tuples), and
  * a byte-for-byte port of the FNV-1a-64 fold in ``ra_schema_hash()``.

The generated hash is asserted equal to the golden pin before the file is
written, so a registry change in the header that was not mirrored here fails
loudly at generation time rather than silently at open.

Regenerate::

    python atx-vol/tools/gen_runarchive_schema.py

Run with ``--check`` to fail (nonzero exit) if the committed ``_schema.py`` is
stale relative to the header, without rewriting it.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# RaDType / RaSectionKind numeric codes (run_archive_schema.hpp).
_DTYPE = {"F64": 0, "I64": 1, "U32": 2, "U8Enum": 3, "DictStr": 4}
_KIND = {"ScalarKV": 0, "TimeSeries": 1, "SubTable": 2}

_GOLDEN_SCHEMA_HASH = 0xDCCE47781AC8390D

_HERE = pathlib.Path(__file__).resolve()
_HEADER = _HERE.parents[1] / "include" / "atx" / "vol" / "run_archive_schema.hpp"
_OUT = _HERE.parents[1] / "python" / "src" / "atxvol" / "report" / "_schema.py"


def _parse_header(src: str):
    """Return (magic_bytes, major, minor, salt, sections) parsed from the header."""
    # Format identity.
    magic_chars = re.search(
        r"kRaMagic\[8\]\s*=\s*\{([^}]*)\}", src
    )
    magic = bytes(
        ord(c) for c in re.findall(r"'(.)'", magic_chars.group(1))
    )
    major = int(re.search(r"kRaMajor\s*=\s*(\d+)", src).group(1))
    minor = int(re.search(r"kRaMinor\s*=\s*(\d+)", src).group(1))
    salt = int(re.search(r"kRaSchemaSalt\s*=\s*(0x[0-9a-fA-F]+)", src).group(1), 16)

    # Column arrays: `inline constexpr RaColumn kXxxCols[] = { {"n", RaDType::T, "u"}, ... };`
    col_arrays: dict[str, list[tuple[str, int, str]]] = {}
    for m in re.finditer(r"RaColumn\s+(k\w+)\s*\[\]\s*=\s*\{(.*?)\};", src, re.S):
        arr, body = m.group(1), m.group(2)
        cols = [
            (cm.group(1), _DTYPE[cm.group(2)], cm.group(3))
            for cm in re.finditer(
                r'\{\s*"([^"]*)"\s*,\s*RaDType::(\w+)\s*,\s*"([^"]*)"\s*\}', body
            )
        ]
        col_arrays[arr] = cols

    # Registry: `inline constexpr RaSection kRaSections[] = { {"n", RaSectionKind::K, kArr}, ... };`
    reg = re.search(r"RaSection\s+kRaSections\s*\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if reg is None:
        raise SystemExit("gen_runarchive_schema: kRaSections table not found")
    sections = []
    for m in re.finditer(
        r'\{\s*"([^"]*)"\s*,\s*RaSectionKind::(\w+)\s*,\s*(k\w+)\s*\}', reg.group(1)
    ):
        name, kind, arr = m.group(1), _KIND[m.group(2)], m.group(3)
        if arr not in col_arrays:
            raise SystemExit(f"gen_runarchive_schema: unknown column array {arr}")
        sections.append((name, kind, col_arrays[arr]))
    if not sections:
        raise SystemExit("gen_runarchive_schema: empty registry")
    return magic, major, minor, salt, sections


def _schema_hash(salt: int, sections) -> int:
    """Port of ra_schema_hash() (FNV-1a-64 fold, ASCII field/record/group seps)."""
    mask = 0xFFFFFFFFFFFFFFFF
    prime = 0x100000001B3

    def fb(h: int, b: int) -> int:
        return ((h ^ b) * prime) & mask

    def fbytes(h: int, s: str) -> int:
        for c in s.encode("ascii"):
            h = fb(h, c)
        return h

    h = 0xCBF29CE484222325
    for i in range(8):  # fnv1a_u64(salt), little-endian bytes
        h = fb(h, (salt >> (8 * i)) & 0xFF)
    for name, kind, cols in sections:
        h = fbytes(h, name)
        h = fb(h, 0x1F)
        h = fb(h, kind)
        for cname, dtype, unit in cols:
            h = fbytes(h, cname)
            h = fb(h, 0x1F)
            h = fb(h, dtype)
            h = fbytes(h, unit)
            h = fb(h, 0x1E)
        h = fb(h, 0x1D)
    return h


def _render(magic: bytes, major: int, minor: int, salt: int, sections) -> str:
    lines: list[str] = []
    w = lines.append
    w('"""RunArchive (ATXRUN01) column registry — GENERATED, do not edit by hand.')
    w("")
    w("Source of truth: atx-vol/include/atx/vol/run_archive_schema.hpp")
    w("Regenerate:      python atx-vol/tools/gen_runarchive_schema.py")
    w("")
    w("Carries the format identity and the column registry as plain data, plus a")
    w("byte-for-byte port of the C++ ``ra_schema_hash()`` FNV-1a-64 fold so the")
    w("pure-Python reader can pin a file's schema at open. No third-party imports.")
    w('"""')
    w("")
    w("from __future__ import annotations")
    w("")
    w(f"RA_MAGIC = {magic!r}")
    w(f"RA_MAJOR = {major}")
    w(f"RA_MINOR = {minor}")
    w(f"RA_SCHEMA_SALT = 0x{salt:016X}")
    w("")
    w("# RaDType numeric codes (run_archive_schema.hpp).")
    w("F64, I64, U32, U8ENUM, DICTSTR = 0, 1, 2, 3, 4")
    w("# RaSectionKind numeric codes.")
    w("SCALARKV, TIMESERIES, SUBTABLE = 0, 1, 2")
    w("")
    w("# Registry: (section_name, kind_code, ((col_name, dtype_code, unit), ...)).")
    w("# Order and contents mirror kRaSections exactly and are load-bearing for the")
    w("# schema hash.")
    w("SECTIONS = (")
    for name, kind, cols in sections:
        w(f"    ({name!r}, {kind}, (")
        for cname, dtype, unit in cols:
            w(f"        ({cname!r}, {dtype}, {unit!r}),")
        w("    )),")
    w(")")
    w("")
    w("")
    w("def ra_schema_hash() -> int:")
    w('    """FNV-1a-64 fold over the registry, salted, mirroring the C++ pin."""')
    w("    mask = 0xFFFFFFFFFFFFFFFF")
    w("    prime = 0x100000001B3")
    w("")
    w("    def fb(h: int, b: int) -> int:")
    w("        return ((h ^ b) * prime) & mask")
    w("")
    w("    def fbytes(h: int, s: str) -> int:")
    w('        for c in s.encode("ascii"):')
    w("            h = fb(h, c)")
    w("        return h")
    w("")
    w("    h = 0xCBF29CE484222325")
    w("    for i in range(8):  # fnv1a_u64(RA_SCHEMA_SALT), little-endian")
    w("        h = fb(h, (RA_SCHEMA_SALT >> (8 * i)) & 0xFF)")
    w("    for name, kind, cols in SECTIONS:")
    w("        h = fbytes(h, name)")
    w("        h = fb(h, 0x1F)")
    w("        h = fb(h, kind)")
    w("        for cname, dtype, unit in cols:")
    w("            h = fbytes(h, cname)")
    w("            h = fb(h, 0x1F)")
    w("            h = fb(h, dtype)")
    w("            h = fbytes(h, unit)")
    w("            h = fb(h, 0x1E)")
    w("        h = fb(h, 0x1D)")
    w("    return h")
    w("")
    w("")
    w("RA_SCHEMA_HASH = ra_schema_hash()")
    w("")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed _schema.py is stale (do not write)")
    args = ap.parse_args(argv)

    src = _HEADER.read_text(encoding="utf-8")
    magic, major, minor, salt, sections = _parse_header(src)

    got = _schema_hash(salt, sections)
    if got != _GOLDEN_SCHEMA_HASH:
        raise SystemExit(
            f"gen_runarchive_schema: parsed registry hashes to 0x{got:016x}, "
            f"golden is 0x{_GOLDEN_SCHEMA_HASH:016x} — header parse drifted"
        )

    rendered = _render(magic, major, minor, salt, sections)

    if args.check:
        current = _OUT.read_text(encoding="utf-8") if _OUT.exists() else ""
        if current != rendered:
            print(f"{_OUT} is stale; rerun gen_runarchive_schema.py", file=sys.stderr)
            return 1
        print("_schema.py is up to date")
        return 0

    _OUT.write_text(rendered, encoding="utf-8")
    print(f"wrote {_OUT} (schema_hash 0x{got:016x}, {len(sections)} sections)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
