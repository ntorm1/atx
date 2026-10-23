"""Label-free universe census runner (lane U0, vol-sota r6, 2026-10-23).

Runs atx-vol-quote-bench over the L5u label-free universe (universe-20261014) under declared compute bounds and
tabulates support, refusal, cost and failure modes. Nothing is scored: the dataset has no labels and this tool
never reads marks, vendor scores or holdout membership.

Subcommands:
  verify     recompute the dataset manifest counts and the per-stratum chain lists (sha256 of everything)
  plan       draw the stratified sample (or order the full board) and pack whole underliers into job CSVs
  run        run one checkpointed chunk of a plan (resumable; bounded wall, memory, per-chain timeout)
  summarize  aggregate a run's job outputs into the census tables

Units. A chain is (underlier, root, snapshot, expiry) = the dataset's chain_id. A job is a set of WHOLE
underliers, because fit_quote_surface fits each (underlier, root, snapshot) surface jointly.

Strata (chain level; see docs/universe-census.md):
  U underlier class   two-sided contracts of the underlier: U1 >= 500, U2 100-499, U3 25-99, U4 < 25
  C chain size        two-sided contracts of the chain: C0 0, C1 1-10, C2 11-40, C3 41-100, C4 > 100
  E expiry count      distinct expiries of the underlier: E1 <= 3, E2 4-5, E3 6-11, E4 >= 12
  D cash dividend     D1 when any row of the chain has ddiv_pv > 0
  T days to expiry    T0 0DTE, T1 1-3, T2 4-30, T3 31-365, T4 > 365
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import math
import os
import subprocess
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import polars as pl

try:  # psutil lives in the user site on this host: run with -E -P, not -I
    import psutil
except ImportError:  # pragma: no cover - the runner refuses below
    psutil = None

NS_PER_DAY = 86_400 * 1_000_000_000
UNDERLIER_FIELD = 4  # 0-based column of `underlier` in the quote-bench CSV layout
CHAIN_FIELD = 1
TIERS = ("U1", "U2", "U3", "U4")


# ── strata ───────────────────────────────────────────────────────────────────────────────────────────


def underlier_class(und_two_sided: int) -> str:
    if und_two_sided >= 500:
        return "U1"
    if und_two_sided >= 100:
        return "U2"
    if und_two_sided >= 25:
        return "U3"
    return "U4"


def chain_size_class(two_sided: int) -> str:
    if two_sided <= 0:
        return "C0"
    if two_sided <= 10:
        return "C1"
    if two_sided <= 40:
        return "C2"
    if two_sided <= 100:
        return "C3"
    return "C4"


def expiry_count_class(expiries: int) -> str:
    if expiries <= 3:
        return "E1"
    if expiries <= 5:
        return "E2"
    if expiries <= 11:
        return "E3"
    return "E4"


def dte_class(dte: int) -> str:
    if dte <= 0:
        return "T0"
    if dte <= 3:
        return "T1"
    if dte <= 30:
        return "T2"
    if dte <= 365:
        return "T3"
    return "T4"


def chain_table(quotes: pl.DataFrame) -> pl.DataFrame:
    """One row per chain_id: counts, days to expiry, dividend flag, exercise and the underlier axes."""
    two_sided = (
        pl.col("bid").is_finite() & pl.col("ask").is_finite() & (pl.col("bid") > 0) & (pl.col("ask") > 0)
        & (pl.col("ask") >= pl.col("bid"))
    )
    rows = quotes.with_columns(
        two_sided.alias("_two"),
        (pl.col("expiry_days") - (pl.col("snapshot_ns") // NS_PER_DAY)).cast(pl.Int64).alias("_dte"),
    )
    chains = rows.group_by("chain_id").agg(
        pl.col("underlier").first(),
        pl.col("root").first(),
        pl.col("expiry_days").first(),
        pl.col("_dte").first().alias("dte"),
        pl.len().alias("rows"),
        pl.col("_two").sum().cast(pl.Int64).alias("two_sided"),
        (pl.col("ddiv_pv") > 0).any().alias("ddiv"),
        (pl.col("exercise").first() == "American").alias("american"),
        (pl.col("input_status") == "ok").sum().cast(pl.Int64).alias("eligible"),
        pl.col("years").median().alias("years"),
    )
    und = rows.group_by("underlier").agg(
        pl.col("_two").sum().cast(pl.Int64).alias("und_two_sided"),
        pl.col("expiry_days").n_unique().cast(pl.Int64).alias("und_expiries"),
        pl.col("chain_id").n_unique().cast(pl.Int64).alias("und_chains"),
    )
    return chains.join(und, on="underlier", how="left")


def assign_strata(chains: pl.DataFrame) -> pl.DataFrame:
    out = chains.with_columns(
        pl.col("und_two_sided").map_elements(underlier_class, return_dtype=pl.Utf8).alias("U"),
        pl.col("two_sided").map_elements(chain_size_class, return_dtype=pl.Utf8).alias("C"),
        pl.col("und_expiries").map_elements(expiry_count_class, return_dtype=pl.Utf8).alias("E"),
        pl.when(pl.col("ddiv")).then(pl.lit("D1")).otherwise(pl.lit("D0")).alias("D"),
        pl.col("dte").map_elements(dte_class, return_dtype=pl.Utf8).alias("T"),
    )
    return out.with_columns(
        pl.concat_str([pl.col("U"), pl.col("C"), pl.col("E"), pl.col("D"), pl.col("T")], separator="|")
        .alias("stratum")
    )


def hash_key(seed: str, underlier: str) -> str:
    return hashlib.sha256(f"{seed}|{underlier}".encode()).hexdigest()


def hash_order(underliers: list[str], seed: str) -> list[str]:
    return sorted(underliers, key=lambda u: hash_key(seed, u))


@dataclass
class SampleDraw:
    underliers: list[str]
    report: dict[str, Any] = field(default_factory=dict)


def _largest_remainder(total: int, weights: dict[str, int]) -> dict[str, int]:
    mass = sum(weights.values())
    if mass == 0:
        return {k: 0 for k in weights}
    raw = {k: total * w / mass for k, w in weights.items()}
    out = {k: math.floor(v) for k, v in raw.items()}
    rest = total - sum(out.values())
    for k in sorted(raw, key=lambda k: (-(raw[k] - out[k]), k))[:rest]:
        out[k] += 1
    return out


def draw_sample(chains: pl.DataFrame, *, seed: str, target_frac: float, max_frac: float) -> SampleDraw:
    """Cover every non-empty stratum with whole underliers, then fill by tier to target_frac (<= max_frac)."""
    n_total = chains.height
    max_chains = math.floor(max_frac * n_total)
    target = min(round(target_frac * n_total), max_chains)
    und_strata: dict[str, set[str]] = defaultdict(set)
    und_n: Counter[str] = Counter()
    und_tier: dict[str, str] = {}
    stratum_unds: dict[str, list[str]] = defaultdict(list)
    for u, s, t in chains.select(["underlier", "stratum", "U"]).iter_rows():
        und_strata[u].add(s)
        und_n[u] += 1
        und_tier[u] = t
    for u, strata in und_strata.items():
        for s in strata:
            stratum_unds[s].append(u)
    key = {u: hash_key(seed, u) for u in und_n}
    counts = Counter(chains["stratum"].to_list())
    covered: set[str] = set()
    picked: list[str] = []
    picked_set: set[str] = set()
    total = 0
    for s in sorted(counts, key=lambda s: (counts[s], s)):
        if s in covered:
            continue
        best = min(stratum_unds[s], key=lambda u: (-len(und_strata[u] - covered) / und_n[u], key[u]))
        picked.append(best)
        picked_set.add(best)
        covered |= und_strata[best]
        total += und_n[best]
    cover_underliers, cover_chains = len(picked), total
    if total > max_chains:
        raise ValueError(f"stratum cover needs {total} chains, over the budget of {max_chains}")
    tier_total = Counter(und_tier[u] for u in chains["underlier"].to_list())
    quota = _largest_remainder(target, {t: tier_total.get(t, 0) for t in TIERS})
    have = Counter()
    for u in picked:
        have[und_tier[u]] += und_n[u]
    by_tier: dict[str, list[str]] = defaultdict(list)
    for u in und_n:
        by_tier[und_tier[u]].append(u)
    for t in TIERS:
        for u in sorted(by_tier[t], key=lambda u: key[u]):
            if have[t] >= quota[t]:
                break
            if u in picked_set or total + und_n[u] > max_chains:
                continue
            picked.append(u)
            picked_set.add(u)
            have[t] += und_n[u]
            total += und_n[u]
    if covered != set(counts) or total > max_chains:
        raise ValueError("draw failed its own check (cover or budget)")
    ordered = sorted(picked, key=lambda u: key[u])
    report = {
        "seed": seed, "rule": "cover rarest stratum first by max new strata per chain; fill by tier, largest "
        "remainder, sha256(seed|underlier) order; whole underliers",
        "universe_chains": n_total, "target_chains": target, "max_chains": max_chains,
        "chains": total, "fraction": total / n_total if n_total else 0.0, "underliers": len(ordered),
        "strata_nonempty": len(counts), "strata_covered": len(covered),
        "cover_underliers": cover_underliers, "cover_chains": cover_chains,
        "tier_quota": quota, "tier_chains": {t: have.get(t, 0) for t in TIERS},
    }
    return SampleDraw(ordered, report)


def pack_jobs(underliers: list[str], chains_per_underlier: dict[str, int], *, max_chains: int) -> list[dict]:
    """Sequential packing in the given order; an underlier above max_chains is a job by itself."""
    jobs: list[dict] = []
    current: list[str] = []
    size = 0

    def close() -> None:
        nonlocal current, size
        if current:
            jobs.append({"id": f"j{len(jobs):05d}", "underliers": current, "chains": size})
        current, size = [], 0

    for u in underliers:
        n = chains_per_underlier[u]
        if current and size + n > max_chains:
            close()
        current.append(u)
        size += n
        if size >= max_chains:
            close()
    close()
    return jobs


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _field(line: bytes, index: int) -> bytes:
    """Field `index` of one CSV line. The universe CSV (polars) quotes every string field; a quoted field
    that holds a comma or a quote cannot be split this way and is refused (the universe has none:
    every line carries exactly 24 commas)."""
    value = line.split(b",", index + 1)[index].rstrip(b"\r\n")
    if value.startswith(b'"'):
        if len(value) < 2 or not value.endswith(b'"') or b'"' in value[1:-1]:
            raise ValueError("quoted CSV field with an embedded comma or quote: the line slicer cannot split it")
        return value[1:-1]
    return value


def write_job_csvs(src_csv: Path, jobs: list[dict], out_dir: Path, *, batch: int = 400) -> dict[str, dict]:
    """Copy each job's rows out of the universe CSV byte for byte (header first, source order kept)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    owner = {u.encode(): j["id"] for j in jobs for u in j["underliers"]}
    ids = [j["id"] for j in jobs]
    rows: Counter[str] = Counter()
    for start in range(0, len(ids), batch):
        chunk = set(ids[start:start + batch])
        handles = {}
        try:
            with open(src_csv, "rb") as src:
                header = src.readline()
                for jid in sorted(chunk):
                    handles[jid] = open(out_dir / f"{jid}.csv", "xb")
                    handles[jid].write(header)
                for line in src:
                    jid = owner.get(_field(line, UNDERLIER_FIELD))
                    if jid in chunk:
                        handles[jid].write(line if line.endswith(b"\n") else line + b"\n")
                        rows[jid] += 1
        finally:
            for h in handles.values():
                h.close()
    return {jid: {"rows": rows[jid], "path": str(out_dir / f"{jid}.csv"),
                  "sha256": _sha256_file(out_dir / f"{jid}.csv")} for jid in ids}


