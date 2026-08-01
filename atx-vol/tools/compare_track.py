"""Compare two backtest tracks under the replay sprint's economic-parity band.

Usage::

    python compare_track.py GOLDEN CANDIDATE

Byte-identical files pass immediately.  Otherwise the tabular header must agree
exactly, exact columns must agree cell-for-cell, and floating-point columns may
drift by at most 1e-9 relative to a nonzero golden value or 1e-12 absolute when
the golden value is zero. Comment metadata is outside the table gate because the
shipped track preamble contains result-derived floating-point summaries.

This tool intentionally depends only on the standard library.  It is used to
gate native build variants and must remain runnable even when ``atxvol._core``
has not been built in the active worktree.
"""

from __future__ import annotations

import argparse
import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


REL_TOL = 1.0e-9
ZERO_ABS_TOL = 1.0e-12


class TrackError(ValueError):
    """A malformed or unreadable input track."""


@dataclass(frozen=True)
class Track:
    metadata: tuple[str, ...]
    header: tuple[str, ...]
    rows: tuple[tuple[str, ...], ...]


@dataclass
class Drift:
    max_abs: float = 0.0
    max_rel: float = 0.0


def _read_bytes(path: Path, role: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise TrackError(f"cannot read {role} {path}: {exc}") from None


def _parse(raw: bytes, role: str) -> Track:
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise TrackError(f"{role} is not UTF-8: {exc}") from None

    lines = text.splitlines()
    metadata: list[str] = []
    cursor = 0
    while cursor < len(lines) and lines[cursor].startswith("#"):
        metadata.append(lines[cursor])
        cursor += 1

    if cursor >= len(lines):
        raise TrackError(f"{role} has no TSV header")

    header = tuple(lines[cursor].split("\t"))
    cursor += 1
    if not header or any(not name for name in header):
        raise TrackError(f"{role} has an empty column name")
    if len(set(header)) != len(header):
        raise TrackError(f"{role} has duplicate column names")

    rows: list[tuple[str, ...]] = []
    for row_number, line in enumerate(lines[cursor:], start=1):
        cells = tuple(line.split("\t"))
        if len(cells) != len(header):
            raise TrackError(
                f"{role} row={row_number} has {len(cells)} cell(s), "
                f"expected {len(header)}"
            )
        rows.append(cells)
    return Track(tuple(metadata), header, tuple(rows))


def _float_lexeme(value: str) -> bool:
    """The plan's type rule: parseable as float and visibly non-integral.

    Classification uses the golden column only.  Letting candidate formatting
    reclassify an integer/count column would turn ``1`` -> ``1.0`` into an
    accidental parity pass, defeating the exact-column contract.
    """

    folded = value.lower()
    if "." not in folded and "e" not in folded:
        return False
    try:
        float(value)
    except ValueError:
        return False
    return True


def _fmt(value: float) -> str:
    return f"{value:.17g}"


def compare(golden: Track, candidate: Track) -> tuple[bool, list[str]]:
    """Return ``(passed, output_lines)`` for two parsed non-byte-equal tracks."""

    # The parity contract governs the TSV table, not its comment preamble.  The
    # shipped track metadata includes result-derived final_nav/total_return, so
    # requiring byte-equal metadata would reject the exact ULP-class data drift
    # this tool exists to certify before it ever examined the numeric columns.
    if golden.header != candidate.header:
        return False, ["FAIL header mismatch"]
    if len(golden.rows) != len(candidate.rows):
        return False, [
            f"FAIL row count golden={len(golden.rows)} candidate={len(candidate.rows)}"
        ]

    is_float = [
        any(_float_lexeme(row[column]) for row in golden.rows)
        for column in range(len(golden.header))
    ]
    drift = {name: Drift() for name, numeric in zip(golden.header, is_float) if numeric}

    max_abs = 0.0
    max_rel = 0.0
    worst_col = "-"
    worst_normalized = -1.0

    for row_number, (golden_row, candidate_row) in enumerate(
        zip(golden.rows, candidate.rows), start=1
    ):
        for column, name in enumerate(golden.header):
            reference_text = golden_row[column]
            candidate_text = candidate_row[column]
            if not is_float[column]:
                if reference_text != candidate_text:
                    return False, [
                        f"FAIL exact mismatch col={name} row={row_number} "
                        f"golden={reference_text!r} candidate={candidate_text!r}"
                    ]
                continue

            # Identical spelling is exact even for a non-finite diagnostic
            # sentinel.  A differently-spelled non-finite value is not a
            # bounded numeric drift and fails below.
            if reference_text == candidate_text:
                reference = candidate_value = 0.0
            else:
                try:
                    reference = float(reference_text)
                    candidate_value = float(candidate_text)
                except ValueError:
                    return False, [
                        f"FAIL nonnumeric float col={name} row={row_number} "
                        f"golden={reference_text!r} candidate={candidate_text!r}"
                    ]
                if not math.isfinite(reference) or not math.isfinite(candidate_value):
                    return False, [
                        f"FAIL nonfinite drift col={name} row={row_number} "
                        f"golden={reference_text!r} candidate={candidate_text!r}"
                    ]

            absolute = abs(candidate_value - reference)
            if reference == 0.0:
                relative = 0.0
                normalized = absolute / ZERO_ABS_TOL
                within = absolute <= ZERO_ABS_TOL
                limit = f"abs<={_fmt(ZERO_ABS_TOL)}"
            else:
                relative = absolute / abs(reference)
                normalized = relative / REL_TOL
                within = relative <= REL_TOL
                limit = f"rel<={_fmt(REL_TOL)}"

            column_drift = drift[name]
            column_drift.max_abs = max(column_drift.max_abs, absolute)
            column_drift.max_rel = max(column_drift.max_rel, relative)
            max_abs = max(max_abs, absolute)
            max_rel = max(max_rel, relative)
            if normalized > worst_normalized:
                worst_normalized = normalized
                worst_col = name

            if not within:
                return False, [
                    f"FAIL float drift col={name} row={row_number} "
                    f"golden={reference_text!r} candidate={candidate_text!r} "
                    f"abs={_fmt(absolute)} rel={_fmt(relative)} limit={limit}"
                ]

    lines = [
        f"PASS parity max_rel={_fmt(max_rel)} max_abs={_fmt(max_abs)} "
        f"worst_col={worst_col}"
    ]
    lines.extend(
        f"COL {name} max_rel={_fmt(values.max_rel)} max_abs={_fmt(values.max_abs)}"
        for name, values in drift.items()
    )
    return True, lines


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare backtest track TSVs under the economic-parity band."
    )
    parser.add_argument("golden", help="reference track.tsv")
    parser.add_argument("candidate", help="candidate track.tsv")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    golden_path = Path(os.path.abspath(args.golden))
    candidate_path = Path(os.path.abspath(args.candidate))
    try:
        golden_raw = _read_bytes(golden_path, "golden")
        candidate_raw = _read_bytes(candidate_path, "candidate")
        if golden_raw == candidate_raw:
            print("PASS byte")
            return 0
        golden = _parse(golden_raw, "golden")
        candidate = _parse(candidate_raw, "candidate")
        passed, lines = compare(golden, candidate)
    except TrackError as exc:
        print(f"FAIL {exc}")
        return 1

    for line in lines:
        print(line)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
