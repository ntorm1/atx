"""Wave evaluation runner (tier-1 v2 node 1.13): a registered wave's features against its labels, selection sample.

The first wave is ``w1_price``: the 38 price natives of node 1.12 (a pinned feature-store manifest) against the
provisional price labels (a pinned label set, sealed from 2024-01-01), graded under the frozen policy v4
(``776db445...``) on ``grade_basis=provisional_labels``. Holdout formations (2024-01 and later) are refused.

Every input is pinned on the command line, never a default and never "latest": ``--manifest-sha``,
``--catalog-digest``, ``--lake-snapshot``, ``--bench-snapshot`` and ``--label-sha``. ``prepare`` refuses features,
labels and a lake snapshot that were not built on one spine (their spine digests must agree).

Commands
    register    freeze the wave in the trial registry (ruling R-6: before any label join). Without ``--confirm``
                it only prints the planned registration (rows, cells, first formations). Worker-sized: run it
                under the guard at 0.6 GiB. Commit ``src/atx_db/seeds/research_trial_registry_anchor.jsonl``
                right after (grading reads the anchor from git HEAD).
    run         (default) orchestrator: prepare -> eval (feature groups, capped workers) -> grade. Start it under
                the guard as an orchestrator (ruling C-58)::

                    run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 --
                        python scripts/research_run_wave.py --wave w1_price --formations 2013-01..2023-12 --workers 2
                        --manifest-sha <sha> --catalog-digest <sha> --lake-snapshot <id> --bench-snapshot <id>
                        --label-sha <sha> [--require-artifact-flag] [--universe-flag <spine column>]

    stage NAME  worker entry points (``prepare``, ``eval``, ``grade``, ``stub_labels``), each run by ``run`` under
                its own 0.6 GiB guard (``research.workers.run_jobs``).

Stages
    prepare     checks the frozen policy, the catalog digest, the registration (committed anchor, catalog and
                policy of the registration), the lake snapshot and every feature file against the manifest,
                then builds the ONE evaluation spec of the run (``qualification.evaluation_spec_kwargs_v4``:
                sealed holdout, bound to the registration; ``created_at`` fixed here) and keeps it in
                ``prepare.json``: every worker and every resume uses that spec (1.9 r2 / C-60), never a rebuilt one.
    eval        one job per feature group. Per job: the formation calendar (XNYS rule sessions, supplied NYSE
                20/50 ME breakpoints from the French file, in market-cap dollars), the context (the spine lines of
                each formation: price, ``me_line``; a ``vendor_artifact_suspect`` row (ruling C-79) or a row outside
                ``--universe-flag`` is withheld from the ranked universe and counted), the labels
                (``LabelMatrix.r3b_inputs`` per horizon, after the registration check; the grid check of 1.9 r2
                n3; ``invalid`` labels, e.g. C-79's, are R3b status 1: withheld and counted in ``labels_invalid``),
                the store adapter, the controls
                (``ret_12_1`` only: the defaults are not w1 rows, 1.9 r2 n4), then ``eval_cache.basis_key`` BEFORE
                ``evaluate_bases`` (live frames) and one cache entry per feature (family columns cleared: they
                are recomputed over the whole wave at grading). Memory marks (private commit) at every step.
    grade       reads every feature's cells back from the cache, recomputes the family statistics over the whole
                wave, grades with ``qualification.grade_wave_v4`` (provisional labels) and writes the ledger and
                the report tables (``report.json``, ``report.md``) the wave's research note is written from.

Dry run (``--stub-labels``, with ``--out`` a scratch root): the ``stub_labels`` stage writes a label set of
seeded random returns (independent of every feature; never a result) for the spine rows under ``--out``, and
the run uses it instead of the real labels. No registration is read or written (a stand-in registration binds
the spec), no real label or return is read, and every output goes under ``--out``. It measures the read side
(store adapter, label read, ``basis_key``) and the whole path at real scale.

No warehouse is opened (ruling R-5): inputs are the research lake, the feature store and the label matrix.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
import re
import sys
import time
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

ROOT = Path("C:/atx/atx-db/data/research")
ANCHOR = Path("C:/atx/atx-db/src/atx_db/seeds/research_trial_registry_anchor.jsonl")
BASIS = "reconstructed"
HORIZONS = (1, 3, 6, 12)
HOLDOUT_START = dt.date(2024, 1, 1)
#: The label read window ends at the holdout start (the provisional labels read no bar on or after it).
LABEL_CUTOFF = dt.datetime(2024, 1, 1, 0, 0)
POLICY_SHA = "776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a"
STUB_SEED = 20260926
REBALANCE_METHOD = "pinned_label_old_entry_to_current_entry_v1"
MIB = 1024 ** 2
GIB = 1024 ** 3

#: Spine column of ruling C-79: a (line, month) whose inputs touch a flagged vendor bar (a >= x50 factor increase
#: on the 2021-01-04 artifact session, a flat-close factor step, or a past-anchored sentinel close).
#: A flagged context row is withheld from the ranked
#: universe (its values are dropped and counted by the engine as values_dropped_not_valid); its labels are
#: ``invalid`` in the label set (R3b status 1: counted in labels_invalid, never used).
ARTIFACT_FLAG = "vendor_artifact_suspect"
_SHA = re.compile(r"^[0-9a-f]{64}$")
_SNAPSHOT = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,127}$")

#: Per-wave constants (only the price wave exists so far). The inputs themselves -- the feature manifest, the lake
#: snapshot, the label set and the catalog digest -- are never defaults: every run pins them on the command line
#: (``--manifest-sha``, ``--lake-snapshot``, ``--bench-snapshot``, ``--label-sha``, ``--catalog-digest``).
WAVES: Mapping[str, Mapping[str, Any]] = {
    "w1_price": {
        # The retained vendor file's first session (TickerHistory3, node 1.12): the history clock of
        # every native starts here (first_formations).
        "data_start": dt.date(2012, 3, 26),
        # FM controls: the w1 momentum row (the R3b defaults book_to_market / momentum_12_1 are not w1 rows,
        # 1.9 r2 n4); no value row exists in w1. Size is the engine's log market cap (me_line).
        "control_features": ("ret_12_1",),
        # Ruling C-82 (3): pre-registered rows that are NOT registered in this wave (the feature cannot measure
        # what it claims on this input); reported as unsupported with their monthly all-zero counts.
        "unsupported": {
            "zero_trade_21d": "vendor_zero_volume_absent",
            "zero_trade_252d": "vendor_zero_volume_absent",
        },
        # The pre-registration table: task-1.12-brief.md (feature ids and signs), before any return was read.
        "preregistration": {
            "ret_12_1": 1, "ret_6_1": 1, "ret_9_1": 1, "ret_12_7": 1, "ret_36_13": -1, "ret_60_13": -1,
            "chmom": -1, "frog_in_pan": -1, "seas_1_1an": 1, "seas_2_5an": 1, "ret_1_0": -1,
            "rvol_21d": -1, "rvol_252d": -1, "rmax5_21d": -1, "rmax1_21d": -1, "rskew_252d": -1,
            "beta_ew_252d": -1, "ivol_ew_252d": -1, "ivol_ew_21d": -1, "beta_dimson_252d": -1,
            "beta_down_252d": -1, "coskew_252d": -1, "beta_bab_1260d": -1, "zero_trade_21d": 1,
            "zero_trade_252d": 1, "turnover_126d": -1, "turnover_252d": -1, "std_turn_126d": -1,
            "std_dvol_126d": -1, "ami_126d": 1, "ami_252d": 1, "bidask_cs_21d": 1, "bidask_ar_21d": 1,
            "prc_log": -1, "prc_highprc_252d": 1, "me_line_log": -1, "dolvol_126d": -1, "price_delay_52w": 1,
        },
        # BasisInputs.meta (in the evaluation cache key; identical in every job of a run).
        "meta": {
            "identity_basis": "vendor_security_id_line_no_owner_link",
            "universe_basis": "pit_vendor_earnings_evidence_name_pattern_exclusion",
            "classification_basis": "vendor_earnings_evidence_by_formation_and_directory_noncommon_exclusion",
            "availability_basis": "xnys_formation_session_22utc",
            "market_cap_basis": "me_line_vendor_shares_lag90_unverified",
            "size_breakpoints_basis": "french_nyse_me_p20_p50_p80_supplied",
            "venue_basis": "none_no_point_in_time_venue",
        },
    },
}


# ---------------------------------------------------------------------------
# Small helpers (standard library only: the orchestrator runs at a 0.2 GiB cap)
# ---------------------------------------------------------------------------

def _now() -> str:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None, microsecond=0).isoformat()


def _write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{time.monotonic_ns()}.tmp")
    tmp.write_text(json.dumps(payload, indent=1, sort_keys=True, default=str) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_evidence(path: Path, payload: Any) -> None:
    """Keep the exact previous receipt/summary bytes before publishing the latest attempt."""
    if path.is_file():
        previous = path.read_bytes()
        digest = hashlib.sha256(previous).hexdigest()
        history = path.parent / "history" / f"{path.stem}-{digest}.json"
        history.parent.mkdir(parents=True, exist_ok=True)
        if history.exists():
            if history.read_bytes() != previous:
                raise ValueError(f"evidence history differs from its digest: {history}")
        else:
            tmp = history.with_name(f".{history.name}.{os.getpid()}.{time.monotonic_ns()}.tmp")
            tmp.write_bytes(previous)
            os.replace(tmp, history)
    _write_json(path, payload)


def _canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False, default=str)


def _sha_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _code_digest() -> str:
    return hashlib.sha256(Path(__file__).read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def _month(text: str) -> dt.date:
    """``YYYY-MM`` -> that month's calendar month end."""
    year, month = (int(x) for x in text.strip().split("-", 1))
    return dt.date(year + month // 12, month % 12 + 1, 1) - dt.timedelta(days=1)


def _formations(text: str) -> tuple[dt.date, dt.date]:
    """``2013-01..2023-12`` -> (first eom, last eom); formations on/after the holdout start are refused."""
    if ".." not in text:
        raise SystemExit("--formations must look like 2013-01..2023-12")
    first, last = (_month(part) for part in text.split("..", 1))
    if first > last:
        raise SystemExit("--formations: the first month is after the last")
    if last >= HOLDOUT_START:
        raise SystemExit(f"--formations reaches {last}: formations from {HOLDOUT_START} are the sealed holdout")
    return first, last


def _work(args: argparse.Namespace) -> Path:
    return Path(args.out) / "work" / "wave_eval" / args.wave / args.run


class _Trace:
    """Private commit (the job cap's measure) at named steps; ``peak_gb`` is the process peak so far."""

    def __init__(self) -> None:
        self.marks: list[dict[str, Any]] = []
        self.started = time.perf_counter()

    @staticmethod
    def sample() -> dict[str, float]:
        if os.name != "nt":
            return {}

        class _Counters(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_ulong), ("page_faults", ctypes.c_ulong)] + [
                (name, ctypes.c_size_t) for name in (
                    "peak_working_set", "working_set", "quota_peak_paged", "quota_paged", "quota_peak_nonpaged",
                    "quota_nonpaged", "pagefile", "peak_pagefile", "private")]

        kernel = ctypes.WinDLL("kernel32")
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.K32GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Counters), ctypes.c_ulong]
        counters = _Counters()
        counters.cb = ctypes.sizeof(counters)
        if not kernel.K32GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
            return {}
        return {"private_gb": round(counters.private / GIB, 4), "peak_gb": round(counters.peak_pagefile / GIB, 4),
                "working_set_gb": round(counters.working_set / GIB, 4)}

    def mark(self, step: str, **extra: Any) -> None:
        self.marks.append({"step": step, "seconds": round(time.perf_counter() - self.started, 2),
                           **self.sample(), **extra})
        print(f"[mem] {self.marks[-1]}", flush=True)


