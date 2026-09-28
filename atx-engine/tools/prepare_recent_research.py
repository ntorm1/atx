"""Bounded research projection and lagged-liquidity role writer.

No warehouse access. Raw Parquet row groups mix years: projected columns are
physically decoded, then dates are filtered BEFORE numeric QA/statistics/output.
This is archive research, not authenticated publication or common-stock evidence.
Only completed immutable projection caches can be resumed/reused.

Universe (``role --universe``). The default ``research-prior63-usd-adv-topn-v1`` is the
lagged-liquidity top-N membership above, unchanged. ``linked-operating-v1`` (v4-prereg.md
"## v6 revision" item V6-U, declared before any v6 TRAIN read) restricts an EXISTING role
(``--base-role``, e.g. TRAIN role v2 with its factor-break repair) instead of re-projecting:
a base member stays a member at session t only if, with information published by t 22:00 UTC,
  * its line has exactly one point-in-time issuer link that is the issuer's primary (P) line
    (identity-bridge rows start <= date(t) <= end_incl and available_at <= t 22:00, the
    prepare_research_fields LINK_RULE: the same cells the fundamentals / grp_* fields use);
  * the qualifying bridge row's class_status is ``common`` (r4 then-known common-share
    evidence; units, warrants, rights, preferreds and unknown classes fail);
  * the linked CIK has a visible SIC row (FSDS SUB, accepted_utc < date(t-1) 22:00 UTC, age
    <= 550 days: exactly the grp_ff12 clock, so finite(grp_ff12) == linked-P & visible SIC)
    whose code is not a pooled vehicle, blank check or royalty trust (6189 asset-backed, 6221
    commodity pools, 6722 / 6726 investment companies and trusts, 6770 blank checks = SPACs,
    6792 oil royalty traders and 6795 mineral royalty traders = royalty trusts, pass-through
    vehicles). REITs (6798) are operating companies here and stay.
Unlinked lines (ETFs, ETNs, most ADRs, unbridged names) fail the first test. Every payload but
member.u8 is copied byte for byte; the manifest keeps the base keys (membership_recipe included,
so role readers admit it) with score_member_counts recomputed and a ``universe`` block: the id,
the rule, pinned inputs, per-reason drop counts and the dropped member share per session.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import time

import duckdb
import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

DAY_NS = 86_400_000_000_000
SEAL = dt.date(2025, 1, 1)
COLUMNS = ("tradingDate", "securityID", "close", "volume", "cumulReturnFactor")
CACHE_SCHEMA = "atx.recent-research-projection/v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
CLOCK = "modeled-session+22h-mark+23h-decision-v1"
DEFAULT_UNIVERSE = "research-prior63-usd-adv-topn-v1"   # the membership rule above (unchanged)
LINKED_OPERATING_UNIVERSE = "linked-operating-v1"       # v6 revision V6-U
UNIVERSES = (DEFAULT_UNIVERSE, LINKED_OPERATING_UNIVERSE)
UNIVERSE_SIC_LAG_SESSIONS = 1     # the grp_* / fundamentals clock of fields-v6 (v4-prereg R2 --fund-lag-sessions 1)
UNIVERSE_SIC_STALE_DAYS = 550     # prepare_research_fields.GRP_STALE_DAYS (asserted at run time)
# Not operating companies: asset-backed (6189), commodity pools (6221), open-end funds (6722), unit investment
# trusts / closed-end funds (6726), blank checks (6770 = SPACs), royalty trusts (6792 oil royalty traders, 6795
# mineral royalty traders: pass-through vehicles; controller ruling, V6-W fix round 1). REITs (6798) stay.
NON_OPERATING_SIC = (6189, 6221, 6722, 6726, 6770, 6792, 6795)
UNIVERSE_CLASS = "common"
UNIVERSE_REASONS = ("unlinked", "ambiguous", "secondary_line", "class_not_common", "no_visible_sic",
                    "non_operating_sic")
LINKED_OPERATING_RULE = (
    "linked-operating-v1: base member at session t AND exactly one point-in-time identity-bridge link, on the issuer's "
    "primary (P) line (bridge row start <= date(t) <= end_incl, available_at <= date(t) 22:00 UTC; prepare_research_"
    "fields LINK_RULE) AND every qualifying bridge row has class_status 'common' AND the linked CIK's latest SIC row "
    "with accepted_utc < date(t-1) 22:00 UTC (lag 1 session) is <= 550 days old AND its SIC is not in "
    "non_operating_sic (pooled vehicles, blank checks, royalty trusts; REITs stay); reasons are assigned in the order unlinked, ambiguous, secondary_line, class_not_common, "
    "no_visible_sic, non_operating_sic (first failing test)")
UNIVERSE_PIT = (
    "every input is visible by the session's 22:00 UTC mark: bridge rows by available_at (T19: <= start 22:00), their "
    "class_status is the visible version's, SIC rows by accepted_utc one session earlier; nothing after the mark and "
    "nothing on or after 2025-01-01 is used; the decision at t 23:00 sees only the mark-t membership")
UNIVERSE_LIMITS = [
    "ADR lines of a linked, filing foreign issuer with common class evidence stay (V6-U drops ADRs without an issuer link)",
    "identity is the r4 rehearsal bridge (rehearsal_identity, scope_complete false): unbridged operating stocks drop",
    "membership_recipe keeps the base ADV top-N rule (an upper envelope); the universe block is the restriction",
]


def day(value: str | dt.date) -> int:
    return (dt.date.fromisoformat(value) if isinstance(value, str) else value).toordinal() - dt.date(1970, 1, 1).toordinal()


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


class Limits:
    def __init__(self, seconds=300, memory_bytes=768 << 20, disk_bytes=12 << 30):
        if not math.isfinite(seconds) or not 0 < seconds <= 600 or memory_bytes < 256 << 20 or disk_bytes < 64 << 20:
            raise ValueError("invalid resource budget")
        self.deadline = time.monotonic() + seconds
        self.memory_bytes, self.disk_bytes = memory_bytes, disk_bytes

    def check(self, stage: str):
        if time.monotonic() > self.deadline:
            raise TimeoutError(f"time budget at {stage}; preserve incomplete output; use fresh directory")

    def report(self, stage: str, **values):
        self.check(stage)
        print(canonical({"stage": stage, **values}), flush=True)

    def check_owned(self, directory: Path):
        # Only the newly owned artifact directory, never warehouse/source paths.
        size = 0
        for p in directory.rglob("*"):
            try:
                if p.is_file():
                    size += p.stat().st_size
            except FileNotFoundError:
                pass  # a private external-sort temporary was retired meanwhile
        if size > self.disk_bytes:
            raise ValueError("owned-directory total byte budget exceeded")
        self.check("owned-directory budget")
        return size


def sha_file(path: Path, limits: Limits | None = None) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
            if limits:
                limits.check("hash")
    return h.hexdigest()


def publish(path: Path, value, limits: Limits):
    # Exclusive directory ownership; never replace another completed manifest.
    content = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if len(content) > 1 << 20 or limits.check_owned(path.parent) + len(content) > limits.disk_bytes:
        raise ValueError("manifest publication exceeds metadata/total owned budget")
    pending = path.with_name("." + path.name + ".pending")
    with pending.open("xb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    # Atomic exclusive publication on the same filesystem, including Windows.
    # A reader sees either no manifest or its complete fsynced bytes.
    os.link(pending, path)
    pending.unlink()
    limits.check_owned(path.parent)


def sql_path(path: Path) -> str:
    return "'" + path.as_posix().replace("'", "''") + "'"


def check_source_schema(schema):
    expected = [pa.date32(), pa.int64(), pa.float32(), pa.float64(), pa.float64()]
    for name, dtype in zip(COLUMNS, expected):
        if schema.field(name).type != dtype:
            raise ValueError(f"unexpected {name} type; expected {dtype}")


def prepare_cache(source: Path, output: Path, begin: str, end: str, limits: Limits,
                  batch_rows=65536):
    """Filter five columns vectorially, spill to a PRIVATE bounded DuckDB, sort once."""
    first, last = dt.date.fromisoformat(begin), dt.date.fromisoformat(end)
    if not first < last <= SEAL or not 1024 <= batch_rows <= 65536:
        raise ValueError("projection needs ordered dates ending no later than2025 and bounded batch")
    captured = source.stat()
    pf = pq.ParquetFile(source)
    check_source_schema(pf.schema_arrow)
    # Conservative total source-row bound includes DB, sorted copy and temp spill.
    if pf.metadata.num_rows > 100_000_000 or pf.metadata.num_rows * 128 > limits.disk_bytes:
        raise ValueError("source-row worst-case projection disk bound exceeds budget")
    if shutil.disk_usage(output.parent).free < limits.disk_bytes:
        raise ValueError("insufficient admitted free disk")
    output.mkdir(parents=False, exist_ok=False)
    limits.report("source-hash-start", source_bytes=captured.st_size)
    source_sha = sha_file(source, limits)  # exactly once; cache reuse hashes cache only
    def unchanged():
        now = source.stat()
        return (captured.st_dev, captured.st_ino, captured.st_size, captured.st_mtime_ns) == (
            now.st_dev, now.st_ino, now.st_size, now.st_mtime_ns)
    if not unchanged():
        raise ValueError("source changed during initial hash")
    limits.report("source-hash-complete", source_sha256=source_sha)
    db_path, accepted = output / "projection.duckdb", output / "accepted.parquet"
    connection = duckdb.connect(str(db_path))
    memory_mib = max(64, (limits.memory_bytes - (192 << 20)) >> 20)
    connection.execute(f"SET memory_limit='{memory_mib}MiB'")
    connection.execute("SET threads=1")
    connection.execute("SET preserve_insertion_order=false")
    connection.execute(f"SET temp_directory={sql_path(output / 'spill')}")
    # Reserve the admitted fixed-width DB/output envelope outside the spill cap.
    spill_bytes = limits.disk_bytes - pf.metadata.num_rows * 128
    if spill_bytes < 1 << 20:
        connection.close()
        raise ValueError("no remaining external-sort spill budget")
    connection.execute(f"SET max_temp_directory_size='{spill_bytes >> 20}MiB'")
    connection.execute("CREATE TABLE rows(d DATE, id BIGINT, raw DOUBLE, volume DOUBLE, factor DOUBLE)")
    selected = 0
    try:
        for batch_index, batch in enumerate(pf.iter_batches(batch_size=batch_rows, columns=list(COLUMNS), use_threads=False)):
            limits.check("source-batch")
            # Nothing except date selection is computed on excluded rows.
            date = batch.column(0)
            mask = pc.and_(pc.greater_equal(date, pa.scalar(first)), pc.less(date, pa.scalar(last)))
            table = pa.Table.from_batches([batch.filter(pc.fill_null(mask, False))])
            if table.num_rows:
                table = table.rename_columns(["d", "id", "raw", "volume", "factor"])
                connection.register("selected_batch", table)
                connection.execute("INSERT INTO rows SELECT * FROM selected_batch")
                connection.unregister("selected_batch")
                selected += table.num_rows
            if batch_index % 16 == 0:
                limits.report("projection-batch", batch=batch_index, selected_rows=selected,
                              owned_bytes=limits.check_owned(output))
        calendar = connection.execute("SELECT DISTINCT d FROM rows ORDER BY d LIMIT 4097").fetchall()
        if len(calendar) > 4096:
            raise ValueError("selected source calendar exceeds bound")
        # count duplicate keys BEFORE numerical QA: all rows of a positive duplicate
        # key are quarantined, rather than choosing highest volume/current ticker.
        query = """WITH keyed AS (SELECT *, count(*) OVER(PARTITION BY d,id) AS copies FROM rows)
        SELECT d, id, raw, volume, raw*factor AS close FROM keyed
        WHERE id>0 AND copies=1 AND isfinite(raw) AND raw>0
          AND isfinite(volume) AND volume>=0 AND isfinite(factor) AND factor>0
          AND isfinite(raw*factor) AND raw*factor>0
        ORDER BY d,id"""
        limits.report("sort-start", selected_rows=selected)
        # interrupt() supplies a time bound even during the external sort.
        import threading
        timer = threading.Timer(max(.01, limits.deadline - time.monotonic()), connection.interrupt)
        timer.daemon = True
        stopped = threading.Event()
        disk_failure = []
        def monitor():
            while not stopped.wait(.25):
                try:
                    limits.check_owned(output)
                except (ValueError, TimeoutError) as error:
                    disk_failure.append(error)
                    connection.interrupt()
                    break
        monitor_thread = threading.Thread(target=monitor, daemon=True)
        monitor_thread.start()
        timer.start()
        try:
            connection.execute(f"COPY ({query}) TO {sql_path(accepted)} (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 65536)")
        finally:
            timer.cancel()
            stopped.set()
            monitor_thread.join()
        if disk_failure:
            raise disk_failure[0]
        limits.check("sort-complete")
        limits.check_owned(output)
    finally:
        connection.close()
    if not unchanged():
        raise ValueError("source changed during captured projection")
    metadata = pq.ParquetFile(accepted).metadata
    limits.check_owned(output)
    manifest = {"schema": CACHE_SCHEMA, "status": "complete", "source": str(source.resolve()),
                "source_sha256": source_sha, "source_bytes": captured.st_size,
                "source_mtime_ns": captured.st_mtime_ns, "start": begin, "end_exclusive": end,
                "source_rows": pf.metadata.num_rows, "selected_rows": selected,
                "session_days": [day(x[0]) for x in calendar],
                "calendar": "source-observed-session-labels-before-QA;not-exchange-authenticated",
                "accepted_rows": metadata.num_rows, "rejected_rows": selected - metadata.num_rows,
                "accepted": {"bytes": accepted.stat().st_size, "sha256": sha_file(accepted, limits)},
                "qa": "all-positive-duplicate-keys-quarantined;finite-positive-close-factor;nonnegative-volume-v1",
                "physical_decode": "mixed-era row groups decoded; dates filtered before QA/statistics/output",
                "raw_precision": "source-f32-close widened before f64 factor multiply",
                "common_stock_verified": False, "historical_vintage_verified": False}
    # Private spill database can be retained for audit, but is never a resumable
    # authority. Only this publish-last cache manifest grants reuse.
    publish(output / "manifest.json", manifest, limits)
    limits.report("projection-complete", accepted_rows=metadata.num_rows)
    return manifest


def cache_receipt(directory: Path, limits: Limits):
    path = directory / "manifest.json"
    if path.stat().st_size > 1 << 20:
        raise ValueError("oversized cache manifest")
    manifest_bytes = path.read_bytes()
    if len(manifest_bytes) > 1 << 20:
        raise ValueError("oversized changed cache manifest")
    m = json.loads(manifest_bytes)
    p = directory / "accepted.parquet"
    if m.get("schema") != CACHE_SCHEMA or m.get("status") != "complete":
        raise ValueError("unpublished projection cannot be resumed")
    stat = p.stat()
    identity = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
    if stat.st_size != m["accepted"]["bytes"] or sha_file(p, limits) != m["accepted"]["sha256"]:
        raise ValueError("cache bytes do not match immutable receipt")
    m["_admitted_manifest_sha256"] = hashlib.sha256(manifest_bytes).hexdigest()
    m["_admitted_file_identity"] = identity
    verify_cache(directory, m, limits)
    return m


def verify_cache(directory: Path, receipt, limits: Limits, *, rehash=False):
    stat = (directory / "accepted.parquet").stat()
    identity = (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)
    if identity != receipt["_admitted_file_identity"] or sha_file(directory / "manifest.json", limits) != receipt["_admitted_manifest_sha256"]:
        raise ValueError("admitted cache changed between role passes")
    if rehash and sha_file(directory / "accepted.parquet", limits) != receipt["accepted"]["sha256"]:
        raise ValueError("admitted cache content changed before role publication")
    # Hashing itself must not race a replacement/extent change.
    final = (directory / "accepted.parquet").stat()
    if (final.st_dev, final.st_ino, final.st_size, final.st_mtime_ns) != identity:
        raise ValueError("admitted cache changed during revalidation")


def date_rows(path: Path, begin: int, end: int, limits: Limits):
    """Sorted accepted batches -> bounded vectors for one date, no Python row loop."""
    pending = None
    pf = pq.ParquetFile(path)
    # The accepted cache is date sorted; unlike the vendor source, its footer
    # now permits skipping unrelated roles/years before numeric column decoding.
    groups = []
    for i in range(pf.metadata.num_row_groups):
        stats = pf.metadata.row_group(i).column(0).statistics
        if not stats or not stats.has_min_max or (day(stats.max) >= begin and day(stats.min) < end):
            groups.append(i)
    for b in pf.iter_batches(batch_size=65536, row_groups=groups, use_threads=False):
        limits.check("accepted-date-scan")
        a = [b.column(0).cast(pa.int32()).to_numpy(), *[b.column(i).to_numpy() for i in range(1, 5)]]
        keep = (a[0] >= begin) & (a[0] < end)
        if not keep.any():
            continue
        a = [v[keep] for v in a]
        if pending is not None:
            a = [np.concatenate((x, y)) for x, y in zip(pending, a)]
        split = np.flatnonzero(a[0][1:] != a[0][:-1]) + 1
        start = 0
        for stop in split:
            yield int(a[0][start]), tuple(x[start:stop] for x in a[1:])
            start = stop
        pending = [x[start:] for x in a]
        if len(pending[0]) > 100000:
            raise ValueError("one date exceeds bounded source names")
    if pending is not None:
        yield int(pending[0][0]), tuple(x for x in pending[1:])


def calendar_rows(path: Path, dates, limits: Limits):
    it = iter(date_rows(path, int(dates[0]), int(dates[-1]) + 1, limits))
    current = next(it, None)
    empty = (np.empty(0, dtype=np.int64), *(np.empty(0) for _ in range(3)))
    for d in dates:
        if current is not None and current[0] < d:
            raise ValueError("accepted axis is not ordered/bound to source calendar")
        if current is not None and current[0] == d:
            yield int(d), current[1]
            current = next(it, None)
        else:
            yield int(d), empty
    if current is not None:
        raise ValueError("accepted axis exceeds captured source calendar")


def create_role(cache: Path, output: Path, warmup: str, score_start: str, score_end: str,
                limits: Limits, top_n=3000, max_union=8000, max_output_bytes=1 << 30):
    m = cache_receipt(cache, limits)
    begin, start, end = map(day, (warmup, score_start, score_end))
    if not day(m["start"]) <= begin < start < end <= day(m["end_exclusive"]) <= day(SEAL):
        raise ValueError("role outside sealed reusable projection")
    if not 2 <= top_n <= max_union <= 20000 or max_output_bytes < 1 << 20:
        raise ValueError("invalid role budget/cohort")
    output.mkdir(parents=False, exist_ok=False)
    accepted = cache / "accepted.parquet"
    # Bounded axis discovery only after accepted date filtering. All-time IDs are
    # stable, never reidentified by today's ticker or compacted independently by year.
    all_ids = np.empty(0, dtype=np.int64)
    dates = np.asarray([d for d in m["session_days"] if begin <= d < end], dtype=np.int64)
    if not len(dates) or len(dates) > 4096 or np.any(np.diff(dates) <= 0):
        raise ValueError("invalid source session calendar")
    for d, row in calendar_rows(accepted, dates, limits):
        all_ids = np.union1d(all_ids, row[0])
        if len(all_ids) > 100000:
            raise ValueError("source-axis admission exceeded")
    verify_cache(cache, m, limits)
    score_begin = int(np.searchsorted(dates, start))
    if score_begin < 383 or score_begin >= len(dates):
        raise ValueError("role needs >=320 usable membership warmup sessions AFTER63 unready sessions")
    n, count = len(all_ids), len(dates)
    # ring + per-date vectors + Arrow source batch + selected membership records.
    required = n * (63 * 8 + 128) + count * top_n * 8 + (96 << 20)
    if required > limits.memory_bytes:
        raise ValueError("liquidity ring/date workspace exceeds memory budget")
    ring = np.full((63, n), np.nan)
    prior_raw = np.full(n, np.nan)
    members, selected_union = [], np.zeros(n, dtype=bool)
    for t, (_, row) in enumerate(calendar_rows(accepted, dates, limits)):
        # Membership at t is computed BEFORE today's bar enters any liquidity or
        # price statistic. Full 63 prior market sessions, no missing-name compression.
        if t >= 63:
            adv = np.mean(ring, axis=0)
            eligible = np.flatnonzero(np.isfinite(adv) & (adv > 5_000_000) & (prior_raw > 5))
            order = np.lexsort((all_ids[eligible], -adv[eligible]))
            chosen = eligible[order[:top_n]]
        else:
            chosen = np.empty(0, dtype=np.int64)
        members.append(np.sort(chosen))
        selected_union[chosen] = True
        slots = np.searchsorted(all_ids, row[0])
        ring[t % 63].fill(np.nan)
        with np.errstate(over="ignore", invalid="ignore"):
            dollars = row[1] * row[2]
        dollars[~np.isfinite(dollars)] = np.nan
        ring[t % 63, slots] = dollars
        prior_raw.fill(np.nan); prior_raw[slots] = row[1]
        if t % 128 == 0:
            limits.report("membership", dates=t + 1, source_names=n, selected_union=int(selected_union.sum()))
    verify_cache(cache, m, limits)
    union = np.flatnonzero(selected_union)
    if not len(union) or len(union) > max_union:
        raise ValueError("selected all-time union exceeds role budget or is empty")
    output_bytes = count * len(union) * 26 + count * 8 + len(union) * 8
    if output_bytes > max_output_bytes or output_bytes > limits.disk_bytes or shutil.disk_usage(output).free < output_bytes:
        raise ValueError("dense role byte/disk admission exceeded")
    # Release the large history ring before output mapping. Mapping bound is charged
    # together with remaining membership/Arrow scratch, not treated as free memory.
    del ring, prior_raw
    if output_bytes + count * top_n * 8 + (96 << 20) > limits.memory_bytes:
        raise ValueError("mapped role plus decode workspace exceeds joint budget")
    files = {}
    def write_axis(name, value):
        with (output / name).open("xb") as f:
            f.write(value.tobytes())
    write_axis("sessions.i64", (dates * DAY_NS).astype("<i8"))
    write_axis("ids.u64", all_ids[union].astype("<u8"))
    arrays = {}
    for name, dtype in (("close.f64", "<f8"), ("raw_close.f64", "<f8"), ("volume.f64", "<f8"), ("present.u8", "u1"), ("member.u8", "u1")):
        p = output / name
        with p.open("xb") as f:
            f.truncate(count * len(union) * np.dtype(dtype).itemsize)
        arrays[name] = np.memmap(p, mode="r+", dtype=dtype, shape=(count, len(union)))
        arrays[name][:] = np.nan if dtype == "<f8" else 0
    for t, (_, row) in enumerate(calendar_rows(accepted, dates, limits)):
        slots = np.searchsorted(all_ids, row[0])
        keep = selected_union[slots]
        dest = np.searchsorted(union, slots[keep])
        arrays["raw_close.f64"][t, dest] = row[1][keep]
        arrays["volume.f64"][t, dest] = row[2][keep]
        arrays["close.f64"][t, dest] = row[3][keep]
        arrays["present.u8"][t, dest] = 1
        arrays["member.u8"][t, np.searchsorted(union, members[t])] = 1
        if t % 128 == 0:
            limits.report("role-write", dates=t + 1, instruments=len(union))
    for a in arrays.values():
        a.flush()
        a._mmap.close()
    limits.check_owned(output)
    verify_cache(cache, m, limits, rehash=True)
    for name in ("sessions.i64", "ids.u64", *arrays):
        p = output / name
        files[name] = {"bytes": p.stat().st_size, "sha256": sha_file(p, limits)}
    membership = canonical({"rule": "research-prior63-usd-adv-topn-v1", "top_n": top_n,
        "min_raw_price_exclusive": 5, "min_adv_exclusive": 5_000_000,
        "lookback_sessions": 63, "lag_sessions": 1, "ties": "securityID-ascending",
        "missing": "complete-prior-calendar-window-required", "common_stock_verified": False})
    result = {"schema": ROLE_SCHEMA, "status": "complete", "dates": count, "instruments": len(union),
        "instrument_namespace": "spiderrock.securityID", "score_begin": score_begin, "score_end": count,
        "score_start_ns": start * DAY_NS, "score_end_ns": end * DAY_NS,
        "warmup_start": warmup, "source_sha256": m["source_sha256"],
        "projection_manifest_sha256": m["_admitted_manifest_sha256"],
        "membership_recipe": membership, "clock_recipe": CLOCK,
        "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor", "volume_basis": "raw-share-volume",
        "common_stock_verified": False, "historical_vintage_verified": False,
        "source_identity": "vendor-securityID; recycled-line risk unverified",
        "physical_presence": "accepted-current-row-independent-from-prior-membership",
        "declared_output_bytes": output_bytes,
        "score_member_counts": [int(len(x)) for x in members[score_begin:]], "files": files}
    verify_cache(cache, m, limits)
    publish(output / "manifest.json", result, limits)
    limits.report("role-complete", dates=count, instruments=len(union), bytes=output_bytes)
    return result


# ---------------------------------------------------------------------------
# Universe linked-operating-v1 (v6 revision V6-U): restrict an existing role
# ---------------------------------------------------------------------------

def classify_linked_operating(member, link, primary, not_common, sic_visible, sic_code):
    """Pure per-session classifier over the role's instruments (all inputs visible by the session mark).

    member      base decision membership (bool)
    link        dense CIK index of the qualifying bridge row (-1 unlinked, -2 two CIKs = ambiguous)
    primary     a qualifying row is the issuer's primary (P) line
    not_common  some qualifying row's class_status is not ``common``
    sic_visible the linked CIK has a visible SIC row no older than the staleness limit (primary-linked cells)
    sic_code    that row's SIC
    Returns (keep, reason): reason is -1 for non-members, 0 kept, else 1 + index into UNIVERSE_REASONS of the first
    failing test."""
    member = np.asarray(member, dtype=bool)
    link = np.asarray(link)
    primary = np.asarray(primary, dtype=bool)
    visible = np.asarray(sic_visible, dtype=bool)
    tests = (link == -1, link == -2, (link >= 0) & ~primary, np.asarray(not_common, dtype=bool), ~visible,
             np.isin(np.asarray(sic_code), NON_OPERATING_SIC))
    reason = np.where(member, 0, -1).astype(np.int8)
    for code, failed in enumerate(tests, start=1):
        reason[(reason == 0) & failed] = code
    return reason == 0, reason


def _bridge_class(prf, bridge: Path, bridge_sha256: str, role, links: dict, budget):
    """class_status == 'common' per kept bridge row, aligned with prepare_research_fields.load_bridge's rows."""
    m, _ = prf.pinned_manifest(bridge, bridge_sha256, "identity-bridge", prf.BRIDGE_ADAPTER["schema"])
    table, _ = prf.read_listed(bridge, m, prf.BRIDGE_ADAPTER, "identity-bridge", budget, extra=("class_status",))
    sid = prf.as_ids(prf.column_of(table, "sr_id"), "identity-bridge sr_id")
    avail = prf.as_instants_ns(prf.column_of(table, "available_at"), "identity-bridge available_at")
    known = pc.is_in(prf.as_text(prf.column_of(table, "primary")),
                     value_set=pa.array(list(prf.BRIDGE_KINDS))).to_numpy(zero_copy_only=False)
    excluded = pc.is_in(prf.as_text(prf.column_of(table, "basis")),
                        value_set=pa.array(list(prf.BRIDGE_EXCLUDED_BASES))).to_numpy(zero_copy_only=False)
    pos, on = role.columns_of(sid)
    keep = known & ~excluded & ~(avail >= prf.SEAL_NS) & on  # load_bridge's row filter, verbatim
    if not (np.array_equal(pos[keep], links["col"]) and np.array_equal(avail[keep], links["avail"])):
        raise ValueError("identity-bridge class rows do not align with the link rows")
    status = np.asarray(prf.as_text(prf.column_of(table, "class_status")).to_pylist(), dtype=object)[keep]
    values, counts = np.unique(status.astype(str), return_counts=True)
    return status == UNIVERSE_CLASS, {str(v): int(c) for v, c in zip(values, counts)}


