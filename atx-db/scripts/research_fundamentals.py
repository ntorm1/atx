"""Parquet-only shared accounting item engine. Always invoke through the memory guard."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time

from atx_db.research.fundamental_sources import (
    atomic_json, canonical_sha256, file_sha256, normalize_source, prepare_filing_index,
    check_file, validate_source_receipt,
)


def run_retained_jobs(jobs, log_parent: Path):
    """Persist every native guard receipt before run_jobs removes temporary state."""
    from atx_db.research.workers import run_jobs
    log_dir = log_parent / str(time.time_ns())
    atomic_json(log_dir / "jobs.json", {"jobs": jobs, "job_gb": 0.6, "max_workers": 1})
    def finished(index, code, receipt):
        atomic_json(log_dir / f"job-{index:05d}.json", {**receipt, "returncode": code})
    return run_jobs(jobs, max_workers=1, job_gb=0.6, log_dir=log_dir, on_finish=finished)


def main() -> None:
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("command", choices=["prepare", "normalize", "normalize-all", "plan", "build", "audit", "publish"])
    parser.add_argument("--pins", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path("data/research"))
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--source", choices=["cf", "fsds"])
    parser.add_argument("--slice")
    parser.add_argument("--buckets", type=int, default=128)
    parser.add_argument("--bucket", type=int)
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--diagnostic", action="store_true")
    parser.add_argument("--limit-sources", type=int)
    parser.add_argument("--part", type=int, default=0)
    parser.add_argument("--parts", type=int, default=1)
    parser.add_argument("--source-parts", type=int, default=8)
    parser.add_argument("--reuse-filings", type=Path)
    parser.add_argument("--reuse-filings-sha256")
    args = parser.parse_args()
    pins = json.loads(args.pins.read_text(encoding="utf-8"))
    # Reuse the accepted source audit. Manifest drift is checked immediately; every
    # worker independently verifies each file it actually consumes.
    for kind in ("cf", "fsds"):
        if file_sha256(pins[kind+"_manifest"]) != pins[kind+"_sha256"]:
            raise ValueError(f"{kind} source manifest drift")
    if not pins.get("scope_complete") or args.buckets < 128:
        raise ValueError("complete source pins and at least 128 owner buckets required")
    args.work.mkdir(parents=True, exist_ok=True)
    if args.command == "prepare":
        from atx_db.research.items_map import load_mapping
        mapping = load_mapping()
        atomic_json(args.work / "mapping.json", mapping)
        if args.reuse_filings is not None:
            index = json.loads(args.reuse_filings.read_text())
            check_file(index)
            if not args.reuse_filings_sha256 or index["sha256"] != args.reuse_filings_sha256:
                raise ValueError("reusing accepted SUB requires its explicit independent content pin")
            if index["rows"] != pins["fsds_rows"]["sub"] or index["conflicting_sub_rows"]:
                raise ValueError("reused SUB index scope/conflict mismatch")
        else:
            index = prepare_filing_index(pins, args.work / "filings.parquet", root=args.root)
        atomic_json(args.work / "filings.json", index)
        print(json.dumps({"mapping_sha256": mapping["sha256"], "aliases": len(mapping["aliases"]),
                          "rules": len(mapping["rules"]), "unsupported": mapping["unsupported"],
                          "filings": index}, default=str))
    elif args.command == "normalize":
        if args.source is None or args.slice is None:
            parser.error("normalize requires --source and --slice")
        index = json.loads((args.work / "filings.json").read_text())
        slice_id = int(args.slice) if args.source == "cf" else args.slice
        suffix = "" if args.parts == 1 else f"-p{args.part:03d}-of{args.parts:03d}"
        result = normalize_source(pins, args.source, slice_id, filing_index=index,
                                  output=args.work / "sources" / f"{args.source}-{args.slice}{suffix}",
                                  root=args.root, buckets=args.buckets, part=args.part, parts=args.parts)
        print(json.dumps({k: v for k, v in result.items() if k != "files"}, default=str))
    elif args.command == "normalize-all":
        source_slices = [("fsds", f"{2009+(q+1)//4}q{(q+1)%4+1}") for q in range(69)]
        source_slices += [("cf", str(n)) for n in range(85)]
        if args.source is not None or args.slice is not None:
            if args.source is None or args.slice is None:
                parser.error("normalize-all source selection requires both --source and --slice")
            if (args.source, args.slice) not in source_slices:
                parser.error("selected source slice is outside pinned declared scope")
            source_slices = [(args.source, args.slice)]
        if args.limit_sources:
            source_slices = source_slices[:args.limit_sources]
        index = json.loads((args.work / "filings.json").read_text())
        check_file(index)
        jobs = [[sys.executable, str(Path(__file__).resolve()), "normalize", "--pins", str(args.pins.resolve()),
                 "--work", str(args.work.resolve()), "--root", str(args.root.resolve()),
                 "--source", source, "--slice", source_slice, "--buckets", str(args.buckets),
                 "--part", str(part), "--parts", str(args.source_parts if source == "fsds" else 1)]
                for source, source_slice in source_slices
                for part in range(args.source_parts if source == "fsds" else 1)]
        results = run_retained_jobs(jobs, args.work / "normalize-guards")
        if any(results):
            raise SystemExit(f"source workers incomplete: {results}")
    elif args.command == "plan":
        from atx_db.research.items_map import load_mapping
        from atx_db.research.item_vintages import PERIOD_POLICY
        mapping = load_mapping()
        expected = [("fsds", f"{2009+(q+1)//4}q{(q+1)%4+1}") for q in range(69)]
        expected += [("cf", str(n)) for n in range(85)]
        receipts, missing = [], []
        index = json.loads((args.work / "filings.json").read_text())
        check_file(index)
        for source, source_slice in expected:
            whole = args.work / "sources" / f"{source}-{source_slice}" / "complete.json"
            paths = [whole] if source == "cf" else [
                args.work / "sources" / f"{source}-{source_slice}-p{part:03d}-of{args.source_parts:03d}" / "complete.json"
                for part in range(args.source_parts)]
            source_count = 0
            complete_source = True
            for part, path in enumerate(paths):
                if not path.is_file():
                    missing.append(path.parent.name)
                    complete_source = False
                    continue
                record = validate_source_receipt(path, pins, source, source_slice, filing_index=index,
                                                  buckets=args.buckets, part=part, parts=len(paths))
                source_count += record["rows"]
                receipts.append({"path": path.resolve().as_posix(), "sha256": file_sha256(path),
                                 "source": source, "slice": source_slice, "files": record["files"],
                                 "source_identity": record["identity"], "rows": record["rows"]})
            if complete_source:
                expected_rows = next(f["rows"] for f in pins["files"] if f["source"] == source
                                     and str(f["slice"]) == source_slice and f["table"] == ("num" if source == "fsds" else "facts"))
                if source_count != expected_rows:
                    raise ValueError(f"normalization row denominator differs from pinned source: {source}-{source_slice}")
        if missing and not args.diagnostic:
            raise ValueError("normalization incomplete; partial builds require explicit --diagnostic")
        code_paths = [Path(__file__).resolve()]
        code_paths += [Path(__file__).resolve().parents[1] / "src/atx_db/research" / name for name in
                       ("fundamental_sources.py", "items_map.py", "item_vintages.py")]
        plan = {"schema": "fundamental_build_plan_v1", "snapshot_date": "2026-09-20", "mapping": mapping,
                "source_pins_sha256": file_sha256(args.pins), "source_receipts": receipts,
                "scope_complete": not missing, "missing_source_slices": missing,
                "diagnostic": args.diagnostic, "buckets": args.buckets, "period_policy": PERIOD_POLICY,
                "code": {p.as_posix(): file_sha256(p) for p in code_paths},
                "root": args.root.resolve().as_posix(), "work": args.work.resolve().as_posix()}
        plan["build_sha256"] = canonical_sha256(plan)
        plan["build_id"] = "fund-"+plan["build_sha256"][:20]
        target = args.work / plan["build_id"] / "plan.json"
        atomic_json(target, plan)
        print(json.dumps({"plan": target.as_posix(), "build_id": plan["build_id"],
                          "sources": len(receipts), "scope_complete": plan["scope_complete"],
                          "mapping_sha256": mapping["sha256"]}))
    elif args.command in {"build", "audit", "publish"}:
        from atx_db.research.item_vintages import build_bucket, audit_bucket, publish_bucket, publish_index
        if args.plan is None:
            parser.error("build requires explicit --plan")
        plan = json.loads(args.plan.read_text())
        for path, expected_sha in plan["code"].items():
            if file_sha256(path) != expected_sha:
                raise ValueError("build code differs from immutable plan; create a new plan")
        if file_sha256(args.pins) != plan["source_pins_sha256"]:
            raise ValueError("source pin file drift")
        for receipt in plan["source_receipts"]:
            if file_sha256(receipt["path"]) != receipt["sha256"]:
                raise ValueError("source completion receipt drift")
        if args.bucket is None:
            jobs = [[sys.executable, str(Path(__file__).resolve()), args.command, "--pins", str(args.pins.resolve()),
                     "--work", str(args.work.resolve()), "--root", str(args.root.resolve()),
                     "--plan", str(args.plan.resolve()), "--bucket", str(bucket)]
                    for bucket in range(plan["buckets"])]
            codes = run_retained_jobs(jobs, args.plan.parent / (args.command+"-guards"))
            if any(codes):
                raise SystemExit(f"item workers incomplete: {codes}")
            if args.command == "publish":
                print(json.dumps(publish_index(plan, args.plan.parent, root=args.root)))
        else:
            if not 0 <= args.bucket < plan["buckets"]:
                raise ValueError("bucket outside plan")
            files = [check_file(f) for receipt in plan["source_receipts"] for f in receipt["files"] if f["bucket"] == args.bucket]
            output = args.plan.parent / f"b{args.bucket:03d}"
            if args.command == "build":
                result = build_bucket(files, plan["mapping"], root=args.root, output=output,
                                      build_sha256=plan["build_sha256"], bucket=args.bucket)
            elif args.command == "audit":
                result = audit_bucket(files, plan, output=output, root=args.root)
            else:
                result = publish_bucket(files, plan, output=output, root=args.root)
                if args.diagnostic:
                    result["index"] = publish_index(plan, args.plan.parent, root=args.root,
                                                     diagnostic_buckets=[args.bucket])
            print(json.dumps({k: v for k, v in result.items() if k not in {"files", "coverage"}}, default=str))


if __name__ == "__main__":
    main()