# ---------------------------------------------------------------------------
# Wave definition (catalog rows, cells, first formations)
# ---------------------------------------------------------------------------

def _wave_entries(wave: str) -> list[Any]:
    from atx_db.research.catalog import load_anomaly_catalog

    return sorted((e for e in load_anomaly_catalog() if e.wave == wave), key=lambda e: e.feature_id)


def _served_variants(anomaly_class: str) -> tuple[str, ...]:
    """The R3b variants this run evaluates for a row: its expected variants that the feature store serves
    (``rank_normal``, ``signed_raw``, ``zscore``). ``winsor`` and the neutralized variants are not evaluated here
    (no read-time neutralization): ruling C-82 (1) registers exactly the evaluated cells, and any other variant is
    a later wave of its own."""
    from atx_db.research import evaluation as ev
    from atx_db.research import feature_store as fs

    return tuple(v for v in ev.expected_variants(anomaly_class) if v in fs.STORE_VARIANTS)


def _planned_cells(entries: Sequence[Any]) -> list[tuple[str, str, int]]:
    """Exactly the cells this run evaluates (ruling C-82): served variants x the policy horizons."""
    return sorted((e.feature_id, v, h) for e in entries for v in _served_variants(e.anomaly_class) for h in HORIZONS)


def _engine_catalog(entries: Sequence[Any]) -> list[Any]:
    """The R3b expected family of the registered rows, narrowed to the evaluated variants (so the family holds
    exactly the registered cells: no ``not_produced`` cell outside the registration)."""
    from dataclasses import replace

    from atx_db.research import evaluation as ev

    return [replace(item, variants=_served_variants(item.anomaly_class)) for item in ev.catalog_features(entries)]


def _first_formations(entries: Sequence[Any], data_start: dt.date) -> dict[str, str]:
    """Each row's first history-eligible formation: the first month-end formation session with at least the
    row's catalog ``min_history_sessions`` XNYS sessions from the data start up to it (inclusive). Calendar
    facts and the pre-registered catalog field only (no feature value, no return)."""
    from atx_db import calendar as xnys

    sessions = xnys.xnys_sessions(data_start, dt.date(2023, 12, 31))
    index = {day: i for i, day in enumerate(sessions)}
    out: dict[str, str] = {}
    for entry in entries:
        need = max(int(entry.min_history_sessions or 1), 1)
        year, month = data_start.year, data_start.month
        while True:
            session = xnys.expected_month_end_session(year, month)
            if session in index and index[session] + 1 >= need:
                out[entry.feature_id] = session.isoformat()
                break
            year, month = (year + 1, 1) if month == 12 else (year, month + 1)
            if dt.date(year, month, 1) >= HOLDOUT_START:
                raise SystemExit(f"{entry.feature_id}: no history-eligible formation before the holdout")
    return out


def _registration_plan(wave: str, pins: Mapping[str, str], root: Path) -> dict[str, Any]:
    """What the wave registers (ruling C-82): the pre-registered rows minus the unsupported ones, exactly the cells
    this run evaluates, and first formations from each row's catalog ``min_history_sessions``."""
    from atx_db.research import trial_registry as tr
    from atx_db.research.catalog import anomaly_catalog_sha256
    from atx_db.research.qualification import load_policy_v4

    config = WAVES[wave]
    policy = load_policy_v4()
    if policy.sha256 != POLICY_SHA:
        raise SystemExit(f"policy v4 sha {policy.sha256} is not the frozen {POLICY_SHA}")
    digest = anomaly_catalog_sha256()
    if digest != pins["catalog_digest"]:
        raise SystemExit(f"the working-tree catalog's digest is {digest}, not the pinned --catalog-digest "
                         f"{pins['catalog_digest']}: stop and report (the catalog changed)")
    entries = _wave_entries(wave)
    ids = [e.feature_id for e in entries]
    prereg = dict(config["preregistration"])
    manifest = _manifest_ids(wave, pins["manifest_sha"], root)
    if set(ids) != set(prereg):
        raise SystemExit(f"{wave}: catalog rows {len(ids)} and pre-registration {len(prereg)} differ: "
                         f"{sorted(set(ids) ^ set(prereg))}")
    resigned = sorted(e.feature_id for e in entries if int(e.expected_sign) != int(prereg[e.feature_id]))
    if resigned:
        raise SystemExit(f"{wave}: catalog signs differ from the pre-registration table for {resigned}")
    unsupported = dict(config.get("unsupported") or {})
    if set(unsupported) - set(ids):
        raise SystemExit(f"unsupported rows outside the wave: {sorted(set(unsupported) - set(ids))}")
    registered = [e for e in entries if e.feature_id not in unsupported]
    ids = [e.feature_id for e in registered]
    if set(ids) - manifest:
        raise SystemExit(f"registered rows missing from the pinned manifest: {sorted(set(ids) - manifest)}")
    cells = _planned_cells(registered)
    first = _first_formations(registered, config["data_start"])
    reported = sorted(set(policy.reported_only) & set(ids))
    return {"wave": wave, "catalog_digest": digest, "manifest_sha": pins["manifest_sha"], "policy_id": policy.version,
            "policy_sha": policy.sha256, "feature_ids": ids, "cells": cells, "n_cells": len(cells),
            "variants": sorted({v for _, v, _ in cells}), "first_formations": first,
            "first_formations_rule": "catalog min_history_sessions XNYS sessions from the data start",
            "unsupported": unsupported, "reported_only_by_policy": reported,
            # The table register_wave checks the rows against: the pre-registration table (task-1.12-brief.md)
            # without the unsupported rows (ruling C-82 (3)); the full table's signs were checked above.
            "preregistration": {f: prereg[f] for f in ids}, "signs_match_preregistration": True,
            "row_sha256": {e.feature_id: tr.row_digest(e) for e in registered},
            "evidence_classes": sorted({str(e.evidence_class) for e in registered}),
            "populations": sorted({str(e.population) for e in registered})}


def _manifest_ids(wave: str, manifest_sha: str, root: Path) -> set[str]:
    path = Path(root) / "manifests" / wave / f"{manifest_sha}.json"
    if not path.is_file():
        raise SystemExit(f"pinned manifest {wave}/{manifest_sha} not found under {root}")
    return {entry["feature_id"] for entry in _read_json(path)["entries"]}


def _pins(args: argparse.Namespace, names: Sequence[str]) -> dict[str, str]:
    """The pinned inputs named on the command line (never defaults, never "latest")."""
    pins: dict[str, str] = {}
    for name in names:
        value = getattr(args, name, None)
        pattern = _SNAPSHOT if name.endswith("snapshot") else _SHA
        if not isinstance(value, str) or not pattern.fullmatch(value):
            raise SystemExit(f"--{name.replace('_', '-')} is required: pin it explicitly "
                             f"({'a snapshot id' if name.endswith('snapshot') else 'a sha256 hex digest'})")
        pins[name] = value
    return pins


REGISTER_PINS = ("manifest_sha", "catalog_digest")
RUN_PINS = ("manifest_sha", "catalog_digest", "lake_snapshot", "bench_snapshot", "label_sha")


def _check_pins(args: argparse.Namespace, prep: Mapping[str, Any]) -> None:
    """A worker's pinned arguments must be the prepared run's (a run never mixes inputs)."""
    from atx_db.research import evaluation as ev
    from atx_db.research import feature_store as fs

    differ = [name for name in RUN_PINS if getattr(args, name, None) not in (None, prep["inputs"].get(name))]
    if differ:
        raise SystemExit(f"pinned {differ} differ from the prepared run's inputs: use the run's pins or a new --run")
    if prep["inputs"].get("accounting_code_digest") != _code_digest():
        raise SystemExit("runner code differs from the frozen accounting adapter: use the prepared code or a new run")
    expected = {"runner": _code_digest(), "evaluation": ev._code_sha(), "adapter": fs.adapter_code_digest()}
    if prep.get("code") != expected:
        raise SystemExit("prepared runner/evaluation/feature adapter source pins changed: restore the frozen source")


def _require_committed_catalog(digest: str) -> None:
    """A real registration or run binds the catalog as committed in HEAD: the pinned digest must also be the digest
    of the HEAD blob, so an uncommitted working-tree edit of the seed can never be registered or graded."""
    import hashlib
    import subprocess

    from atx_db.research.catalog import ANOMALY_CATALOG_PATH

    path = Path(ANOMALY_CATALOG_PATH).resolve()
    done = subprocess.run(["git", "show", f"HEAD:./{path.name}"], cwd=str(path.parent), capture_output=True,
                          timeout=60, check=False)
    if done.returncode != 0:
        raise SystemExit(f"cannot read the committed catalog seed from HEAD: {done.stderr.decode(errors='replace')}")
    committed = hashlib.sha256(done.stdout.replace(b"\r\n", b"\n").replace(b"\r", b"\n")).hexdigest()
    if committed != digest:
        raise SystemExit(f"the catalog seed committed in HEAD has digest {committed}, not the pinned {digest}: "
                         "commit the catalog (its owner) before a real registration or run")


def _register(args: argparse.Namespace) -> int:
    """Print the planned registration; with ``--confirm`` write it to the production registry."""
    from atx_db.research import trial_registry as tr

    plan = _registration_plan(args.wave, _pins(args, REGISTER_PINS), Path(args.root))
    if args.confirm:
        _require_committed_catalog(plan["catalog_digest"])
    shown = {k: v for k, v in plan.items() if k != "cells"}
    shown["cells_by_variant"] = {v: sum(1 for _, var, _ in plan["cells"] if var == v)
                                 for v in sorted({var for _, var, _ in plan["cells"]})}
    print(json.dumps(shown, indent=1, sort_keys=True))
    if not args.confirm:
        print("PLAN ONLY (no registry write). Re-run with --confirm to register.")
        return 0
    entries = [e for e in _wave_entries(args.wave) if e.feature_id in set(plan["feature_ids"])]
    registration_id = tr.register_wave(args.wave, plan["catalog_digest"], plan["policy_sha"], plan["feature_ids"],
                                       plan["cells"], rows=entries, preregistration=plan["preregistration"],
                                       first_formations=plan["first_formations"])
    registry = tr.TrialRegistry()
    registration = registry.require_registration(args.wave, catalog_digest=plan["catalog_digest"],
                                                 policy_sha=plan["policy_sha"])
    result = {"registration_id": registration_id, "registered_at": registration.registered_at,
              "sequence": registration.sequence, "n_cells": registration.n_cells,
              "reported_only": list(registration.reported_only),
              "gating_feature_ids": len(registration.gating_feature_ids),
              "trials_so_far": registry.trials_so_far(), "registry": registry.path.as_posix(),
              "anchor": registry.anchor_path.as_posix(),
              "next": f"git diff -- {ANCHOR.as_posix()} ; then commit it (grading reads the anchor from HEAD)"}
    print("REGISTERED " + json.dumps(result, sort_keys=True))
    return 0


