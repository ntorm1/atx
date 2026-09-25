#!/usr/bin/env python
"""Build forward from a sealed R4 ledger: redundancy annotation, composites, one holdout evaluation (P5).

Examples (light: opens only the research store, never the warehouse's write path)::

    python scripts/research_composite.py policy
    python scripts/research_composite.py build --ledger-id eval_2026_09_rr4:r4-qualification-v3 \
        --factor-run p4_2026_09_rr3
    python scripts/research_composite.py qualify --ledger-id eval_2026_09_rr4:r4-qualification-v3
    python scripts/research_composite.py render-doc                  # policy-only doc (no build yet)

``build`` annotates the ledger's qualified signals (``novel`` / ``spanned_by_known_factors``
/ ``duplicate_of:<id>``; the R4 statuses are never changed), fits the composites on the
train + validation formations only, stores the build (exactly one per R4 ledger: an
identical rerun is byte-identical, a different one is refused), writes the composites as
R2b feature versions (store schema 3, class ``composite``) and evaluates them once with
the R3b harness under the source run's spec. ``qualify`` grades that run under the frozen
R4 policy with the composite catalog, so composites pass the same significance gates.
Refusals exit 2 with the reason.
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
from atx_db.research.store import ResearchStore, default_research_db_path

DEFAULT_DOC = Path(__file__).resolve().parents[1] / "docs" / "research" / "COMPOSITES.md"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("policy", "build", "qualify", "render-doc"))
    parser.add_argument("--ledger-id", help="sealed R4 ledger (<run_id>:<policy_version>)")
    parser.add_argument("--factor-run", help="complete P4 factor run (monthly factors for the spanning test)")
    parser.add_argument("--policy", type=Path, default=cmp.POLICY_PATH, help="frozen composite policy JSON")
    parser.add_argument("--r4-policy", type=Path, default=rq.POLICY_PATH, help="frozen R4 policy (qualify)")
    parser.add_argument("--research-db", type=Path, default=None,
                        help="default: $ATX_RESEARCH_DB_PATH or <data dir>/research/research.duckdb")
    parser.add_argument("--warehouse", type=Path, default=None, help="attached read-only (labels, venue)")
    parser.add_argument("--doc", type=Path, default=None, help="write the generated COMPOSITES.md here")
    parser.add_argument("--memory-limit", default="512MB")
    parser.add_argument("--threads", type=int, default=1)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    policy = cmp.load_composite_policy(args.policy)
    if args.command == "policy":
        print(json.dumps({"policy_version": policy.version, "policy_sha256": policy.sha256,
                          "content": policy.content}, indent=2, sort_keys=True))
        return 0
    if args.command == "render-doc":
        doc = args.doc or DEFAULT_DOC
        doc.write_text(cmp.render_composites_markdown(policy), encoding="utf-8", newline="\n")
        print(f"wrote {doc}")
        return 0
    if not args.ledger_id:
        raise SystemExit(f"{args.command} needs --ledger-id")
    if args.command == "build" and not args.factor_run:
        raise SystemExit("build needs --factor-run (P4 monthly factors for the spanning test)")
    store = ResearchStore(args.research_db or default_research_db_path(), warehouse_path=args.warehouse,
                          memory_limit=args.memory_limit, threads=args.threads)
    with store:
        try:
            if args.command == "build":
                outcome = cmp.run_build_forward(store, args.ledger_id, factor_run_id=args.factor_run, policy=policy)
                print(json.dumps(outcome, indent=2, sort_keys=True))
                return 0
            ledger = cmp.qualify_composites(store.con, args.ledger_id, rq.load_policy(args.r4_policy))
        except (cmp.CompositeError, rq.QualificationRefused) as refused:
            reasons = list(refused.reasons) if isinstance(refused, rq.QualificationRefused) else [str(refused)]
            print(json.dumps({"ledger_id": args.ledger_id, "refused": reasons}, indent=2))
            return 2
        print(json.dumps({"ledger_id": ledger.ledger_id, "ledger_sha256": ledger.sha256,
                          "status_counts": ledger.manifest["status_counts"], "blockers": ledger.manifest["blockers"]},
                         indent=2, sort_keys=True))
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
