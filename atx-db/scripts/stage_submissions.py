"""Retained Submissions planning, bounded extraction, assembly and legacy proof."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from atx_db import submissions_stage as stage


def _reserve(args):
    """Keep the caller's other-lane reservation as well as our future staging."""
    if args.external_reserve_bytes < 0 or args.staging_reserve_bytes < 0:
        raise ValueError("negative submissions disk reservation")
    if args.staging_reserve_bytes:
        used = 0
        for directory, subdirs, files in os.walk(args.out):
            if any((Path(directory) / name).is_symlink() for name in (*subdirs, *files)):
                raise ValueError("submissions reservation cannot follow staging symlinks")
            used += sum((Path(directory) / name).stat().st_size for name in files)
        if used > args.staging_reserve_bytes:
            raise ValueError("submissions actual staging exceeded its reserved forecast")
        remaining = max(args.staging_reserve_bytes-used, 512*1024**2)
        free = shutil.disk_usage(args.out).free
        required = 35*1024**3 + args.external_reserve_bytes + remaining
        if free < required:
            raise ValueError(f"submissions combined disk reservation no longer fits: {free=} {required=}")
        print(json.dumps({"disk_reservation": {"free_bytes": free, "required_bytes": required,
            "external_bytes": args.external_reserve_bytes, "staging_used_bytes": used,
            "staging_remaining_bytes": remaining}}), flush=True)


def _guard(args, command, name, cap):
    root = args.receipts.resolve()
    root.mkdir(parents=True, exist_ok=True)
    argv = [sys.executable, str(args.guard), "--quiet", "--job-gb", str(cap), "--wait-minutes", "30",
            "--parent-job", os.environ["ATX_GUARD_JOB"],
            "--disk-path", str(args.out.resolve()), "--min-free-disk-gb", str(35+args.external_reserve_bytes/1024**3),
            "--receipt", str(root / f"{name}.json"), "--stdout", str(root / f"{name}.out"),
            "--stderr", str(root / f"{name}.err"), "--", sys.executable, str(Path(__file__).resolve()), *command]
    for attempt in range(4):
        _reserve(args)
        actual = [value.replace(f"{name}.", f"{name}-attempt{attempt}.") for value in argv]
        flags = (subprocess.CREATE_NO_WINDOW | subprocess.CREATE_BREAKAWAY_FROM_JOB) if os.name == "nt" else 0
        process = subprocess.Popen(actual, env={**os.environ, "OPENBLAS_NUM_THREADS": "1"},
                                   stdin=subprocess.DEVNULL, creationflags=flags)
        try:
            code = process.wait()
        except BaseException:
            process.kill()
            process.wait()
            raise
        receipt = json.loads((root / f"{name}-attempt{attempt}.json").read_text(encoding="utf-8"))
        if receipt.get("cap_hit"):
            raise RuntimeError(f"submissions {name} hit its allocation cap; re-engineer before resuming")
        if receipt.get("status") in ("refused_nested_in_guard", "refused_low_disk", "stopped_low_disk",
                                     "refused_parent_gone", "stopped_parent_gone"):
            raise RuntimeError(f"submissions {name} refused: {receipt['status']}")
        if code not in (78, 137):
            if code or receipt.get("status") != "completed" or receipt.get("returncode") != 0:
                raise RuntimeError(f"submissions {name} failed: {code}")
            return
    raise RuntimeError(f"submissions {name} needs a later guarded resume")