# ---------------------------------------------------------------------------
# Inputs: calendar, context, labels
# ---------------------------------------------------------------------------

def _calendar(first: dt.date, last: dt.date) -> Any:
    """The R3b calendar of the formations (every month formed): XNYS formation session, entry, cutoff."""
    import pandas as pd

    from atx_db import calendar as xnys
    from atx_db.research import spine

    rows = []
    for index, eom in enumerate(spine.formation_months(first, last)):
        formation = xnys.expected_month_end_session(eom.year, eom.month)
        rows.append({"month_index": index, "month_start": pd.Timestamp(eom.replace(day=1)), "eom": eom,
                     "formation_date": pd.Timestamp(formation), "entry_date": pd.Timestamp(xnys.next_session(eom)),
                     "cutoff": pd.Timestamp(xnys.decision_cutoff_utc(formation).replace(tzinfo=None)),
                     "status": "formed"})
    return pd.DataFrame(rows)


def _spine_columns(root: Path, snapshot: str) -> list[str]:
    from atx_db.research import research_lake as lake

    files = lake.lake_files(snapshot, "spine_monthly", root=root)
    con = lake.connect_bounded(None, root=root, memory_limit="64MB", threads=1)
    try:
        return [row[0] for row in con.execute(f"DESCRIBE SELECT * FROM read_parquet({lake.sql_text(files[0])})")
                .fetchall()]
    finally:
        con.close()


def _context(root: Path, inputs: Mapping[str, Any], calendar: Any,
             trace: _Trace) -> tuple[Any, dict[str, int], Any, dict[str, Any]]:
    """Context rows (the spine lines of each formation), the line codes and the calendar with its supplied NYSE
    ME breakpoints (French ``me_p20`` / ``me_p50`` in dollars, the units of ``me_line``) and eligible members.

    Every line has no owner link (the vendor file carries no issuer identity): each is an ``unlinked_member``
    ranked as its own name (R2b ``owner_basis='unlinked_line'``), never a valid primary cohort row. A row flagged
    ``vendor_artifact_suspect`` (C-79, when the snapshot carries the column) or outside ``--universe-flag`` (when
    named) stays a context row but is not a member: withheld from ranking and from the coverage denominator, its
    values dropped and counted by the engine. The spine's own returns (``ret_1m``) are never read.
    """
    import numpy as np
    import pandas as pd
    import pyarrow as pa

    from atx_db.research import research_lake as lake

    eoms = list(calendar["eom"])
    first, last = min(eoms), max(eoms)
    years = list(range(first.year, last.year + 1))
    spine_files = lake.lake_files(inputs["lake_snapshot"], "spine_monthly", years=years, root=root)
    bench_files = lake.lake_files(inputs["bench_snapshot"], "bench_french_me_breakpoints", years=years, root=root)
    withheld = ["false"]
    if inputs.get("artifact_flag"):
        withheld.append(f"coalesce({ARTIFACT_FLAG}, false)")
    if inputs.get("universe_flag"):
        withheld.append(f"NOT coalesce({inputs['universe_flag']}, false)")
    withheld_sql = " OR ".join(withheld)
    con = lake.connect_bounded(None, root=root, memory_limit="128MB", threads=1)
    try:
        spine_list = "[" + ", ".join(lake.sql_text(p) for p in spine_files) + "]"
        table = con.execute(f"""
            SELECT eom, line_id, price, me_line, ({withheld_sql}) AS withheld FROM read_parquet({spine_list})
            WHERE eom BETWEEN ? AND ? ORDER BY eom, line_id
        """, [first, last]).to_arrow_table()
        bench_list = "[" + ", ".join(lake.sql_text(p) for p in bench_files) + "]"
        breaks = con.execute(f"""
            SELECT month_end, me_p20_musd, me_p50_musd, me_p80_musd FROM read_parquet({bench_list})
            WHERE month_end BETWEEN ? AND ?
        """, [first, last]).fetchall()
    finally:
        con.close()
    trace.mark("context_read", rows=table.num_rows)
    eom = np.asarray(table.column("eom").cast(pa.int32()).to_numpy(zero_copy_only=False), dtype=np.int64)
    calendar_days = np.asarray([(e - dt.date(1970, 1, 1)).days for e in eoms], dtype=np.int64)
    month_index = np.searchsorted(calendar_days, eom)
    if not np.array_equal(calendar_days[month_index], eom):
        raise SystemExit("spine rows at a month end outside the calendar")
    lines = pd.Categorical(table.column("line_id").to_pylist())
    securities = {str(line): int(code) for code, line in enumerate(lines.categories)}
    price = table.column("price").to_numpy(zero_copy_only=False).astype(float)
    cap = table.column("me_line").to_numpy(zero_copy_only=False).astype(float)
    cap[~(np.isfinite(cap) & (cap > 0))] = np.nan
    withheld = np.asarray(table.column("withheld").to_numpy(zero_copy_only=False), dtype=bool)
    n = table.num_rows
    del table
    context = pd.DataFrame({"month_index": month_index.astype(np.int32), "security": lines.codes.astype(np.int32),
                            "market_cap": cap, "price": price,
                            "valid_member": np.zeros(n, dtype=bool), "primary_line": np.zeros(n, dtype=bool),
                            "unlinked_member": ~withheld, "venue_pit": np.zeros(n, dtype=bool),
                            "is_nyse_pit": np.zeros(n, dtype=bool)})
    by_year = np.bincount(np.asarray([eoms[i].year for i in month_index[withheld]], dtype=np.int64) - years[0],
                          minlength=len(years)) if withheld.any() else np.zeros(len(years), np.int64)
    stats = {"rows": n, "lines": len(securities), "withheld_rows": int(withheld.sum()),
             "withheld_flags": [f for f in (ARTIFACT_FLAG if inputs.get("artifact_flag") else None,
                                            inputs.get("universe_flag")) if f],
             "withheld_by_year": {str(y): int(c) for y, c in zip(years, by_year, strict=True) if c},
             "market_cap_rows": int(np.isfinite(cap).sum()), "price_rows": int(np.isfinite(price).sum())}
    by_eom = {row[0]: (float(row[1]) * 1e6, float(row[2]) * 1e6,
                       float(row[3]) * 1e6 if row[3] is not None else np.nan) for row in breaks
              if row[1] is not None and row[2] is not None}
    missing = [e.isoformat() for e in eoms if e not in by_eom]
    if missing:
        raise SystemExit(f"no French NYSE ME breakpoints for {missing[:6]}")
    calendar = calendar.copy()
    calendar["nyse_me_p20"] = [by_eom[e][0] for e in eoms]
    calendar["nyse_me_p50"] = [by_eom[e][1] for e in eoms]
    calendar["nyse_me_p80"] = [by_eom[e][2] for e in eoms]
    calendar["eligible_members"] = np.bincount(month_index[~withheld], minlength=len(eoms)).astype(np.int64)
    trace.mark("context", rows=n, lines=len(securities), withheld=int(withheld.sum()))
    return context, securities, calendar, stats


def _labels(labels_root: Path, label_sha: str, calendar: Any, securities: Mapping[str, int],
            trace: _Trace) -> tuple[Any, Any, dict[str, Any]]:
    """The R3b label and maturity frames through ``LabelMatrix.r3b_inputs``, one horizon per read (bounds the
    DuckDB result and the frame), compact dtypes; then the grid check (1.9 r2 n3)."""
    import numpy as np
    import pandas as pd

    from atx_db.research import label_matrix as lm

    frames, maturities, infos = [], [], []
    with lm.LabelMatrix(labels_root, memory_limit="128MB") as matrix:
        for h in HORIZONS:
            labels, maturity, info = matrix.r3b_inputs(label_sha, [h], calendar=calendar, securities=securities,
                                                       label_cutoff=LABEL_CUTOFF)
            frames.append(pd.DataFrame({
                "month_index": labels["month_index"].to_numpy(np.int32),
                "security": labels["security"].to_numpy(np.int32),
                "horizon_months": labels["horizon_months"].to_numpy(np.int8),
                "forward_return": labels["forward_return"].to_numpy(np.float64),
                "status": labels["status"].to_numpy(np.int8), "terminal": labels["terminal"].to_numpy(np.int8),
                "anchor_date": pd.to_datetime(labels["anchor_date"]).to_numpy("datetime64[s]")}))
            del labels
            maturities.append(maturity)
            infos.append(info)
            trace.mark(f"labels_h{h}", rows=len(frames[-1]))
    labels = pd.concat(frames, ignore_index=True)
    del frames
    maturity = pd.concat(maturities, ignore_index=True)
    reads = {json.dumps({k: i.get(k) for k in ("label_sha", "provisional", "holdout_start", "eom_before",
                                                "holdout_opening")}, sort_keys=True, default=str) for i in infos}
    if len(reads) != 1:
        raise SystemExit(f"the per-horizon label reads differ: {reads}")
    info = dict(infos[0])
    info["files"] = {name: sha for i in infos for name, sha in i["files"].items()}
    info["reasons"] = [row for i in infos for row in i["reasons"]]
    info["label_rows"] = int(sum(i["label_rows"] for i in infos))
    info["windows_not_aligned"] = int(sum(i["windows_not_aligned"] for i in infos))
    info["windows_aligned"] = int(sum(i["windows_aligned"] for i in infos))
    if info["windows_not_aligned"]:
        raise SystemExit(f"{info['windows_not_aligned']} label windows do not align with the formation calendar")
    # 1.9 r2 n3: every formed month's window at each horizon is either matured (and has label rows) or ends in
    # the sealed holdout; a matured month without labels is a hole in the label set, never a silent gap.
    ends = pd.to_datetime(maturity["expected_end"])
    grid: dict[str, Any] = {}
    for h in HORIZONS:
        part = maturity[maturity["horizon_months"] == h]
        part_ends = ends[maturity["horizon_months"] == h]
        matured = set(part.loc[part["matured"].astype(bool), "month_index"].astype(int))
        sealed = set(part.loc[~part["matured"].astype(bool) & (part_ends >= pd.Timestamp(HOLDOUT_START)),
                              "month_index"].astype(int))
        other = set(part["month_index"].astype(int)) - matured - sealed
        with_labels = set(labels.loc[labels["horizon_months"] == h, "month_index"].astype(int))
        holes = sorted(matured - with_labels)
        if other or holes or len(part) != len(calendar):
            raise SystemExit(f"label grid h={h}: {len(part)} windows for {len(calendar)} formations, "
                             f"unmatured outside the holdout {sorted(other)[:6]}, matured without labels {holes[:6]}")
        grid[str(h)] = {"formations": len(part), "matured": len(matured), "sealed_holdout_windows": len(sealed),
                        "label_rows": int((labels["horizon_months"] == h).sum())}
    info["grid_check"] = grid
    trace.mark("labels", rows=len(labels))
    return labels, maturity, info


