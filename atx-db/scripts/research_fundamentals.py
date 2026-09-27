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


def source_receipt_path(work, source, source_slice, part, parts):
    suffix = "" if parts == 1 else f"-p{part:03d}-of{parts:03d}"
    return work / "sources" / f"{source}-{source_slice}{suffix}" / "complete.json"


def validate_source_batch(args, pins):
    """Amortize guard admission, never any source identity/content validation."""
    if args.validation_plan is None or file_sha256(args.validation_plan) != args.validation_plan_sha256:
        raise ValueError("source validation batch requires its explicit immutable plan hash")
    batch = json.loads(args.validation_plan.read_text())
    expected = {"schema": "fundamental_source_reuse_batch_v1", "pins_sha256": file_sha256(args.pins),
                "validator_sha256": file_sha256(__file__),
                "normalizer_sha256": file_sha256(Path(__file__).resolve().parents[1] / "src/atx_db/research/fundamental_sources.py"),
                "work": args.work.resolve().as_posix(), "buckets": args.buckets}
    if any(batch.get(name) != value for name, value in expected.items()) or not 1 <= len(batch["entries"]) <= 16:
        raise ValueError("source validation batch contract drift or unbounded scope")
    index = json.loads((args.work / "filings.json").read_text())
    check_file(index)
    if canonical_sha256(index) != batch["filing_index_sha256"]:
        raise ValueError("source validation filing index drift")
    verified, seen = [], set()
    for entry in batch["entries"]:
        key = entry["source"], entry["slice"], entry["part"], entry["parts"]
        if key in seen:
            raise ValueError("duplicate source partition in validation batch")
        seen.add(key)
        path = source_receipt_path(args.work, *key)
        if file_sha256(path) != entry["receipt_sha256"]:
            raise ValueError("source completion receipt changed after validation planning")
        record = validate_source_receipt(path, pins, entry["source"], entry["slice"], filing_index=index,
                                         buckets=args.buckets, part=entry["part"], parts=entry["parts"])
        verified.append({**entry, "identity": record["identity"], "rows": record["rows"],
                         "files": len(record["files"]), "bytes": sum(f["bytes"] for f in record["files"])})
    result = {**expected, "validation_plan_sha256": args.validation_plan_sha256,
              "filing_index_sha256": batch["filing_index_sha256"], "verified": verified}
    target = args.validation_plan.with_suffix(".verified.json")
    atomic_json(target, result)
    print(json.dumps({"receipt": target.as_posix(), "verified": len(verified), "bytes": sum(e["bytes"] for e in verified)}))


def verified_resume_jobs(args, pins, index, source_slices):
    """Skip a prefix only after bounded workers validate every completed slice."""
    directory = args.work / "source-reuse" / str(time.time_ns())
    complete, pending = [], []
    for source, source_slice in source_slices:
        parts = args.source_parts if source == "fsds" else 1
        for part in range(parts):
            entry = {"source": source, "slice": source_slice, "part": part, "parts": parts}
            path = source_receipt_path(args.work, source, source_slice, part, parts)
            if path.is_file():
                complete.append({**entry, "receipt_sha256": file_sha256(path)})
            else:
                pending.append(entry)
    batches, jobs = [], []
    for offset in range(0, len(complete), 16):
        batch = {"schema": "fundamental_source_reuse_batch_v1", "pins_sha256": file_sha256(args.pins),
                 "validator_sha256": file_sha256(__file__),
                 "normalizer_sha256": file_sha256(Path(__file__).resolve().parents[1] / "src/atx_db/research/fundamental_sources.py"),
                 "work": args.work.resolve().as_posix(), "buckets": args.buckets,
                 "filing_index_sha256": canonical_sha256(index), "entries": complete[offset:offset+16]}
        path = directory / f"batch-{offset//16:04d}.json"
        atomic_json(path, batch)
        digest = file_sha256(path)
        batches.append((path, digest, batch))
        jobs.append([sys.executable, str(Path(__file__).resolve()), "validate-sources", "--pins", str(args.pins.resolve()),
                     "--work", str(args.work.resolve()), "--root", str(args.root.resolve()), "--buckets", str(args.buckets),
                     "--validation-plan", str(path.resolve()), "--validation-plan-sha256", digest])
    if jobs and any(run_retained_jobs(jobs, directory / "guards")):
        raise ValueError("completed-source validation failed; no completed slice may be skipped")
    proofs = []
    for path, digest, batch in batches:
        proof_path = path.with_suffix(".verified.json")
        proof = json.loads(proof_path.read_text())
        if (proof["validation_plan_sha256"] != digest or file_sha256(path) != digest or
                any(proof.get(k) != v for k, v in batch.items() if k != "entries") or
                [{k: entry[k] for k in ("source", "slice", "part", "parts", "receipt_sha256")}
                 for entry in proof["verified"]] != batch["entries"]):
            raise ValueError("completed-source verification receipt differs from requested scope")
        # Detect a receipt replacement between the worker and skip decision.
        for entry in proof["verified"]:
            if file_sha256(source_receipt_path(args.work, entry["source"], entry["slice"], entry["part"], entry["parts"])) != entry["receipt_sha256"]:
                raise ValueError("completed source receipt changed after validation")
        proofs.append({"path": proof_path.resolve().as_posix(), "sha256": file_sha256(proof_path), "bytes": proof_path.stat().st_size})
    atomic_json(directory / "complete.json", {"verified_slices": len(complete), "pending_slices": pending,
                "proofs": proofs, "pins_sha256": file_sha256(args.pins), "validator_sha256": file_sha256(__file__)})
    return pending


