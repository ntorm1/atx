#!/usr/bin/env python
"""Qualify a sealed R3b evaluation run under the frozen R4 policy (research store only).

Examples (light: opens only the research DuckDB, never the warehouse)::

    python scripts/research_qualify.py policy
    python scripts/research_qualify.py freeze                       # before RR4: record the policy hash
    python scripts/research_qualify.py qualify --run-id eval_2026_09_rr4 \
        --out-dir data/research/qualification --doc docs/research/QUALIFIED_SIGNALS.md
    python scripts/research_qualify.py verify --run-id eval_2026_09_rr4
    python scripts/research_qualify.py render-doc                   # policy-only doc (no ledger yet)

``qualify`` re-hashes the run's stored rows against its seal, refuses an unqualifiable run
(no frozen split or another split, subset or incomplete family, unproduced cells; exit 2
with every reason), registers the policy, stores the sealed ledger and writes the
versioned JSON/CSV artifacts. ``verify`` recomputes the ledger and compares it with the
stored one (exit 1 on a mismatch). The RR4 evaluation must run with
``--split-file src/atx_db/seeds/research_qualification_policy.json``.
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
    parser.add_argument("--no-verify-seal", action="store_true",
                        help="skip re-hashing the run's stored rows (not for production ledgers)")
    parser.add_argument("--no-catalog", action="store_true", help="grade without the R1a catalog annotations")
    return parser.parse_args(argv)


def _connect(args: argparse.Namespace, *, read_only: bool = False) -> duckdb.DuckDBPyConnection:
    path = args.research_db or default_research_db_path()
    if read_only and not path.is_file():
        raise SystemExit(f"research store not found: {path}")
    return duckdb.connect(str(path), read_only=read_only,
                          config={"memory_limit": args.memory_limit, "threads": args.threads})


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    policy = rq.load_policy(args.policy)
    if args.command == "policy":
        print(json.dumps({"policy_version": policy.version, "policy_sha256": policy.sha256,
                          "split_sha256": policy.split.sha256, "file_sha256s": sorted(policy.file_sha256s),
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
            print(json.dumps({"policy_version": policy.version, "policy_sha256": policy.sha256,
                              "registration": rq.register_policy(con, policy)}, sort_keys=True))
        finally:
            con.close()
        return 0
    if not args.run_id:
        raise SystemExit(f"{args.command} needs --run-id")
    catalog = None if args.no_catalog else read_anomaly_catalog()
    con = _connect(args, read_only=args.command == "verify")
    try:
        try:
            ledger = rq.qualify_run(con, args.run_id, policy, catalog=catalog, verify_seal=not args.no_verify_seal,
                                    persist=args.command == "qualify")
        except rq.QualificationRefused as refused:
            print(json.dumps({"run_id": args.run_id, "refused": list(refused.reasons)}, indent=2))
            return 2
        if args.command == "verify":
            stored = rq.stored_ledger_sha256(con, ledger.ledger_id)
            print(json.dumps({"ledger_id": ledger.ledger_id, "stored_sha256": stored,
                              "recomputed_sha256": ledger.sha256, "reproduced": stored == ledger.sha256},
                             indent=2, sort_keys=True))
            return 0 if stored == ledger.sha256 else 1
        out_dir = args.out_dir or (args.research_db or default_research_db_path()).parent / "qualification"
        json_path, csv_path = rq.write_artifacts(ledger, out_dir)
        if args.doc is not None:
            args.doc.write_text(rq.render_qualified_signals_markdown(policy, ledger), encoding="utf-8", newline="\n")
        print(json.dumps({"ledger_id": ledger.ledger_id, "ledger_sha256": ledger.sha256,
                          "status_counts": ledger.manifest["status_counts"], "blockers": ledger.manifest["blockers"],
                          "json": str(json_path), "csv": str(csv_path)}, indent=2, sort_keys=True))
        run_file = ledger.manifest["evaluation"]["policy_file_sha256"]
        if policy.file_sha256s and run_file not in policy.file_sha256s:
            print(f"WARNING: run {args.run_id} was evaluated with policy file sha256 {run_file}, "
                  f"not this policy file", file=sys.stderr)
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    raise SystemExit(main())
