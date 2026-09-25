#!/usr/bin/env python
"""Run or verify an R3b monthly evaluation in the research store (RX6).

Examples (heavy slot, pinned export, warehouse attached read-only)::

    python scripts/research_evaluate.py run --run-id eval_2026_09_rr4 \
        --feature-version <reconstructed sha> --feature-version <strict sha> \
        --label-cutoff 2026-09-20T22:00:00Z --policy src/atx_db/seeds/research_qualification_policy.json

    python scripts/research_evaluate.py verify --run-id eval_2026_09_rr4

``--policy`` builds the spec from a frozen R4 qualification policy (v3+) through
``qualification.evaluation_spec_kwargs``: split, horizons, subperiods, the name and
formation thresholds and ``verify_panels`` come from the policy, and the policy file's
byte hash is recorded as the run's ``policy_sha256``. It is the qualifying (RR4) form: R4
refuses a run whose sealed spec differs from its policy. The flags the policy pins
(``--split-file``, ``--allow-unsplit``, ``--horizons``, ``--min-names``,
``--min-formations``, ``--skip-panel-validation``) cannot be combined with it. Before any
work the run exits 2 unless the policy is frozen, current (not superseded, pins an
``evaluation_spec``) and registered in the target research store (``research_qualify.py
freeze``, RX7).

``run`` refuses to start without a frozen split (RX7) unless ``--allow-unsplit`` marks the
run as exploratory (recorded as a blocker). The family is the R1a catalog's expected
cells: ``--features``/``--variants``/``--horizons`` subsets still correct for the whole
family, carry the blocker ``partial_family_subset`` and are never ``family_complete``
(qualification must refuse them); a run without both bases is not ``family_complete``
either. Size is verified (DEI-share) market cap only and NYSE size buckets use the
point-in-time venue (``venue_basis``). ``verify`` re-derives the run from its sealed
manifest and reports whether the stored rows and a recomputation are byte-identical.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db.research.evaluation import (
    DEFAULT_HORIZONS,
    EvaluationSpec,
    load_frozen_split,
    run_evaluation,
    verify_evaluation_run,
)
from atx_db.research.store import ResearchStore

DEFAULT_MIN_NAMES = 200
DEFAULT_MIN_FORMATIONS = 36


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
    parser.add_argument("--policy", type=Path, default=None,
                        help="frozen R4 qualification policy (v3+): the spec values it pins come from it")
    pinned = parser.add_argument_group("spec values a --policy pins (not allowed with --policy)")
    pinned.add_argument("--split-file", type=Path, help="frozen split JSON (or a policy JSON with a 'split')")
    pinned.add_argument("--allow-unsplit", action="store_true", help="exploratory run without a frozen split")
    pinned.add_argument("--horizons", type=int, nargs="+", default=None,
                        help=f"default {' '.join(map(str, DEFAULT_HORIZONS))}")
    pinned.add_argument("--min-names", type=int, default=None, help=f"default {DEFAULT_MIN_NAMES}")
    pinned.add_argument("--min-formations", type=int, default=None, help=f"default {DEFAULT_MIN_FORMATIONS}")
    pinned.add_argument("--skip-panel-validation", action="store_true",
                        help="trust the R2a panel seal and the R2b version seal without running their validators")
    parser.add_argument("--features", type=_names, default=None, help="comma list; default: every feature")
    parser.add_argument("--variants", type=_names, default=None, help="comma list; default: every variant")
    parser.add_argument("--bootstrap-resamples", type=int, default=1999)
    parser.add_argument("--no-label-diagnostics", action="store_true")
    parser.add_argument("--resume", action="store_true", help="continue a building/failed run with the same spec")
    args = parser.parse_args(argv)
    if args.policy is not None:
        given = [flag for flag, value in (("--split-file", args.split_file), ("--allow-unsplit", args.allow_unsplit),
                                          ("--horizons", args.horizons), ("--min-names", args.min_names),
                                          ("--min-formations", args.min_formations),
                                          ("--skip-panel-validation", args.skip_panel_validation))
                 if value not in (None, False)]
        if given:
            parser.error(f"--policy pins these spec values; drop {', '.join(given)}")
    return args


def policy_preflight(store: Any, path: Path) -> str | None:
    """Why a ``--policy`` run must not start (None: it may), checked before any evaluation work.

    The policy must load frozen and pinned, be current (not superseded, pins an
    ``evaluation_spec``) and be registered (``research_qualify.py freeze``) in this research
    store before the run: R4 refuses a run evaluated before its policy was frozen (RX7).
    Uses R4's read-only helpers (``current_policy_problem`` when present, else the same
    checks directly, and ``policy_registered_at``).
    """
    from atx_db.research import qualification as rq

    try:
        policy = rq.load_policy(path)
        current = getattr(rq, "current_policy_problem", None)
        problem = current(policy) if current is not None else _policy_problem(rq, policy)
        if problem is not None:
            return problem
        registered = rq.policy_registered_at(store.con, policy)
    except (ValueError, OSError) as error:  # PolicyError is a ValueError: edited, unpinned or re-registered
        return f"policy_invalid:{error}"
    if registered is None:
        return f"policy_not_registered_in_store:{policy.version} (run research_qualify.py freeze first, RX7)"
    return None


def _policy_problem(rq: Any, policy: Any) -> str | None:
    """Fallback for R4's ``current_policy_problem``: superseded by a committed policy, or no evaluation_spec."""
    for committed in (rq.POLICY_PATH, *rq.POLICY_HISTORY_PATHS.values()):
        if json.loads(Path(committed).read_bytes().decode("utf-8")).get("supersedes") == policy.version:
            return f"policy_superseded:{policy.version}"
    if policy.evaluation_spec is None:
        return f"policy_pins_no_evaluation_spec:{policy.version}"
    return None


