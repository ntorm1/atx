#!/usr/bin/env python
"""Build forward from a sealed R4 ledger: redundancy annotation, composites, one holdout evaluation (P5).

Examples (the research store; the warehouse is attached read-only for labels and venue)::

    python scripts/research_composite.py policy
    python scripts/research_composite.py freeze                      # MANDATORY before the RR4 run (RX7)
    python scripts/research_composite.py build --ledger-id eval_2026_09_rr4:r4-qualification-v3 \
        --factor-run p4_2026_09_rr3
    python scripts/research_composite.py qualify --holdout-key <split_sha256>:reconstructed
    python scripts/research_composite.py render-doc                  # policy-only doc (no build yet)

``build`` selects constituents on R4's train + validation gates only (never the holdout),
annotates them (``novel`` / ``spanned_by_known_factors`` / ``duplicate_of:<id>``; R4
statuses are never changed), fits the composites on train + validation, stores the build
(one holdout evaluation per split and basis: an identical rerun is a no-op, an
unevaluated build may be retried, anything else after the evaluation is refused), writes
the composite feature versions (the source version plus the composites) and evaluates
the full family once with the R3b harness. The composite policy must have been frozen
(``freeze``) before the source run was created, unless ``--allow-post-hoc-policy``
(stamped). ``qualify`` grades that run under the frozen R4 policy within the full family,
every row stamped ``composite_post_selection``. Refusals exit 2 with the reason.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db.research import composites as cmp
from atx_db.research import qualification as rq
from atx_db.research.catalog import read_anomaly_catalog
from atx_db.research.store import ResearchStore, default_research_db_path

DEFAULT_DOC = Path(__file__).resolve().parents[1] / "docs" / "research" / "COMPOSITES.md"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("policy", "freeze", "build", "qualify", "render-doc"))
    parser.add_argument("--ledger-id", help="sealed R4 ledger (<run_id>:<policy_version>) to build from")
    parser.add_argument("--holdout-key", help="holdout window <split_sha256>:<basis> (qualify)")
    parser.add_argument("--factor-run", help="complete P4 factor run (monthly factors for the spanning test)")
    parser.add_argument("--policy", type=Path, default=cmp.POLICY_PATH, help="frozen composite policy JSON")
    parser.add_argument("--r4-policy", type=Path, default=rq.POLICY_PATH, help="frozen R4 policy (qualify)")
    parser.add_argument("--research-db", type=Path, default=None,
                        help="default: $ATX_RESEARCH_DB_PATH or <data dir>/research/research.duckdb")
    parser.add_argument("--warehouse", type=Path, default=None, help="attached read-only (labels, venue)")
    parser.add_argument("--doc", type=Path, default=None, help="write the generated COMPOSITES.md here")
    parser.add_argument("--allow-post-hoc-policy", action="store_true",
                        help="build although the composite policy was frozen after the source run (stamped)")
    parser.add_argument("--memory-limit", default="128MB",
                        help="DuckDB limit (the build measured 420 MB peak at 40 signals x 5,000 names with 128MB)")
    parser.add_argument("--threads", type=int, default=1)
    return parser.parse_args(argv)


def _refused(key: str | None, reasons: list[str]) -> int:
    print(json.dumps({"target": key, "refused": reasons}, indent=2))
    return 2


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        policy = cmp.load_composite_policy(args.policy)
    except cmp.CompositeError as error:
        return _refused(str(args.policy), [str(error)])
    if args.command == "policy":
        print(json.dumps({"policy_version": policy.version, "policy_sha256": policy.sha256,
                          "content": policy.content}, indent=2, sort_keys=True))
        return 0
    if args.command == "render-doc":
        doc = args.doc or DEFAULT_DOC
        doc.write_text(cmp.render_composites_markdown(policy), encoding="utf-8", newline="\n")
        print(f"wrote {doc}")
        return 0
    if args.command == "build" and not (args.ledger_id and args.factor_run):
        raise SystemExit("build needs --ledger-id and --factor-run")
    if args.command == "qualify" and not args.holdout_key:
        raise SystemExit("qualify needs --holdout-key")
    store = ResearchStore(args.research_db or default_research_db_path(), warehouse_path=args.warehouse,
                          memory_limit=args.memory_limit, threads=args.threads)
    with store:
        con = store.con
        try:
            if args.command == "freeze":
                outcome = cmp.register_composite_policy(con, policy)
                print(json.dumps({"policy_version": policy.version, "policy_sha256": policy.sha256,
                                  "registration": outcome,
                                  "registered_at": cmp.composite_policy_registered_at(con, policy)}, sort_keys=True))
                return 0
            if args.command == "build":
                outcome = cmp.run_build_forward(store, args.ledger_id, factor_run_id=args.factor_run, policy=policy,
                                                allow_post_hoc_policy=args.allow_post_hoc_policy)
                print(json.dumps(outcome, indent=2, sort_keys=True))
                return 0
            ledger = cmp.qualify_composites(con, args.holdout_key, rq.load_policy(args.r4_policy),
                                            read_anomaly_catalog())
        except rq.QualificationRefused as refused:
            return _refused(args.holdout_key or args.ledger_id, list(refused.reasons))
        except (cmp.CompositeError, rq.PolicyError) as error:
            return _refused(args.holdout_key or args.ledger_id, [str(error)])
        print(json.dumps({"ledger_id": ledger.ledger_id, "ledger_sha256": ledger.sha256,
                          "status_counts": ledger.manifest["status_counts"], "blockers": ledger.manifest["blockers"]},
                         indent=2, sort_keys=True))
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