def _rebalance_inputs(labels: Any, maturity: Any, calendar: Any, securities: Mapping[str, int],
                      info: Mapping[str, Any], labels_root: Path, trace: _Trace) -> tuple[Any, str, dict[str, Any]]:
    """Exact ex-post accounting intervals from already-authorized labels; never decision inputs.

    Reuse the compact return/status arrays already read by the sealed LabelMatrix adapter. Independently
    project only keys and actual entry/exit dates from those verified label files, one horizon at a time.
    A previous formation j may supply accounting at m=j+h only if its actual endpoints equal entry[j]
    and entry[m], its window is matured, and both formed entries precede the holdout. Unmatched intervals
    are absent (the evaluator keeps drift/cost/net NULL); missing held names are never renormalized away.
    """
    import numpy as np
    import pandas as pd

    from atx_db.research import research_lake as lake

    if info.get("holdout_start") != HOLDOUT_START.isoformat() or info.get("allow_holdout") \
            or info.get("holdout_opening") or info.get("holdout_wave") \
            or dt.date.fromisoformat(str(info["eom_before"])) > HOLDOUT_START \
            or dt.datetime.fromisoformat(str(info["label_cutoff"])) > LABEL_CUTOFF:
        raise SystemExit("rebalance adapter requires the closed selection label read")
    label_sha = str(info["label_sha"])
    pin = _sha_text(_canonical({"label_sha": label_sha, "files": info["files"],
                               "windows_sha256": info["windows_sha256"], "method": REBALANCE_METHOD,
                               "code_digest": _code_digest(), "cutoff": LABEL_CUTOFF,
                               "holdout_start": HOLDOUT_START}))
    months = len(calendar)
    entry = pd.to_datetime(calendar["entry_date"]).to_numpy("datetime64[D]")
    entry_day = entry.astype(np.int64)
    formed = calendar["status"].to_numpy() == "formed"
    old_month = labels["month_index"].to_numpy(np.int32, copy=False)
    security = labels["security"].to_numpy(np.int32, copy=False)
    horizon = labels["horizon_months"].to_numpy(np.int8, copy=False)
    anchor_day = pd.to_datetime(labels["anchor_date"]).to_numpy("datetime64[D]").astype(np.int64)
    # One preallocated compact result, filled by horizon. No second four-horizon label frame or
    # concat of full-size accounting frames is kept. DataFrame adopts these arrays without copying.
    n = len(labels)
    result = {"month_index": np.empty(n, np.int32), "security": np.empty(n, np.int32),
              "horizon_months": np.empty(n, np.int8), "previous_entry_date": np.empty(n, "datetime64[s]"),
              "entry_date": np.empty(n, "datetime64[s]"), "realized_return": np.empty(n, np.float64),
              "status": np.empty(n, np.int8)}
    stats: dict[str, Any] = {"method": REBALANCE_METHOD, "source_sha256": pin, "label_sha": label_sha,
                             "window_basis": "actual_label_old_entry_to_current_entry", "horizons": {}}
    con = lake.connect_bounded(None, root=labels_root, memory_limit="128MB", threads=1)
    cursor = 0
    try:
        con.register("_reb_months", pd.DataFrame({"month_index": np.arange(months, dtype=np.int32),
                                                 "eom": pd.to_datetime(calendar["eom"])}))
        con.register("_reb_codes", pd.DataFrame({"line_id": list(securities),
                                                "security": np.fromiter(securities.values(), np.int32)}))
        for h in HORIZONS:
            idx = np.flatnonzero(horizon == h)
            old = old_month[idx]
            current = old + h
            in_calendar = current < months
            dest = np.minimum(current, months - 1)
            eligible = in_calendar & formed[old] & formed[dest] & (entry[dest] < np.datetime64(HOLDOUT_START))
            mat = maturity[maturity["horizon_months"] == h]
            mat_month = mat["month_index"].to_numpy(np.int64)
            matured = np.zeros(months, bool)
            end_day = np.full(months, np.iinfo(np.int64).min, np.int64)
            matured[mat_month] = mat["matured"].to_numpy(bool)
            end_day[mat_month] = pd.to_datetime(mat["expected_end"]).to_numpy("datetime64[D]").astype(np.int64)
            eligible &= matured[old] & (end_day[old] == entry_day[dest]) & (entry_day[old] < entry_day[dest])
            files = [str(labels_root / "labels" / label_sha / name) for name in sorted(info["files"])
                     if name.startswith(f"h={h}/")]
            if not files:
                raise SystemExit(f"rebalance adapter: verified files missing for h={h}")
            for name in sorted(info["files"]):
                if name.startswith(f"h={h}/") and lake.sha256_file(labels_root / "labels" / label_sha / name) \
                        != info["files"][name]:
                    raise SystemExit("rebalance source changed since the authorized label read")
            literal = "[" + ", ".join(lake.sql_text(p) for p in files) + "]"
            con.register("_reb_keys", pd.DataFrame({"month_index": old, "security": security[idx]}))
            endpoints = con.execute(f"""
                SELECT k.month_index, k.security,
                       coalesce(date_diff('day', DATE '1970-01-01', r.entry_date), -2147483648) AS entry_day,
                       coalesce(date_diff('day', DATE '1970-01-01', r.exit_date), -2147483648) AS exit_day
                FROM _reb_keys k JOIN _reb_months m USING(month_index)
                JOIN _reb_codes c USING(security)
                LEFT JOIN read_parquet({literal}) r ON r.eom=m.eom AND r.line_id=c.line_id
                ORDER BY k.month_index, k.security
            """).fetchnumpy()
            con.unregister("_reb_keys")
            if not np.array_equal(endpoints["month_index"], old) \
                    or not np.array_equal(endpoints["security"], security[idx]):
                raise SystemExit("rebalance endpoint keys differ from the verified label read")
            matched = eligible & (endpoints["entry_day"] == entry_day[old]) \
                & (anchor_day[idx] == entry_day[old]) & (endpoints["exit_day"] == entry_day[dest])
            selected = idx[matched]
            count = len(selected)
            target = slice(cursor, cursor + count)
            result["month_index"][target] = current[matched]
            result["security"][target] = security[selected]
            result["horizon_months"][target] = h
            result["previous_entry_date"][target] = entry[old[matched]]
            result["entry_date"][target] = entry[current[matched]]
            result["realized_return"][target] = labels["forward_return"].to_numpy(copy=False)[selected]
            result["status"][target] = labels["status"].to_numpy(copy=False)[selected]
            stats["horizons"][str(h)] = {"source_rows": len(idx), "eligible_intervals": int(eligible.sum()),
                                         "matched_rows": count,
                                         "endpoint_mismatch_rows": int((eligible & ~matched).sum()),
                                         "outside_calendar_rows": int((~in_calendar).sum())}
            cursor += count
            del endpoints
    finally:
        con.close()
    frame = pd.DataFrame({key: values[:cursor] for key, values in result.items()}, copy=False) if cursor else None
    stats["rows"] = cursor
    trace.mark("rebalance_accounting", rows=cursor)
    return frame, pin, stats


def _require_registration(wave: str, prep: Mapping[str, Any]) -> Any:
    """R-6 in code: the production registry is anchored in git HEAD and holds this wave's registration under the
    run's catalog digest and policy, and the run's spec is bound to it (checked before any label read)."""
    from atx_db.research import trial_registry as tr

    registry = tr.TrialRegistry()
    anchor = registry.verify_anchor()
    if not anchor.committed:
        raise SystemExit("the trial-registry anchor is not committed: commit it before any label join")
    registration = registry.require_registration(wave, catalog_digest=prep["catalog_digest"],
                                                 policy_sha=prep["policy_sha"])
    payload = prep["spec"]["payload"]
    if payload.get("wave_registration_id") != registration.registration_id:
        raise SystemExit("the run's spec is not bound to the wave registration")
    if dt.datetime.fromisoformat(payload["created_at"]) < dt.datetime.fromisoformat(registration.registered_at):
        raise SystemExit("the run's spec was built before the wave registration")
    if registry.holdout_opened(wave):
        raise SystemExit(f"wave {wave}: the holdout is opened; a provisional run keeps it sealed")
    return registration


def _stand_in_registration(plan: Mapping[str, Any]) -> Any:
    """Dry run only: a WaveRegistration built in memory (never written) so the spec and the evidence builder can
    run on stub labels. Its id is the sha256 of a fixed dry-run string: no registry holds it."""
    from atx_db.research import trial_registry as tr

    reported = tuple(sorted(plan["reported_only_by_policy"]))
    return tr.WaveRegistration(
        wave=plan["wave"], sequence=-1, registration_id=_sha_text("dry-run stand-in registration: never written"),
        catalog_digest=plan["catalog_digest"], policy_id=plan["policy_id"], policy_sha=plan["policy_sha"],
        feature_ids=tuple(plan["feature_ids"]), reported_only=reported,
        gating_feature_ids=tuple(f for f in plan["feature_ids"] if f not in reported),
        cells=tuple(tuple(c) for c in plan["cells"]), first_formations=dict(plan["first_formations"]),
        registered_at=_now(), rows={}, row_digest_fields=(), preregistration_sha256=None,
        registry_version="dry-run-stand-in")


# ---------------------------------------------------------------------------
# Worker stages
# ---------------------------------------------------------------------------

def _stage_stub_labels(args: argparse.Namespace) -> None:
    """Dry run: a provisional label set of seeded random returns for every spine row, under ``--out``."""
    import numpy as np
    import pyarrow as pa

    from atx_db.research import label_matrix as lm
    from atx_db.research import research_lake as lake
    from atx_db.research import spine

    snapshot = _pins(args, ("lake_snapshot",))["lake_snapshot"]
    out = Path(args.out)
    if out.resolve() == ROOT.resolve():
        raise SystemExit("stub labels go to a scratch --out, never the research root")
    trace = _Trace()
    spec = {"kind": "dry_run_stub_labels", "provisional": True, "holdout_start": HOLDOUT_START.isoformat(),
            "holdout_basis": "dry run: the policy v4 holdout start", "code_digest": _code_digest(),
            "input_digests": {"spine_monthly": lake.lake_dataset_digest(snapshot, "spine_monthly", root=args.root),
                              "rng": f"numpy.default_rng({STUB_SEED}).normal"},
            "horizons": list(HORIZONS),
            "note": "random returns independent of every feature (a null): never a result"}
    label_sha = lm.compute_label_sha(spec)
    windows = spine.label_windows()
    by_window = {(eom, h): (entry, exit_) for eom, h, _, entry, exit_ in windows}
    years = sorted({w[0].year for w in windows})
    matrix = lm.LabelMatrix(out, memory_limit="128MB")
    matrix.create(label_sha, spec)
    matrix.write_windows(label_sha, pa.table({
        "eom": [w[0] for w in windows], "h": [w[1] for w in windows], "formation_date": [w[2] for w in windows],
        "entry_date": [w[3] for w in windows], "exit_date": [w[4] for w in windows]}).cast(lm.WINDOW_SCHEMA))
    rng = np.random.default_rng(STUB_SEED)
    con = lake.connect_bounded(None, root=out, memory_limit="128MB", threads=1)
    counts: dict[str, int] = {}
    try:
        for year in years:
            files = lake.lake_files(snapshot, "spine_monthly", years=[year], root=args.root)
            rows = con.execute(
                f"SELECT eom, line_id FROM read_parquet([{', '.join(lake.sql_text(p) for p in files)}]) "
                f"WHERE eom >= DATE '{spine.FIRST_EOM}' AND year(eom) = {year} ORDER BY eom, line_id"
            ).to_arrow_table()
            eoms = rows.column("eom").to_pylist()
            n = len(eoms)
            for h in HORIZONS:
                entry = [by_window[(e, h)][0] for e in eoms]
                exit_ = [by_window[(e, h)][1] for e in eoms]
                matured = np.fromiter((x < HOLDOUT_START for x in exit_), dtype=bool, count=n)
                ret = np.where(matured, rng.normal(0.01 * h, 0.12 * np.sqrt(h), n), np.nan)
                table = pa.table({
                    "eom": rows.column("eom"), "line_id": rows.column("line_id"), "owner_id": pa.nulls(n, pa.string()),
                    "entry_date": pa.array(entry, pa.date32()), "exit_date": pa.array(exit_, pa.date32()),
                    "ret": pa.array(ret, pa.float64(), from_pandas=True), "ret_exc": pa.nulls(n, pa.float64()),
                    "basis": pa.array(["dry_run_stub"] * n, pa.string()),
                    "reason": pa.array(np.where(matured, 0, lm.LABEL_REASONS["not_matured"]).astype(np.int8),
                                       pa.int8())})
                matrix.write(label_sha, h, year, [table.cast(lm.LABEL_SCHEMA)], meta={"stage": "dry_run_stub"})
                counts[f"h{h}"] = counts.get(f"h{h}", 0) + n
            trace.mark(f"stub_{year}", rows=n)
    finally:
        con.close()
    matrix.complete(label_sha, HORIZONS, years)
    matrix.close()
    _write_json(_work(args) / "stub_labels.json", {"label_sha": label_sha, "labels_root": out.as_posix(),
                                                   "rows": counts, "memory": trace.marks})


