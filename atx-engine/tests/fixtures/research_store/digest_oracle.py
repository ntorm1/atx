"""Test-only oracle of the record digest ``atx.record-digest/v1`` (sql-design section 3.5).

Product code computes digests in C++ only (atx-engine research/store DigestStream); this oracle exists to produce
the golden vectors the gtest ``ResearchStoreDigest.GoldenVectors`` checks, the way the T1 ``eval_tie`` fixtures do.
Input ``golden_rows.json``: a list of rows ``{name, table, version, columns: [{name, type, ...}]}`` where the value
is ``value`` (int, bool, text kinds, or null) or ``hex`` (u64 as 16 hex digits, blob as hex bytes) or ``bits``
(real as the 16 hex digits of its IEEE-754 bits, so NaN payloads and -0.0 are exact).
Output ``golden_digests.json``: ``{name: digest}`` in row order.

Usage: python digest_oracle.py [--write]   (prints the digests; --write replaces golden_digests.json)
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
DOMAIN = b"atx.record-digest/v1\n"
TEXT_KINDS = ("text", "sha256", "relpath", "json")


def column_line(column: dict) -> bytes:
    name = column["name"].encode("utf-8")
    kind = column["type"]
    if "value" in column and column["value"] is None:
        return name + b"=~\n"
    if kind == "int":
        return name + b"=i" + str(int(column["value"])).encode("ascii") + b"\n"
    if kind == "bool":
        return name + (b"=b1\n" if column["value"] else b"=b0\n")
    if kind == "real":
        return name + b"=r" + column["bits"].lower().encode("ascii") + b"\n"
    if kind == "u64":
        return name + b"=u" + column["hex"].lower().rjust(16, "0").encode("ascii") + b"\n"
    if kind in TEXT_KINDS:
        data = column["value"].encode("utf-8")
        return name + b"=t" + str(len(data)).encode("ascii") + b":" + data + b"\n"
    if kind == "blob":
        data = bytes.fromhex(column["hex"])
        return name + b"=x" + str(len(data)).encode("ascii") + b":" + data + b"\n"
    raise ValueError(f"unknown column type {kind!r}")


def encode(row: dict) -> bytes:
    out = DOMAIN + f"row {row['table']}@{row['version']}\n".encode("utf-8")
    for column in row["columns"]:
        out += column_line(column)
    return out


def digest(row: dict) -> str:
    return hashlib.sha256(encode(row)).hexdigest()


def digests(rows: list[dict]) -> dict:
    return {row["name"]: digest(row) for row in rows}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args(argv)
    rows = json.loads((HERE / "golden_rows.json").read_text(encoding="utf-8"))
    text = json.dumps(digests(rows), indent=2) + "\n"
    if args.write:
        (HERE / "golden_digests.json").write_bytes(text.encode("ascii"))
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
