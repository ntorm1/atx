#!/usr/bin/env python3
"""Generate the GENERATED REGION of ``atxvol/report/_schema.py`` from
``run_archive_schema.hpp``.

The C++ header ``atx-vol/include/atx/vol/detail/run_archive_schema.hpp`` is the single
source of truth for the RunArchive (ATXRUN01) column registry and its
``ra_schema_hash()``. The pure-Python reader must recompute the same schema hash
to reject a drifted archive at open, so this script parses that header and emits
a small block carrying:

  * the format identity (magic / major / minor / schema salt),
  * the registry as plain data (section name, kind code, column tuples), and
  * a byte-for-byte port of the FNV-1a-64 fold in ``ra_schema_hash()``.

The generated hash is asserted equal to the golden pin before anything is
written, so a registry change in the header that was not mirrored here fails
loudly at generation time rather than silently at open.

WHY THIS IS A *REGION* AND NOT THE WHOLE FILE (RECONCILE 3, 2026-07-25).
``_schema.py`` carries two things this script does not and cannot produce:

  * the module docstring's PROVENANCE narrative, and
  * the trailing ``COLUMN_NOTES`` table, which documents the ``gross_vega`` unit
    hazard -- one column name over two units 100x apart plus a gross/net flip.
    That is documentation of the *same* hazard family that produced the E1
    100x book-sizing defect caught at the main -> feat/pipeline-m merge.

When the header and this script landed in-tree, FIX-5/I5's anti-drift guard
flipped from its dormant branch to a live ``--check`` with no edit, exactly as
designed -- and ``--check`` failed. It was RIGHT to fail and it was right not to
be silenced by regenerating: the registry was proven byte-identical, and a
regenerate-everything script would have DELETED both hand-maintained regions to
turn the check green.

So the file is now explicitly partitioned. Everything between the BEGIN/END
markers is owned by this script and is what ``--check`` compares; everything
outside them is preserved verbatim through regeneration and is not compared.

The boundary is MACHINE-CHECKED, not a convention -- three ways, because a guard
that cannot fire is worse than no guard:

  1. Missing, duplicated, or inverted markers are a hard error, so deleting the
     markers cannot silently turn ``--check`` into a no-op.
  2. The comparison is over the exact rendered block, so moving the END marker
     upward (which would smuggle generated content into the "preserved" region)
     truncates the block and fails the compare.
  3. No name this script owns -- ``RA_MAGIC``, ``RA_MAJOR``, ``RA_MINOR``,
     ``RA_SCHEMA_SALT``, ``SECTIONS``, ``RA_SCHEMA_HASH``, ``ra_schema_hash``,
     and the dtype/kind codes -- may be assigned or defined in the preserved
     region. Otherwise the epilogue could shadow the generated registry with a
     value the header never produced and ``--check`` would still pass.

Regenerate::

    python atx-vol/tools/gen_runarchive_schema.py

Run with ``--check`` to fail (nonzero exit) if the committed generated region is
stale relative to the header, without rewriting anything. ``--header`` and
``--out`` override the two paths so a test can drive the whole thing over
throwaway copies -- which is how the "real drift still fails" property is
proven (``test_runarchive.py``).
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
_HEADER = _HERE.parents[1] / "include" / "atx" / "vol" / "detail" / "run_archive_schema.hpp"
_OUT = _HERE.parents[1] / "python" / "src" / "atxvol" / "report" / "_schema.py"

# The region markers. ASCII only and matched on the whole stripped line, so a
# reflow or an editor's trailing whitespace cannot half-match one.
_BEGIN = "# --- BEGIN GENERATED: gen_runarchive_schema.py <- run_archive_schema.hpp ---"
_END = "# --- END GENERATED ---"

# Names this script owns. Assigning or defining any of them outside the
# generated region would let the preserved region override the parsed registry
# while --check still passed, so it is refused. (Property 3 in the docstring.)
_OWNED_NAMES = frozenset({
    "RA_MAGIC", "RA_MAJOR", "RA_MINOR", "RA_SCHEMA_SALT",
    "SECTIONS", "RA_SCHEMA_HASH", "ra_schema_hash",
    "F64", "I64", "U32", "U8ENUM", "DICTSTR",
    "SCALARKV", "TIMESERIES", "SUBTABLE",
})

_DEF = re.compile(r"^\s*(?:def|class)\s+(\w+)")
_ASSIGN = re.compile(r"^\s*([A-Za-z_][\w\s,]*?)\s*=(?!=)")
_IDENT = re.compile(r"^[A-Za-z_]\w*$")


class RegionError(Exception):
    """The committed file's generated region is not machine-locatable."""


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


def _render_region(magic: bytes, major: int, minor: int, salt: int, sections) -> str:
    """The GENERATED REGION, markers included. Nothing outside it is this
    script's business."""
    lines: list[str] = []
    w = lines.append
    w(_BEGIN)
    w("# Everything between these markers is machine-written from the C++ header and")
    w("# is what `gen_runarchive_schema.py --check` compares. Do not edit it by hand;")
    w("# edit run_archive_schema.hpp and regenerate. Text OUTSIDE the markers (the")
    w("# module docstring above, COLUMN_NOTES below) is hand-maintained and preserved")
    w("# verbatim across regeneration.")
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
    w(_END)
    return "\n".join(lines)