def _stage_prepare(args: argparse.Namespace) -> None:
    """Checks, then the ONE spec of the run (kept in ``prepare.json``; an existing one is reused, never rebuilt)."""
    from atx_db.research import evaluation as ev
    from atx_db.research import feature_store as fs
    from atx_db.research import qualification as rq
    from atx_db.research import research_lake as lake
    from atx_db.research.research_lake import sha256_file

    trace = _Trace()
    config = WAVES[args.wave]
    work = _work(args)
    target = work / "prepare.json"
    first, last = _formations(args.formations)
    stub = bool(args.stub_labels)
    pins = _pins(args, [p for p in RUN_PINS if not (stub and p == "label_sha")])
    if stub:
        stub_info = _read_json(work / "stub_labels.json")
        label_sha, labels_root = stub_info["label_sha"], Path(stub_info["labels_root"])
    else:
        label_sha, labels_root = pins["label_sha"], Path(args.root)
    plan = _registration_plan(args.wave, pins, Path(args.root))
    columns = _spine_columns(Path(args.root), pins["lake_snapshot"])
    if args.universe_flag and args.universe_flag not in columns:
        raise SystemExit(f"--universe-flag {args.universe_flag} is not a spine_monthly column ({columns})")
    if args.require_artifact_flag and ARTIFACT_FLAG not in columns:
        raise SystemExit(f"--require-artifact-flag: spine_monthly of {pins['lake_snapshot']} has no {ARTIFACT_FLAG}")
    inputs = {"wave": args.wave, "formations": [first.isoformat(), last.isoformat()], "stub_labels": stub,
              "label_sha": label_sha, "labels_root": labels_root.as_posix(), "manifest_sha": pins["manifest_sha"],
              "lake_snapshot": pins["lake_snapshot"], "bench_snapshot": pins["bench_snapshot"],
              "catalog_digest": plan["catalog_digest"], "policy_sha": plan["policy_sha"],
              "control_features": list(config["control_features"]), "artifact_flag": ARTIFACT_FLAG in columns,
              "universe_flag": args.universe_flag or None, "accounting_method": REBALANCE_METHOD,
              "accounting_code_digest": _code_digest()}
    if stub:
        registration = _stand_in_registration(plan)
    else:
        from atx_db.research import trial_registry as tr

        _require_committed_catalog(plan["catalog_digest"])
        registry = tr.TrialRegistry()
        anchor = registry.verify_anchor()
        if not anchor.committed:
            raise SystemExit("the trial-registry anchor is not committed")
        registration = registry.require_registration(args.wave, catalog_digest=plan["catalog_digest"],
                                                     policy_sha=plan["policy_sha"])
        if registry.holdout_opened(args.wave):
            raise SystemExit(f"{args.wave}: the holdout is opened; this runner grades the sealed selection sample")
        if sorted(tuple(c) for c in registration.cells) != sorted(tuple(c) for c in plan["cells"]) or \
                dict(registration.first_formations) != plan["first_formations"] or \
                sorted(registration.feature_ids) != sorted(plan["feature_ids"]):
            raise SystemExit("the registration's features, cells or first formations differ from this runner's plan")
    inputs["registration_id"] = registration.registration_id
    if target.is_file():
        existing = _read_json(target)
        if existing["inputs"] != json.loads(_canonical(inputs)):
            raise SystemExit(f"{target} was prepared from other inputs; use a new --run name")
        _check_pins(args, existing)
        print(f"[prepare] reusing the run's spec (created_at {existing['spec']['payload']['created_at']})")
        return
    policy = rq.load_policy_v4()
    lake_check = lake.verify_lake_snapshot(pins["lake_snapshot"], args.root)
    bench_check = lake.verify_lake_snapshot(pins["bench_snapshot"], args.root)
    if not (lake_check.get("ok") and bench_check.get("ok")):
        raise SystemExit(f"lake snapshot verification failed: {lake_check} / {bench_check}")
    entries = fs.read_manifest(Path(args.root), args.wave, pins["manifest_sha"])
    store = fs.FeatureStore(Path(args.root))
    files = {}
    spines = set()
    for entry in entries:
        meta = store.read_meta(entry["basis"], entry["feature_id"], entry["feature_sha"])
        actual = sha256_file(store.path(entry["basis"], entry["feature_id"], entry["feature_sha"]))
        if actual != meta["file_sha256"]:
            raise SystemExit(f"{entry['feature_id']}: file sha256 {actual} != sidecar {meta['file_sha256']}")
        files[entry["feature_id"]] = {"feature_sha": entry["feature_sha"], "file_sha256": actual,
                                      "rows": meta["rows"], "value_rows": meta["value_rows"]}
        spines.add(str(((meta.get("meta") or {}).get("inputs") or {}).get("spine")))
    # One build: every feature file, the label set and the lake snapshot come from the same spine (the snapshot
    # id ends with the spine digest's first 12 hex, node 1.12's naming); a mismatch is refused, never mixed.
    suffix = pins["lake_snapshot"].rsplit("-", 1)[-1]
    if len(spines) != 1 or not next(iter(spines)).startswith(suffix):
        raise SystemExit(f"the features were built on spine digests {sorted(spines)}, not the lake snapshot "
                         f"{pins['lake_snapshot']}'s")
    label_check: dict[str, Any] = {"spine_digest": next(iter(spines))}
    if not stub:
        from atx_db.research import label_matrix as lm

        label_spec = lm.LabelMatrix(labels_root).spec(label_sha)   # the spec only: no label row is read
        complete = (labels_root / "labels" / label_sha / "_complete.json").is_file()
        universe = str((label_spec.get("input_digests") or {}).get("universe"))
        if label_spec.get("provisional") is not True or label_spec.get("holdout_start") != HOLDOUT_START.isoformat() \
                or not complete or universe not in spines:
            raise SystemExit(f"label set {label_sha[:12]}: provisional={label_spec.get('provisional')}, holdout "
                             f"{label_spec.get('holdout_start')}, complete={complete}, universe digest {universe[:12]} "
                             f"(features {sorted(spines)}): not the provisional set of this build")
        label_check.update({"provisional": True, "holdout_start": label_spec["holdout_start"], "complete": True,
                            "universe_digest_matches": True})
    trace.mark("verified")
    kwargs = rq.evaluation_spec_kwargs_v4(policy, registration=registration)
    spec = ev.validate_spec(ev.EvaluationSpec(
        run_id=f"{args.wave}_provisional", verify_panels=False, label_cutoff=LABEL_CUTOFF,
        control_features=tuple(config["control_features"]), control_variant="rank_normal", **kwargs))
    payload = ev.spec_payload(spec)
    problems = rq.v4_spec_problems(payload, policy)
    if problems:
        raise SystemExit(f"the spec cannot feed a v4 grade: {problems}")
    _write_json(target, {
        "inputs": json.loads(_canonical(inputs)), "spec": {"run_id": spec.run_id, "payload": payload},
        "manifest": {"entries": entries, "files": files}, "plan": {k: v for k, v in plan.items() if k != "row_sha256"},
        "registration": {"registration_id": registration.registration_id,
                         "registered_at": registration.registered_at, "stand_in": stub},
        "code": {"runner": _code_digest(), "evaluation": ev._code_sha(), "adapter": fs.adapter_code_digest()},
        "lake_verify": {"spine": {k: lake_check.get(k) for k in ("ok", "files")},
                        "bench": {k: bench_check.get(k) for k in ("ok", "files")}},
        "build_check": label_check, "spine_columns": columns, "prepared_at": _now(), "memory": trace.marks})
    print(f"[prepare] spec created_at {payload['created_at']}, registration {registration.registration_id[:12]}")


def _clear_family(cells: Any) -> Any:
    """Family columns set to typed nulls: they depend on the whole wave (recomputed at grading, never cached)."""
    import numpy as np
    import pandas as pd

    from atx_db.research import evaluation as ev

    kinds = dict(ev.CELL_COLUMNS)
    out = cells.copy()
    for name in ev.FAMILY_COLUMNS:
        kind = kinds[name]
        if kind == "DOUBLE":
            out[name] = np.nan
        elif kind == "BOOLEAN":
            out[name] = pd.array([pd.NA] * len(out), dtype="boolean")
        else:
            out[name] = pd.array([pd.NA] * len(out), dtype="Int64")
    return out


def _cached_basis(cache: Any, key: tuple[str, str, str], parts: Mapping[str, Any],
                  feature_id: str) -> dict[str, Any]:
    """Recover the writer-validated evaluated basis only from an intact, exact current cache key.

    EvalCache.write checks that key_parts.basis equals the evaluator's basis. Its sidecar thus
    preserves the four basis fields needed by grading even when an earlier job receipt is absent.
    Extra evaluator diagnostics are not inferred; previous full receipts remain in history.
    """
    from atx_db.research.eval_cache import EVAL_CACHE_VERSION
    from atx_db.research.research_lake import sha256_file

    side = cache.read_meta(*key)
    if side.get("cache_version") != EVAL_CACHE_VERSION or any(
        side.get(name) != value for name, value in zip(
            ("feature_sha", "label_sha", "eval_spec_sha"), key, strict=True)
    ) or side.get("meta", {}).get("feature_id") != feature_id:
        raise ValueError(f"{feature_id}: cached identity does not match the requested key")
    cached = side.get("key_parts")
    if not isinstance(cached, Mapping) or _sha_text(_canonical(cached)) != key[2]:
        raise ValueError(f"{feature_id}: cached key parts do not hash to the requested key")
    if _canonical(cached) != _canonical(parts):
        raise ValueError(f"{feature_id}: cached basis/spec differs from the current live basis/spec")
    required = {"cells", "slices", "series", "quantiles", "decay", "feature_inputs"}
    files = side.get("files") or {}
    if not required.issubset(files):
        raise ValueError(f"{feature_id}: incomplete cached tables")
    for name, info in files.items():
        path = cache.path(*key, table=name)
        if not path.is_file() or path.stat().st_size != info.get("bytes") or sha256_file(path) != info.get("sha256"):
            raise ValueError(f"{feature_id}: cached {name} bytes differ from their sidecar")
    return dict(cached["basis"])