def split_csv_by_underlier(src_csv: Path, out_paths: dict[str, Path]) -> dict[str, dict]:
    """Retry CSVs: one per underlier of a failed job; returns rows and distinct chains per underlier."""
    handles = {}
    rows: Counter[str] = Counter()
    chains: dict[str, set[bytes]] = defaultdict(set)
    try:
        with open(src_csv, "rb") as src:
            header = src.readline()
            for u, p in out_paths.items():
                p.parent.mkdir(parents=True, exist_ok=True)
                handles[u] = open(p, "xb")
                handles[u].write(header)
            for line in src:
                u = _field(line, UNDERLIER_FIELD).decode()
                if u in handles:
                    handles[u].write(line if line.endswith(b"\n") else line + b"\n")
                    rows[u] += 1
                    chains[u].add(_field(line, CHAIN_FIELD))
    finally:
        for h in handles.values():
            h.close()
    return {u: {"rows": rows[u], "chains": len(chains[u])} for u in out_paths}


# ── runner ───────────────────────────────────────────────────────────────────────────────────────────


class _PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]


def _os_peak(proc: subprocess.Popen) -> tuple[int | None, int | None]:
    """PeakWorkingSetSize / PeakPagefileUsage of an exited child (Windows; None elsewhere)."""
    if sys.platform != "win32":
        return None, None
    pmc = _PMC()
    pmc.cb = ctypes.sizeof(_PMC)
    handle = getattr(proc, "_handle", None)
    if handle is None:
        return None, None
    ok = ctypes.windll.kernel32.K32GetProcessMemoryInfo(ctypes.c_void_p(int(handle)), ctypes.byref(pmc),
                                                        pmc.cb)
    return (int(pmc.PeakWorkingSetSize), int(pmc.PeakPagefileUsage)) if ok else (None, None)


