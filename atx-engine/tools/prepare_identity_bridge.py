"""Pinned, point-in-time SpiderRock securityID -> SEC CIK identity bridge (``atx.identity-bridge/v1``).

Source: the 3.3 identity rehearsal r4 export (``identity_links_v1``, ``rehearsal=true``,
``scope_complete=false``), default ``C:/atx/atx-db/data/research/identity_rehearsal/session8-phased-r4``.
The source is opened read-only; nothing is ever written inside it. Its ``manifest.json`` and the two
pinned datasets (``security_company_links``, ``security_permanent_ids``) are copied byte-for-byte
into ``<output>/source/`` after their SHA-256 and byte counts match the source manifest, and every
row below is derived from those pinned copies (never from the live source path).

Rule (``r4-links-asof-v1``): the r4 producer's own ``identity_links.links_asof_sql(cutoff)``
evaluated at the session mark ``cutoff = d 22:00:00`` (naive UTC, the r4 and role clock), for
every calendar date ``d`` before the seal, then compressed into date intervals:

1. version visible: ``available_at <= cutoff < valid_until`` (``valid_until`` NULL = open; evidence
   versions are half-open) AND ``link_start <= d <= link_end`` (business end inclusive, NULL = open);
2. a line visible under more than one company at ``cutoff`` is ambiguous: no link that day;
3. tier filter after version selection: ``tier in (high, medium)`` or basis ``strict_dated`` /
   ``current_ticker_verified`` (a later low tier cannot resurrect an older high tier);
4. primary re-derived per company at ``cutoff``: ``P`` for the smallest securityID among the
   company's visible ``P``/``J`` lines, ``J`` for its other ``P``/``J`` lines, ``N`` stays ``N``.

``N`` rows are dropped and basis ``current_ticker_verified`` (links that start at the 2026-09-20
snapshot and never backfill history) is dropped after step 2 and before step 4 (so every ``J`` day
has a ``P`` line; identical to r4 on every date before the snapshot). Versions with
``available_at >= seal`` are ignored and every interval is clipped to ``end_incl <= seal - 1 day``
(seal default: the research seal of ``research_window.py``, ``SEAL_DATE``; nothing known on or after it
is used, and a later ``--seal`` is refused). Identity is labelled
``rehearsal_identity=true`` (not an accepted identity).

Output directory (new or empty; ``manifest.json`` is published last, exclusively and fsynced; no
wall-clock value in any output byte, so a rerun on the same source bytes is byte-identical):

``links.parquet`` -- normalised dated rows, one row per (sr_id, maximal constant interval), sorted
by (sr_id, start). Columns:

=============== ============== ===========================================================
column          arrow type     meaning
=============== ============== ===========================================================
sr_id           int64          SpiderRock securityID (= role ``ids.u64`` value = r4
                               ``perm_security_id``; verified against ``TBLTICKERHISTORY-<id>``)
cik             int64          SEC CIK (= r4 ``perm_company_id``; 10-digit zero-padded in r4)
start           date32         first calendar date d the link applies (inclusive)
end_incl        date32         last calendar date d the link applies (inclusive, <= seal - 1)
available_at    timestamp[us]  r4 evidence-version clock (naive UTC); always <= start 22:00
primary         string         ``P`` (the company's primary line that day) or ``J``
tier            string         ``high`` | ``medium``
basis           string         r4 ``link_basis`` (``reconstructed_*``; never
                               ``current_ticker_verified``)
class_status    string         r4 class status of the version (informational)
=============== ============== ===========================================================

Lookup semantics (what consumers such as prepare_research_fields implement): line ``sr_id`` at
session ``d`` links to ``cik`` iff a row has ``start <= d <= end_incl``. The knowledge clock is
already applied: every row satisfies ``available_at <= start 22:00``, so the intervals ARE the
point-in-time answer at each session's 22:00 mark. Do not re-filter with a strict
``available_at < d 22:00`` (it would drop the first day of rows whose version became visible at
exactly the mark); if a guard is wanted, use ``<=``. Invariants (verified before publishing):
per ``sr_id`` intervals are disjoint (at most one CIK per line per day); per ``(cik, d)`` at most
one ``P`` row, and a ``J`` row only on days with a ``P`` row; ``primary in {P, J}``; ``end_incl < seal``. Dates not covered by any row are
unlinked (no evidence, ambiguous, N-only, low tier, or before the evidence clock).

``ciks.txt`` -- the distinct CIKs of ``links.parquet`` (P or J), 10-digit zero-padded, sorted,
one per line, LF: the fundamentals producer's CIK scope.

``source/manifest.json`` and ``source/<dataset>.parquet`` -- pinned byte copies of the r4 export.

``manifest.json`` (schema ``atx.identity-bridge/v1``): ``rehearsal_identity: true``, the rule
statement, seal, source path and manifest SHA-256, per-file SHA-256/bytes/rows of the pinned copies,
r4 ``snapshot_date`` / ``scope_complete`` / ``remaining_acceptance``, output file SHA-256s, counts
and the executed code identity.

``--check BRIDGE_DIR`` (metadata only; writes nothing): verifies the bridge manifest and file
hashes, then prints JSON lines of per-year coverage of each role's instrument ids (role
``ids.u64``, ``sessions.i64``, ``member.u8``; default the TRAIN v2 and VAL v1 roles under
``build-equity/``) on scored member cells, plus, when ``--static-warehouse`` is given, a read-only
diagnostic comparison with the legacy static bridge (warehouse ``security_identifier_history``).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import sys
import time

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import research_window as rw  # same directory: the research window (the seal)

SCHEMA = "atx.identity-bridge/v1"
SOURCE_SCHEMA = "identity_links_v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
RULE = "r4-links-asof-v1"
DEFAULT_SOURCE = Path(r"C:\atx\atx-db\data\research\identity_rehearsal\session8-phased-r4")
# --check reads role sessions: only roles that end before the seal (the 2023-2024 validation role
# reaches it and is refused, so it is no longer a default).
DEFAULT_ROLES = (Path("build-equity/recent-fast-train-2020-2022-v2"),)
PINNED_DATASETS = ("security_company_links", "security_permanent_ids")
MARK = dt.time(22, 0)
SEAL = rw.SEAL  # first sealed date (research_window.py); --seal may only be earlier
KEEP_TIERS = frozenset({"high", "medium"})
TIER_BYPASS_BASES = frozenset({"strict_dated", "current_ticker_verified"})
EXCLUDED_BASES = frozenset({"current_ticker_verified"})
SR_PREFIX = "TBLTICKERHISTORY-"
MAX_SOURCE_BYTES = 256 << 20
DAY_NS = 86_400_000_000_000
ONE_DAY = dt.timedelta(days=1)
LINK_COLUMNS = ("perm_security_id", "perm_company_id", "cik", "link_start", "link_end", "link_basis",
                "link_primary", "tier", "available_at", "valid_until", "class_status")
OUT_SCHEMA = pa.schema([
    ("sr_id", pa.int64()), ("cik", pa.int64()), ("start", pa.date32()), ("end_incl", pa.date32()),
    ("available_at", pa.timestamp("us")), ("primary", pa.string()), ("tier", pa.string()),
    ("basis", pa.string()), ("class_status", pa.string())])
RULE_STATEMENT = (
    "r4 identity_links.links_asof_sql(cutoff=d 22:00 naive UTC) for every calendar date d < seal: "
    "version visible iff available_at <= cutoff < valid_until (NULL open) and link_start <= d <= link_end "
    "(NULL open); a line visible under >1 company is ambiguous (unlinked); then tier in (high, medium) or "
    "basis in (strict_dated, current_ticker_verified); N rows and basis current_ticker_verified dropped; primary "
    "re-derived per company: P = min securityID among remaining visible P/J lines, others J. Versions with "
    "available_at >= seal ignored, end_incl clipped to seal - 1 day; lookup: row applies to d iff "
    "start <= d <= end_incl (available_at <= start 22:00 by construction).")


# ---------------------------------------------------------------------------
# Hashing, publishing
# ---------------------------------------------------------------------------

def sha_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def code_identity(path: Path) -> dict:
    raw = path.read_bytes()
    lf = raw.replace(b"\r\n", b"\n")
    return {"code_sha256": sha_bytes(raw), "code_sha256_lf": sha_bytes(lf),
            "code_git_blob_sha1": hashlib.sha1(b"blob %d\0" % len(lf) + lf).hexdigest()}


def write_exclusive(path: Path, content: bytes) -> str:
    with path.open("xb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    return sha_bytes(content)


def publish(path: Path, value) -> bytes:
    """Exclusive publish-last manifest: a reader sees no manifest or its complete fsynced bytes."""
    content = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    pending = path.with_name("." + path.name + ".pending")
    with pending.open("xb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.link(pending, path)
    pending.unlink()
    return content


def _inside(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


# ---------------------------------------------------------------------------
# Source pinning
# ---------------------------------------------------------------------------

def read_source(source: Path, expect_sha256: str | None = None) -> tuple[bytes, dict, dict[str, bytes]]:
    """Read the r4 manifest and the pinned datasets read-only; verify every byte against the manifest."""
    manifest_bytes = (source / "manifest.json").read_bytes()
    manifest_sha = sha_bytes(manifest_bytes)
    if expect_sha256 and manifest_sha != expect_sha256.lower():
        raise ValueError(f"source manifest SHA-256 {manifest_sha} does not match --expect-source-sha256")
    m = json.loads(manifest_bytes)
    if m.get("schema_version") != SOURCE_SCHEMA or m.get("rehearsal") is not True:
        raise ValueError("source is not an identity_links_v1 rehearsal export")
    entries = {}
    for entry in m.get("files", []):
        entries.setdefault(entry.get("dataset"), []).append(entry)
    blobs = {}
    for dataset in PINNED_DATASETS:
        found = entries.get(dataset, [])
        if len(found) != 1:
            raise ValueError(f"source manifest must list exactly one {dataset} file")
        entry = found[0]
        rel = Path(entry["file"])
        if rel.is_absolute() or ".." in rel.parts:
            raise ValueError(f"source manifest file path for {dataset} escapes the export")
        path = source / rel
        if path.stat().st_size > MAX_SOURCE_BYTES:
            raise ValueError(f"{dataset} exceeds the {MAX_SOURCE_BYTES >> 20} MiB bound")
        with path.open("rb") as f:
            blob = f.read()
        if len(blob) != entry["bytes"] or sha_bytes(blob) != entry["sha256"]:
            raise ValueError(f"{dataset} bytes do not match the source manifest")
        blobs[dataset] = blob
    return manifest_bytes, m, blobs


def _table(blob: bytes) -> pa.Table:
    return pq.read_table(pa.BufferReader(blob))


# ---------------------------------------------------------------------------
# Point-in-time resolution
# ---------------------------------------------------------------------------

def first_visible_date(available_at: dt.datetime) -> dt.date:
    """First calendar date d with available_at <= d 22:00."""
    d = available_at.date()
    return d if available_at.time() <= MARK else d + ONE_DAY


def first_invisible_date(valid_until: dt.datetime) -> dt.date:
    """First calendar date d with d 22:00 >= valid_until (the version is gone from that mark on)."""
    d = valid_until.date()
    return d if valid_until.time() <= MARK else d + ONE_DAY


def visible_span(row: dict, seal: dt.date) -> tuple[dt.date, dt.date] | None:
    """Inclusive calendar-date range on which the version is visible under the r4 rule, clipped to the seal."""
    if row["available_at"] is None or row["link_start"] is None:
        raise ValueError(f"link row for {row['perm_security_id']} lacks available_at or link_start")
    if row["available_at"] >= dt.datetime.combine(seal, dt.time()):
        return None
    lo = max(row["link_start"], first_visible_date(row["available_at"]))
    hi = seal - ONE_DAY
    if row["link_end"] is not None:
        hi = min(hi, row["link_end"])
    if row["valid_until"] is not None:
        hi = min(hi, first_invisible_date(row["valid_until"]) - ONE_DAY)
    return (lo, hi) if lo <= hi else None


def _segments(spans):
    """Elementary [a, b] date segments of a set of inclusive spans."""
    bounds = sorted({lo for lo, _ in spans} | {hi + ONE_DAY for _, hi in spans})
    return [(a, b - ONE_DAY) for a, b in zip(bounds, bounds[1:])]


def _pick(rows):
    # Several visible P/J versions of one line for one company: the newest evidence version wins.
    return max(rows, key=lambda r: (r["available_at"], r["link_basis"], r["tier"] or "", r["class_status"] or ""))


def resolve(link_rows: list[dict], seal: dt.date = SEAL) -> tuple[list[dict], dict]:
    """Evaluate the r4 as-of rule on every date and compress it into disjoint per-line intervals."""
    stats = {"versions": len(link_rows), "versions_visible_before_seal": 0, "ambiguous_line_days": 0,
             "duplicate_version_line_days": 0}
    by_sr: dict[int, list] = {}
    for row in link_rows:
        if row["link_primary"] not in ("P", "J", "N"):
            raise ValueError(f"unknown link_primary {row['link_primary']!r}")
        if int(row["cik"]) != int(row["perm_company_id"]):
            raise ValueError(f"cik and perm_company_id disagree for line {row['perm_security_id']}")
        span = visible_span(row, seal)
        if span is not None:
            stats["versions_visible_before_seal"] += 1
            by_sr.setdefault(int(row["perm_security_id"]), []).append((span, row))

    # Steps 1-3 per line: visible versions, ambiguity, tier filter.
    by_cik: dict[int, list] = {}
    for sr, items in by_sr.items():
        for a, b in _segments([span for span, _ in items]):
            vis = [row for (lo, hi), row in items if lo <= a and hi >= a]
            if not vis:
                continue
            days = (b - a).days + 1
            if len({int(row["cik"]) for row in vis}) > 1:
                stats["ambiguous_line_days"] += days
                continue
            kept = [row for row in vis if row["tier"] in KEEP_TIERS or row["link_basis"] in TIER_BYPASS_BASES]
            # Excluded bases leave before the primary re-derivation, so every J day keeps a P line. This
            # departs from r4 only while a current_ticker_verified link is visible, i.e. from the
            # 2026-09-20 snapshot on, which is after the seal: no effect on any published row.
            pj = [row for row in kept if row["link_primary"] in ("P", "J") and row["link_basis"] not in EXCLUDED_BASES]
            if not pj:
                continue
            if len(pj) > 1:
                stats["duplicate_version_line_days"] += days
            by_cik.setdefault(int(pj[0]["cik"]), []).append((sr, a, b, _pick(pj)))

    # Step 4 per company: P = smallest visible P/J line id.
    raw = []
    for cik, items in by_cik.items():
        for a, b in _segments([(lo, hi) for _, lo, hi, _ in items]):
            act = [(sr, row) for sr, lo, hi, row in items if lo <= a and hi >= a]
            if not act:
                continue
            primary_sr = min(sr for sr, _ in act)
            for sr, row in act:
                raw.append({"sr_id": sr, "cik": cik, "start": a, "end_incl": b,
                            "available_at": row["available_at"], "primary": "P" if sr == primary_sr else "J",
                            "tier": row["tier"], "basis": row["link_basis"], "class_status": row["class_status"]})

    raw.sort(key=lambda r: (r["sr_id"], r["start"]))
    out = []
    same = ("sr_id", "cik", "available_at", "primary", "tier", "basis", "class_status")
    for r in raw:
        prev = out[-1] if out else None
        if prev and all(prev[k] == r[k] for k in same) and prev["end_incl"] + ONE_DAY == r["start"]:
            prev["end_incl"] = r["end_incl"]
        else:
            out.append(dict(r))
    verify_invariants(out, seal)
    return out, stats


def verify_invariants(rows: list[dict], seal: dt.date) -> None:
    last: dict[int, dt.date] = {}
    for r in rows:  # sorted by (sr_id, start)
        if r["primary"] not in ("P", "J") or r["basis"] in EXCLUDED_BASES or r["tier"] not in KEEP_TIERS:
            raise AssertionError(f"excluded row survived for line {r['sr_id']}")
        if not r["start"] <= r["end_incl"] < seal:
            raise AssertionError(f"bad interval for line {r['sr_id']}")
        if r["available_at"] > dt.datetime.combine(r["start"], MARK):
            raise AssertionError(f"row for line {r['sr_id']} starts before its evidence clock")
        if r["sr_id"] in last and last[r["sr_id"]] >= r["start"]:
            raise AssertionError(f"overlapping intervals for line {r['sr_id']}")
        last[r["sr_id"]] = r["end_incl"]
    primaries: dict[int, list] = {}
    for r in rows:
        if r["primary"] == "P":
            primaries.setdefault(r["cik"], []).append((r["start"], r["end_incl"]))
    for cik, spans in primaries.items():
        spans.sort()
        for (_, e0), (s1, _) in zip(spans, spans[1:]):
            if s1 <= e0:
                raise AssertionError(f"two primary lines on one day for CIK {cik}")
    for r in rows:  # every J day has the company's P line
        if r["primary"] == "J":
            need = r["start"]
            for s, e in primaries.get(r["cik"], []):
                if s <= need <= e:
                    need = e + ONE_DAY
                if need > r["end_incl"]:
                    break
            if need <= r["end_incl"]:
                raise AssertionError(f"J line {r['sr_id']} without a P line for CIK {r['cik']} on {need}")


def verify_line_namespace(link_rows: list[dict], perm_ids: pa.Table) -> int:
    """Every linked perm_security_id is the vendor securityID (security_id == TBLTICKERHISTORY-<id>)."""
    ids = perm_ids.column("perm_security_id").to_pylist()
    names = perm_ids.column("security_id").to_pylist()
    known = {i for i, n in zip(ids, names) if n == f"{SR_PREFIX}{i}"}
    if len(known) != len(ids):
        raise ValueError("security_permanent_ids has ids outside the TBLTICKERHISTORY-<securityID> namespace")
    missing = {int(r["perm_security_id"]) for r in link_rows} - known
    if missing:
        raise ValueError(f"{len(missing)} linked lines are absent from security_permanent_ids")
    return len(known)


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def rows_to_table(rows: list[dict]) -> pa.Table:
    return pa.Table.from_pylist(rows, schema=OUT_SCHEMA)


def table_bytes(table: pa.Table) -> bytes:
    sink = pa.BufferOutputStream()
    pq.write_table(table, sink, compression="zstd", use_dictionary=True, write_statistics=True)
    return sink.getvalue().to_pybytes()


def _counts(rows: list[dict]) -> dict:
    def tally(key):
        out = {}
        for r in rows:
            out[r[key]] = out.get(r[key], 0) + 1
        return dict(sorted(out.items()))
    at_mark = sum(1 for r in rows if r["available_at"] == dt.datetime.combine(r["start"], MARK))
    return {"rows": len(rows), "lines": len({r["sr_id"] for r in rows}), "ciks": len({r["cik"] for r in rows}),
            "primary_lines": len({r["sr_id"] for r in rows if r["primary"] == "P"}),
            "rows_by_primary": tally("primary"), "rows_by_basis": tally("basis"), "rows_by_tier": tally("tier"),
            "line_days_by_primary": {p: sum((r["end_incl"] - r["start"]).days + 1 for r in rows if r["primary"] == p)
                                     for p in ("P", "J")},
            "rows_available_at_exactly_start_mark": at_mark,
            "min_start": min(r["start"] for r in rows).isoformat() if rows else None,
            "max_end_incl": max(r["end_incl"] for r in rows).isoformat() if rows else None}


def build(source: Path, output: Path, *, expect_sha256: str | None = None, seal: dt.date = SEAL) -> dict:
    if _inside(output, source) or _inside(source, output):
        raise ValueError("output must not be inside (or contain) the source export")
    if output.exists() and any(output.iterdir()):
        raise FileExistsError(f"{output} exists and is not empty; the bridge output is exclusive")
    manifest_bytes, src, blobs = read_source(source, expect_sha256)
    links = _table(blobs["security_company_links"])
    missing = [c for c in LINK_COLUMNS if c not in links.column_names]
    if missing:
        raise ValueError(f"security_company_links lacks columns {missing}")
    link_rows = links.select(list(LINK_COLUMNS)).to_pylist()
    perm_lines = verify_line_namespace(link_rows, _table(blobs["security_permanent_ids"]))
    rows, stats = resolve(link_rows, seal)

    output.mkdir(parents=True, exist_ok=True)
    (output / "source").mkdir()
    pinned = {"manifest.json": {"bytes": len(manifest_bytes), "sha256": write_exclusive(
        output / "source" / "manifest.json", manifest_bytes)}}
    for entry in src["files"]:
        if entry["dataset"] in PINNED_DATASETS:
            name = f"{entry['dataset']}.parquet"
            sha = write_exclusive(output / "source" / name, blobs[entry["dataset"]])
            pinned[name] = {"bytes": entry["bytes"], "rows": entry["rows"], "sha256": sha,
                            "source_file": entry["file"], "dataset": entry["dataset"]}
    links_blob = table_bytes(rows_to_table(rows))
    ciks_blob = "".join(f"{c:010d}\n" for c in sorted({r["cik"] for r in rows})).encode("ascii")
    outputs = {"links.parquet": {"bytes": len(links_blob), "sha256": write_exclusive(output / "links.parquet", links_blob)},
               "ciks.txt": {"bytes": len(ciks_blob), "sha256": write_exclusive(output / "ciks.txt", ciks_blob)}}
    manifest = {
        "schema": SCHEMA, "status": "complete", "rehearsal_identity": True, "rule": RULE,
        "rule_statement": RULE_STATEMENT, "mark_utc": MARK.isoformat(), "seal": seal.isoformat(),
        "instrument_namespace": "spiderrock.securityID", "cik_format": "int64 in links.parquet; 10-digit in ciks.txt",
        "excluded": {"link_primary": ["N"], "link_basis": sorted(EXCLUDED_BASES)},
        "tiers_kept": sorted(KEEP_TIERS),
        "source": {"path": str(source), "manifest_sha256": sha_bytes(manifest_bytes),
                   "schema_version": src.get("schema_version"), "snapshot_date": src.get("snapshot_date"),
                   "rehearsal": src.get("rehearsal"), "scope_complete": src.get("scope_complete"),
                   "end_date_semantics": src.get("end_date_semantics"),
                   "availability_basis": src.get("availability_basis"),
                   "remaining_acceptance": src.get("remaining_acceptance"),
                   "code_sha256": src.get("code_sha256"), "security_permanent_ids_lines": perm_lines},
        "pinned_files": pinned, "files": outputs, "stats": stats, "counts": _counts(rows),
        "caveats": ["rehearsal identity (3.3 r4), not an accepted identity; scope_complete=false",
                    "coverage is limited to reconstructed high/medium links; unlinked lines have no CIK",
                    "link_end and class status are the r4 producer's PIT claims, pinned not re-derived"],
        "tool": {"path": "atx-engine/tools/prepare_identity_bridge.py", **code_identity(Path(__file__))},
    }
    publish(output / "manifest.json", manifest)
    return manifest


# ---------------------------------------------------------------------------
# Loading (for consumers) and --check
# ---------------------------------------------------------------------------

def load_bridge(directory: Path) -> tuple[pa.Table, dict]:
    """Verified links table and manifest of a published bridge."""
    manifest_bytes = (directory / "manifest.json").read_bytes()
    m = json.loads(manifest_bytes)
    if m.get("schema") != SCHEMA or m.get("status") != "complete" or m.get("rehearsal_identity") is not True:
        raise ValueError("not a complete atx.identity-bridge/v1 output")
    blob = (directory / "links.parquet").read_bytes()
    entry = m["files"]["links.parquet"]
    if len(blob) != entry["bytes"] or sha_bytes(blob) != entry["sha256"]:
        raise ValueError("links.parquet bytes do not match the bridge manifest")
    table = _table(blob)
    if not table.schema.equals(OUT_SCHEMA):
        raise ValueError("links.parquet schema differs from atx.identity-bridge/v1")
    m["manifest_sha256"] = sha_bytes(manifest_bytes)
    return table, m


def read_role(directory: Path) -> dict:
    m = json.loads((directory / "manifest.json").read_bytes())
    if m.get("schema") != ROLE_SCHEMA or m.get("status") != "complete":
        raise ValueError(f"{directory} is not a complete {ROLE_SCHEMA} payload")
    if m.get("instrument_namespace") != "spiderrock.securityID":
        raise ValueError(f"{directory} role namespace is not spiderrock.securityID")
    blobs = {}
    for name in ("sessions.i64", "ids.u64", "member.u8"):
        blob = (directory / name).read_bytes()
        entry = m["files"][name]
        if len(blob) != entry["bytes"] or sha_bytes(blob) != entry["sha256"]:
            raise ValueError(f"role {name} bytes do not match the role manifest")
        blobs[name] = blob
    sessions = np.frombuffer(blobs["sessions.i64"], dtype="<i8").astype(np.int64)
    n_dates, n = int(m["dates"]), int(m["instruments"])
    if len(sessions) != n_dates or np.any(sessions % DAY_NS) or np.any(np.diff(sessions) <= 0):
        raise ValueError("role sessions are not strictly increasing midnight labels")
    rw.refuse_sealed(int(sessions[-1]), f"role {directory} holds a session")
    ids = np.frombuffer(blobs["ids.u64"], dtype="<u8").astype(np.int64)
    if len(ids) != n or np.any(np.diff(ids) <= 0):
        raise ValueError("role ids are not strictly increasing")
    member = np.frombuffer(blobs["member.u8"], dtype="u1").reshape(n_dates, n)
    return {"dir": directory, "days": sessions // DAY_NS, "ids": ids, "member": member,
            "score_begin": int(m.get("score_begin", 0)), "score_end": int(m.get("score_end", n_dates))}


def link_matrix(table: pa.Table, role: dict, primary_only: bool) -> tuple[np.ndarray, dict[int, set]]:
    """Boolean [role dates x role ids] of cells linked by the bridge, and each role id's linked CIKs."""
    days, ids = role["days"], role["ids"]
    linked = np.zeros((len(days), len(ids)), dtype=bool)
    ciks: dict[int, set] = {}
    epoch = dt.date(1970, 1, 1)
    cols = {c: table.column(c).to_pylist() for c in ("sr_id", "cik", "start", "end_incl", "primary")}
    for sr, cik, start, end, primary in zip(*(cols[c] for c in ("sr_id", "cik", "start", "end_incl", "primary"))):
        if primary_only and primary != "P":
            continue
        j = int(np.searchsorted(ids, sr))
        if j >= len(ids) or ids[j] != sr:
            continue
        t0 = int(np.searchsorted(days, (start - epoch).days, side="left"))
        t1 = int(np.searchsorted(days, (end - epoch).days, side="right"))
        if t1 > t0:
            if linked[t0:t1, j].any():
                raise AssertionError(f"line {sr} linked twice on one session")
            linked[t0:t1, j] = True
            ciks.setdefault(sr, set()).add(cik)
    return linked, ciks


