"""Bounded research projection and lagged-liquidity role writer.

No warehouse access. Raw Parquet row groups mix years: projected columns are
physically decoded, then dates are filtered BEFORE numeric QA/statistics/output.
This is archive research, not authenticated publication or common-stock evidence.
Only completed immutable projection caches can be resumed/reused.
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


def publish(path: Path, value):
    # Exclusive directory ownership; never replace another completed manifest.
    with path.open("x", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n")
        f.flush()
        os.fsync(f.fileno())


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
    publish(output / "manifest.json", manifest)
    limits.report("projection-complete", accepted_rows=metadata.num_rows)
    return manifest


def cache_receipt(directory: Path, limits: Limits):
    path = directory / "manifest.json"
    if path.stat().st_size > 1 << 20:
        raise ValueError("oversized cache manifest")
    m = json.loads(path.read_text(encoding="utf-8"))
    p = directory / "accepted.parquet"
    if m.get("schema") != CACHE_SCHEMA or m.get("status") != "complete":
        raise ValueError("unpublished projection cannot be resumed")
    if p.stat().st_size != m["accepted"]["bytes"] or sha_file(p, limits) != m["accepted"]["sha256"]:
        raise ValueError("cache bytes do not match immutable receipt")
    return m


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
        "projection_manifest_sha256": sha_file(cache / "manifest.json"),
        "membership_recipe": membership, "clock_recipe": CLOCK,
        "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor", "volume_basis": "raw-share-volume",
        "common_stock_verified": False, "historical_vintage_verified": False,
        "source_identity": "vendor-securityID; recycled-line risk unverified",
        "physical_presence": "accepted-current-row-independent-from-prior-membership",
        "declared_output_bytes": output_bytes,
        "score_member_counts": [int(len(x)) for x in members[score_begin:]], "files": files}
    publish(output / "manifest.json", result)
    limits.report("role-complete", dates=count, instruments=len(union), bytes=output_bytes)
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
    a = p.parse_args(); limits = Limits(a.max_seconds, a.memory_mib << 20, a.disk_mib << 20)
    if a.mode == "project":
        if a.source is None: p.error("project requires --source")
        prepare_cache(a.source, a.out, a.start, a.end, limits)
    else:
        if a.cache is None: p.error("role requires --cache")
        create_role(a.cache, a.out, a.start, a.score_start, a.end, limits, a.top_n, a.max_union, a.max_output_mib << 20)


if __name__ == "__main__":
    main()
