#!/usr/bin/env python
"""Qualify a sealed R3b evaluation run under the frozen R4 policy (research store only).

Examples (light: opens only the research DuckDB, never the warehouse)::

    python scripts/research_qualify.py policy
    python scripts/research_qualify.py freeze                       # MANDATORY before RR4 (RX7)
    python scripts/research_qualify.py qualify --run-id eval_2026_09_rr4 \
        --out-dir data/research/qualification --doc docs/research/QUALIFIED_SIGNALS.md
    python scripts/research_qualify.py verify --run-id eval_2026_09_rr4
    python scripts/research_qualify.py render-doc                   # policy-only doc (no ledger yet)

RX7 order: ``freeze`` records the policy in the research store; RR4 then builds its
evaluation spec from the policy (``qualification.evaluation_spec_kwargs``: split,
subperiods, thresholds, ``verify_panels`` and the policy file hash). ``qualify`` refuses
(exit 2, every reason listed) a run that was created before the freeze, evaluated under
another policy file or another evaluation spec, is a subset or incomplete family, has
unproduced cells or catalog drift, or whose stored rows no longer match its seal. It then
stores the sealed ledger and writes the versioned JSON/CSV artifacts.
``--allow-post-hoc-policy`` passes only the policy-order refusals and stamps the ledger,
every row and the doc. ``--no-catalog`` / ``--no-verify-seal`` are allowed only with
``--dry-run`` (nothing is stored). ``verify`` recomputes the stored ledger from the run and
the ledger's own catalog snapshot and compares the sha (exit 1 on a mismatch).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import duckdb

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db.research import qualification as rq
from atx_db.research.catalog import read_anomaly_catalog
from atx_db.research.store import default_research_db_path

DEFAULT_DOC = Path(__file__).resolve().parents[1] / "docs" / "research" / "QUALIFIED_SIGNALS.md"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("policy", "freeze", "qualify", "verify", "render-doc"))
    parser.add_argument("--run-id", help="sealed R3b evaluation run (qualify, verify)")
    parser.add_argument("--policy", type=Path, default=rq.POLICY_PATH, help="frozen policy JSON")
    parser.add_argument("--research-db", type=Path, default=None,
                        help="default: $ATX_RESEARCH_DB_PATH or <data dir>/research/research.duckdb")
    parser.add_argument("--out-dir", type=Path, default=None,
                        help="versioned JSON/CSV artifacts (default: <research db dir>/qualification)")
    parser.add_argument("--doc", type=Path, default=None, help="write the generated QUALIFIED_SIGNALS.md here")
    parser.add_argument("--memory-limit", default="512MB")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--dry-run", action="store_true", help="grade and print only; store and write nothing")
    parser.add_argument("--no-verify-seal", action="store_true", help="skip re-hashing the run's rows (dry run only)")
    parser.add_argument("--no-catalog", action="store_true", help="skip the R1a catalog check (dry run only)")
    parser.add_argument("--allow-post-hoc-policy", action="store_true",
                        help="grade a run not evaluated under this frozen policy; stamps the ledger as post hoc")
    return parser.parse_args(argv)


def _connect(args: argparse.Namespace, *, read_only: bool = False) -> duckdb.DuckDBPyConnection:
    path = args.research_db or default_research_db_path()
    if read_only and not path.is_file():
        raise SystemExit(f"research store not found: {path}")
    return duckdb.connect(str(path), read_only=read_only,
                          config={"memory_limit": args.memory_limit, "threads": args.threads})


def _verify(con: duckdb.DuckDBPyConnection, args: argparse.Namespace, policy: rq.QualificationPolicy) -> int:
    ledger_id = f"{args.run_id}:{policy.version}"
    row = con.execute("SELECT manifest_json, ledger_sha256 FROM research_qualification_ledgers WHERE ledger_id = ?",
                      [ledger_id]).fetchone() if rq.stored_ledger_sha256(con, ledger_id) else None
    if row is None:
        raise SystemExit(f"no stored ledger {ledger_id}")
    manifest = json.loads(row[0])
    ledger = rq.qualify_run(con, args.run_id, policy, catalog=rq.stored_catalog_snapshot(con, ledger_id),
                            persist=False, allow_post_hoc_policy=bool(manifest.get("post_hoc_policy")))
    print(json.dumps({"ledger_id": ledger_id, "stored_sha256": row[1], "recomputed_sha256": ledger.sha256,
                      "reproduced": row[1] == ledger.sha256}, indent=2, sort_keys=True))
    return 0 if row[1] == ledger.sha256 else 1


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    policy = rq.load_policy(args.policy)
    if args.command == "policy":
        print(json.dumps({"policy_version": policy.version, "policy_sha256": policy.sha256,
                          "split_sha256": policy.split.sha256, "file_sha256": policy.file_sha256,
                          "file_sha256s": sorted(policy.file_sha256s), "evaluation_spec": policy.evaluation_spec,
                          "primary_horizon_months": policy.primary_horizon,
                          "primary_variant": policy.primary_variant}, indent=2, sort_keys=True))
        return 0
    if args.command == "render-doc":
        doc = args.doc or DEFAULT_DOC
        doc.write_text(rq.render_qualified_signals_markdown(policy), encoding="utf-8", newline="\n")
        print(f"wrote {doc}")
        return 0
    if args.command == "freeze":
        con = _connect(args)
        try:
            outcome = rq.register_policy(con, policy)
            print(json.dumps({"policy_version": policy.version, "policy_sha256": policy.sha256,
                              "registration": outcome, "registered_at": rq.policy_registered_at(con, policy)},
                             sort_keys=True))
        finally:
            con.close()
        return 0
    if not args.run_id:
        raise SystemExit(f"{args.command} needs --run-id")
    if (args.no_catalog or args.no_verify_seal) and not args.dry_run:
        raise SystemExit("--no-catalog / --no-verify-seal are allowed only with --dry-run (never persisted)")
    con = _connect(args, read_only=args.command == "verify" or args.dry_run)
    try:
        if args.command == "verify":
            return _verify(con, args, policy)
        catalog = None if args.no_catalog else read_anomaly_catalog()
        try:
            ledger = rq.qualify_run(con, args.run_id, policy, catalog=catalog, verify_seal=not args.no_verify_seal,
                                    persist=not args.dry_run, allow_post_hoc_policy=args.allow_post_hoc_policy)
        except rq.QualificationRefused as refused:
            print(json.dumps({"run_id": args.run_id, "refused": list(refused.reasons)}, indent=2))
            return 2
        summary = {"ledger_id": ledger.ledger_id, "ledger_sha256": ledger.sha256, "dry_run": args.dry_run,
                   "post_hoc_policy": ledger.manifest.get("post_hoc_policy"),
                   "status_counts": ledger.manifest["status_counts"], "blockers": ledger.manifest["blockers"]}
        if not args.dry_run:
            out_dir = args.out_dir or (args.research_db or default_research_db_path()).parent / "qualification"
            json_path, csv_path = rq.write_artifacts(ledger, out_dir)
            summary.update({"json": str(json_path), "csv": str(csv_path)})
            if args.doc is not None:
                args.doc.write_text(rq.render_qualified_signals_markdown(policy, ledger), encoding="utf-8",
                                    newline="\n")
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    raise SystemExit(main())