def verify_sample(out, catalog_path, result_path):
    """Call the actual old bulk loader on exactly the planned 200 retained CIKs.

    Its isolated comparison table has identical column types/nullability but no
    DEFAULT now(): the legacy DELETE path is forbidden against such defaults.
    Source clock/run provenance is compared separately from wall-clock load time.
    This does not initialize a warehouse or migrate any schema.
    """
    import duckdb
    from atx_db.batch_runner import store_for
    from atx_db.sec_submissions import SecSubmissionsBulkDataset, SecSubmissionsBulkOptions
    from atx_db.companyfacts_stage import _write_json_atomic, _file_sha256

    plan = stage.load_plan(out)
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    ciks = plan["scope"]["ciks"]
    if ciks is None or len(ciks) != 200:
        raise ValueError("legacy proof requires exactly the frozen 200-CIK pilot")
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    spill = out / "parity-spill"
    spill.mkdir(exist_ok=True)
    con = duckdb.connect(config={"memory_limit": "256MB", "threads": 1,
                                "preserve_insertion_order": False, "temp_directory": str(spill)})
    try:
        for table in ("sec_submissions", "raw_source_files", "data_quality_checks"):
            fields = [f'"{name}" {kind}' + (" NOT NULL" if not nullable else "")
                      for name, kind, default, nullable in catalog[table]["columns"]]
            # Metadata severity has a useful literal default; no clock default.
            fields = [field.replace('"severity" VARCHAR NOT NULL', '"severity" VARCHAR DEFAULT \'error\' NOT NULL')
                      for field in fields]
            con.execute(f"CREATE TABLE {table} ({','.join(fields)})")
        con.execute("CREATE TABLE sec_company_tickers(cik VARCHAR,security_id VARCHAR,ticker VARCHAR,source_loaded_at TIMESTAMP)")
        store = store_for(con)
        legacy = SecSubmissionsBulkDataset().load(store, SecSubmissionsBulkOptions(
            zip_path=Path(plan["archive"]["path"]), forms=None, ciks=tuple(ciks),
            include_history_files=True, batch_ciks=1, run_id="submissions-200-cik-legacy-proof"))
        files = [str((out / b["files"]["rows"]["file"]).resolve()) for b in manifest["batches"]]
        con.execute("CREATE TEMP VIEW staged_source AS SELECT * FROM read_parquet(" + repr(files) + ")")
        columns = ",".join(name for name, _ in stage.SOURCE_COLUMNS)
        for left, right in (("sec_submissions", "staged_source"), ("staged_source", "sec_submissions")):
            if con.execute(f"SELECT 1 FROM (SELECT {columns} FROM {left} EXCEPT ALL SELECT {columns} FROM {right}) LIMIT 1").fetchone():
                raise ValueError(f"200-CIK legacy exact parity failed: {left} -> {right}")
        if con.execute("SELECT 1 FROM sec_submissions GROUP BY cik,accession_number HAVING count(*)>1 LIMIT 1").fetchone():
            raise ValueError("legacy proof has duplicate CIK/accession keys")
        if con.execute("SELECT 1 FROM sec_submissions WHERE security_id <> 'SEC-CIK-' || cik LIMIT 1").fetchone():
            raise ValueError("legacy unresolved security identity differs")
        measures = con.execute("""SELECT count(*),count(DISTINCT cik),
            count(*) FILTER(WHERE upper(trim(form))='8-K' AND regexp_matches(coalesce(items,''),'(^|[^0-9])2\\.02([^0-9]|$)')),
            count(*) FILTER(WHERE acceptance_datetime_raw IS NULL),
            count(*) FILTER(WHERE primary_doc_description IS NULL)
            FROM sec_submissions""").fetchone()
        if measures[0] != manifest["rows"] or measures[2] != manifest["item_202_candidates"]:
            raise ValueError("legacy candidate/row denominator differs")
        result = {"exact_legacy_multiset_equality": True, "pilot_ciks": len(ciks),
            "rows": measures[0], "nonempty_ciks": measures[1], "item_202_candidates": measures[2],
            "null_acceptance_raw": measures[3], "null_primary_doc_description": measures[4],
            "compared_columns": [name for name, _ in stage.SOURCE_COLUMNS],
            "legacy_loader_details": legacy.details, "source_identity": "SEC-CIK-{cik}; empty current map",
            "run_id": "submissions-200-cik-legacy-proof", "source_loaded_at": "excluded wall-clock load receipt",
            "archive_sha256": stage.ARCHIVE_SHA, "plan_sha256": _file_sha256(out / "plan.json"),
            "manifest_sha256": _file_sha256(out / "manifest.json"), "catalog_sha256": _file_sha256(catalog_path),
            "legacy_code_sha256": _file_sha256(Path(sys.modules['atx_db.sec_submissions'].__file__))}
        _write_json_atomic(result_path, result)
        return result
    finally:
        con.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("plan", "worker", "assemble", "run", "verify-sample"))
    parser.add_argument("--archive", type=Path, default=Path("C:/atx/atx-db/data/cache/submissions.zip"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--pilot-inventory", type=Path)
    parser.add_argument("--batch-id", type=int)
    parser.add_argument("--stop-after", type=int)
    parser.add_argument("--receipts", type=Path, default=Path("C:/atx/.superpowers/sdd/tier1-v2/receipts"))
    parser.add_argument("--guard", type=Path, default=Path("C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py"))
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--result", type=Path)
    parser.add_argument("--external-reserve-bytes", type=int, default=0)
    parser.add_argument("--staging-reserve-bytes", type=int, default=0)
    args = parser.parse_args(argv)
    if args.command == "plan":
        ciks = json.loads(args.pilot_inventory.read_text(encoding="utf-8"))["pilot_ciks"] if args.pilot_inventory else None
        result = stage.write_plan(args.archive, args.out, ciks=ciks)
        print(json.dumps({"batches": len(result["batches"]), "members": result["directory"]["rows"],
                          "archive_sha256": result["archive"]["sha256"]}))
    elif args.command == "worker":
        if args.batch_id is None:
            parser.error("worker requires --batch-id")
        result = stage.extract_batch(args.out, args.batch_id)
        print(json.dumps(result, sort_keys=True))
    elif args.command == "assemble":
        print(json.dumps(stage.assemble(args.out), sort_keys=True))
    elif args.command == "verify-sample":
        if not args.catalog or not args.result:
            parser.error("verify-sample requires --catalog and --result")
        print(json.dumps(verify_sample(args.out, args.catalog, args.result), default=str, sort_keys=True))
    else:
        if not os.environ.get("ATX_GUARD_JOB"):
            raise ValueError("run requires the 0.2 GiB nested-guard orchestrator")
        if not args.pilot_inventory and (args.external_reserve_bytes <= 0 or args.staging_reserve_bytes <= 0):
            raise ValueError("full extraction requires explicit current external and staging disk reservations")
        args.out.mkdir(parents=True, exist_ok=True)
        base = ["--out", str(args.out.resolve())]
        plan_args = ["plan", *base, "--archive", str(args.archive.resolve())]
        if args.pilot_inventory:
            plan_args += ["--pilot-inventory", str(args.pilot_inventory.resolve())]
        prefix = args.out.name
        _guard(args, plan_args, prefix + "-plan", 0.6)
        plan = json.loads((args.out / "plan.json").read_text(encoding="utf-8"))
        batches = plan["batches"]
        if not args.pilot_inventory:
            # Stress the most numerous tiny/empty CIKs before the full sweep;
            # the accepted pilot already stresses the largest history groups.
            first = list(dict.fromkeys(max(range(len(batches)), key=lambda i: batches[i][key])
                                       for key in ("main_members", "members")))
            batches = [batches[i] for i in first] + [b for i,b in enumerate(batches) if i not in first]
            print(json.dumps({"initial_stress_batches": first}), flush=True)
        for ordinal, batch in enumerate(batches):
            if args.stop_after is not None and ordinal >= args.stop_after:
                print(json.dumps({"status": "bounded_stop", "batches": ordinal}))
                return 0
            _guard(args, ["worker", *base, "--batch-id", str(batch["batch_id"])],
                   f'{prefix}-batch-{batch["batch_id"]:04d}', 0.5)
        _guard(args, ["assemble", *base], prefix + "-assemble", 0.6)
        print(json.dumps({"status": "assembled", "batches": len(plan["batches"])}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