def _check_fields(prf, fields: Path, fields_sha256: str, base_sha256: str, visible: np.ndarray, link_counts: dict,
                  limits: Limits) -> dict:
    """The u pass's fields manifest agrees with this classification: same role, same SIC lag, same
    link_member_cells, and finite(grp_ff12) == primary-linked & visible SIC on every cell."""
    blob = (fields / "manifest.json").read_bytes()
    if hashlib.sha256(blob).hexdigest() != fields_sha256.lower():
        raise ValueError("fields manifest SHA-256 does not match --check-fields-sha256")
    fm = json.loads(blob)
    if fm.get("schema") != prf.SCHEMA or fm.get("status") != "complete":
        raise ValueError("--check-fields is not a complete research-role-fields manifest")
    if (fm.get("role") or {}).get("manifest_sha256") != base_sha256.lower():
        raise ValueError("--check-fields is bound to another role than --base-role")
    issuer = (fm.get("source_checks") or {}).get("issuer") or {}
    if issuer.get("fund_lag_sessions") != UNIVERSE_SIC_LAG_SESSIONS:
        raise ValueError("--check-fields grp_* clock lag differs from the universe SIC lag")
    if issuer.get("link_member_cells") != link_counts:
        raise ValueError(f"link_member_cells {link_counts} differ from the fields manifest's "
                         f"{issuer.get('link_member_cells')}")
    entry = next((f for f in fm.get("fields", []) if f.get("name") == "grp_ff12"), None)
    if entry is None or entry.get("shape") != list(visible.shape):
        raise ValueError("--check-fields has no grp_ff12 on the base role's axes")
    path = fields / entry["file"]
    if sha_file(path, limits) != entry["sha256"]:
        raise ValueError("grp_ff12 bytes do not match the fields manifest")
    grid = np.memmap(path, dtype="<f8", mode="r", shape=visible.shape)
    try:
        mismatched = int(np.count_nonzero(np.isfinite(grid) != visible))
    finally:
        del grid
    if mismatched:
        raise ValueError(f"finite(grp_ff12) differs from primary-linked & visible SIC on {mismatched} cells")
    return {"fields_manifest_sha256": fields_sha256.lower(), "grp_ff12_sha256": entry["sha256"],
            "link_member_cells_equal": True, "grp_ff12_finite_equals_linked_primary_visible_sic_cells": int(visible.size)}