def build_spec(args: argparse.Namespace) -> EvaluationSpec:
    """The run's EvaluationSpec: from the policy (``evaluation_spec_kwargs``) or from the flags."""
    common: dict[str, Any] = {
        "run_id": args.run_id, "feature_versions": tuple(args.feature_version), "label_cutoff": args.label_cutoff,
        "features": args.features, "variants": args.variants, "bootstrap_resamples": args.bootstrap_resamples,
        "label_diagnostics": not args.no_label_diagnostics}
    if args.policy is not None:
        from atx_db.research.qualification import evaluation_spec_kwargs, load_policy

        return EvaluationSpec(**common, **evaluation_spec_kwargs(load_policy(args.policy)))
    min_names = DEFAULT_MIN_NAMES if args.min_names is None else args.min_names
    return EvaluationSpec(
        **common, horizons_months=tuple(args.horizons or DEFAULT_HORIZONS),
        split=None if args.split_file is None else load_frozen_split(args.split_file),
        policy_sha256=None if args.split_file is None else hashlib.sha256(args.split_file.read_bytes()).hexdigest(),
        allow_unsplit=args.allow_unsplit, min_names=min_names, fm_min_obs=min_names,
        min_formations=DEFAULT_MIN_FORMATIONS if args.min_formations is None else args.min_formations,
        verify_panels=not args.skip_panel_validation)


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
        if args.policy is not None:
            problem = policy_preflight(store, args.policy)
            if problem is not None:  # fail fast: never start a multi-hour run R4 would refuse
                print(f"research_evaluate: error: --policy {args.policy}: {problem}", file=sys.stderr)
                return 2
        result = run_evaluation(store, build_spec(args), resume=args.resume)
        print(json.dumps({"run_id": result.run_id, "status": result.status, "cells": result.cells,
                          "results_sha256": result.results_sha256, "inputs_sha256": result.inputs_sha256,
                          "family_complete": result.family_complete, "family": result.family,
                          "blockers": list(result.blockers)}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
