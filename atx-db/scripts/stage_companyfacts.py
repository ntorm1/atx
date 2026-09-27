"""CF-R extract (tier-1 v2 node 1.4): stage the pinned SEC CompanyFacts archive as Parquet batches.

No warehouse and no network: the input is the retained ``data/cache/companyfacts.zip``; the output is
``data/staging/companyfacts/<archive_sha16>/`` (see ``atx_db.companyfacts_stage``), which node 1.5
loads. Every step is a job under the memory guard (ruling C-58); ``run`` is the orchestrator:

    run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 60 -- \\
        .venv\\Scripts\\python.exe scripts\\stage_companyfacts.py run [--batches 0-2] [--workers 2]

``run`` plans once (a 0.6 GiB job: the concept allowlist comes from
``fundamental_statements.default_companyfacts_concepts``, which imports pandas), extracts every
pending batch in 0.5 GiB workers (``--batches-per-job`` batches per fresh interpreter, each batch
committed on its own), re-runs guard stops (137) and admission timeouts (78), then assembles
``manifest.json`` + ``members.parquet`` once no batch is pending. A killed run resumes: complete batches (file + manifest row, same plan and archive sha256)
are skipped, a partial ``.tmp`` is discarded. Each finished job is appended to ``extract-log.jsonl``
(exit, native peak, seconds).

Other subcommands (each normally a guarded job): ``plan``, ``worker``, ``assemble``;
``verify-sample`` (0.6 GiB: ``normalize_companyfacts`` equivalence on sampled members and the ops-a
prefix dispositions); ``determinism`` (orchestrator: re-extract batches into a scratch copy of the
plan and compare Parquet sha256).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import sys
import time
from pathlib import Path
from typing import Any

ATX_DB = Path(__file__).resolve().parents[1]
DEFAULT_ZIP = ATX_DB / "data" / "cache" / "companyfacts.zip"
DEFAULT_OUT_ROOT = ATX_DB / "data" / "staging" / "companyfacts"
LOG_FILE = "extract-log.jsonl"
# The ops-a receipted prefix (tier1-parity ops-a-report.md): members through CIK 0001496383.
OPS_A_PREFIX_LAST_CIK = 1_496_383
OPS_A_PREFIX = {"members": 10_832, "loaded": 9_462, "empty": 1_327, "unavailable": 43,
                "empty_reasons": {"unsupported_or_empty_taxonomy": 1_272, "allowlist_empty": 53,
                                  "no_valid_fact_rows": 2}}


def _utc() -> str:
    return dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def _parse_batches(text: str | None, count: int) -> list[int]:
    if not text:
        return list(range(count))
    chosen: set[int] = set()
    for part in text.split(","):
        if "-" in part:
            low, high = part.split("-", 1)
            chosen.update(range(int(low), int(high) + 1))
        elif part.strip():
            chosen.add(int(part))
    bad = sorted(i for i in chosen if not 0 <= i < count)
    if bad:
        raise SystemExit(f"batch ids out of range 0..{count - 1}: {bad[:10]}")
    return sorted(chosen)


# ---------------------------------------------------------------------------
# Orchestration (stdlib + run_jobs only; every step is its own guarded job)
# ---------------------------------------------------------------------------

def _jobs(args: argparse.Namespace, out_dir: Path, stage: str, argvs: list[list[str]], labels: list[Any], *,
          job_gb: float, workers: int) -> dict[Any, dict[str, Any]]:
    from atx_db.research.workers import EXIT_HEADROOM, EXIT_NOT_ADMITTED, run_jobs

    script = str(Path(__file__).resolve())
    full = [[sys.executable, script, *argv] for argv in argvs]
    log_path = out_dir / LOG_FILE
    results: dict[Any, dict[str, Any]] = {}
    pending = list(range(len(full)))
    for attempt in range(1, args.attempts + 1):
        if not pending:
            break
        current = list(pending)

        def finished(index: int, code: int, receipt: dict[str, Any], current: list[int] = current,
                     attempt: int = attempt) -> None:
            label = labels[current[index]]
            entry = {"utc": _utc(), "stage": stage, "label": label, "attempt": attempt, "exit": code,
                     "status": receipt.get("status"), "peak_gb": receipt.get("peak_job_memory_gb"),
                     "cap_hit": receipt.get("cap_hit"), "seconds": receipt.get("seconds"),
                     "wait_seconds": receipt.get("wait_seconds"), "error": receipt.get("error")}
            results[label] = entry
            with log_path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(entry, sort_keys=True) + "\n")
            print(f"[{stage}] {label} exit={code} peak={entry['peak_gb']} s={entry['seconds']}", flush=True)

        started = time.perf_counter()
        run_jobs([full[i] for i in current], max_workers=workers, job_gb=job_gb, wait_minutes=args.wait_minutes,
                 log_dir=out_dir / "logs" / stage / f"attempt-{attempt}", on_finish=finished)
        pending = [i for i in current if results[labels[i]]["exit"] in (EXIT_HEADROOM, EXIT_NOT_ADMITTED)]
        print(f"[{stage}] attempt {attempt}: {len(current)} jobs in {time.perf_counter() - started:.0f} s, "
              f"{len(pending)} to re-run (137/78)", flush=True)
        if pending:
            time.sleep(60)
    failed = {label: r for label, r in results.items() if r["exit"] != 0}
    if failed:
        raise SystemExit(f"{stage}: {len(failed)} jobs failed: {list(failed.values())[:3]} "
                         f"(logs {out_dir / 'logs' / stage})")
    return results


def cmd_run(args: argparse.Namespace) -> int:
    from atx_db import companyfacts_stage as cf

    zip_path = args.zip.resolve()
    started = time.perf_counter()
    digest = cf.archive_sha256(zip_path)
    out_dir = cf.stage_directory(args.out_root, digest)
    out_dir.mkdir(parents=True, exist_ok=True)
    if not (out_dir / cf.PLAN_FILE).is_file():
        _jobs(args, out_dir, "plan", [["plan", "--zip", str(zip_path), "--out-root", str(args.out_root),
                                       "--target-bytes", str(args.target_bytes)]], ["plan"],
              job_gb=args.plan_job_gb, workers=1)
    _plan, batches = cf.load_companyfacts_plan(out_dir)
    selected = set(_parse_batches(args.batches, len(batches)))
    pending = [b for b in cf.pending_companyfacts_batches(out_dir) if b.batch_id in selected]
    print(json.dumps({"out_dir": str(out_dir), "batches": len(batches), "selected": len(selected),
                      "complete_skipped": len(selected) - len(pending), "pending": len(pending)}), flush=True)
    if pending:
        # A few batches per job amortize guard admission (minutes on a busy host) over ~10 s batches;
        # each batch still commits on its own, so a stopped job loses at most its current batch.
        ids = [b.batch_id for b in pending]
        chunks = [ids[i:i + args.batches_per_job] for i in range(0, len(ids), args.batches_per_job)]
        _jobs(args, out_dir, "extract",
              [["worker", "--out", str(out_dir), "--zip", str(zip_path), "--batch", ",".join(map(str, chunk))]
               for chunk in chunks],
              [",".join(map(str, chunk)) for chunk in chunks], job_gb=args.job_gb, workers=args.workers)
    remaining = cf.pending_companyfacts_batches(out_dir)
    if not remaining and not args.no_assemble:
        _jobs(args, out_dir, "assemble", [["assemble", "--out", str(out_dir), "--zip", str(zip_path)]],
              ["assemble"], job_gb=args.job_gb, workers=1)
    print(json.dumps({"out_dir": str(out_dir), "remaining": [b.batch_id for b in remaining],
                      "assembled": (out_dir / cf.MANIFEST_FILE).is_file() and not remaining,
                      "wall_seconds": round(time.perf_counter() - started, 1)}), flush=True)
    return 0


def cmd_determinism(args: argparse.Namespace) -> int:
    """Re-extract batches into a scratch directory holding a copy of the plan; compare file sha256."""
    from atx_db import companyfacts_stage as cf

    plan, batches = cf.load_companyfacts_plan(args.out)
    chosen = _parse_batches(args.batches, len(batches))
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.out / cf.PLAN_FILE, scratch / cf.PLAN_FILE)
    zip_path = Path(plan["archive"]["path"])
    todo = [i for i in chosen if cf.batch_receipt(scratch, batches[i]) is None]
    if todo:
        _jobs(args, scratch, "determinism",
              [["worker", "--out", str(scratch), "--zip", str(zip_path), "--batch", str(i)] for i in todo],
              todo, job_gb=args.job_gb, workers=args.workers)
    compared = []
    for i in chosen:
        main_row = cf.batch_receipt(args.out, batches[i])
        again = cf.batch_receipt(scratch, batches[i])
        if main_row is None or again is None:
            raise SystemExit(f"batch {i}: missing a complete manifest row (main={main_row is not None})")
        compared.append({"batch_id": i, "rows": again["rows"], "sha256_main": main_row["parquet_sha256"],
                         "sha256_again": again["parquet_sha256"],
                         "identical": main_row["parquet_sha256"] == again["parquet_sha256"]
                         and main_row["members"] == again["members"]})
    result = {"utc": _utc(), "batches": compared, "all_identical": all(c["identical"] for c in compared)}
    (scratch / "determinism.json").write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=1), flush=True)
    return 0 if result["all_identical"] else 1


# ---------------------------------------------------------------------------
# Guarded jobs
# ---------------------------------------------------------------------------

def cmd_plan(args: argparse.Namespace) -> int:
    from atx_db import companyfacts_stage as cf

    if args.concepts_file is not None:
        concepts = [line.strip() for line in args.concepts_file.read_text(encoding="utf-8").splitlines()
                    if line.strip()]
        source = f"file:{args.concepts_file.resolve()}"
    else:
        from atx_db.fundamental_statements import default_companyfacts_concepts

        concepts = list(default_companyfacts_concepts())
        source = "atx_db.fundamental_statements.default_companyfacts_concepts"
    started = time.perf_counter()
    batches = cf.plan_companyfacts_batches(args.zip, args.target_bytes)
    planner_seconds = time.perf_counter() - started
    started = time.perf_counter()
    digest = cf.archive_sha256(args.zip)
    hash_seconds = time.perf_counter() - started
    path = cf.write_companyfacts_plan(args.zip, args.out_root, concepts=concepts, concepts_source=source,
                                      target_uncompressed_bytes=args.target_bytes, archive_digest=digest)
    sizes = [len(b.members) for b in batches]
    print(json.dumps({"plan": str(path), "archive_sha256": digest, "batches": len(batches),
                      "members": sum(sizes), "members_per_batch_mean": round(sum(sizes) / len(sizes), 1),
                      "planner_seconds": round(planner_seconds, 3), "hash_seconds": round(hash_seconds, 2),
                      "allowlist_count": len(set(concepts)), "allowlist_sha256": cf.allowlist_fingerprint(concepts)}),
          flush=True)
    return 0


def cmd_worker(args: argparse.Namespace) -> int:
    """Extract one or more batches in sequence; each commits its own file + manifest row."""
    from atx_db import companyfacts_stage as cf

    plan, batches = cf.load_companyfacts_plan(args.out)
    zip_path = args.zip or Path(plan["archive"]["path"])
    for batch_id in _parse_batches(args.batch, len(batches)):
        started = time.perf_counter()
        result = cf.extract_companyfacts_batch(zip_path, batches[batch_id], args.out)
        print(json.dumps({**result, "seconds": round(time.perf_counter() - started, 2)}), flush=True)
    return 0


def cmd_assemble(args: argparse.Namespace) -> int:
    from atx_db import companyfacts_stage as cf

    plan, _ = cf.load_companyfacts_plan(args.out)
    manifest = cf.assemble_companyfacts_stage(args.zip or Path(plan["archive"]["path"]), args.out)
    print(json.dumps(manifest["totals"], sort_keys=True), flush=True)
    return 0


_FACT_DDL = """(
    source VARCHAR NOT NULL, security_id VARCHAR NOT NULL, cik VARCHAR NOT NULL, taxonomy VARCHAR NOT NULL,
    concept VARCHAR NOT NULL, label VARCHAR, description VARCHAR, unit VARCHAR NOT NULL, period_start DATE,
    period_end DATE, filed_date DATE NOT NULL, fiscal_year INTEGER, fiscal_period VARCHAR, form VARCHAR,
    accession_number VARCHAR, frame VARCHAR, value DOUBLE, available_at TIMESTAMP, run_id VARCHAR,
    source_url VARCHAR NOT NULL)"""  # schema.py sec_company_facts minus entity_id / source_loaded_at
_POINT_DDL = """(
    source VARCHAR NOT NULL, security_id VARCHAR NOT NULL, symbol VARCHAR, metric VARCHAR NOT NULL,
    taxonomy VARCHAR, unit VARCHAR, period_start DATE, period_end DATE, as_of_date DATE NOT NULL,
    fiscal_year INTEGER, fiscal_period VARCHAR, form VARCHAR, accession_number VARCHAR, value DOUBLE,
    available_at TIMESTAMP, run_id VARCHAR)"""  # schema.py fundamental_points minus source_loaded_at
_FACT_COLS = ("source, security_id, cik, taxonomy, concept, label, description, unit, period_start, period_end, "
              "filed_date, fiscal_year, fiscal_period, form, accession_number, frame, value, available_at, source_url")
_POINT_COLS = ("source, security_id, symbol, metric, taxonomy, unit, period_start, period_end, as_of_date, "
               "fiscal_year, fiscal_period, form, accession_number, value, available_at")
_POINT_FROM_PARQUET = ("source, security_id, CAST(NULL AS VARCHAR) AS symbol, concept AS metric, taxonomy, unit, "
                       "period_start, period_end, filed_date AS as_of_date, fiscal_year, fiscal_period, form, "
                       "accession_number, value, available_at")


def _expected_value_exact(raw: bytes, concepts: set[str]) -> Any:
    """The ``value_exact`` multiset the rule implies, derived without the stage's parser hooks.

    Re-parses the member keeping every number's literal text, applies the loader's row filters
    (us-gaap/dei, allowlist, end and filed present, end <= filed) and the rule's definition
    directly: an integer literal whose float64 differs from it, or a fraction/exponent literal
    that is not the shortest round-trip rendering of its float64. Keyed per fact row.
    """
    import math
    from collections import Counter
    from decimal import Decimal

    def lossy(kind: str, text: str) -> bool:
        if kind == "int":
            return float(int(text)) != int(text)
        return not (math.isfinite(float(text)) and Decimal(repr(float(text))) == Decimal(text))

    payload = json.loads(raw, parse_int=lambda s: ("int", s), parse_float=lambda s: ("frac", s))
    expected: Counter[tuple[Any, ...]] = Counter()
    for taxonomy, taxonomy_facts in payload["facts"].items():
        if taxonomy not in ("us-gaap", "dei"):
            continue
        for concept, concept_payload in taxonomy_facts.items():
            if concept not in concepts:
                continue
            for unit, items in concept_payload["units"].items():
                for item in items:
                    end, filed = item.get("end"), item.get("filed")
                    if not end or not filed or dt.date.fromisoformat(end) > dt.date.fromisoformat(filed):
                        continue
                    val = item.get("val")
                    literal = val[1] if isinstance(val, tuple) and lossy(*val) else None
                    expected[(taxonomy, concept, unit, item.get("accn"), end, filed, literal)] += 1
    return expected


def cmd_verify_sample(args: argparse.Namespace) -> int:
    """Legacy ``normalize_companyfacts`` (+ the loader's DuckDB insert) vs the Parquet rows, per member.

    Also checks ``value_exact`` (not a legacy column) per checked member against
    :func:`_expected_value_exact`, and archive-wide that every ``value_exact`` rounds to ``value``.
    """
    import hashlib
    import tempfile
    import zipfile
    from collections import Counter

    import duckdb
    import pyarrow.parquet as pq

    from atx_db import companyfacts_stage as cf
    from atx_db import fundamentals as legacy
    from atx_db.connection import bounded_config
    from atx_db.warehouse import json_dumps

    started = time.perf_counter()
    plan, batches = cf.load_companyfacts_plan(args.out)
    concepts = set(plan["allowlist"]["concepts"])
    constants = {
        "SOURCE_NAME": cf.SOURCE_NAME == legacy.SOURCE_NAME,
        "SEC_COMPANY_FACTS_ZIP_URL": cf.SEC_COMPANY_FACTS_ZIP_URL == legacy.SEC_COMPANY_FACTS_ZIP_URL,
        "SUPPORTED_FACT_TAXONOMIES": cf.SUPPORTED_FACT_TAXONOMIES == legacy.SUPPORTED_FACT_TAXONOMIES,
        "COMPANYFACTS_MEMBER_PATTERN":
            cf.COMPANYFACTS_MEMBER_PATTERN.pattern == legacy.COMPANYFACTS_MEMBER_PATTERN.pattern,
        "UNRESOLVED_COMPANYFACTS_CIK_PREFIX":
            cf.UNRESOLVED_COMPANYFACTS_CIK_PREFIX == legacy.UNRESOLVED_COMPANYFACTS_CIK_PREFIX,
        "allowlist_fingerprint_formula": cf.allowlist_fingerprint(concepts) == hashlib.sha256(json_dumps({
            "concepts": sorted(concepts), "taxonomies": legacy.SUPPORTED_FACT_TAXONOMIES}).encode()).hexdigest(),
        "plan_allowlist_equals_DEFAULT_CONCEPTS": concepts == set(legacy.DEFAULT_CONCEPTS),
    }
    members = pq.read_table(args.out / cf.MEMBERS_FILE).to_pylist()
    batch_of = {name: b.batch_id for b in batches for name in b.members}
    loaded = sorted((m for m in members if m["disposition"] == "loaded"),
                    key=lambda m: (m["uncompressed_bytes"], m["member"]))
    n = args.sample
    sample = [loaded[round(i * (len(loaded) - 1) / (n - 1))] for i in range(n)] if n > 1 else loaded[-1:]
    include = {f"{int(c):010d}" for c in args.include.split(",") if c.strip()} if args.include else set()
    sampled = {m["member"] for m in sample}
    forced = [m for m in members if m["cik"] in include and m["member"] not in sampled]
    if len(forced) + sum(1 for m in sample if m["cik"] in include) != len(include):
        raise SystemExit(f"--include names CIKs without an archive member: {sorted(include)}")
    others: list[dict[str, Any]] = []
    for kind, reason in (("empty", "unsupported_or_empty_taxonomy"), ("empty", "allowlist_empty"),
                         ("empty", "no_valid_fact_rows"), ("unavailable", cf.PLACEHOLDER_REASON)):
        others += [m for m in members if m["disposition"] == kind and m["reason"] == reason][:2]
    others += [m for m in members if m["disposition"] == "error"][:5]

    tmp = Path(tempfile.mkdtemp(prefix="cf_verify_"))
    con = duckdb.connect(":memory:", config=bounded_config("256MB", 1, temp_directory=tmp / "spill"))
    con.execute(f"CREATE TABLE legacy_facts {_FACT_DDL}")
    con.execute(f"CREATE TABLE legacy_points {_POINT_DDL}")
    checks: list[dict[str, Any]] = []
    selection = ["sample"] * len(sample) + ["include"] * len(forced) + ["other"] * len(others)
    with zipfile.ZipFile(plan["archive"]["path"]) as archive:
        for m, selected_by in zip(sample + forced + others, selection, strict=True):
            cik, member = m["cik"], m["member"]
            raw = archive.read(member)
            parquet = str(args.out / f"batch-{batch_of[member]:04d}.parquet")
            check: dict[str, Any] = {"member": member, "selected_by": selected_by, "disposition": m["disposition"],
                                     "reason": m["reason"], "uncompressed_bytes": m["uncompressed_bytes"],
                                     "rows": m["rows"]}
            if raw == b"{}":
                check["legacy"] = "placeholder"
                check["ok"] = m["disposition"] == "unavailable"
                checks.append(check)
                continue
            try:
                payload = json.loads(raw)
                if not isinstance(payload, dict) or not isinstance(payload.get("facts"), dict):
                    raise ValueError("companyfacts payload requires a facts object")
                legacy._validate_archive_payload_cik(payload, cik)
                facts, points = legacy.normalize_companyfacts(
                    payload, symbol=f"CIK{cik}", security_id=f"{legacy.UNRESOLVED_COMPANYFACTS_CIK_PREFIX}{cik}",
                    cik=cik, source_url=legacy._companyfacts_zip_member_url(cik), concepts=concepts, run_id=None)
            except Exception as exc:  # the loader's skippable failure
                check["legacy"] = f"error {type(exc).__name__}"
                check["ok"] = m["disposition"] == "error" and m["reason"].startswith(type(exc).__name__)
                checks.append(check)
                continue
            del payload
            if facts.empty:
                check["legacy"] = "empty"
                check["ok"] = m["disposition"] == "empty" and m["rows"] == 0
                checks.append(check)
                continue
            points["security_id"] = facts["security_id"]  # archive mode, before identity resolution
            points["symbol"] = None
            con.execute("DELETE FROM legacy_facts")
            con.execute("DELETE FROM legacy_points")
            for table, frame in (("legacy_facts", facts), ("legacy_points", points)):
                con.register("legacy_frame", frame)  # warehouse.insert_frame's projection
                con.execute(f"INSERT INTO {table} ({', '.join(frame.columns)}) "
                            f"SELECT {', '.join(frame.columns)} FROM legacy_frame")
                con.unregister("legacy_frame")
            del facts, points
            con.execute("CREATE OR REPLACE TEMP TABLE staged AS SELECT * FROM read_parquet(?) WHERE cik = ?",
                        [parquet, cik])
            fact_row = con.execute(f"""SELECT
                (SELECT count(*) FROM legacy_facts), (SELECT count(*) FROM staged),
                (SELECT count(*) FROM (SELECT {_FACT_COLS} FROM legacy_facts EXCEPT ALL SELECT {_FACT_COLS} FROM staged)),
                (SELECT count(*) FROM (SELECT {_FACT_COLS} FROM staged EXCEPT ALL SELECT {_FACT_COLS} FROM legacy_facts))
            """).fetchone()
            point_row = con.execute(f"""SELECT
                (SELECT count(*) FROM legacy_points),
                (SELECT count(*) FROM (SELECT {_POINT_COLS} FROM legacy_points
                                       EXCEPT ALL SELECT {_POINT_FROM_PARQUET} FROM staged)),
                (SELECT count(*) FROM (SELECT {_POINT_FROM_PARQUET} FROM staged
                                       EXCEPT ALL SELECT {_POINT_COLS} FROM legacy_points))
            """).fetchone()
            assert fact_row is not None and point_row is not None
            check.update(legacy="loaded", legacy_rows=fact_row[0], parquet_rows=fact_row[1],
                         legacy_minus_parquet=fact_row[2], parquet_minus_legacy=fact_row[3],
                         legacy_points=point_row[0], points_legacy_minus_parquet=point_row[1],
                         points_parquet_minus_legacy=point_row[2])
            expected = _expected_value_exact(raw, concepts)
            del raw
            staged_exact: Counter[tuple[Any, ...]] = Counter(con.execute(
                "SELECT taxonomy, concept, unit, accession_number, CAST(period_end AS VARCHAR), "
                "CAST(filed_date AS VARCHAR), value_exact FROM staged").fetchall())
            exact_mismatch = sum((expected - staged_exact).values()) + sum((staged_exact - expected).values())
            check.update(value_exact_rows=sum(n for key, n in staged_exact.items() if key[-1] is not None),
                         value_exact_expected=sum(n for key, n in expected.items() if key[-1] is not None),
                         value_exact_mismatch=exact_mismatch)
            del expected, staged_exact
            check["ok"] = (m["disposition"] == "loaded" and fact_row[0] == fact_row[1] == m["rows"] == point_row[0]
                           and fact_row[2] == fact_row[3] == point_row[1] == point_row[2] == 0
                           and exact_mismatch == 0)
            checks.append(check)
    types = dict(con.execute("SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM read_parquet(?))",
                             [str(args.out / "batch-0000.parquet")]).fetchall())
    batch_glob = str(args.out / "batch-*.parquet")
    exact_row = con.execute("""SELECT count(*), count(DISTINCT cik),
            count(*) FILTER (WHERE regexp_full_match(value_exact, '-?[0-9]+')),
            count(*) FILTER (WHERE TRY_CAST(value_exact AS DOUBLE) IS DISTINCT FROM value)
        FROM read_parquet(?) WHERE value_exact IS NOT NULL""", [batch_glob]).fetchone()
    assert exact_row is not None
    exact_examples = con.execute("""SELECT cik, taxonomy, concept, unit, accession_number,
            CAST(period_end AS VARCHAR) AS period_end, value, value_exact,
            CAST(TRY_CAST(value AS HUGEINT) - TRY_CAST(value_exact AS HUGEINT) AS VARCHAR) AS int_error
        FROM read_parquet(?) WHERE value_exact IS NOT NULL
        ORDER BY cik, taxonomy, concept, unit, accession_number, period_end, value_exact LIMIT 40""",
                                 [batch_glob]).fetchall()
    value_exact = {"rows": exact_row[0], "ciks": exact_row[1], "integer_literals": exact_row[2],
                   "fraction_literals": exact_row[0] - exact_row[2], "not_rounding_to_value": exact_row[3],
                   "examples": [dict(zip(("cik", "taxonomy", "concept", "unit", "accession_number", "period_end",
                                          "value", "value_exact", "int_error"), row, strict=True))
                                for row in exact_examples]}
    con.close()
    shutil.rmtree(tmp, ignore_errors=True)
    prefix = [m for m in members if int(m["cik"]) <= OPS_A_PREFIX_LAST_CIK]
    prefix_counts: dict[str, Any] = {"members": len(prefix)}
    for kind in cf.DISPOSITIONS:
        prefix_counts[kind] = sum(1 for m in prefix if m["disposition"] == kind)
    prefix_counts["empty_reasons"] = {}
    for m in prefix:
        if m["disposition"] == "empty":
            prefix_counts["empty_reasons"][m["reason"]] = prefix_counts["empty_reasons"].get(m["reason"], 0) + 1
    prefix_ok = all(prefix_counts.get(k) == v for k, v in OPS_A_PREFIX.items())
    result = {
        "utc": _utc(), "seconds": round(time.perf_counter() - started, 1), "constants": constants,
        "parquet_types": types, "sampled_loaded": len(sample), "included": len(forced),
        "other_dispositions": len(others),
        "all_ok": all(c["ok"] for c in checks) and value_exact["not_rounding_to_value"] == 0
        and all(v for k, v in constants.items() if k != "plan_allowlist_equals_DEFAULT_CONCEPTS"),
        "ops_a_prefix": {"expected": OPS_A_PREFIX, "observed": prefix_counts, "ok": prefix_ok},
        "value_exact": value_exact, "checks": checks,
    }
    text = json.dumps(result, indent=1, default=str)
    if args.result is not None:
        args.result.write_text(text + "\n", encoding="utf-8")
    print(text, flush=True)
    return 0 if result["all_ok"] and prefix_ok else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def orchestrator_flags(p: argparse.ArgumentParser) -> None:
        p.add_argument("--workers", type=int, default=2)
        p.add_argument("--job-gb", type=float, default=0.5)
        p.add_argument("--wait-minutes", type=float, default=60.0)
        p.add_argument("--attempts", type=int, default=5)
        p.add_argument("--batches", help="subset, e.g. 0-2 or 0,40,79 (default: all)")

    run = sub.add_parser("run", help="orchestrator: plan -> pending batches -> assemble")
    run.add_argument("--zip", type=Path, default=DEFAULT_ZIP)
    run.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT)
    run.add_argument("--target-bytes", type=int, default=230_000_000)
    run.add_argument("--plan-job-gb", type=float, default=0.6)
    run.add_argument("--no-assemble", action="store_true")
    run.add_argument("--batches-per-job", type=int, default=4)
    orchestrator_flags(run)
    run.set_defaults(func=cmd_run)

    det = sub.add_parser("determinism", help="orchestrator: re-extract batches into a scratch plan copy")
    det.add_argument("--out", type=Path, required=True)
    det.add_argument("--scratch", type=Path, required=True)
    orchestrator_flags(det)
    det.set_defaults(func=cmd_determinism)

    plan = sub.add_parser("plan")
    plan.add_argument("--zip", type=Path, default=DEFAULT_ZIP)
    plan.add_argument("--out-root", type=Path, default=DEFAULT_OUT_ROOT)
    plan.add_argument("--target-bytes", type=int, default=230_000_000)
    plan.add_argument("--concepts-file", type=Path, help="pin another allowlist (one concept per line)")
    plan.set_defaults(func=cmd_plan)

    worker = sub.add_parser("worker")
    worker.add_argument("--out", type=Path, required=True)
    worker.add_argument("--batch", required=True, help="batch id(s): 7, 7,8 or 7-10")
    worker.add_argument("--zip", type=Path)
    worker.set_defaults(func=cmd_worker)

    assemble = sub.add_parser("assemble")
    assemble.add_argument("--out", type=Path, required=True)
    assemble.add_argument("--zip", type=Path)
    assemble.set_defaults(func=cmd_assemble)

    verify = sub.add_parser("verify-sample")
    verify.add_argument("--out", type=Path, required=True)
    verify.add_argument("--sample", type=int, default=20)
    verify.add_argument("--include", help="CIKs to check besides the sample, e.g. 1065088,320193")
    verify.add_argument("--result", type=Path)
    verify.set_defaults(func=cmd_verify_sample)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