def _stage_eval(args: argparse.Namespace) -> None:
    """One feature group: build the basis once, key it (``basis_key`` on live frames), evaluate, cache per feature."""
    from atx_db.research import eval_cache as ec
    from atx_db.research import evaluation as ev
    from atx_db.research import feature_store as fs

    trace = _Trace()
    trace.mark("start")
    config = WAVES[args.wave]
    work = _work(args)
    prep = _read_json(work / "prepare.json")
    _check_pins(args, prep)
    stub = bool(prep["inputs"]["stub_labels"])
    if stub != bool(args.stub_labels):
        raise SystemExit("--stub-labels differs from the prepared run")
    spec = ev.spec_from_payload(prep["spec"]["payload"], prep["spec"]["run_id"])
    if _canonical(ev.spec_payload(spec)) != _canonical(prep["spec"]["payload"]):
        raise SystemExit("the spec does not round-trip from prepare.json (one spec per run)")
    features = [f for f in args.features.split(",") if f]
    manifest = {entry["feature_id"]: entry for entry in prep["manifest"]["entries"]}
    registered = set(prep["plan"]["feature_ids"])
    if set(features) - registered:
        raise SystemExit(f"features not registered in this wave (C-82): {sorted(set(features) - registered)}")
    if not stub:
        _require_registration(args.wave, prep["inputs"] | {"spec": prep["spec"]})   # before any label read
    wave_entries = _wave_entries(args.wave)
    catalog = {item.feature_id: item for item in ev.catalog_features(wave_entries)}   # the adapter's sign check
    engine = {item.feature_id: item for item in _engine_catalog([e for e in wave_entries
                                                                   if e.feature_id in registered])}
    first, last = (dt.date.fromisoformat(d) for d in prep["inputs"]["formations"])
    calendar = _calendar(first, last)
    context, securities, calendar, context_stats = _context(Path(args.root), prep["inputs"], calendar, trace)
    label_sha = prep["inputs"]["label_sha"]
    labels, maturity, info = _labels(Path(prep["inputs"]["labels_root"]), label_sha, calendar, securities, trace)
    rebalance, rebalance_pin, rebalance_stats = _rebalance_inputs(
        labels, maturity, calendar, securities, info, Path(prep["inputs"]["labels_root"]), trace)
    store = fs.FeatureStore(Path(args.root), memory_limit="128MB")
    table = fs.load_feature_table_from_store(store, list(manifest.values()), calendar=calendar,
                                             securities=securities, catalog=catalog)
    controls = table.load_controls(spec.control_features, spec.control_variant)
    trace.mark("controls", rows=len(controls))
    meta = {**dict(config["meta"]), "wave": args.wave, "lake_snapshot": prep["inputs"]["lake_snapshot"],
            "manifest_sha": prep["inputs"]["manifest_sha"], "withheld_flags": context_stats["withheld_flags"],
            ev.LABEL_READ_META: ev.label_read_meta(info)}
    if stub:
        meta["labels"] = "dry_run_stub_random_normal_never_a_result"
    digests = {"manifest_sha": prep["inputs"]["manifest_sha"], "lake_snapshot": prep["inputs"]["lake_snapshot"],
               "label_sha": label_sha, "files": {f: prep["manifest"]["files"][f]["file_sha256"] for f in manifest}}
    served = {f: tuple(v for v in table.variants_by_feature[f] if v in engine[f].variants) for f in features}
    from atx_db.research import factor_returns as fr

    # Stub runs never read real benchmark return values. Actual runs reached this point only after
    # the registration check above; all optional benchmark inputs come from the pinned snapshot.
    factors = {} if stub else fr.benchmark_span_factors(
        calendar, prep["inputs"]["bench_snapshot"], root=Path(args.root))
    paper_t = {} if stub else fr.benchmark_original_paper_t(
        prep["inputs"]["bench_snapshot"], root=Path(args.root))
    inputs = ev.BasisInputs(BASIS, ev.BASIS_AVAILABLE, len(securities), calendar, labels, maturity, context, controls,
                            served, table.load_feature, meta, digests, release_frames=True,
                            reported_variants_by_feature=table.variants_by_feature,
                            factors=factors, original_paper_t=paper_t, beta_neutral_registration=None,
                            rebalance_returns=rebalance, rebalance_returns_sha256=rebalance_pin)
    del labels, maturity, context, controls, rebalance   # basis owns frames; evaluator releases them
    basis = ec.basis_key(inputs, spec)
    trace.mark("basis_key")
    payload = ev.spec_payload(ev.validate_spec(spec))
    window = {"label_sha": label_sha, "eom_before": info["eom_before"], "allow_holdout": False,
              "holdout_wave": None}
    parts = ec.eval_key_parts(payload, basis=basis, label_window=window)
    eval_sha = ec.compute_eval_spec_sha(payload, basis=basis, label_window=window)
    cache = ec.EvalCache(Path(args.out))
    todo = [f for f in features if not cache.has(manifest[f]["feature_sha"], label_sha, eval_sha)]
    cached_basis = None
    for feature_id in sorted(set(features) - set(todo)):
        cached_basis = _cached_basis(cache, (manifest[feature_id]["feature_sha"], label_sha, eval_sha),
                                     parts, feature_id)
    receipt: dict[str, Any] = {"features": features, "done_before": sorted(set(features) - set(todo)),
                               "eval_spec_sha": eval_sha, "label_sha": label_sha, "basis_key": basis,
                               "context": context_stats, "rebalance_accounting": rebalance_stats,
                               "label_info": {k: info.get(k) for k in ("label_rows", "reasons", "eom_before",
                                                                       "windows_aligned", "grid_check",
                                                                       "provisional", "holdout_start")}}
    if todo:
        inputs.variants_by_feature = {f: served[f] for f in todo}
        started = time.perf_counter()
        tables = ev.evaluate_bases([inputs], spec, catalog=[engine[f] for f in todo])
        trace.mark("evaluated", features=len(todo), seconds_eval=round(time.perf_counter() - started, 1))
        evaluated = tables.bases[BASIS]
        receipt["bases"] = tables.bases
        receipt["bases_source"] = "evaluated"
        receipt["family_partial"] = tables.family
        for feature_id in todo:
            def part(frame: Any, fid: str = feature_id) -> Any:
                return frame[frame["feature_id"] == fid].reset_index(drop=True)

            extra = {"slices": part(tables.slices), "series": part(tables.series),
                     "quantiles": part(tables.quantiles), "decay": part(tables.decay),
                     "feature_inputs": part(tables.feature_inputs)}
            cache.write(manifest[feature_id]["feature_sha"], label_sha, eval_sha, _clear_family(part(tables.cells)),
                        {"feature_id": feature_id, "wave": args.wave, "run": args.run, "stub_labels": stub,
                         "family_columns": "cleared: recomputed over the whole wave at grading"},
                        extra=extra, key_parts=parts, evaluated=evaluated)
        _write_json(work / "attrition.json", tables.attrition.to_dict("records"))
        del tables
    else:
        if cached_basis is None:
            raise ValueError("no evaluated or validated cached basis")
        receipt["bases"] = {BASIS: cached_basis}
        receipt["bases_source"] = "validated_cache_key_parts"
    store.close()
    cache.close()
    trace.mark("done")
    receipt["memory"] = trace.marks
    receipt["peak_gb"] = max((m.get("peak_gb") or 0.0) for m in trace.marks)
    _write_evidence(work / "jobs" / f"eval-{features[0]}.json", receipt)