def static_bridge(warehouse: Path, sr_ids) -> dict[int, int]:
    import duckdb  # diagnostic only; read-only connection
    con = duckdb.connect(str(warehouse), read_only=True)
    try:
        rows = con.execute("select id_value, security_id from security_identifier_history "
                           "where id_type = 'TBLTICKERHISTORY_SECURITY_ID'").fetchall()
    finally:
        con.close()
    wanted = {str(int(s)) for s in sr_ids}
    cands: dict[int, set] = {}
    for sr, sec in rows:
        if sr in wanted and sec.startswith("SEC-CIK-"):
            cands.setdefault(int(sr), set()).add(int(sec.removeprefix("SEC-CIK-")))
    return {sr: next(iter(c)) for sr, c in cands.items() if len(c) == 1}


def check(bridge: Path, roles, static_warehouse: Path | None = None, out=None) -> list[dict]:
    out = out if out is not None else sys.stdout
    table, m = load_bridge(bridge)
    records = [{"bridge": str(bridge), "manifest_sha256": m["manifest_sha256"], "rule": m["rule"],
                "rehearsal_identity": m["rehearsal_identity"], "rows": table.num_rows}]
    for role_dir in roles:
        role = read_role(Path(role_dir))
        scored = np.zeros(len(role["days"]), dtype=bool)
        scored[role["score_begin"]:role["score_end"]] = True
        member = role["member"].astype(bool) & scored[:, None]
        p_linked, p_ciks = link_matrix(table, role, primary_only=True)
        any_linked, _ = link_matrix(table, role, primary_only=False)
        years = role["days"].astype("datetime64[D]").astype("datetime64[Y]").astype(np.int64) + 1970
        for year in [int(y) for y in np.unique(years[scored])] + ["scored"]:
            rows_mask = scored if year == "scored" else (years == year) & scored
            mem = member & rows_mask[:, None]
            cells = int(mem.sum())
            ids_member = mem.any(axis=0)
            ids_linked = (mem & p_linked).any(axis=0)
            records.append({
                "role": str(role_dir), "year": year, "sessions": int(rows_mask.sum()),
                "role_ids_member": int(ids_member.sum()), "role_ids_member_p_linked": int(ids_linked.sum()),
                "id_coverage_p": round(float(ids_linked.sum() / max(1, ids_member.sum())), 4),
                "member_cells": cells, "member_cells_p_linked": int((mem & p_linked).sum()),
                "cell_coverage_p": round(float((mem & p_linked).sum() / max(1, cells)), 4),
                "cell_coverage_p_or_j": round(float((mem & any_linked).sum() / max(1, cells)), 4)})
        if static_warehouse is not None:
            ever_member = member.any(axis=0)
            member_ids = role["ids"][ever_member]
            try:
                static = static_bridge(static_warehouse, member_ids)
            except Exception as exc:  # diagnostic only: a locked or moved warehouse must not fail the check
                records.append({"role": str(role_dir), "static_bridge": "unavailable", "error": repr(exc)[:200]})
                continue
            agree = disagree = bridge_only = static_only = 0
            for sr in member_ids.tolist():
                b, s = p_ciks.get(sr, set()), static.get(sr)
                if b and s is not None:
                    agree, disagree = (agree + 1, disagree) if s in b else (agree, disagree + 1)
                elif b:
                    bridge_only += 1
                elif s is not None:
                    static_only += 1
            records.append({"role": str(role_dir), "static_bridge": "legacy-static-v1 (diagnostic only)",
                            "member_ids": int(len(member_ids)), "agree": agree, "disagree": disagree,
                            "bridge_only": bridge_only, "static_only": static_only})
    for rec in records:
        print(json.dumps(rec, sort_keys=True), file=out, flush=True)
    return records


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path, help="new (or empty) output directory for the bridge")
    mode.add_argument("--check", type=Path, metavar="BRIDGE_DIR", help="print role coverage of a published bridge")
    p.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="r4 identity export directory (read-only)")
    p.add_argument("--expect-source-sha256", help="required SHA-256 of the source manifest.json")
    p.add_argument("--seal", type=dt.date.fromisoformat, default=SEAL,
                   help=f"first excluded date (default and latest allowed: the research seal {rw.SEAL_DATE}, "
                        f"{rw.WINDOW_ID})")
    p.add_argument("--role", type=Path, action="append", help="role directory for --check (repeatable)")
    p.add_argument("--static-warehouse", type=Path, help="--check: warehouse.duckdb for the static-bridge diagnostic")
    a = p.parse_args(argv)
    t0 = time.monotonic()
    if a.check:
        check(a.check, a.role or list(DEFAULT_ROLES), a.static_warehouse)
    else:
        if a.seal > SEAL:
            raise SystemExit(f"--seal later than the research seal {rw.SEAL_DATE} ({rw.WINDOW_ID}) is not allowed")
        m = build(a.source, a.output, expect_sha256=a.expect_source_sha256, seal=a.seal)
        print(json.dumps({"published": str(a.output / "manifest.json"), "source_manifest_sha256":
                          m["source"]["manifest_sha256"], "counts": m["counts"], "stats": m["stats"]}, sort_keys=True))
    try:
        import psutil
        rss = psutil.Process().memory_info().rss >> 20
    except ImportError:  # pragma: no cover
        rss = None
    print(json.dumps({"elapsed_s": round(time.monotonic() - t0, 2), "rss_mib": rss}), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
