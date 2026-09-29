"""Stage I: dated ``security_id -> cik`` links (rule ``r4-links-asof-v1``).

Source: the identity rehearsal r4 export (``session8-phased-r4``), read-only.
Output: ``identity/links.parquet`` under the build root plus ``identity/manifest.json``.

Rule ``r4-links-asof-v1`` (see ``docs/ALPHA_PANEL_FUNDAMENTALS.md``). For every calendar
date ``d`` before the seal, with ``cutoff = d 22:00`` naive UTC:

1. a link version is visible iff ``available_at <= cutoff < valid_until`` (NULL open) and
   ``link_start <= d <= link_end`` (NULL open); versions with ``available_at >= seal`` are
   ignored and every day is ``<= seal - 1 day``;
2. a line visible under more than one company that day (counting every visible version,
   whatever its tier, basis or primary flag) is ambiguous and unlinked that day;
3. keep versions with tier in (high, medium) or basis in (strict_dated, current_ticker_verified);
   then drop primary ``N`` rows and basis ``current_ticker_verified`` (those start at the
   2026-09-20 snapshot and never backfill);
4. the primary flag is re-derived per company per day: ``P`` for the minimum securityID among
   the company's remaining visible lines, ``J`` for the others.

The per-day state ``(cik, primary, tier, basis, available_at)`` -- ``available_at`` being the
maximum ``available_at`` of the versions behind the day -- is compressed into maximal constant
intervals ``[start, end_incl]``. Because ``available_at`` is part of the state, every interval
satisfies ``available_at <= start 22:00``.

The computation is exact and interval based (elementary segments between version
boundaries), so no per-day expansion is materialised.

Usage (under the memory guard)::

    python -m atx_db.alpha_panel.identity_links build [--seal 2026-09-21]
    python -m atx_db.alpha_panel.identity_links validate      # seal 2025-01-01 reference numbers
    python -m atx_db.alpha_panel.identity_links attach-receipt --receipt <guard receipt.json>
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import sys
import time
from collections.abc import Iterable, Iterator
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from . import common

RULE = "r4-links-asof-v1"
CODE_VERSION = "identity-links-v1"
R4_EXPORT = common.IDENTITY_R4_DIR / "phases" / "export-001"
LINKS_SRC = R4_EXPORT / "security_company_links.part-0000.parquet"
PERM_IDS_SRC = R4_EXPORT / "security_permanent_ids.part-0000.parquet"
R4_MANIFEST = common.IDENTITY_R4_DIR / "manifest.json"

ONE_DAY = dt.timedelta(days=1)
MARK = dt.timedelta(hours=common.MARK_HOUR_UTC)
KEEP_TIERS = frozenset({"high", "medium"})
KEEP_BASES = frozenset({"strict_dated", "current_ticker_verified"})
DROP_BASES = frozenset({"current_ticker_verified"})

VALIDATION_SEAL = dt.date(2025, 1, 1)
VALIDATION_EXPECTED = {
    "rows": 6780,
    "lines": 6669,
    "ciks": 6583,
    "min_start": "2012-03-27",
    "rows_by_tier": {"high": 2472, "medium": 4308},
    "ambiguous_line_days": 2292,
}

RULE_TEXT = (
    "r4-links-asof-v1: for every calendar date d < seal, cutoff = d 22:00 naive UTC. "
    "(1) version visible iff available_at <= cutoff < valid_until (NULL open) and "
    "link_start <= d <= link_end (NULL open); versions with available_at >= seal ignored; "
    "days clipped to <= seal - 1 day. (2) a line visible under more than one company that day "
    "(all visible versions) is ambiguous -> unlinked that day. (3) keep tier in (high, medium) "
    "or basis in (strict_dated, current_ticker_verified); drop link_primary N and basis "
    "current_ticker_verified. (4) primary re-derived per company per day: P = min securityID "
    "among the company's remaining visible lines, others J. Compress the daily state "
    "(cik, primary, tier, basis, max available_at of the versions behind the day) into maximal "
    "constant intervals [start, end_incl]; available_at <= start 22:00 holds by construction."
)


def _ceil_day(ts: dt.datetime) -> dt.date:
    day = ts.date()
    return day if ts == dt.datetime.combine(day, dt.time()) else day + ONE_DAY


def _first_visible_day(available_at: dt.datetime) -> dt.date:
    """Smallest d with d 22:00 >= available_at."""
    return _ceil_day(available_at - MARK)


def _last_visible_day_before(valid_until: dt.datetime) -> dt.date:
    """Largest d with d 22:00 < valid_until."""
    return _ceil_day(valid_until - MARK) - ONE_DAY


def _segments(items: list[tuple[dt.date, dt.date, Any]]) -> Iterator[tuple[dt.date, dt.date, list[Any]]]:
    """Elementary segments of inclusive day intervals: yields (start, end_incl, active payloads)."""
    bounds = sorted({s for s, _, _ in items} | {e + ONE_DAY for _, e, _ in items})
    for a, b in zip(bounds, bounds[1:]):
        last = b - ONE_DAY
        active = [p for s, e, p in items if s <= a and e >= last]
        if active:
            yield a, last, active


def load_versions() -> list[tuple]:
    con = common.connect(memory="256MB", threads=1)
    try:
        return con.execute(
            f"""
            SELECT perm_security_id, cik, link_start, link_end, link_basis, link_primary, tier,
                   available_at, valid_until
            FROM read_parquet('{LINKS_SRC.as_posix()}')
            """
        ).fetchall()
    finally:
        con.close()


def compute_links(versions: Iterable[tuple], seal: dt.date) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    seal_ts = dt.datetime.combine(seal, dt.time())
    last_day = seal - ONE_DAY
    counts: collections.Counter[str] = collections.Counter()
    visible: list[tuple] = []
    for sid, cik, start, end, basis, primary, tier, available_at, valid_until in versions:
        counts["versions"] += 1
        if available_at is None or available_at >= seal_ts:
            counts["ignored_available_at_or_after_seal"] += 1
            continue
        first = max(start, _first_visible_day(available_at))
        last = min(end or last_day, last_day)
        if valid_until is not None:
            last = min(last, _last_visible_day_before(valid_until))
        if first > last:
            counts["never_visible"] += 1
            continue
        visible.append((int(sid), int(cik), first, last, basis, primary, tier, available_at))
    counts["visible_versions"] = len(visible)

    def keep(v: tuple) -> bool:
        _, _, _, _, basis, primary, tier, _ = v
        if not (tier in KEEP_TIERS or basis in KEEP_BASES):
            return False
        return primary in ("P", "J") and basis not in DROP_BASES

    by_line: dict[int, list[tuple]] = collections.defaultdict(list)
    for v in visible:
        by_line[v[0]].append(v)

    ambiguous_line_days = 0
    multi_version_days = 0
    line_segments: list[tuple] = []  # (sid, cik, start, end, tier, basis, available_at)
    for sid, vs in by_line.items():
        for s, e, active in _segments([(v[2], v[3], v) for v in vs]):
            days = (e - s).days + 1
            if len({v[1] for v in active}) > 1:
                ambiguous_line_days += days
                continue
            kept = [v for v in active if keep(v)]
            if not kept:
                continue
            if len(kept) > 1:
                multi_version_days += days
            chosen = max(kept, key=lambda v: (v[7], v[6] == "high", v[4]))
            line_segments.append((sid, chosen[1], s, e, chosen[6], chosen[4], max(v[7] for v in kept)))

    by_company: dict[int, list[tuple]] = collections.defaultdict(list)
    for seg in line_segments:
        by_company[seg[1]].append(seg)
    pieces: list[tuple] = []
    for cik, segs in by_company.items():
        for s, e, active in _segments([(x[2], x[3], x) for x in segs]):
            primary_sid = min(x[0] for x in active)
            for x in active:
                pieces.append((x[0], s, e, (cik, "P" if x[0] == primary_sid else "J", x[4], x[5], x[6])))
    pieces.sort(key=lambda p: (p[0], p[1]))

    merged: list[list] = []
    for sid, s, e, state in pieces:
        if merged and merged[-1][0] == sid and merged[-1][3] == state and merged[-1][2] == s - ONE_DAY:
            merged[-1][2] = e
        else:
            if merged and merged[-1][0] == sid and merged[-1][2] >= s:
                raise AssertionError(f"overlapping intervals for line {sid} at {s}")
            merged.append([sid, s, e, state])

    rows = []
    for sid, s, e, (cik, primary, tier, basis, available_at) in merged:
        if available_at > dt.datetime.combine(s, dt.time()) + MARK:
            raise AssertionError(f"available_at {available_at} after start mark for line {sid} {s}")
        if e > last_day:
            raise AssertionError("interval beyond seal")
        rows.append(
            {
                "security_id": sid,
                "cik": cik,
                "start": s,
                "end_incl": e,
                "primary": primary,
                "tier": tier,
                "basis": basis,
                "available_at": available_at,
            }
        )

    stats = {
        "seal": seal.isoformat(),
        "rows": len(rows),
        "lines": len({r["security_id"] for r in rows}),
        "ciks": len({r["cik"] for r in rows}),
        "min_start": min(r["start"] for r in rows).isoformat() if rows else None,
        "max_end_incl": max(r["end_incl"] for r in rows).isoformat() if rows else None,
        "rows_by_tier": dict(sorted(collections.Counter(r["tier"] for r in rows).items())),
        "rows_by_basis": dict(sorted(collections.Counter(r["basis"] for r in rows).items())),
        "rows_by_primary": dict(sorted(collections.Counter(r["primary"] for r in rows).items())),
        "ambiguous_line_days": ambiguous_line_days,
        "multi_version_line_days": multi_version_days,
        "version_counts": dict(counts),
        "line_days_linked_by_year": _line_days_by_year(rows),
    }
    _check_one_primary_per_company_day(rows)
    return rows, stats


def _line_days_by_year(rows: list[dict[str, Any]]) -> dict[str, int]:
    out: collections.Counter[int] = collections.Counter()
    for r in rows:
        s, e = r["start"], r["end_incl"]
        for year in range(s.year, e.year + 1):
            a = max(s, dt.date(year, 1, 1))
            b = min(e, dt.date(year, 12, 31))
            out[year] += (b - a).days + 1
    return {str(y): out[y] for y in sorted(out)}


def _check_one_primary_per_company_day(rows: list[dict[str, Any]]) -> None:
    by_company: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    for r in rows:
        by_company[r["cik"]].append(r)
    for cik, rs in by_company.items():
        for s, e, active in _segments([(r["start"], r["end_incl"], r) for r in rs]):
            n_primary = sum(1 for r in active if r["primary"] == "P")
            if n_primary != 1:
                raise AssertionError(f"cik {cik} has {n_primary} primary lines on {s}..{e}")


SCHEMA = pa.schema(
    [
        ("security_id", pa.int64()),
        ("cik", pa.int64()),
        ("start", pa.date32()),
        ("end_incl", pa.date32()),
        ("primary", pa.string()),
        ("tier", pa.string()),
        ("basis", pa.string()),
        ("available_at", pa.timestamp("us")),
    ]
)


def write_links(rows: list[dict[str, Any]], dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    table = pa.Table.from_pylist(sorted(rows, key=lambda r: (r["security_id"], r["start"])), schema=SCHEMA)
    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(table, tmp, compression="zstd")
    tmp.replace(dest)


def _session_line_days(links: Path) -> dict[str, int] | None:
    cal = common.calendar_path()
    if not cal.exists():
        return None
    con = common.connect(memory="256MB", threads=1)
    try:
        rows = con.execute(
            f"""
            SELECT year(c.session_date) AS y, count(*)
            FROM read_parquet('{cal.as_posix()}') c
            JOIN read_parquet('{links.as_posix()}') l
              ON c.session_date BETWEEN l."start" AND l.end_incl
            GROUP BY 1 ORDER BY 1
            """
        ).fetchall()
    finally:
        con.close()
    return {str(y): int(n) for y, n in rows}


def _sources() -> dict[str, Any]:
    out = {}
    for name, path in (("security_company_links", LINKS_SRC), ("security_permanent_ids", PERM_IDS_SRC),
                       ("r4_manifest", R4_MANIFEST)):
        out[name] = {**common.file_identity(path), "sha256": common.sha256_file(path)}
    return out


def _check_perm_ids() -> dict[str, int]:
    """security_id namespace check: perm_security_id equals the vendor securityID."""
    con = common.connect(memory="256MB", threads=1)
    try:
        n, same = con.execute(
            f"""
            SELECT count(*),
                   sum((perm_security_id = TRY_CAST(replace(security_id, 'TBLTICKERHISTORY-', '') AS BIGINT))::INT)
            FROM read_parquet('{PERM_IDS_SRC.as_posix()}')
            """
        ).fetchone()
    finally:
        con.close()
    if int(n) != int(same):
        raise AssertionError(f"perm_security_id differs from vendor securityID on {int(n) - int(same)} lines")
    return {"permanent_ids": int(n), "perm_equals_vendor_security_id": int(same)}


def build(seal: dt.date) -> dict[str, Any]:
    t0 = time.perf_counter()
    out_dir = common.stage_dir("identity")
    dest = out_dir / "links.parquet"
    receipt: dict[str, Any] = {"stage": "I", "rule": RULE, "code_version": CODE_VERSION, "rule_text": RULE_TEXT}
    with common.timed(receipt, "load"):
        versions = load_versions()
        receipt["id_namespace"] = _check_perm_ids()
    with common.timed(receipt, "validation_seal_2025_01_01"):
        _, vstats = compute_links(versions, VALIDATION_SEAL)
        receipt["validation"] = _validation_result(vstats)
        if not receipt["validation"]["pass"]:
            raise SystemExit(f"validation mode mismatch: {receipt['validation']}")
    with common.timed(receipt, "compute"):
        rows, stats = compute_links(versions, seal)
    with common.timed(receipt, "write"):
        write_links(rows, dest)
        stats["session_line_days_by_year"] = _session_line_days(dest)
    with common.timed(receipt, "hash"):
        receipt["sources"] = _sources()
        receipt["outputs"] = {"links.parquet": {**common.file_identity(dest), "sha256": common.sha256_file(dest),
                                                "rows": len(rows)}}
    receipt["stats"] = stats
    receipt["created_utc"] = dt.datetime.now(dt.UTC).isoformat(timespec="seconds")
    receipt["timings_s"]["total"] = round(time.perf_counter() - t0, 3)
    common.write_json_atomic(out_dir / "manifest.json", receipt)
    return receipt


def _validation_result(stats: dict[str, Any]) -> dict[str, Any]:
    got = {k: stats[k] for k in VALIDATION_EXPECTED}
    return {"seal": VALIDATION_SEAL.isoformat(), "expected": VALIDATION_EXPECTED, "got": got,
            "pass": got == VALIDATION_EXPECTED}


def attach_receipt(receipt_path: Path) -> None:
    manifest_path = common.build_root() / "identity" / "manifest.json"
    manifest = common.read_json(manifest_path)
    guard = common.read_json(receipt_path)
    manifest["guard"] = {
        "receipt": str(receipt_path),
        "job_limit_gb": guard.get("job_limit_gb"),
        "native_peak_job_memory_gb": guard.get("native_peak_job_memory_gb"),
        "status": guard.get("status"),
        "exit_code": guard.get("exit_code", guard.get("returncode")),
    }
    common.write_json_atomic(manifest_path, manifest)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--seal", default=common.IDENTITY_SEAL.isoformat())
    sub.add_parser("validate")
    a = sub.add_parser("attach-receipt")
    a.add_argument("--receipt", required=True, type=Path)
    args = ap.parse_args(argv)
    if args.cmd == "build":
        r = build(dt.date.fromisoformat(args.seal))
        s = r["stats"]
        print({k: s[k] for k in ("rows", "lines", "ciks", "min_start", "rows_by_tier", "ambiguous_line_days")})
        print("validation", r["validation"]["pass"], "timings", r["timings_s"])
    elif args.cmd == "validate":
        _, stats = compute_links(load_versions(), VALIDATION_SEAL)
        res = _validation_result(stats)
        print(res)
        return 0 if res["pass"] else 1
    else:
        attach_receipt(args.receipt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