def main() -> None:
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("command", choices=["prepare", "normalize", "normalize-all", "validate-sources", "plan", "build", "audit", "publish", "relocate", "prepare-release", "release"])
    parser.add_argument("--pins", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path("data/research"))
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--source", choices=["cf", "fsds"])
    parser.add_argument("--slice")
    parser.add_argument("--buckets", type=int, default=128)
    parser.add_argument("--bucket", type=int)
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--diagnostic", action="store_true")
    parser.add_argument("--storage-only", action="store_true",
                        help="verify one already completed/relocated bucket; never compute, audit, or publish anew")
    parser.add_argument("--limit-sources", type=int)
    parser.add_argument("--part", type=int, default=0)
    parser.add_argument("--parts", type=int, default=1)
    parser.add_argument("--source-parts", type=int, default=8)
    parser.add_argument("--reuse-filings", type=Path)
    parser.add_argument("--reuse-filings-sha256")
    parser.add_argument("--release-manifest", type=Path)
    parser.add_argument("--release-manifest-sha256")
    parser.add_argument("--validation-plan", type=Path)
    parser.add_argument("--validation-plan-sha256")
    parser.add_argument("--batch-verified-resume", action="store_true")
    args = parser.parse_args()
    if args.storage_only and (args.command not in {"build", "audit", "publish"} or args.bucket is None):
        parser.error("--storage-only requires build/audit/publish and one explicit --bucket")
    if args.command in {"prepare-release", "release"} and args.bucket is None:
        parser.error("release operations require one explicit --bucket")
    if args.command == "release" and (args.release_manifest is None or not args.release_manifest_sha256):
        parser.error("release requires an explicitly reviewed manifest and its SHA256")
    pins = json.loads(args.pins.read_text(encoding="utf-8"))
    # Reuse the accepted source audit. Manifest drift is checked immediately; every
    # worker independently verifies each file it actually consumes.
    for kind in ("cf", "fsds"):
        if file_sha256(pins[kind+"_manifest"]) != pins[kind+"_sha256"]:
            raise ValueError(f"{kind} source manifest drift")
    if not pins.get("scope_complete") or args.buckets < 128:
        raise ValueError("complete source pins and at least 128 owner buckets required")
    args.work.mkdir(parents=True, exist_ok=True)
    if args.command == "validate-sources":
        validate_source_batch(args, pins)
    elif args.command == "prepare":
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
        scope = (verified_resume_jobs(args, pins, index, source_slices) if args.batch_verified_resume else
                 [{"source": source, "slice": source_slice, "part": part,
                   "parts": args.source_parts if source == "fsds" else 1}
                  for source, source_slice in source_slices
                  for part in range(args.source_parts if source == "fsds" else 1)])
        jobs = [[sys.executable, str(Path(__file__).resolve()), "normalize", "--pins", str(args.pins.resolve()),
                 "--work", str(args.work.resolve()), "--root", str(args.root.resolve()),
                 "--source", e["source"], "--slice", e["slice"], "--buckets", str(args.buckets),
                 "--part", str(e["part"]), "--parts", str(e["parts"])] for e in scope]
        results = run_retained_jobs(jobs, args.work / "normalize-guards") if jobs else []
        if any(results):
            raise SystemExit(f"source workers incomplete: {results}")
    elif args.command == "plan":
        from atx_db.research.items_map import load_mapping
        from atx_db.research.item_vintages import PERIOD_POLICY, MAPPING_OUTCOMES_VERSION
        mapping = load_mapping()
        expected = [("fsds", f"{2009+(q+1)//4}q{(q+1)%4+1}") for q in range(69)]
        authority = mapping["fsds_endpoint_authority"]
        if (authority["manifest_sha256"] != pins["fsds_sha256"] or
                set(authority["quarter_readme_sha256"]) != {quarter for _, quarter in expected}):
            raise ValueError("C114 endpoint authority differs from the pinned FSDS scope")
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
                "mapping_outcomes_version": MAPPING_OUTCOMES_VERSION,
                "code": {p.as_posix(): file_sha256(p) for p in code_paths},
                "root": args.root.resolve().as_posix(), "work": args.work.resolve().as_posix()}
        plan["build_sha256"] = canonical_sha256(plan)
        plan["build_id"] = "fund-"+plan["build_sha256"][:20]
        target = args.work / plan["build_id"] / "plan.json"
        atomic_json(target, plan)
        print(json.dumps({"plan": target.as_posix(), "build_id": plan["build_id"],
                          "sources": len(receipts), "scope_complete": plan["scope_complete"],
                          "mapping_sha256": mapping["sha256"]}))
    elif args.command in {"build", "audit", "publish", "relocate", "prepare-release", "release"}:
        from atx_db.research.item_vintages import (
            build_bucket, audit_bucket, publish_bucket, publish_index, prepare_bucket_relocation,
            verify_bucket_storage, validate_original_plan,
            prepare_bucket_release, release_bucket_intermediates,
        )
        if args.plan is None:
            parser.error("build requires explicit --plan")
        plan = json.loads(args.plan.read_text())
        storage_operation = args.command in {"relocate", "prepare-release", "release"} or args.storage_only
        if not storage_operation:
            for path, expected_sha in plan["code"].items():
                if file_sha256(path) != expected_sha:
                    raise ValueError("build code differs from immutable plan; create a new plan")
        # Relocation performs no item computation. It preserves the original
        # computational plan and separately pins/verifies its storage code and
        # semantic equality to already sealed outputs. It never removes files.
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
            output = args.plan.parent / f"b{args.bucket:03d}"
            validate_original_plan(plan, output=output, root=args.root)
            files = [] if storage_operation else [
                check_file(f) for receipt in plan["source_receipts"] for f in receipt["files"] if f["bucket"] == args.bucket]
            if args.storage_only:
                result = verify_bucket_storage(plan, output=output, root=args.root)
                result["requested_command"] = args.command
            elif args.command == "build":
                result = build_bucket(files, plan["mapping"], root=args.root, output=output,
                                      build_sha256=plan["build_sha256"], bucket=args.bucket)
            elif args.command == "audit":
                result = audit_bucket(files, plan, output=output, root=args.root)
            elif args.command == "relocate":
                result = prepare_bucket_relocation(plan, output=output, root=args.root)
            elif args.command == "prepare-release":
                result = prepare_bucket_release(plan, output=output, root=args.root)
            elif args.command == "release":
                result = release_bucket_intermediates(plan, output=output, root=args.root,
                    release_manifest=args.release_manifest, release_manifest_sha256=args.release_manifest_sha256)
            else:
                result = publish_bucket(files, plan, output=output, root=args.root)
                if args.diagnostic:
                    result["index"] = publish_index(plan, args.plan.parent, root=args.root,
                                                     diagnostic_buckets=[args.bucket])
            print(json.dumps({k: v for k, v in result.items() if k not in {"files", "coverage"}}, default=str))


if __name__ == "__main__":
    main()
