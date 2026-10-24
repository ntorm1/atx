"""Universe census runner (tools/universe_census.py): strata, sample draw, job packing, checkpoint / resume.

The runner never needs the real quote-bench here: a tiny fake bench (a Python script written into tmp_path)
mimics its CLI and misbehaves on purpose for underliers named HANG*, BOOM*, FAT* and SLOW*.
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import polars as pl
import pytest


TOOLS = Path(__file__).resolve().parents[2] / "tools"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


census = _load("universe_census")

SNAP_NS = 1791988200000000000  # 2026-10-14 14:30 UTC
SNAP_DAY = 20740

HEADER = (
    "row_id,chain_id,snapshot_ns,observation_ns,underlier,root,expiry_days,strike,is_call,exercise,spot_bid,"
    "spot_ask,spot,bid,ask,bid_size,ask_size,years,years_calendar,rate,sdiv,ddiv,ddiv_pv,split,input_status"
)


def quote_rows(underlier: str, chain_id: int, dte: int, two_sided: int, one_sided: int = 0,
               ddiv_pv: float = 0.0, exercise: str = "American", start_row: int = 0) -> list[dict]:
    rows = []
    for i in range(two_sided + one_sided):
        bid = 1.0 if i < two_sided else 0.0
        rows.append({
            "row_id": start_row + i, "chain_id": chain_id, "snapshot_ns": SNAP_NS, "observation_ns": SNAP_NS,
            "underlier": underlier, "root": underlier, "expiry_days": SNAP_DAY + dte, "strike": 10.0 + i,
            "is_call": i % 2, "exercise": exercise, "spot_bid": 9.9, "spot_ask": 10.1, "spot": 10.0,
            "bid": bid, "ask": 1.2, "bid_size": 1, "ask_size": 1, "years": max(dte, 1) / 365.0,
            "years_calendar": max(dte, 1) / 365.0, "rate": 0.04, "sdiv": 0.0, "ddiv": ddiv_pv, "ddiv_pv": ddiv_pv,
            "split": "universe", "input_status": "ok",
        })
    return rows


def frame(rows: list[dict]) -> pl.DataFrame:
    return pl.DataFrame(rows)


# ── strata ───────────────────────────────────────────────────────────────────────────────────────────


def test_chain_table_counts_two_sided_dte_dividends_and_underlier_axes():
    rows = (quote_rows("AAA", 1, 0, 3, 2) + quote_rows("AAA", 2, 30, 4, ddiv_pv=0.5, start_row=100)
            + quote_rows("BBB", 3, 400, 1, 1, exercise="European", start_row=200))
    chains = census.chain_table(frame(rows)).sort("chain_id")
    assert chains["chain_id"].to_list() == [1, 2, 3]
    assert chains["rows"].to_list() == [5, 4, 2]
    assert chains["two_sided"].to_list() == [3, 4, 1]
    assert chains["dte"].to_list() == [0, 30, 400]
    assert chains["ddiv"].to_list() == [False, True, False]
    assert chains["american"].to_list() == [True, True, False]
    assert chains["und_two_sided"].to_list() == [7, 7, 1]
    assert chains["und_expiries"].to_list() == [2, 2, 1]
    assert chains["und_chains"].to_list() == [2, 2, 1]


@pytest.mark.parametrize("und_two_sided,label", [(500, "U1"), (499, "U2"), (100, "U2"), (99, "U3"),
                                                 (25, "U3"), (24, "U4"), (0, "U4")])
def test_underlier_class_edges(und_two_sided, label):
    assert census.underlier_class(und_two_sided) == label


@pytest.mark.parametrize("two_sided,label", [(0, "C0"), (1, "C1"), (10, "C1"), (11, "C2"), (40, "C2"),
                                             (41, "C3"), (100, "C3"), (101, "C4")])
def test_chain_size_edges(two_sided, label):
    assert census.chain_size_class(two_sided) == label


@pytest.mark.parametrize("expiries,label", [(1, "E1"), (3, "E1"), (4, "E2"), (5, "E2"), (6, "E3"),
                                            (11, "E3"), (12, "E4")])
def test_expiry_count_edges(expiries, label):
    assert census.expiry_count_class(expiries) == label


@pytest.mark.parametrize("dte,label", [(0, "T0"), (1, "T1"), (3, "T1"), (4, "T2"), (30, "T2"), (31, "T3"),
                                       (365, "T3"), (366, "T4")])
def test_dte_edges(dte, label):
    assert census.dte_class(dte) == label


def test_assign_strata_builds_the_composite_key():
    rows = quote_rows("AAA", 1, 2, 12, ddiv_pv=0.1)
    chains = census.assign_strata(census.chain_table(frame(rows)))
    assert chains["stratum"].to_list() == ["U4|C2|E1|D1|T1"]


# ── sample draw ──────────────────────────────────────────────────────────────────────────────────────


def synthetic_universe(n_underliers: int = 400) -> pl.DataFrame:
    """Many common underliers plus a few rare strata, so a 5 % sample can cover every stratum."""
    rows: list[dict] = []
    chain = 0
    row = 0
    for u in range(n_underliers):
        name = f"U{u:04d}"
        for e in range(5):
            chain += 1
            rows += quote_rows(name, chain, 10 + 40 * e, 5, start_row=row)
            row += 5
    for k, (dte, size, ddiv) in enumerate([(0, 120, 0.0), (500, 50, 1.0), (2, 20, 0.0)]):
        chain += 1
        rows += quote_rows(f"RARE{k}", chain, dte, size, ddiv_pv=ddiv, start_row=row)
        row += size
    return frame(rows)


def test_draw_covers_every_stratum_within_budget_and_is_seeded():
    chains = census.assign_strata(census.chain_table(synthetic_universe()))
    draw = census.draw_sample(chains, seed="s1", target_frac=0.04, max_frac=0.05)
    picked = chains.filter(pl.col("underlier").is_in(draw.underliers))
    assert set(picked["stratum"].unique()) == set(chains["stratum"].unique())
    assert picked.height <= 0.05 * chains.height
    assert {"RARE0", "RARE1", "RARE2"} <= set(draw.underliers)
    # whole underliers only: every chain of a picked underlier is in
    full = chains.group_by("underlier").len()
    got = picked.group_by("underlier").len()
    assert got.join(full, on="underlier").filter(pl.col("len") != pl.col("len_right")).height == 0
    again = census.draw_sample(chains, seed="s1", target_frac=0.04, max_frac=0.05)
    assert again.underliers == draw.underliers
    other = census.draw_sample(chains, seed="s2", target_frac=0.04, max_frac=0.05)
    assert other.underliers != draw.underliers
    assert draw.report["chains"] == picked.height
    assert draw.report["strata_covered"] == draw.report["strata_nonempty"]


def test_draw_refuses_when_the_cover_cannot_fit_the_budget():
    rows = []
    for k in range(6):  # six singleton underliers, each its own stratum: no 5 % sample covers them
        rows += quote_rows(f"S{k}", k + 1, [0, 2, 10, 40, 400, 900][k], 1 + 30 * k, start_row=1000 * k)
    chains = census.assign_strata(census.chain_table(frame(rows)))
    with pytest.raises(ValueError, match="cover"):
        census.draw_sample(chains, seed="s1", target_frac=0.04, max_frac=0.05)


# ── job packing and job CSVs ─────────────────────────────────────────────────────────────────────────


def test_pack_jobs_keeps_underliers_whole_and_caps_chains():
    sizes = {"A": 3, "B": 30, "C": 10, "D": 12, "E": 1}
    jobs = census.pack_jobs(["A", "B", "C", "D", "E"], sizes, max_chains=24)
    assert [j["underliers"] for j in jobs] == [["A"], ["B"], ["C", "D", "E"]]
    assert [j["chains"] for j in jobs] == [3, 30, 23]
    assert [j["id"] for j in jobs] == ["j00000", "j00001", "j00002"]


def write_universe_csv(path: Path, rows: list[dict]) -> None:
    lines = [HEADER]
    for r in rows:
        lines.append(",".join(str(r[c]) for c in HEADER.split(",")))
    path.write_bytes(("\n".join(lines) + "\n").encode())


def test_write_job_csvs_is_a_byte_exact_subset_with_header(tmp_path):
    rows = quote_rows("AAA", 1, 5, 2) + quote_rows("BBB", 2, 5, 2, start_row=10) + quote_rows("AAA", 3, 9, 1, start_row=20)
    src = tmp_path / "quotes.csv"
    write_universe_csv(src, rows)
    jobs = [{"id": "j00000", "underliers": ["AAA"]}, {"id": "j00001", "underliers": ["BBB"]}]
    out = census.write_job_csvs(src, jobs, tmp_path / "jobs", batch=1)
    src_lines = src.read_bytes().splitlines()
    a = (tmp_path / "jobs" / "j00000.csv").read_bytes().splitlines()
    b = (tmp_path / "jobs" / "j00001.csv").read_bytes().splitlines()
    assert a[0] == src_lines[0] and b[0] == src_lines[0]
    assert a[1:] == [src_lines[i] for i in (1, 2, 5)]
    assert b[1:] == [src_lines[i] for i in (3, 4)]
    assert out["j00000"]["rows"] == 3 and len(out["j00000"]["sha256"]) == 64


def test_write_job_csvs_reads_the_quoted_polars_layout(tmp_path):
    """The universe quotes.csv quotes every string field ("AAPL"); the slicer must match the bare ticker."""
    src = tmp_path / "quotes.csv"
    src.write_bytes(
        b'"row_id","chain_id","underlier_pad","x","underlier"\n'
        b'1,11,0,0,"AAA"\n'
        b'2,12,0,0,"BBB"\n'
        b'3,13,0,0,"AAA"\n')
    out = census.write_job_csvs(src, [{"id": "j00000", "underliers": ["AAA"]}], tmp_path / "jobs")
    lines = (tmp_path / "jobs" / "j00000.csv").read_bytes().splitlines()
    assert lines == [b'"row_id","chain_id","underlier_pad","x","underlier"', b'1,11,0,0,"AAA"',
                     b'3,13,0,0,"AAA"']
    assert out["j00000"]["rows"] == 2


def test_a_field_with_an_embedded_comma_is_refused(tmp_path):
    src = tmp_path / "quotes.csv"
    src.write_bytes(b'"a","b","c","d","underlier"\n1,2,3,4,"A,B"\n')
    with pytest.raises(ValueError, match="quoted"):
        census.write_job_csvs(src, [{"id": "j00000", "underliers": ["A,B"]}], tmp_path / "jobs")


# ── runner: checkpoint / resume, bounds ──────────────────────────────────────────────────────────────

FAKE_BENCH = r'''
import csv, json, os, sys, time
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
with open(args["--input"], newline="") as f:
    rows = list(csv.DictReader(f))
unds = sorted({r["underlier"] for r in rows})
if any(u.startswith("HANG") for u in unds):
    time.sleep(60)
if any(u.startswith("BOOM") for u in unds):
    sys.stderr.write("InvalidArgument: fake failure for " + ",".join(unds) + "\n")
    sys.exit(1)
if any(u.startswith("FAT") for u in unds):
    blob = bytearray(300 * 1024 * 1024)
    for i in range(0, len(blob), 4096):
        blob[i] = 1
    time.sleep(5)
if any(u.startswith("SLOW") for u in unds):
    time.sleep(1.5)
with open(args["--output"], "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["row_id", "status", "chain_id"])
    for r in rows:
        w.writerow([r["row_id"], "ok", r["chain_id"]])
chains = sorted({r["chain_id"] for r in rows})
rep = {"rows": len(rows), "graded_stats": {"slices": [{"chain_id": int(c), "timing": {"wall_us": 1000}} for c in chains]}}
with open(args["--report"], "w") as f:
    json.dump(rep, f)
'''


def make_plan(tmp_path: Path, groups: list[list[str]]) -> Path:
    rows = []
    row = 0
    chain = 0
    for group in groups:
        for u in group:
            chain += 1
            rows += quote_rows(u, chain, 10, 2, start_row=row)
            row += 2
    src = tmp_path / "quotes.csv"
    write_universe_csv(src, rows)
    jobs = []
    for i, group in enumerate(groups):
        jobs.append({"id": f"j{i:05d}", "underliers": group, "chains": len(group), "rows": 2 * len(group)})
    plan_dir = tmp_path / "plan"
    info = census.write_job_csvs(src, jobs, plan_dir / "jobs")
    for j in jobs:
        j["csv"] = f"jobs/{j['id']}.csv"
        j["csv_sha256"] = info[j["id"]]["sha256"]
    plan = {"seed": "test", "jobs": jobs}
    (plan_dir / "plan.json").write_text(json.dumps(plan))
    return plan_dir / "plan.json"


def runner_config(tmp_path: Path, **over) -> dict:
    fake = tmp_path / "fake_bench.py"
    fake.write_text(FAKE_BENCH)
    cfg = dict(command=[sys.executable, str(fake)], bench_args=[], policy=None, workers=1, k=2,
               chunk_wall_s=60.0, kill_at_s=90.0, total_cap_s=600.0, per_chain_timeout_s=20.0,
               mem_cap_bytes=2 * 1024 ** 3, host_floor_bytes=0, progress_every_s=0.5, poll_s=0.05)
    cfg.update(over)
    return cfg


def records(state: Path) -> list[dict]:
    return [json.loads(line) for line in (state / "jobs.jsonl").read_text().splitlines() if line.strip()]


def test_run_completes_every_job_once_and_records_hashes(tmp_path):
    plan = make_plan(tmp_path, [["A1"], ["B1", "B2"], ["C1"]])
    state = tmp_path / "state"
    summary = census.run_chunk(plan, state, **runner_config(tmp_path))
    recs = records(state)
    assert summary["status"] == "complete"
    assert sorted(r["job"] for r in recs) == ["j00000", "j00001", "j00002"]
    assert all(r["outcome"] == "ok" and r["final"] for r in recs)
    assert all(len(r["predictions_sha256"]) == 64 for r in recs)
    again = census.run_chunk(plan, state, **runner_config(tmp_path))
    assert again["launched"] == 0 and again["status"] == "complete"
    assert len(records(state)) == 3
    log = (state / "progress.log").read_text()
    assert "CHUNK_START" in log and "CHUNK_END" in log


def test_a_chunk_stop_is_resumed_in_a_new_attempt_directory(tmp_path):
    plan = make_plan(tmp_path, [["SLOW1"], ["SLOW2"], ["SLOW3"], ["A1"]])
    state = tmp_path / "state"
    cfg = runner_config(tmp_path, k=1, chunk_wall_s=0.2, kill_at_s=0.8)
    first = census.run_chunk(plan, state, **cfg)
    assert first["status"] == "partial"
    recs = records(state)
    stopped = [r for r in recs if r["outcome"] == "chunk_stop"]
    assert stopped and not any(r["final"] for r in stopped)
    second = census.run_chunk(plan, state, **runner_config(tmp_path, k=4))
    assert second["status"] == "complete"
    final = {r["job"]: r for r in records(state) if r["final"]}
    assert sorted(final) == ["j00000", "j00001", "j00002", "j00003"]
    assert all(r["outcome"] == "ok" for r in final.values())
    stopped_job = stopped[0]["job"]
    assert final[stopped_job]["attempt"] == 2
    assert Path(final[stopped_job]["out_dir"]).name == "a2"
    chunks = [json.loads(x) for x in (state / "chunks.jsonl").read_text().splitlines()]
    assert len(chunks) == 2 and all(c["wall_s"] > 0 for c in chunks)


def test_timeout_kills_the_job_and_retries_its_underliers_alone(tmp_path):
    plan = make_plan(tmp_path, [["HANG1", "OK1"]])
    state = tmp_path / "state"
    summary = census.run_chunk(plan, state, **runner_config(tmp_path, per_chain_timeout_s=1.0))
    assert summary["status"] == "complete"
    by_job = {r["job"]: r for r in records(state)}
    parent = by_job["j00000"]
    assert parent["outcome"] == "timeout" and parent["final"] and parent["budget_s"] == pytest.approx(2.0)
    assert parent["retry_children"] == ["j00000.u0", "j00000.u1"]
    assert by_job["j00000.u0"]["underliers"] == ["HANG1"] and by_job["j00000.u0"]["outcome"] == "timeout"
    assert by_job["j00000.u1"]["underliers"] == ["OK1"] and by_job["j00000.u1"]["outcome"] == "ok"
    assert "j00000.u0.u0" not in by_job


def test_an_exception_is_isolated_and_its_first_reason_kept(tmp_path):
    plan = make_plan(tmp_path, [["BOOM1", "OK1"]])
    state = tmp_path / "state"
    census.run_chunk(plan, state, **runner_config(tmp_path))
    by_job = {r["job"]: r for r in records(state)}
    assert by_job["j00000"]["outcome"] == "exception" and by_job["j00000"]["exit_code"] == 1
    assert by_job["j00000.u0"]["outcome"] == "exception"
    assert by_job["j00000.u0"]["stderr_first"].startswith("InvalidArgument: fake failure for BOOM1")
    assert by_job["j00000.u1"]["outcome"] == "ok"


def test_the_memory_cap_kills_a_process_above_it(tmp_path):
    plan = make_plan(tmp_path, [["FAT1"]])
    state = tmp_path / "state"
    census.run_chunk(plan, state, **runner_config(tmp_path, mem_cap_bytes=150 * 1024 ** 2))
    rec = records(state)[0]
    assert rec["outcome"] == "memory_cap" and rec["final"]
    assert rec["max_sampled_wset_bytes"] > 150 * 1024 ** 2


def test_the_total_cap_refuses_a_chunk_once_spent(tmp_path):
    plan = make_plan(tmp_path, [["SLOW1"], ["SLOW2"]])
    state = tmp_path / "state"
    state.mkdir()
    (state / "chunks.jsonl").write_text(json.dumps({"chunk": 1, "wall_s": 601.0}) + "\n")
    summary = census.run_chunk(plan, state, **runner_config(tmp_path, total_cap_s=600.0))
    assert summary["status"] == "cap_reached" and summary["launched"] == 0
    assert not (state / "jobs.jsonl").exists() or records(state) == []