def restrict_role(base: Path, base_sha256: str, output: Path, limits: Limits, *, bridge: Path, bridge_sha256: str,
                  sic_events: Path, sic_events_sha256: str, fields: Path | None = None,
                  fields_sha256: str | None = None, universe: str = LINKED_OPERATING_UNIVERSE):
    """``--universe linked-operating-v1``: a new role = the base role with member.u8 restricted (see module doc)."""
    import prepare_research_fields as prf  # same directory: the fields' own link / SIC semantics
    if universe != LINKED_OPERATING_UNIVERSE:
        raise ValueError(f"restrict_role implements {LINKED_OPERATING_UNIVERSE} only")
    if prf.GRP_STALE_DAYS != UNIVERSE_SIC_STALE_DAYS:
        raise ValueError("prepare_research_fields.GRP_STALE_DAYS changed; the universe SIC staleness is declared 550")
    if (fields is None) != (fields_sha256 is None):
        raise ValueError("--check-fields and --check-fields-sha256 go together")
    budget = prf.Budget(max_rss_mib=limits.memory_bytes >> 20,
                        max_seconds=max(1.0, min(7200.0, limits.deadline - time.monotonic())))
    role = prf.Role(base, base_sha256)  # pin, schema, namespace, axes, member bytes, < 2025 seal
    m = role.manifest
    if m.get("universe") is not None:
        raise ValueError("base role already carries a universe restriction")
    nd, n = role.n_dates, role.n
    budget.admit(nd * n * 10 + (64 << 20), "universe-link-matrix")
    links, bridge_sources, bridge_st = prf.load_bridge(bridge, bridge_sha256, role, budget)
    common_rows, class_tally = _bridge_class(prf, bridge, bridge_sha256, role, links, budget)
    marks = role.days * DAY_NS + prf.MARK_NS
    ciks, link, primary, link_st = prf.resolve_links(links, role, marks, budget)
    # a cell is not common when any row qualifying there (resolve_links' own qualification) is not common
    t_lo = np.maximum(np.searchsorted(role.days, links["start"], side="left"),
                      np.searchsorted(marks, links["avail"], side="left"))
    t_hi = np.searchsorted(role.days, links["end"], side="right")
    not_common = np.zeros((nd, n), dtype=bool)
    for k in np.flatnonzero(~common_rows):
        if t_lo[k] < t_hi[k]:
            not_common[int(t_lo[k]):int(t_hi[k]), int(links["col"][k])] = True
    em, events_source = prf.pinned_manifest(sic_events, sic_events_sha256, "fund-events", prf.EVENTS_ADAPTER["schema"])
    sic, sic_sources, sic_st = prf.load_events(sic_events, em, prf.SIC_ADAPTER, [], ciks, "sic-events", budget)
    limits.check("universe-inputs")
    latest = np.full(len(ciks), -1, dtype=np.int64)
    pointer = 0
    kept = np.zeros((nd, n), dtype=np.uint8)
    visible_all = np.zeros((nd, n), dtype=bool)
    base_counts, kept_counts = [], []
    by_reason = np.zeros(len(UNIVERSE_REASONS), dtype=np.int64)
    by_reason_score = np.zeros(len(UNIVERSE_REASONS), dtype=np.int64)
    link_counts = {"member_cells": 0, "unlinked": 0, "ambiguous": 0, "secondary": 0, "primary": 0}
    for t in range(nd):
        if t >= UNIVERSE_SIC_LAG_SESSIONS:
            pointer = prf.advance(sic, latest, pointer, int(marks[t - UNIVERSE_SIC_LAG_SESSIONS]))
        member, lk = role.member[t] != 0, link[t]
        pline = (lk >= 0) & primary[t]
        s = np.where(pline, latest[np.maximum(lk, 0)], -1)
        ss = np.maximum(s, 0)
        if len(sic["clock"]):
            visible = (s >= 0) & ((int(role.days[t]) - sic["clock"][ss] // DAY_NS) <= UNIVERSE_SIC_STALE_DAYS)
            code = np.where(visible, sic["sic"][ss], 0)
        else:
            visible, code = np.zeros(n, dtype=bool), np.zeros(n, dtype=np.int64)
        visible_all[t] = visible
        keep, reason = classify_linked_operating(member, lk, primary[t], not_common[t], visible, code)
        kept[t] = keep
        base_counts.append(int(np.count_nonzero(member)))
        kept_counts.append(int(np.count_nonzero(keep)))
        tally = np.bincount(reason[reason > 0].astype(np.int64) - 1, minlength=len(UNIVERSE_REASONS))
        by_reason += tally
        if t >= role.score_begin:
            by_reason_score += tally
        link_counts["member_cells"] += int(np.count_nonzero(member))
        link_counts["unlinked"] += int(np.count_nonzero(member & (lk == -1)))
        link_counts["ambiguous"] += int(np.count_nonzero(member & (lk == -2)))
        link_counts["secondary"] += int(np.count_nonzero(member & (lk >= 0) & ~pline))
        link_counts["primary"] += int(np.count_nonzero(member & pline))
        if t % 128 == 0:
            limits.report("universe", dates=t + 1, base_members=base_counts[-1], kept=kept_counts[-1])
    crosscheck = None
    if fields is not None:
        crosscheck = _check_fields(prf, fields, fields_sha256, base_sha256, visible_all, link_counts, limits)
    del visible_all, not_common, link, primary
    # Output: every base payload byte for byte, member.u8 restricted, manifest last (exclusive).
    output.mkdir(parents=False, exist_ok=False)
    files = {}
    for name, entry in sorted(m["files"].items()):
        if name == "member.u8":
            continue
        h, size = hashlib.sha256(), 0
        with (base / name).open("rb") as src, (output / name).open("xb") as dst:
            while chunk := src.read(1 << 20):
                h.update(chunk)
                dst.write(chunk)
                size += len(chunk)
                limits.check("universe-copy")
        if size != entry["bytes"] or h.hexdigest() != entry["sha256"]:
            raise ValueError(f"base role {name} bytes do not match its manifest")
        files[name] = {"bytes": size, "sha256": h.hexdigest()}
    blob = kept.tobytes()
    with (output / "member.u8").open("xb") as f:
        f.write(blob)
    files["member.u8"] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    if files["member.u8"]["bytes"] != m["files"]["member.u8"]["bytes"]:
        raise ValueError("restricted member.u8 changed extent")
    limits.check_owned(output)
    base_cells, kept_cells = int(sum(base_counts)), int(sum(kept_counts))
    score = slice(role.score_begin, nd)
    base_score, kept_score = int(sum(base_counts[score])), int(sum(kept_counts[score]))
    share = [round(1.0 - k / b, 6) if b else None for b, k in zip(base_counts, kept_counts)]
    result = json.loads(json.dumps(m))
    result["files"] = files
    result["score_member_counts"] = kept_counts[role.score_begin:]
    result["universe"] = {
        "id": universe, "rule": LINKED_OPERATING_RULE, "point_in_time": UNIVERSE_PIT, "limits": UNIVERSE_LIMITS,
        "base_role": {"path": str(base.resolve()), "manifest_sha256": base_sha256.lower(),
                      "membership_recipe": m["membership_recipe"], "member_sha256": m["files"]["member.u8"]["sha256"]},
        "class_status_required": UNIVERSE_CLASS, "sic_lag_sessions": UNIVERSE_SIC_LAG_SESSIONS,
        "sic_stale_days": UNIVERSE_SIC_STALE_DAYS, "non_operating_sic": list(NON_OPERATING_SIC),
        "reasons": list(UNIVERSE_REASONS),
        "inputs": {"identity_bridge": {"manifest_sha256": bridge_sha256.lower(), "sources": bridge_sources,
                                       "checks": {**bridge_st, **link_st}, "class_status_rows": class_tally},
                   "sic_events": {"manifest_sha256": sic_events_sha256.lower(), "sources": [events_source] + sic_sources,
                                  "checks": sic_st},
                   "link_rule": prf.LINK_RULE,
                   "code": {"prepare_recent_research": prf.code_identity(Path(__file__).resolve()),
                            "prepare_research_fields": prf.code_identity(Path(prf.__file__).resolve())}},
        "link_member_cells": link_counts, "fields_crosscheck": crosscheck,
        "base_member_cells": base_cells, "kept_member_cells": kept_cells,
        "dropped_by_reason": {r: int(x) for r, x in zip(UNIVERSE_REASONS, by_reason)},
        "dropped_by_reason_score_window": {r: int(x) for r, x in zip(UNIVERSE_REASONS, by_reason_score)},
        "dropped_member_share_all": round(1.0 - kept_cells / base_cells, 6) if base_cells else None,
        "dropped_member_share_score_window": round(1.0 - kept_score / base_score, 6) if base_score else None,
        "base_member_counts": base_counts, "kept_member_counts": kept_counts,
        "dropped_member_share": share,
        "dropped_member_share_definition": "per session: 1 - kept members / base members (null: no base member)",
    }
    publish(output / "manifest.json", result, limits)
    limits.report("universe-complete", universe=universe, base_member_cells=base_cells, kept_member_cells=kept_cells)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode", choices=("project", "role"))
    p.add_argument("--source", type=Path); p.add_argument("--cache", type=Path)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--start", default="2018-06-01"); p.add_argument("--end", default="2025-01-01")
    p.add_argument("--score-start", default="2020-01-01")
    p.add_argument("--top-n", type=int, default=3000); p.add_argument("--max-union", type=int, default=8000)
    p.add_argument("--memory-mib", type=int, default=768); p.add_argument("--disk-mib", type=int, default=12288)
    p.add_argument("--max-output-mib", type=int, default=1024); p.add_argument("--max-seconds", type=float, default=300)
    p.add_argument("--universe", choices=UNIVERSES, default=DEFAULT_UNIVERSE,
                   help=f"role membership universe: {DEFAULT_UNIVERSE} (default; ADV top-N from --cache) or "
                        f"{LINKED_OPERATING_UNIVERSE} (restricts --base-role with the pinned identity bridge and SIC events)")
    p.add_argument("--base-role", type=Path); p.add_argument("--base-role-sha256")
    p.add_argument("--identity-bridge", type=Path); p.add_argument("--identity-bridge-sha256")
    p.add_argument("--sic-events", type=Path, help="fundamental events directory holding sic_events.parquet")
    p.add_argument("--sic-events-sha256")
    p.add_argument("--check-fields", type=Path, help="optional: the base role's fields directory to cross-check")
    p.add_argument("--check-fields-sha256")
    a = p.parse_args(); limits = Limits(a.max_seconds, a.memory_mib << 20, a.disk_mib << 20)
    linked = (a.base_role, a.base_role_sha256, a.identity_bridge, a.identity_bridge_sha256, a.sic_events,
              a.sic_events_sha256, a.check_fields, a.check_fields_sha256)
    if a.mode == "project":
        if a.source is None: p.error("project requires --source")
        if a.universe != DEFAULT_UNIVERSE or any(x is not None for x in linked):
            p.error("--universe and its inputs apply to mode role only")
        prepare_cache(a.source, a.out, a.start, a.end, limits)
    elif a.universe == DEFAULT_UNIVERSE:
        if a.cache is None: p.error("role requires --cache")
        if any(x is not None for x in linked):
            p.error(f"--base-role/--identity-bridge/--sic-events/--check-fields need --universe {LINKED_OPERATING_UNIVERSE}")
        create_role(a.cache, a.out, a.start, a.score_start, a.end, limits, a.top_n, a.max_union, a.max_output_mib << 20)
    else:
        if a.cache is not None: p.error(f"--universe {LINKED_OPERATING_UNIVERSE} restricts --base-role; --cache is not read")
        if any(x is None for x in linked[:6]):
            p.error(f"--universe {LINKED_OPERATING_UNIVERSE} requires --base-role/--identity-bridge/--sic-events and "
                    "their -sha256 pins")
        restrict_role(a.base_role, a.base_role_sha256, a.out, limits, bridge=a.identity_bridge,
                      bridge_sha256=a.identity_bridge_sha256, sic_events=a.sic_events,
                      sic_events_sha256=a.sic_events_sha256, fields=a.check_fields, fields_sha256=a.check_fields_sha256)


if __name__ == "__main__":
    main()
