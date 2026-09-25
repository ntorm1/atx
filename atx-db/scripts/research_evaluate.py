#!/usr/bin/env python
"""Run or verify an R3b monthly evaluation in the research store (RX6).

Examples (heavy slot, pinned export, warehouse attached read-only)::

    python scripts/research_evaluate.py run --run-id eval_2026_09_rr4 \
        --feature-version <reconstructed sha> --feature-version <strict sha> \
        --label-cutoff 2026-09-20T22:00:00Z --split-file seeds/research_qualification_policy.json

    python scripts/research_evaluate.py verify --run-id eval_2026_09_rr4

``run`` refuses to start without a frozen split (RX7) unless ``--allow-unsplit`` marks the
run as exploratory (recorded as a blocker). ``verify`` re-derives the run from its sealed
manifest and reports whether the stored rows and a recomputation are byte-identical.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db.research.evaluation import (
    DEFAULT_HORIZONS,
    EvaluationSpec,
    load_frozen_split,
    run_evaluation,
    verify_evaluation_run,
)
from atx_db.research.store import ResearchStore


def _utc(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise argparse.ArgumentTypeError("label cutoff must carry a UTC offset, e.g. 2026-09-20T22:00:00Z")
    return parsed


def _names(value: str) -> tuple[str, ...]:
    return tuple(item.strip() for item in value.split(",") if item.strip())


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("run", "verify"))
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--research-db", type=Path, default=None, help="default: $ATX_RESEARCH_DB_PATH or data dir")
    parser.add_argument("--warehouse", type=Path, default=None, help="attached READ_ONLY; default: data dir")
    parser.add_argument("--memory-limit", default="512MB")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--feature-version", action="append", default=[],
                        help="R2b feature version; pass one per basis (strict is always attempted)")
    parser.add_argument("--label-cutoff", type=_utc, help="observation vintage of the labels (UTC)")
    parser.add_argument("--split-file", type=Path, help="frozen split JSON (or a policy JSON with a 'split')")
    parser.add_argument("--allow-unsplit", action="store_true", help="exploratory run without a frozen split")
    parser.add_argument("--features", type=_names, default=None, help="comma list; default: every feature")
    parser.add_argument("--variants", type=_names, default=None, help="comma list; default: every variant")
    parser.add_argument("--horizons", type=int, nargs="+", default=list(DEFAULT_HORIZONS))
    parser.add_argument("--min-names", type=int, default=200)
    parser.add_argument("--min-formations", type=int, default=36)
    parser.add_argument("--bootstrap-resamples", type=int, default=1999)
    parser.add_argument("--no-label-diagnostics", action="store_true")
    parser.add_argument("--skip-panel-validation", action="store_true",
                        help="trust the R2a panel seal without re-validating every digest")
    parser.add_argument("--resume", action="store_true", help="continue a building/failed run with the same spec")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    with ResearchStore(args.research_db, warehouse_path=args.warehouse, memory_limit=args.memory_limit,
                       threads=args.threads) as store:
        if args.command == "verify":
            result = verify_evaluation_run(store, args.run_id)
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0 if result["reproduced"] else 1
        if not args.feature_version:
            raise SystemExit("run needs at least one --feature-version")
        if args.label_cutoff is None:
            raise SystemExit("run needs --label-cutoff (the observation vintage of the labels)")
        spec = EvaluationSpec(
            run_id=args.run_id, feature_versions=tuple(args.feature_version), label_cutoff=args.label_cutoff,
            horizons_months=tuple(args.horizons), features=args.features, variants=args.variants,
            split=None if args.split_file is None else load_frozen_split(args.split_file),
            policy_sha256=None if args.split_file is None else hashlib.sha256(args.split_file.read_bytes()).hexdigest(),
            allow_unsplit=args.allow_unsplit, min_names=args.min_names, fm_min_obs=args.min_names,
            min_formations=args.min_formations, bootstrap_resamples=args.bootstrap_resamples,
            label_diagnostics=not args.no_label_diagnostics, verify_panels=not args.skip_panel_validation)
        result = run_evaluation(store, spec, resume=args.resume)
        print(json.dumps({"run_id": result.run_id, "status": result.status, "cells": result.cells,
                          "results_sha256": result.results_sha256, "inputs_sha256": result.inputs_sha256,
                          "family": result.family, "blockers": list(result.blockers)}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