def _stage_grade(args: argparse.Namespace, prep: Mapping[str, Any] | None = None) -> None:
    """Every feature's cached cells: whole-wave family statistics, the v4 grade (provisional), ledger, report.

    ``prep`` defaults to the run's ``prepare.json`` (a scratch probe may pass an edited copy of a dry run's).
    """
    import pandas as pd

    from atx_db.research import eval_cache as ec
    from atx_db.research import evaluation as ev
    from atx_db.research import qualification as rq
    from atx_db.research.catalog import load_anomaly_catalog

    trace = _Trace()
    work = _work(args)
    prep = prep if prep is not None else _read_json(work / "prepare.json")
    _check_pins(args, prep)
    stub = bool(prep["inputs"]["stub_labels"])
    receipts = [_read_json(p) for p in sorted((work / "jobs").glob("eval-*.json"))]
    keys = {r["eval_spec_sha"] for r in receipts}
    bases = [r["basis_key"] for r in receipts]
    if len(keys) != 1 or any(b != bases[0] for b in bases):
        raise SystemExit(f"the eval jobs keyed different bases or specs: {sorted(keys)}")
    eval_sha = keys.pop()
    run_bases = next((r["bases"] for r in receipts if r.get("bases")), None)
    if run_bases is None:
        raise SystemExit("no job receipt carries the evaluated basis manifest")
    if {key: run_bases[BASIS].get(key) for key in ec.BASIS_KEY_FIELDS} != bases[0]:
        raise SystemExit("the evaluated basis differs from the key")
    label_sha = prep["inputs"]["label_sha"]
    cache = ec.EvalCache(Path(args.out))
    frames: dict[str, list[Any]] = {name: [] for name in ("cells", "slices", "series", "quantiles", "decay")}
    registered = set(prep["plan"]["feature_ids"])
    for entry in prep["manifest"]["entries"]:
        if entry["feature_id"] not in registered:
            continue    # unsupported rows (C-82): not registered, not evaluated
        key = (entry["feature_sha"], label_sha, eval_sha)
        if not cache.has(*key):
            raise SystemExit(f"{entry['feature_id']} has no cached cells under {eval_sha[:12]}: run eval first")
        for name in frames:
            frames[name].append(cache.read(*key, table=name).to_pandas())
    cache.close()
    tables = {name: pd.concat(parts, ignore_index=True) for name, parts in frames.items()}
    del frames
    cells = tables["cells"]
    evaluated_cells = set(zip(cells["feature_id"].astype(str), cells["variant"].astype(str),
                              pd.to_numeric(cells["horizon_months"]).astype(int), strict=True))
    planned_cells = {(str(f), str(v), int(h)) for f, v, h in prep["plan"]["cells"]}
    if evaluated_cells != planned_cells:
        raise SystemExit(f"the evaluated cells differ from the registered cells (C-82): "
                         f"{len(evaluated_cells - planned_cells)} unregistered, {len(planned_cells - evaluated_cells)} "
                         "registered but not evaluated")
    family, summary = ev.compute_family(cells)
    cells = cells.drop(columns=list(ev.FAMILY_COLUMNS)).merge(
        family, on=["basis", "feature_id", "variant", "horizon_months"], how="left", validate="one_to_one")
    # Reported EB estimates require the whole wave, unlike the per-feature cache. Never replace
    # FAMILY_COLUMNS or any policy input with these descriptive columns.
    identity = ["basis", "feature_id", "variant", "horizon_months"]
    eb = ev.empirical_bayes_ic(cells)
    reported_eb = [c for c in eb.columns if c not in identity]
    cells = cells.drop(columns=reported_eb, errors="ignore").merge(
        eb, on=identity, how="left", validate="one_to_one")
    trace.mark("assembled", cells=len(cells))
    policy = rq.load_policy_v4()
    payload = prep["spec"]["payload"]
    catalog_rows = list(load_anomaly_catalog())
    run = {"run": args.run, "eval_spec_sha": eval_sha, "label_sha": label_sha,
           "manifest_sha": prep["inputs"]["manifest_sha"], "lake_snapshot": prep["inputs"]["lake_snapshot"],
           "formations": prep["inputs"]["formations"], "jobs": len(receipts), "stub_labels": stub}
    if stub:
        registration = _stand_in_registration(prep["plan"])
        evidence = rq.evidence_from_evaluation(cells, tables["slices"], tables["series"], policy, spec=payload,
                                               catalog=catalog_rows, registration=registration)
        ledger = rq.ledger_from_evidence_v4(
            evidence, policy, wave=args.wave, registration_id=registration.registration_id,
            catalog_digest=prep["inputs"]["catalog_digest"], n_trials=registration.n_cells, prior_gating_hypotheses=0,
            grade_basis=rq.GRADE_PROVISIONAL, holdout_opened=False, run=run, audit={"dry_run": True},
            extra_blockers=["dry_run_stub_labels_not_a_result"])
    else:
        ledger = rq.grade_wave_v4(cells, tables["slices"], tables["series"], policy, wave=args.wave, spec=payload,
                                  catalog=catalog_rows, catalog_digest=prep["inputs"]["catalog_digest"],
                                  grade_basis=rq.GRADE_PROVISIONAL, run_bases=run_bases, run=run)
    trace.mark("graded")
    ledger_dir = Path(args.out) / "eval" / "ledgers" / args.wave
    ledger_dir.mkdir(parents=True, exist_ok=True)
    (ledger_dir / f"{ledger.sha256}.json").write_bytes(ledger.to_json_bytes())
    (ledger_dir / f"{ledger.sha256}.csv").write_bytes(ledger.to_csv_bytes())
    report = _report(args, prep, cells, tables, ledger, summary)
    report["ledger"] = {"sha256": ledger.sha256, "path": (ledger_dir / f"{ledger.sha256}.json").as_posix(),
                        "manifest": ledger.manifest}
    report["memory"] = trace.marks
    report["jobs"] = [{"features": r["features"], "peak_gb": r.get("peak_gb"),
                       "done_before": r.get("done_before")} for r in receipts]
    report["context"] = receipts[0].get("context")
    report["label_info"] = receipts[0].get("label_info")
    report["inputs"] = prep["inputs"]
    _write_json(work / "report.json", report)
    (work / "report.md").write_text(_report_markdown(report), encoding="utf-8")
    print(f"[grade] ledger {ledger.sha256[:12]} status counts {ledger.manifest['status_counts']}")


# ---------------------------------------------------------------------------
# Report tables (the research note is written from these)
# ---------------------------------------------------------------------------

def _f(value: Any) -> float | None:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if number == number and abs(number) != float("inf") else None


def _report(args: argparse.Namespace, prep: Mapping[str, Any], cells: Any, tables: Mapping[str, Any], ledger: Any,
            family_summary: Mapping[str, Any]) -> dict[str, Any]:
    from atx_db.research import evaluation as ev
    from atx_db.research import label_matrix as lm

    registered = set(prep["plan"]["feature_ids"])
    entries = {e.feature_id: e for e in _wave_entries(args.wave) if e.feature_id in registered}
    graded = {row["feature_id"]: row for row in ledger.features}
    slices = tables["slices"]
    rows = []
    for feature_id, entry in sorted(entries.items(), key=lambda kv: (str(kv[1].jkp_theme), kv[0])):
        def cell(h: int, fid: str = feature_id) -> Mapping[str, Any]:
            chosen = cells[(cells["feature_id"] == fid) & (cells["variant"] == "rank_normal")
                           & (cells["horizon_months"] == h)]
            return chosen.iloc[0].to_dict() if len(chosen) else {}

        def piece(kind: str, name: str, h: int, fid: str = feature_id) -> Mapping[str, Any]:
            chosen = slices[(slices["feature_id"] == fid) & (slices["variant"] == "rank_normal")
                            & (slices["horizon_months"] == h) & (slices["slice_kind"] == kind)
                            & (slices["slice_name"] == name)]
            return chosen.iloc[0].to_dict() if len(chosen) else {}

        primary = cell(3)
        investable = piece(ev.INVESTABLE_SLICE_KIND, ev.INVESTABLE_SLICE_NAME, 3)
        jt12, jt6 = piece(ev.JT_SLICE_KIND, "k12", 1), piece(ev.JT_SLICE_KIND, "k6", 1)
        grade = graded.get(feature_id, {})
        turnover = [_f(primary.get(k)) for k in ("top_turnover_1m", "bottom_turnover_1m")]
        rows.append({
            "feature_id": feature_id, "theme": str(entry.jkp_theme), "sign": int(entry.expected_sign),
            "status": grade.get("status"), "status_reasons": grade.get("status_reasons"),
            "reported_only": grade.get("reported_only"), "coverage_share": _f(grade.get("coverage_share")),
            "coverage_formations": grade.get("coverage_formations"), "mean_coverage": _f(primary.get("mean_coverage")),
            "mean_names": _f(primary.get("mean_names")), "ic_n": primary.get("ic_n"),
            "ic_mean": _f(primary.get("ic_mean")), "ic_ir": _f(primary.get("ic_ir")), "ic_z": _f(primary.get("ic_z")),
            "ic_t": _f(primary.get("ic_robust_t")), "p_one_sided": _f(grade.get("p_one_sided")),
            "gating_q": _f(grade.get("gating_q")), "bh_q_reported": _f(primary.get("bh_q")),
            "investable_ic": _f(investable.get("ic_mean")), "investable_t": _f(investable.get("ic_robust_t")),
            "investable_p_one_sided": _f(grade.get("investable_p_one_sided")),
            "investable_formations": investable.get("formations"),
            "ls_ew10": _f(primary.get("ls_ew10_mean")), "ls_ew10_t": _f(primary.get("ls_ew10_robust_t")),
            "ls_vw10": _f(primary.get("ls_vw10_mean")), "ls_vw10_t": _f(primary.get("ls_vw10_robust_t")),
            "jt12_mean": _f(jt12.get("ls_mean")), "jt12_t": _f(jt12.get("ls_robust_t")),
            "jt6_mean": _f(jt6.get("ls_mean")), "jt6_t": _f(jt6.get("ls_robust_t")),
            "turnover_1m": None if None in turnover else (turnover[0] + turnover[1]) / 2.0,
            "fmc_t": _f(primary.get("fmc_robust_t")), "fmc_controls": primary.get("fmc_controls"),
            "small_ic": _f(grade.get("small_ic_mean")), "large_ic": _f(grade.get("large_ic_mean")),
            "persistence_1m": _f(grade.get("persistence_1m")), "ewc_oversize_risk": grade.get("ewc_oversize_risk"),
            "ic_by_h": {str(h): {"ic": _f(cell(h).get("ic_mean")), "t": _f(cell(h).get("ic_robust_t")),
                                 "n": cell(h).get("ic_n"), "status": cell(h).get("status")} for h in HORIZONS},
        })
    terminal: dict[str, Any] = {}
    if not bool(prep["inputs"]["stub_labels"]):
        matrix = lm.LabelMatrix(Path(prep["inputs"]["labels_root"]))
        for h in HORIZONS:
            for year in range(2012, 2024):
                side = matrix.path(prep["inputs"]["label_sha"], h, year).with_suffix(".json")
                if not side.is_file():
                    continue
                sidecar = _read_json(side)
                reasons = sidecar["reasons"]
                matured = int(sidecar["rows"]) - int(reasons.get("not_matured", 0))
                pending = int(reasons.get("terminal_pending", 0))
                terminal.setdefault(str(year), {})[f"h{h}"] = {
                    "terminal_pending": pending, "matured_rows": matured,
                    "share": None if not matured else pending / matured,
                    # C-79: labels spanning a vendor_artifact_suspect bar are invalid (withheld, counted).
                    "invalid": int(reasons.get("invalid", 0)),
                    "missing_bar": int(reasons.get("missing_entry_bar", 0)) + int(reasons.get("missing_exit_bar", 0))}
    counts = cells.groupby(["variant", "status"]).size()
    unsupported = _unsupported_stats(args, prep)
    return {"wave": args.wave, "run": args.run, "rows": rows, "terminal_pending_by_year": terminal,
            "unsupported": unsupported,
            "family_summary": {k: family_summary.get(k) for k in ("family_cells", "n_trials", "tested_cells",
                                                                   "bh_discoveries", "holm_discoveries",
                                                                   "hlz_passes")},
            "cell_status_counts": {f"{v}:{s}": int(n) for (v, s), n in counts.items()},
            "stub_labels": bool(prep["inputs"]["stub_labels"])}