def _default_preamble() -> str:
    """Only used when ``_schema.py`` does not exist yet. The committed file's own
    preamble is preserved instead, because it carries the provenance narrative
    this script cannot reproduce."""
    return (
        '"""RunArchive (ATXRUN01) column registry.\n'
        "\n"
        "Carries the format identity and the column registry as plain data, plus a\n"
        "byte-for-byte port of the C++ ``ra_schema_hash()`` FNV-1a-64 fold so the\n"
        "pure-Python reader can pin a file's schema at open. No third-party imports.\n"
        "\n"
        "The block between the BEGIN/END GENERATED markers is written by\n"
        "atx-vol/tools/gen_runarchive_schema.py from\n"
        "atx-vol/include/atx/vol/detail/run_archive_schema.hpp. Everything outside those\n"
        "markers is hand-maintained and survives regeneration.\n"
        '"""\n'
        "\n"
    )


def _split(text: str) -> tuple[str, str, str]:
    """Split ``text`` into (preamble, generated_region, epilogue).

    The region INCLUDES its two marker lines. Raises ``RegionError`` -- never
    returns a "best effort" split -- if the markers are absent, duplicated, or
    inverted, because every one of those states would otherwise degrade
    ``--check`` into something that cannot fail.
    """
    lines = text.splitlines()
    begins = [i for i, ln in enumerate(lines) if ln.strip() == _BEGIN]
    ends = [i for i, ln in enumerate(lines) if ln.strip() == _END]
    if len(begins) != 1 or len(ends) != 1:
        raise RegionError(
            f"expected exactly one {_BEGIN!r} line and one {_END!r} line, "
            f"found {len(begins)} and {len(ends)}. The generated region is what "
            "--check compares; without intact markers the check cannot fire at all."
        )
    if ends[0] <= begins[0]:
        raise RegionError(
            f"END marker at line {ends[0] + 1} is not after BEGIN at line "
            f"{begins[0] + 1}"
        )
    pre = "\n".join(lines[: begins[0]])
    region = "\n".join(lines[begins[0] : ends[0] + 1])
    post = "\n".join(lines[ends[0] + 1 :])
    if pre:
        pre += "\n"
    if post:
        post += "\n"
    return pre, region, post


def _assigned_names(line: str) -> list[str]:
    m = _DEF.match(line)
    if m:
        return [m.group(1)]
    m = _ASSIGN.match(line)
    if not m:
        return []
    return [t.strip() for t in m.group(1).split(",") if _IDENT.match(t.strip())]


def _check_preserved_region(pre: str, post: str) -> list[str]:
    """Names this script owns that the PRESERVED region assigns or defines."""
    offenders: list[str] = []
    for chunk in (pre, post):
        for line in chunk.splitlines():
            for name in _assigned_names(line):
                if name in _OWNED_NAMES:
                    offenders.append(name)
    return offenders


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate _schema.py's generated region.")
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed generated region is stale (do not write)")
    ap.add_argument("--header", type=pathlib.Path, default=_HEADER,
                    help="override the C++ header path (tests drive throwaway copies)")
    ap.add_argument("--out", type=pathlib.Path, default=_OUT,
                    help="override the _schema.py path (tests drive throwaway copies)")
    args = ap.parse_args(argv)

    src = args.header.read_text(encoding="utf-8")
    magic, major, minor, salt, sections = _parse_header(src)

    got = _schema_hash(salt, sections)
    if got != _GOLDEN_SCHEMA_HASH:
        raise SystemExit(
            f"gen_runarchive_schema: parsed registry hashes to 0x{got:016x}, "
            f"golden is 0x{_GOLDEN_SCHEMA_HASH:016x} — header parse drifted"
        )

    region = _render_region(magic, major, minor, salt, sections)

    if args.check:
        if not args.out.exists():
            print(f"{args.out} does not exist", file=sys.stderr)
            return 1
        current = args.out.read_text(encoding="utf-8")
        try:
            pre, current_region, post = _split(current)
        except RegionError as exc:
            print(f"{args.out}: {exc}", file=sys.stderr)
            return 1
        offenders = _check_preserved_region(pre, post)
        if offenders:
            print(
                f"{args.out}: the preserved (hand-maintained) region assigns "
                f"generator-owned name(s) {sorted(set(offenders))}, which would "
                "override the generated registry without --check noticing",
                file=sys.stderr,
            )
            return 1
        if current_region != region:
            print(f"{args.out} generated region is stale; rerun "
                  "gen_runarchive_schema.py", file=sys.stderr)
            return 1
        print(f"_schema.py generated region is up to date "
              f"(schema_hash 0x{got:016x}, {len(sections)} sections)")
        return 0

    if args.out.exists():
        current = args.out.read_text(encoding="utf-8")
        try:
            pre, _old_region, post = _split(current)
        except RegionError as exc:
            # Refuse to guess. Overwriting here is exactly the "regenerate and
            # lose the hand-maintained notes" failure this partition exists to
            # prevent, so it is an error rather than a fallback to full write.
            raise SystemExit(
                f"gen_runarchive_schema: {args.out}: {exc}\n"
                "Refusing to overwrite: without markers this script cannot tell "
                "which text is hand-maintained, and a full rewrite would delete it."
            )
    else:
        pre, post = _default_preamble(), ""

    args.out.write_text(pre + region + ("\n" + post if post else "\n"), encoding="utf-8")
    print(f"wrote {args.out} generated region "
          f"(schema_hash 0x{got:016x}, {len(sections)} sections); "
          f"{len(pre.splitlines())} preamble + {len(post.splitlines())} epilogue "
          "lines preserved")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