def _wset(ps_proc) -> int:
    mi = ps_proc.memory_info()
    return int(getattr(mi, "wset", mi.rss))


def _utc() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def _read_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(x) for x in path.read_text().splitlines() if x.strip()]


@dataclass
class _Running:
    job: dict
    attempt: int
    proc: subprocess.Popen
    ps: Any
    start: float
    budget: float
    out_dir: Path
    files: list
    launch_utc: str
    max_wset: int = 0
    last_wset: int = 0


def _job_universe(plan_path: Path, records: list[dict]) -> tuple[list[dict], dict[str, dict]]:
    plan = json.loads(plan_path.read_text())
    order: list[dict] = []
    by_id: dict[str, dict] = {}
    for j in plan["jobs"]:
        spec = dict(j, csv=str((plan_path.parent / j["csv"]).resolve()), parent=None)
        order.append(spec)
        by_id[spec["id"]] = spec
    for r in records:
        for child in r.get("children_spec", []):
            if child["id"] not in by_id:
                order.append(child)
                by_id[child["id"]] = child
    return order, by_id


def _first_line(path: Path) -> str:
    try:
        for line in path.read_text(errors="replace").splitlines():
            if line.strip():
                return line.strip()[:300]
    except OSError:
        pass
    return ""


def run_chunk(plan_path: Path, state_dir: Path, *, command: list[str], bench_args: list[str],
              policy: str | None, workers: int | None, k: int, chunk_wall_s: float, kill_at_s: float,
              total_cap_s: float, per_chain_timeout_s: float, mem_cap_bytes: int, host_floor_bytes: int,
              progress_every_s: float = 60.0, poll_s: float = 0.1) -> dict:
    """One bounded, checkpointed chunk: launch pending jobs (K in flight) until chunk_wall_s, kill what is
    still running at kill_at_s (re-run next chunk), never past total_cap_s of cumulative chunk wall."""
    if psutil is None:
        raise RuntimeError("psutil is required (run with -E -P so the user site is visible)")
    plan_path = Path(plan_path)
    state_dir = Path(state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    jobs_log = state_dir / "jobs.jsonl"
    chunks_log = state_dir / "chunks.jsonl"
    progress = open(state_dir / "progress.log", "a", buffering=1)
    records = _read_jsonl(jobs_log)
    chunks = _read_jsonl(chunks_log)
    spent = sum(c.get("wall_s", 0.0) for c in chunks)
    remaining = total_cap_s - spent
    chunk_no = len(chunks) + 1
    order, _ = _job_universe(plan_path, records)
    attempts = Counter(r["job"] for r in records)
    final = {r["job"] for r in records if r["final"]}
    pending = [j for j in order if j["id"] not in final]

    def log(line: str) -> None:
        progress.write(line + "\n")

    if remaining <= 0:
        log(f"CHUNK_REFUSED utc={_utc()} chunk={chunk_no} cumulative_wall_s={spent:.1f} cap_s={total_cap_s}")
        progress.close()
        return {"status": "cap_reached", "launched": 0, "pending": len(pending), "cumulative_wall_s": spent}
    kill_at = min(kill_at_s, remaining)
    launch_until = max(0.0, min(chunk_wall_s, kill_at - (kill_at_s - chunk_wall_s)))
    env = dict(os.environ)
    env.pop("ATX_VOL_FAMILY_ROOT_AUDIT", None)
    if workers is not None:
        env["ATX_VOL_FIT_WORKERS"] = str(workers)
    t0 = time.monotonic()
    start_utc = _utc()
    log(f"CHUNK_START utc={start_utc} chunk={chunk_no} pending_jobs={len(pending)} "
        f"pending_chains={sum(j['chains'] for j in pending)} cumulative_wall_s={spent:.1f} k={k} "
        f"workers={workers} launch_until_s={launch_until:.1f} kill_at_s={kill_at:.1f}")
    running: list[_Running] = []
    queue = list(pending)
    launched = 0
    stats = Counter()
    mem_wait = 0.0
    next_progress = progress_every_s

    def finish(run: _Running, outcome: str, exit_code: int | None) -> None:
        for f in run.files:
            f.close()
        peak_wset, peak_commit = _os_peak(run.proc)
        wall = time.monotonic() - run.start
        pred = run.out_dir / "predictions.csv"
        if outcome == "ok" and not pred.exists():
            outcome = "missing_output"
        is_final = outcome != "chunk_stop"
        rec = {
            "job": run.job["id"], "attempt": run.attempt, "parent": run.job.get("parent"),
            "underliers": run.job["underliers"], "chains": run.job["chains"], "rows": run.job.get("rows"),
            "outcome": outcome, "final": is_final, "exit_code": exit_code, "wall_s": round(wall, 4),
            "budget_s": run.budget, "launch_utc": run.launch_utc, "end_utc": _utc(), "chunk": chunk_no,
            "start_offset_s": round(run.start - t0, 4), "workers": workers, "k": k,
            "peak_wset_bytes": peak_wset, "peak_commit_bytes": peak_commit,
            "max_sampled_wset_bytes": run.max_wset, "last_sampled_wset_bytes": run.last_wset,
            "mem_cap_bytes": mem_cap_bytes,
            "over_cap_post_hoc": bool(peak_wset is not None and peak_wset > mem_cap_bytes),
            "stderr_first": _first_line(run.out_dir / "stderr.txt"),
            "predictions_sha256": _sha256_file(pred) if outcome == "ok" else None,
            "out_dir": str(run.out_dir),
        }
        if is_final and outcome != "ok" and len(run.job["underliers"]) > 1:
            paths = {u: state_dir / "retry" / f"{run.job['id']}.u{i}.csv"
                     for i, u in enumerate(run.job["underliers"])}
            info = split_csv_by_underlier(Path(run.job["csv"]), paths)
            children = []
            for i, u in enumerate(run.job["underliers"]):
                children.append({"id": f"{run.job['id']}.u{i}", "underliers": [u], "chains": info[u]["chains"],
                                 "rows": info[u]["rows"], "csv": str(paths[u]), "parent": run.job["id"]})
            rec["retry_children"] = [c["id"] for c in children]
            rec["children_spec"] = children
            queue[0:0] = children  # isolate the failure next, before new work
        with open(jobs_log, "a") as f:
            f.write(json.dumps(rec) + "\n")
        records.append(rec)
        stats[outcome] += 1
        log(f"JOB {rec['job']} a{rec['attempt']} {outcome} exit={exit_code} wall_s={wall:.2f} "
            f"chains={rec['chains']} peak_wset_mib={(peak_wset or 0) / 2**20:.1f}")

    while True:
        now = time.monotonic() - t0
        for run in list(running):
            code = run.proc.poll()
            if code is not None:
                running.remove(run)
                finish(run, "ok" if code == 0 else ("exception" if code == 1 else "crash"), code)
                continue
            try:
                w = _wset(run.ps)
                run.last_wset = w
                run.max_wset = max(run.max_wset, w)
            except Exception:  # the child may exit between poll() and the sample
                w = 0
            reason = None
            if w > mem_cap_bytes:
                reason = "memory_cap"
            elif time.monotonic() - run.start > run.budget:
                reason = "timeout"
            elif now >= kill_at:
                reason = "chunk_stop"
            if reason:
                run.proc.kill()
                run.proc.wait()
                running.remove(run)
                finish(run, reason, run.proc.returncode)
        if queue and now < launch_until and len(running) < k:
            if host_floor_bytes and psutil.virtual_memory().available < host_floor_bytes:
                mem_wait += poll_s
            else:
                job = queue.pop(0)
                attempt = attempts[job["id"]] + 1
                attempts[job["id"]] = attempt
                out_dir = state_dir / "out" / job["id"] / f"a{attempt}"
                out_dir.mkdir(parents=True, exist_ok=False)
                args = [*command, "--input", job["csv"], "--output", str(out_dir / "predictions.csv"),
                        "--model", "quote-slice"]
                if policy:
                    args += ["--policy", policy]
                args += ["--report", str(out_dir / "native.json"), *bench_args]
                (out_dir / "command.json").write_text(json.dumps({"argv": args, "env_workers": workers}))
                fo = open(out_dir / "stdout.txt", "wb")
                fe = open(out_dir / "stderr.txt", "wb")
                proc = subprocess.Popen(args, stdout=fo, stderr=fe, env=env)
                try:
                    ps_proc = psutil.Process(proc.pid)
                except psutil.Error:  # exited before the first sample
                    ps_proc = None
                running.append(_Running(job, attempt, proc, ps_proc, time.monotonic(),
                                        per_chain_timeout_s * max(1, job["chains"]), out_dir, [fo, fe],
                                        _utc()))
                launched += 1
                continue  # fill every free slot before sleeping
        if not running and (not queue or now >= launch_until):
            break
        if now >= next_progress:
            done = [r for r in records if r["final"] and r["chunk"] == chunk_no]
            vm = psutil.virtual_memory()
            log(f"PROGRESS utc={_utc()} chunk={chunk_no} elapsed_s={now:.1f} launched={launched} "
                f"final={len(done)} chains_final={sum(r['chains'] for r in done)} in_flight={len(running)} "
                f"queue={len(queue)} avail_gib={vm.available / 2**30:.2f} cpu_pct={psutil.cpu_percent():.0f}")
            next_progress += progress_every_s
        time.sleep(poll_s)
    wall = time.monotonic() - t0
    done = [r for r in records if r["final"] and r["chunk"] == chunk_no]
    final_all = {r["job"] for r in records if r["final"]}
    order, _ = _job_universe(plan_path, records)
    still = [j for j in order if j["id"] not in final_all]
    status = "complete" if not still else "partial"
    chunk_rec = {
        "chunk": chunk_no, "start_utc": start_utc, "end_utc": _utc(), "wall_s": round(wall, 3),
        "launched": launched, "finished_final": len(done), "outcomes": dict(stats),
        "chains_final": sum(r["chains"] for r in done), "rows_final": sum(r.get("rows") or 0 for r in done),
        "mem_wait_s": round(mem_wait, 2), "k": k, "workers": workers, "status": status,
        "pending_after": len(still), "cumulative_wall_s": round(spent + wall, 3),
    }
    with open(chunks_log, "a") as f:
        f.write(json.dumps(chunk_rec) + "\n")
    log(f"CHUNK_END utc={chunk_rec['end_utc']} chunk={chunk_no} wall_s={wall:.1f} status={status} "
        f"launched={launched} outcomes={dict(stats)} pending_after={len(still)} "
        f"cumulative_wall_s={spent + wall:.1f}")
    progress.close()
    return dict(chunk_rec, status=status)


# ── CLI ──────────────────────────────────────────────────────────────────────────────────────────────

QUOTE_COLUMNS = ["row_id", "chain_id", "snapshot_ns", "underlier", "root", "expiry_days", "exercise", "bid",
                 "ask", "years", "rate", "sdiv", "ddiv_pv", "input_status"]


def _load_chains(dataset: Path) -> pl.DataFrame:
    quotes = pl.read_parquet(dataset / "quotes.parquet", columns=QUOTE_COLUMNS)
    return assign_strata(chain_table(quotes))


def _write_json(path: Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "x") as f:
        json.dump(obj, f, indent=2, sort_keys=True, default=str)


def _dim_table(chains: pl.DataFrame, dim: str) -> list[dict]:
    t = chains.group_by(dim).agg(
        pl.len().alias("chains"), pl.col("rows").sum().alias("rows"),
        pl.col("two_sided").sum().alias("two_sided"), pl.col("underlier").n_unique().alias("underliers"),
        pl.col("american").sum().alias("american_chains"))
    return t.sort(dim).to_dicts()


def cmd_verify(args: argparse.Namespace) -> int:
    """Deliverable 1: recompute the manifest's counts from the data and list every stratum's chains."""
    dataset = Path(args.dataset)
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f"refusing to overwrite {out}")
    out.mkdir(parents=True)
    manifest = json.loads((dataset / "manifest.json").read_text())
    hashes = {name: _sha256_file(dataset / name) for name in ("quotes.parquet", "quotes.csv", "manifest.json")}
    recorded = {name: manifest["files"][name]["sha256"] for name in ("quotes.parquet", "quotes.csv")}
    quoted = 0
    with open(dataset / "quotes.csv", "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            quoted += block.count(b'"')
    full = pl.read_parquet(dataset / "quotes.parquet")
    quotes = full.select(QUOTE_COLUMNS)
    chains = assign_strata(chain_table(quotes))
    two = quotes.filter(pl.col("bid").is_finite() & pl.col("ask").is_finite() & (pl.col("bid") > 0)
                        & (pl.col("ask") > 0) & (pl.col("ask") >= pl.col("bid")))
    snap_days = (quotes["snapshot_ns"] // NS_PER_DAY).unique().to_list()
    # Carry on eligible rows only: invalid_carry rows hold the -99 sentinel.
    unsupported_regime = chains.join(
        quotes.filter(pl.col("input_status") == "ok").group_by("chain_id")
        .agg(pl.col("rate").median(), pl.col("sdiv").median()), on="chain_id")
    # detail::classify_regime (american.hpp): Unsupported when rate < 0 and rate > yield (puts) or the
    # mirrored call condition; the census applies it to the chain's median rate / continuous yield.
    double_cont = unsupported_regime.filter(
        ((pl.col("rate") < 0) & (pl.col("rate") > pl.col("sdiv")))
        | ((pl.col("sdiv") < 0) & (pl.col("sdiv") > pl.col("rate"))))
    counts = {
        "rows": quotes.height, "underliers": quotes["underlier"].n_unique(), "chains": chains.height,
        "surfaces_underlier_root": quotes.select(["underlier", "root"]).unique().height,
        "two_sided_contracts": two.height,
        "two_sided_american": two.filter(pl.col("exercise") == "American").height,
        "rows_by_input_status": dict(quotes.group_by("input_status").len().sort("input_status").iter_rows()),
        "rows_by_exercise": dict(quotes.group_by("exercise").len().sort("exercise").iter_rows()),
        "chains_by_exercise": {"American": int(chains["american"].sum()),
                               "European": int((~chains["american"]).sum())},
        "snapshot_epoch_days": snap_days, "columns": full.columns,
        "label_columns_present": [c for c in full.columns if c in ("srPrc", "srVol", "reference_price",
                                                                     "reference_vol")],
        "labels_parquet_exists": (dataset / "labels.parquet").exists(),
        "csv_double_quote_bytes": quoted,
    }
    tau_min = 3.0 / 365.0
    domains = {
        "european_exercise (unsupported_exercise)": {
            "chains": int((~chains["american"]).sum()),
            "rows": int(quotes.filter(pl.col("exercise") != "American").height)},
        "dated_cash (ddiv_pv > 0; fitted as funding-PV, records refuse DatedCash)": {
            "chains": int(chains["ddiv"].sum()), "rows": int(chains.filter(pl.col("ddiv"))["rows"].sum())},
        "tau_below_3_over_365 (no sigma family, kAlSigmaFamilyMinTau)": {
            "chains": int(chains.filter(pl.col("years") < tau_min).height),
            "rows": int(chains.filter(pl.col("years") < tau_min)["rows"].sum())},
        "zero_dte": {"chains": int(chains.filter(pl.col("dte") == 0).height),
                     "rows": int(chains.filter(pl.col("dte") == 0)["rows"].sum())},
        "double_continuation (rate < 0 regime)": {"chains": double_cont.height,
                                                  "rows": int(double_cont["rows"].sum())},
        "no_two_sided_quote (C0)": {"chains": int(chains.filter(pl.col("two_sided") == 0).height),
                                    "rows": int(chains.filter(pl.col("two_sided") == 0)["rows"].sum())},
        "events": "unavailable: the label-free dataset carries no event calendar",
    }
    strata_dir = out / "strata"
    strata_dir.mkdir()
    lists = {}
    for dim in ("U", "C", "E", "D", "T"):
        for value, ids in chains.group_by(dim).agg(pl.col("chain_id").sort()).sort(dim).iter_rows():
            path = strata_dir / f"{dim}-{value}.txt"
            path.write_text("".join(f"{i}\n" for i in ids))
            lists[f"{dim}-{value}"] = {"chains": len(ids), "sha256": _sha256_file(path)}
    composite = chains.select(["chain_id", "underlier", "root", "dte", "rows", "two_sided", "eligible",
                               "american", "ddiv", "years", "und_two_sided", "und_expiries", "U", "C", "E",
                               "D", "T", "stratum"]).sort("chain_id")
    composite.write_csv(out / "chains-strata.csv")
    strata = composite.group_by("stratum").agg(pl.len().alias("chains"), pl.col("rows").sum().alias("rows"),
                                               pl.col("underlier").n_unique().alias("underliers"))
    strata.sort("stratum").write_csv(out / "strata-composite.csv")
    report = {
        "dataset": str(dataset), "hashes": hashes, "recorded_in_manifest": recorded,
        "hashes_match_manifest": all(hashes[k] == recorded[k] for k in recorded), "counts": counts,
        "dimensions": {dim: _dim_table(chains, dim) for dim in ("U", "C", "E", "D", "T")},
        "strata_nonempty": strata.height, "strata_possible": 4 * 5 * 4 * 2 * 5,
        "unsupported_domains": domains, "stratum_lists": lists,
        "chains_strata_csv_sha256": _sha256_file(out / "chains-strata.csv"),
        "strata_composite_csv_sha256": _sha256_file(out / "strata-composite.csv"),
    }
    _write_json(out / "manifest-verification.json", report)
    print(json.dumps({"hashes_match_manifest": report["hashes_match_manifest"], "counts": {
        k: counts[k] for k in ("rows", "underliers", "chains", "two_sided_contracts", "two_sided_american")},
        "strata_nonempty": strata.height}))
    return 0


def cmd_plan(args: argparse.Namespace) -> int:
    dataset = Path(args.dataset)
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f"refusing to overwrite {out}")
    chains = _load_chains(dataset)
    sizes = dict(chains.group_by("underlier").len().iter_rows())
    if args.mode == "sample":
        draw = draw_sample(chains, seed=args.seed, target_frac=args.target_frac, max_frac=args.max_frac)
        underliers, report = draw.underliers, draw.report
    else:
        underliers = hash_order(list(sizes), args.seed)
        report = {"seed": args.seed, "rule": "every underlier, sha256(seed|underlier) order",
                  "chains": chains.height, "underliers": len(underliers)}
    jobs = pack_jobs(underliers, sizes, max_chains=args.max_chains_per_job)
    rows = dict(chains.group_by("underlier").agg(pl.col("rows").sum()).iter_rows())
    info = write_job_csvs(dataset / "quotes.csv", jobs, out / "jobs")
    for j in jobs:
        j["csv"] = f"jobs/{j['id']}.csv"
        j["csv_sha256"] = info[j["id"]]["sha256"]
        j["rows"] = info[j["id"]]["rows"]
        if j["rows"] != sum(rows[u] for u in j["underliers"]):
            raise SystemExit(f"row count mismatch in {j['id']}")
    picked = chains.filter(pl.col("underlier").is_in(underliers))
    picked.select(["chain_id", "underlier", "root", "dte", "rows", "two_sided", "eligible", "american", "ddiv",
                   "U", "C", "E", "D", "T", "stratum"]).sort("chain_id").write_csv(out / "chains.csv")
    plan = {"mode": args.mode, "seed": args.seed, "dataset": str(dataset),
            "dataset_quotes_csv_sha256": _sha256_file(dataset / "quotes.csv"),
            "max_chains_per_job": args.max_chains_per_job, "draw": report, "jobs": jobs,
            "totals": {"jobs": len(jobs), "underliers": len(underliers), "chains": picked.height,
                       "rows": int(picked["rows"].sum())}}
    _write_json(out / "plan.json", plan)
    print(json.dumps(plan["totals"] | {"draw": {k: v for k, v in report.items() if k != "rule"}}))
    return 0


# ── summaries ────────────────────────────────────────────────────────────────────────────────────────

COST_COMPONENTS = ("family_us", "root_us", "fit_us", "calendar_us", "final_grid_us", "other_us", "wall_us")
OVER_BOUND_US = 20_000_000  # the declared per-chain timeout (20 s), applied to a completed chain's own wall


def _final_records(state: Path) -> list[dict]:
    return [r for r in _read_jsonl(state / "jobs.jsonl") if r["final"]]


def _quantiles(values: list[float]) -> dict[str, float | None]:
    if not values:
        return {"n": 0, "p50": None, "p90": None, "p99": None, "max": None, "mean": None, "sum": 0.0}
    s = sorted(values)

    def q(p: float) -> float:
        return s[min(len(s) - 1, max(0, math.ceil(p * len(s)) - 1))]

    return {"n": len(s), "p50": q(0.50), "p90": q(0.90), "p99": q(0.99), "max": s[-1],
            "mean": sum(s) / len(s), "sum": float(sum(s))}


def _normalise_reason(text: str) -> str:
    """Stack-free, node- and strike-free reason text (so one failure class is one line)."""
    import re
    text = re.sub(r"node \d+", "node N", text)
    text = re.sub(r"strike [0-9.]+", "strike K", text)
    text = re.sub(r"\b\d+\.\d+\b", "X", text)
    return text


def chain_costs(state: Path) -> pl.DataFrame:
    """Per fitted chain: the cost model buckets from --timing-detail (graded_stats.slices[].timing)."""
    rows = []
    for rec in _final_records(state):
        if rec["outcome"] != "ok":
            continue
        report = json.loads((Path(rec["out_dir"]) / "native.json").read_text())
        for s in report.get("graded_stats", {}).get("slices", []):
            t = s.get("timing")
            if not t:
                continue
            family = t.get("deam_family_build_us", 0)
            rows.append({
                "chain_id": int(s["chain_id"]), "job": rec["job"], "grade": s.get("grade"),
                "profile": s.get("profile"), "error": s.get("error", ""),
                "family_us": family, "root_us": max(0, t["deam_us"] - family),
                "fit_us": t["solve_us"] + t["project_us"], "calendar_us": t["calendar_us"],
                "final_grid_us": t["price_us"] + t["certify_us"], "other_us": t["other_us"],
                "wall_us": t["wall_us"], "family_gt_deam": family > t["deam_us"],
                "repair_rung_us": t.get("repair_rung_us", 0), "al_price_calls": t.get("al_price_calls", 0),
                "reference_requests": t.get("deam_reference_requests", 0),
                "reference_evaluations": t.get("deam_reference_evaluations", 0),
                "reference_budget_refused": t.get("deam_reference_budget_refused", 0),
                "premium_fallbacks": t.get("deam_premium_fallbacks", 0),
                "family_builds": t.get("deam_family_builds", 0), "family_refused": t.get("deam_family_refused", 0),
            })
    if not rows:
        return pl.DataFrame(schema={"chain_id": pl.Int64})
    return pl.DataFrame(rows).with_columns(pl.col("chain_id").cast(pl.UInt64))


def predictions(state: Path) -> pl.DataFrame:
    frames = []
    for rec in _final_records(state):
        if rec["outcome"] != "ok":
            continue
        frames.append(pl.read_csv(Path(rec["out_dir"]) / "predictions.csv", infer_schema_length=0)
                      .with_columns(pl.lit(rec["job"]).alias("job")))
    if not frames:
        return pl.DataFrame()
    return pl.concat(frames, how="diagonal").with_columns(
        pl.col("row_id").cast(pl.UInt64), pl.col("chain_id").cast(pl.UInt64))


def summarize(plan_path: Path, state: Path) -> dict:
    plan = json.loads(Path(plan_path).read_text())
    chains = pl.read_csv(Path(plan_path).parent / "chains.csv", schema_overrides={"chain_id": pl.UInt64})
    recs = _final_records(state)
    chunks = _read_jsonl(state / "chunks.jsonl")
    wall = sum(c["wall_s"] for c in chunks)
    ok_jobs = [r for r in recs if r["outcome"] == "ok"]
    covered_unds = {u for r in ok_jobs for u in r["underliers"]}
    by_outcome = Counter(r["outcome"] for r in recs)
    leaf = [r for r in recs if not r.get("retry_children")]
    failed_leaf = [r for r in leaf if r["outcome"] != "ok"]
    covered = chains.filter(pl.col("underlier").is_in(covered_unds))
    planned_unds = {u for j in plan["jobs"] for u in j["underliers"]}
    pred = predictions(state)
    costs = chain_costs(state)
    out: dict[str, Any] = {"plan": str(plan_path), "state": str(state)}
    out["coverage"] = {
        "planned_underliers": len(planned_unds), "planned_chains": int(chains.height),
        "planned_rows": int(chains["rows"].sum()), "covered_underliers": len(covered_unds),
        "covered_chains": int(covered.height), "covered_rows": int(covered["rows"].sum()),
        "coverage_chains": covered.height / chains.height if chains.height else 0.0,
        "chunks": len(chunks), "chunk_wall_s": [c["wall_s"] for c in chunks], "wall_s_total": wall,
        "k": chunks[-1]["k"] if chunks else None, "workers": chunks[-1]["workers"] if chunks else None,
        "jobs_final_by_outcome": dict(by_outcome),
    }
    out["throughput"] = {
        "chains_per_s": covered.height / wall if wall else None,
        "rows_per_s": int(covered["rows"].sum()) / wall if wall else None,
        "sum_job_wall_s": sum(r["wall_s"] for r in ok_jobs),
        "fit_contexts": (out["coverage"]["k"] or 0) * (out["coverage"]["workers"] or 0),
    }
    cold = []
    for r in ok_jobs:
        rep = json.loads((Path(r["out_dir"]) / "native.json").read_text())
        cold.append(rep["bench"]["stages"]["cold_calibration_s"])
    out["throughput"]["sum_cold_calibration_s"] = sum(cold)
    # failures, exceptions, timeouts (leaf records: the isolated attempts)
    reasons = Counter(_normalise_reason(r["stderr_first"]) for r in failed_leaf if r["stderr_first"])
    out["failures"] = {
        "leaf_outcomes": dict(Counter(r["outcome"] for r in leaf)),
        "timeout_chains": sum(r["chains"] for r in failed_leaf if r["outcome"] == "timeout"),
        "memory_cap_chains": sum(r["chains"] for r in failed_leaf if r["outcome"] == "memory_cap"),
        "exception_chains": sum(r["chains"] for r in failed_leaf if r["outcome"] in ("exception", "crash",
                                                                                  "missing_output")),
        "exit_codes": dict(Counter(str(r["exit_code"]) for r in failed_leaf)),
        "first_20_reasons": reasons.most_common(20),
        "failed_underliers": sorted({u for r in failed_leaf for u in r["underliers"]}),
        "over_cap_post_hoc_jobs": sum(1 for r in recs if r.get("over_cap_post_hoc")),
    }
    fit_fail = Counter()
    for r in ok_jobs:
        rep = json.loads((Path(r["out_dir"]) / "native.json").read_text())
        for k, v in (rep.get("fit_failure_counts") or {}).items():
            fit_fail[_normalise_reason(k)] += v
    out["fit_failure_reasons_top20"] = fit_fail.most_common(20)
    # memory
    peaks = [r["peak_wset_bytes"] / 2**20 for r in recs if r.get("peak_wset_bytes")]
    commits = [r["peak_commit_bytes"] / 2**20 for r in recs if r.get("peak_commit_bytes")]
    last = [r["last_sampled_wset_bytes"] / 2**20 for r in recs if r.get("last_sampled_wset_bytes")]
    per_chain = [r["peak_wset_bytes"] / 2**20 / max(1, r["chains"]) for r in ok_jobs if r.get("peak_wset_bytes")]
    out["memory_mib"] = {"job_peak_wset": _quantiles(peaks), "job_peak_commit": _quantiles(commits),
                         "job_last_sampled_wset": _quantiles(last), "peak_wset_per_chain": _quantiles(per_chain),
                         "cap_mib": (recs[0]["mem_cap_bytes"] / 2**20) if recs else None}
    # cost model
    if costs.height:
        joined = costs.join(chains, on="chain_id", how="left")
        out["cost_model_us"] = {c: _quantiles([float(x) for x in costs[c].to_list()]) for c in COST_COMPONENTS}
        total_wall = float(costs["wall_us"].sum())
        out["cost_shares"] = {c: float(costs[c].sum()) / total_wall for c in COST_COMPONENTS if c != "wall_us"}
        out["cost_counts"] = {c: _quantiles([float(x) for x in costs[c].to_list()])
                              for c in ("al_price_calls", "reference_requests", "reference_evaluations",
                                        "premium_fallbacks", "family_builds", "repair_rung_us")}
        out["cost_flags"] = {"family_gt_deam_chains": int(costs["family_gt_deam"].sum()),
                             "over_bound_chains_wall_gt_20s": int((costs["wall_us"] > OVER_BOUND_US).sum()),
                             "reference_budget_refused": int(costs["reference_budget_refused"].sum()),
                             "fitted_chains_with_timing": costs.height}
        out["cost_by_stratum"] = {}
        for dim in ("U", "C", "E", "D", "T"):
            rows = []
            for key, grp in joined.group_by(dim):
                w = [float(x) for x in grp["wall_us"].to_list()]
                q = _quantiles(w)
                rows.append({dim: key[0], "chains": q["n"], "wall_p50_ms": (q["p50"] or 0) / 1e3,
                             "wall_p99_ms": (q["p99"] or 0) / 1e3, "wall_max_ms": (q["max"] or 0) / 1e3,
                             "wall_sum_s": q["sum"] / 1e6,
                             "root_share": float(grp["root_us"].sum()) / max(1.0, q["sum"]),
                             "final_grid_share": float(grp["final_grid_us"].sum()) / max(1.0, q["sum"])})
            out["cost_by_stratum"][dim] = sorted(rows, key=lambda r: r[dim])
        top = costs.sort("wall_us", descending=True).head(10).join(chains, on="chain_id", how="left")
        out["slowest_chains"] = top.select(["chain_id", "underlier", "wall_us", "root_us", "final_grid_us",
                                            "family_us", "grade", "stratum"]).to_dicts()
    # taxonomy (rows) and support by stratum
    if pred.height:
        served = pl.col("status").is_in(["ok", "ok_price_only"])
        out["taxonomy_rows"] = {c: dict(pred.group_by(c).len().sort("len", descending=True).iter_rows())
                                for c in ("status", "quote_status", "vol_status") if c in pred.columns}
        out["served_rows"] = int(pred.filter(served).height)
        out["identified_iv_rows"] = int(pred.filter(pl.col("vol_status") == "ok").height)
        per_chain = pred.group_by("chain_id").agg(served.sum().alias("served"), pl.len().alias("rows_out"),
                                                   pl.col("status").mode().first().alias("modal_status"))
        cj = covered.join(per_chain, on="chain_id", how="left").with_columns(pl.col("served").fill_null(0))
        out["support_by_stratum"] = {}
        for dim in ("U", "C", "E", "D", "T"):
            t = cj.group_by(dim).agg(pl.len().alias("chains"), (pl.col("served") > 0).sum().alias("chains_served"),
                                     pl.col("rows").sum().alias("rows"), pl.col("served").sum().alias("rows_served"))
            out["support_by_stratum"][dim] = t.with_columns(
                (pl.col("rows_served") / pl.col("rows")).alias("served_row_share")).sort(dim).to_dicts()
        grades = Counter()
        for r in ok_jobs:
            rep = json.loads((Path(r["out_dir"]) / "native.json").read_text())
            for g, n in (rep.get("graded_stats", {}).get("chains") or {}).items():
                grades[g] += n
        out["chain_grades"] = dict(grades)
        out["unsupported_domain_support"] = {
            "zero_dte": _served_share(cj.filter(pl.col("dte") == 0)),
            "dte_1_to_3": _served_share(cj.filter((pl.col("dte") >= 1) & (pl.col("dte") <= 3))),
            "dated_cash_D1": _served_share(cj.filter(pl.col("ddiv"))),
            "european": _served_share(cj.filter(~pl.col("american"))),
            "no_two_sided_C0": _served_share(cj.filter(pl.col("two_sided") == 0)),
        }
    # determinism key: per-job served-row hashes
    hashes = sorted((r["job"], r["predictions_sha256"]) for r in ok_jobs)
    out["served_row_hashes"] = {"jobs": len(hashes), "combined_sha256": hashlib.sha256(
        "".join(f"{j} {h}\n" for j, h in hashes).encode()).hexdigest()}
    return out


def _served_share(frame: pl.DataFrame) -> dict:
    rows = int(frame["rows"].sum()) if frame.height else 0
    served = int(frame["served"].sum()) if frame.height else 0
    return {"chains": frame.height, "chains_served": int((frame["served"] > 0).sum()) if frame.height else 0,
            "rows": rows, "rows_served": served, "served_row_share": served / rows if rows else None}


def compare_states(plan_path: Path, a: Path, b: Path) -> dict:
    """Determinism and served-set comparison of two runs of one plan (per job hash; per row status)."""
    ra = {r["job"]: r for r in _final_records(a) if r["outcome"] == "ok"}
    rb = {r["job"]: r for r in _final_records(b) if r["outcome"] == "ok"}
    common = sorted(set(ra) & set(rb))
    diff = [j for j in common if ra[j]["predictions_sha256"] != rb[j]["predictions_sha256"]]
    pa, pb = predictions(a), predictions(b)
    served = ["ok", "ok_price_only"]
    j = pa.select(["row_id", "status", "predicted_price"]).join(
        pb.select(["row_id", "status", "predicted_price"]), on="row_id", how="full", suffix="_b")
    sa = pl.col("status").is_in(served)
    sb = pl.col("status_b").is_in(served)
    return {
        "jobs_a": len(ra), "jobs_b": len(rb), "jobs_common": len(common), "jobs_hash_equal": len(common) - len(diff),
        "jobs_hash_differ": diff[:50],
        "rows_a": pa.height, "rows_b": pb.height,
        "served_a": int(pa.filter(pl.col("status").is_in(served)).height),
        "served_b": int(pb.filter(pl.col("status").is_in(served)).height),
        "lost_a_not_b": int(j.filter(sa.fill_null(False) & ~sb.fill_null(False)).height),
        "gained_b_not_a": int(j.filter(~sa.fill_null(False) & sb.fill_null(False)).height),
        "status_changed_rows": int(j.filter(pl.col("status") != pl.col("status_b")).height),
        "status_transitions": j.filter(pl.col("status") != pl.col("status_b")).group_by(["status", "status_b"])
        .len().sort("len", descending=True).head(15).to_dicts(),
    }


def cmd_summarize(args: argparse.Namespace) -> int:
    out = Path(args.out)
    s = summarize(Path(args.plan), Path(args.state))
    _write_json(out, s)
    print(json.dumps({"coverage": s["coverage"], "throughput": s["throughput"]}, default=str))
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    c = compare_states(Path(args.plan), Path(args.a), Path(args.b))
    _write_json(Path(args.out), c)
    print(json.dumps({k: v for k, v in c.items() if k != "status_transitions"}, default=str))
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    summary = run_chunk(
        Path(args.plan), Path(args.state), command=[args.binary], bench_args=args.bench_args,
        policy=args.policy, workers=args.workers, k=args.k, chunk_wall_s=args.chunk_wall,
        kill_at_s=args.kill_at, total_cap_s=args.total_cap, per_chain_timeout_s=args.per_chain_timeout,
        mem_cap_bytes=int(args.mem_cap_mib * 2**20), host_floor_bytes=int(args.host_floor_gib * 2**30),
        progress_every_s=args.progress_every)
    print(json.dumps(summary))
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--dataset", required=True)
    v.add_argument("--out", required=True)
    v.set_defaults(fn=cmd_verify)
    pl_ = sub.add_parser("plan")
    pl_.add_argument("--dataset", required=True)
    pl_.add_argument("--out", required=True)
    pl_.add_argument("--mode", choices=["sample", "full"], required=True)
    pl_.add_argument("--seed", required=True)
    pl_.add_argument("--target-frac", type=float, default=0.04)
    pl_.add_argument("--max-frac", type=float, default=0.05)
    pl_.add_argument("--max-chains-per-job", type=int, default=24)
    pl_.set_defaults(fn=cmd_plan)
    r = sub.add_parser("run")
    r.add_argument("--plan", required=True)
    r.add_argument("--state", required=True)
    r.add_argument("--binary", required=True)
    r.add_argument("--policy")
    r.add_argument("--workers", type=int)
    r.add_argument("--k", type=int, default=4)
    r.add_argument("--chunk-wall", type=float, default=540.0)
    r.add_argument("--kill-at", type=float, default=585.0)
    r.add_argument("--total-cap", type=float, default=3600.0)
    r.add_argument("--per-chain-timeout", type=float, default=20.0)
    r.add_argument("--mem-cap-mib", type=float, default=768.0)
    r.add_argument("--host-floor-gib", type=float, default=1.5)
    r.add_argument("--progress-every", type=float, default=60.0)
    r.add_argument("bench_args", nargs=argparse.REMAINDER)
    r.set_defaults(fn=cmd_run)
    s = sub.add_parser("summarize")
    s.add_argument("--plan", required=True)
    s.add_argument("--state", required=True)
    s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_summarize)
    c = sub.add_parser("compare")
    c.add_argument("--plan", required=True)
    c.add_argument("--a", required=True)
    c.add_argument("--b", required=True)
    c.add_argument("--out", required=True)
    c.set_defaults(fn=cmd_compare)
    args = p.parse_args(argv)
    if getattr(args, "bench_args", None) and args.bench_args[:1] == ["--"]:
        args.bench_args = args.bench_args[1:]
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