def _unsupported_stats(args: argparse.Namespace, prep: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Feature-side facts of the unsupported rows (ruling C-82 (3)): per formation year of the run, the months whose
    valid values are all zero, and the most non-zero lines in any month. No label or return is read."""
    from atx_db.research import feature_store as fs
    from atx_db.research import research_lake as lake

    first, last = prep["inputs"]["formations"]
    store = fs.FeatureStore(Path(args.root))
    by_id = {entry["feature_id"]: entry for entry in prep["manifest"]["entries"]}
    out = []
    con = lake.connect_bounded(None, root=Path(args.root), memory_limit="128MB", threads=1)
    try:
        for feature_id, reason in sorted((prep["plan"].get("unsupported") or {}).items()):
            entry = by_id.get(feature_id)
            if entry is None:
                out.append({"feature_id": feature_id, "reason": reason, "in_manifest": False})
                continue
            path = lake.sql_text(store.path(entry["basis"], feature_id, entry["feature_sha"]).as_posix())
            years = con.execute(f"""
                SELECT year(eom), count(*), count(*) FILTER (WHERE nonzero = 0), max(nonzero)
                FROM (SELECT eom, count(*) FILTER (WHERE raw <> 0) AS nonzero FROM read_parquet({path})
                      WHERE reason = 0 AND eom BETWEEN ? AND ? GROUP BY eom)
                GROUP BY 1 ORDER BY 1
            """, [dt.date.fromisoformat(first), dt.date.fromisoformat(last)]).fetchall()
            out.append({"feature_id": feature_id, "reason": reason, "in_manifest": True,
                        "months": int(sum(r[1] for r in years)), "all_zero_months": int(sum(r[2] for r in years)),
                        "max_nonzero_lines": int(max((r[3] for r in years), default=0)),
                        "all_zero_by_year": {str(r[0]): f"{int(r[2])}/{int(r[1])}" for r in years}})
    finally:
        con.close()
    return out


def _fmt(value: Any, digits: int = 3) -> str:
    number = _f(value)
    return "" if number is None else f"{number:.{digits}f}"


def _report_markdown(report: Mapping[str, Any]) -> str:
    lines = ["| theme | feature | sign | status | cov | IC | ICIR | EWC z | inv IC | EW L/S (t) | VW L/S (t) | "
             "JT-12 (t) | turnover | EWC flag |",
             "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for row in report["rows"]:
        lines.append(
            f"| {row['theme']} | `{row['feature_id']}` | {row['sign']:+d} | {row['status']} | "
            f"{_fmt(row['coverage_share'], 2)} | {_fmt(row['ic_mean'], 4)} | {_fmt(row['ic_ir'], 2)} | "
            f"{_fmt(row['ic_z'], 2)} | {_fmt(row['investable_ic'], 4)} | "
            f"{_fmt(row['ls_ew10'], 4)} ({_fmt(row['ls_ew10_t'], 2)}) | {_fmt(row['ls_vw10'], 4)} "
            f"({_fmt(row['ls_vw10_t'], 2)}) | {_fmt(row['jt12_mean'], 4)} ({_fmt(row['jt12_t'], 2)}) | "
            f"{_fmt(row['turnover_1m'], 2)} | {'yes' if row['ewc_oversize_risk'] else ''} |")
    lines += ["", "| feature | IC 1m (t) | IC 3m (t) | IC 6m (t) | IC 12m (t) |", "|---|---:|---:|---:|---:|"]
    for row in report["rows"]:
        parts = [f"{_fmt(row['ic_by_h'][str(h)]['ic'], 4)} ({_fmt(row['ic_by_h'][str(h)]['t'], 2)})" for h in HORIZONS]
        lines.append(f"| `{row['feature_id']}` | " + " | ".join(parts) + " |")
    if report.get("terminal_pending_by_year"):
        lines += ["", "terminal_pending share of matured windows (count); invalid (withheld, C-79) count",
                  "", "| formation year | h1 | h3 | h6 | h12 | invalid h1/h3/h6/h12 |",
                  "|---|---:|---:|---:|---:|---|"]
        for year, item in sorted(report["terminal_pending_by_year"].items()):
            cells = []
            for h in HORIZONS:
                entry = item.get(f"h{h}") or {}
                share = entry.get("share")
                cells.append("" if share is None else f"{100 * share:.2f}% ({entry['terminal_pending']})")
            invalid = "/".join(str((item.get(f"h{h}") or {}).get("invalid", "")) for h in HORIZONS)
            lines.append(f"| {year} | " + " | ".join(cells) + f" | {invalid} |")
    if report.get("unsupported"):
        lines += ["", "Unsupported rows (not registered, ruling C-82): months whose valid values are all zero",
                  "", "| feature | reason | all-zero months | max non-zero lines | all-zero by year |", "|---|---|---:|---:|---|"]
        for item in report["unsupported"]:
            by_year = ", ".join(f"{y} {c}" for y, c in sorted((item.get("all_zero_by_year") or {}).items()))
            lines.append(f"| `{item['feature_id']}` | `{item['reason']}` | {item.get('all_zero_months')}/"
                         f"{item.get('months')} | {item.get('max_nonzero_lines')} | {by_year} |")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Orchestrator
# ---------------------------------------------------------------------------

def _jobs(args: argparse.Namespace, stage: str, argvs: list[list[str]], summary: dict[str, Any]) -> None:
    from atx_db.research.workers import EXIT_HEADROOM, EXIT_NOT_ADMITTED, run_jobs

    script = str(Path(__file__).resolve())
    common = ["--wave", args.wave, "--run", args.run, "--root", str(args.root), "--out", str(args.out),
              "--formations", args.formations] + (["--stub-labels"] if args.stub_labels else [])
    for name in RUN_PINS:
        if getattr(args, name, None):
            common += [f"--{name.replace('_', '-')}", getattr(args, name)]
    if args.universe_flag:
        common += ["--universe-flag", args.universe_flag]
    if args.require_artifact_flag:
        common.append("--require-artifact-flag")
    full = [[sys.executable, script, "stage", stage, *argv, *common] for argv in argvs]
    log_dir = _work(args) / "logs" / stage
    pending = list(range(len(full)))
    results: dict[int, dict[str, Any]] = {}
    for attempt in range(1, 6):
        receipts: list[dict[str, Any]] = []
        started = time.perf_counter()
        def preserve_receipt(index: int, code: int, receipt: dict[str, Any]) -> None:
            digest = _sha_text(_canonical(receipt))
            _write_json(_work(args) / "guard_receipts" / stage / f"{digest}.json", receipt)

        codes = run_jobs([full[i] for i in pending], max_workers=args.workers, job_gb=args.job_gb,
                         wait_minutes=args.wait_minutes, receipts=receipts, log_dir=log_dir / f"attempt-{attempt}",
                         on_finish=preserve_receipt)
        for index, code, receipt in zip(pending, codes, receipts, strict=True):
            results[index] = {"argv": argvs[index], "exit": code, "peak_gb": receipt.get("peak_job_memory_gb"),
                              "cap_hit": receipt.get("cap_hit"), "seconds": receipt.get("seconds"),
                              "wait_seconds": receipt.get("wait_seconds"), "attempt": attempt,
                              "status": receipt.get("status")}
        # 137: stopped in-run by the guard (resume from the cache); 78: not admitted within the wait (C-58).
        pending = [i for i in pending if results[i]["exit"] in (EXIT_HEADROOM, EXIT_NOT_ADMITTED)]
        print(f"[{stage}] attempt {attempt}: {len(codes)} jobs in {time.perf_counter() - started:.0f} s, "
              f"exits {sorted({results[i]['exit'] for i in results})}", flush=True)
        if not pending:
            break
        time.sleep(60)
    failed = {i: r for i, r in results.items() if r["exit"] != 0}
    peaks = [r["peak_gb"] for r in results.values() if r["peak_gb"] is not None]
    summary["stages"][stage] = {"jobs": len(full), "failed": len(failed), "peak_gb_max": max(peaks, default=None),
                                "results": [results[i] for i in sorted(results)]}
    _write_evidence(_work(args) / "summary.json", summary)
    if failed:
        raise SystemExit(f"stage {stage}: {len(failed)} jobs failed: {list(failed.values())[:3]} (logs {log_dir})")


def _run(args: argparse.Namespace) -> int:
    _formations(args.formations)
    _pins(args, [p for p in RUN_PINS if not (args.stub_labels and p == "label_sha")])
    if args.stub_labels and Path(args.out).resolve() == ROOT.resolve():
        raise SystemExit("--stub-labels needs a scratch --out (never the research root)")
    work = _work(args)
    work.mkdir(parents=True, exist_ok=True)
    path = work / "summary.json"
    summary = _read_json(path) if path.is_file() else {"run": args.run, "wave": args.wave, "stages": {}}
    summary["started_at"] = _now()
    stages = ("stub_labels", "prepare", "eval", "grade") if args.stub_labels else ("prepare", "eval", "grade")
    if args.stages != "all":
        stages = tuple(s for s in args.stages.split(",") if s)
    for stage in stages:
        if stage == "stub_labels":
            if (work / "stub_labels.json").is_file():
                continue
            _jobs(args, stage, [[]], summary)
        elif stage == "prepare":
            _jobs(args, stage, [[]], summary)
        elif stage == "eval":
            features = sorted(_read_json(work / "prepare.json")["plan"]["feature_ids"])
            if args.features:
                features = [f for f in features if f in set(args.features.split(","))]
            groups = [features[i:i + args.group_size] for i in range(0, len(features), args.group_size)]
            _jobs(args, stage, [["--features", ",".join(g)] for g in groups], summary)
        elif stage == "grade":
            _jobs(args, stage, [[]], summary)
        else:
            raise SystemExit(f"unknown stage {stage!r}")
    summary["finished_at"] = _now()
    _write_evidence(path, summary)
    for name, item in summary["stages"].items():
        print(f"{name}: jobs {item.get('jobs')} failed {item.get('failed')} peak {item.get('peak_gb_max')} GiB")
    return 0


def _stage(args: argparse.Namespace) -> int:
    started = time.perf_counter()
    handlers = {"stub_labels": _stage_stub_labels, "prepare": _stage_prepare, "eval": _stage_eval,
                "grade": _stage_grade}
    if args.stage_name not in handlers:
        raise SystemExit(f"unknown stage {args.stage_name!r}")
    handlers[args.stage_name](args)
    print(f"[{args.stage_name}] done in {time.perf_counter() - started:.1f} s", flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="run", choices=("run", "register", "stage"))
    parser.add_argument("stage_name", nargs="?")
    parser.add_argument("--wave", default="w1_price", choices=sorted(WAVES))
    parser.add_argument("--formations", default="2013-01..2023-12")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--job-gb", type=float, default=0.6)
    parser.add_argument("--wait-minutes", type=float, default=30.0)
    parser.add_argument("--group-size", type=int, default=1)
    parser.add_argument("--run")
    parser.add_argument("--root", type=Path, default=ROOT, help="inputs: lake, feature store, labels, registry")
    parser.add_argument("--out", type=Path, help="outputs: eval cache, ledgers, work dir (default: --root)")
    parser.add_argument("--stages", default="all")
    parser.add_argument("--features", help="eval: the job's features (comma-separated); run: a subset")
    parser.add_argument("--stub-labels", action="store_true", help="dry run on seeded random labels (see docstring)")
    parser.add_argument("--confirm", action="store_true", help="register: write the registration")
    pinned = parser.add_argument_group("pinned inputs (required; never 'latest')")
    pinned.add_argument("--manifest-sha", help="the wave's feature-store manifest sha")
    pinned.add_argument("--catalog-digest", help="anomaly_catalog_sha256() the wave is registered from")
    pinned.add_argument("--lake-snapshot", help="the price-wave lake snapshot (spine_monthly)")
    pinned.add_argument("--bench-snapshot", help="the benchmark lake snapshot (French NYSE ME breakpoints)")
    pinned.add_argument("--label-sha", help="the provisional label set (not with --stub-labels)")
    parser.add_argument("--universe-flag", help="a boolean spine_monthly column: rows where it is not true are "
                                                "withheld from the ranked universe (counted)")
    parser.add_argument("--require-artifact-flag", action="store_true",
                        help=f"refuse a lake snapshot whose spine_monthly lacks {ARTIFACT_FLAG} (ruling C-79)")
    args = parser.parse_args(argv)
    args.out = args.out or args.root
    if args.universe_flag and not re.fullmatch(r"[a-z_][a-z0-9_]{0,63}", args.universe_flag):
        raise SystemExit("--universe-flag must be a column identifier")
    if args.run is None:
        args.run = "dryrun-stub" if args.stub_labels else f"provisional-{(args.label_sha or 'unpinned')[:12]}"
    if args.command == "register":
        return _register(args)
    if args.command == "stage":
        return _stage(args)
    return _run(args)


if __name__ == "__main__":
    raise SystemExit(main())
