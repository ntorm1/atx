#!/usr/bin/env python3
"""One research cycle driven by a spec file (platform v7 lane L2): replaces the hand-copied vNN_train.sh ladders.

  research_cycle.py plan   SPEC [--root R] [--suffix S [--keep-fields]] [--attempt PHASE=N ...] [--reuse-fields DIR]
                                [--ledger PATH] [--runner-override KEY=VALUE ...] [--lines-only] [--no-git] [--screen]
  research_cycle.py run    SPEC [same options] [--stop-after PHASE]
  research_cycle.py status SPEC [same options]
  research_cycle.py lock   SPEC [--root R] [--relock] [--write]
  research_cycle.py add-alpha --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation "..." --origin prior
                              --parent v71 [--name v72] [--plan-json PATH] [--root R]   (research_add_alpha.py)
  research_cycle.py cache gc --keep-referenced-by SPEC [SPEC ...] [--under DIR] [--root R] [--apply]
                           (research_gc.py: stores no listed spec uses; deleted only with --apply)
  research_cycle.py ledger-protocol --ledger PATH --owner-ruling TEXT --date D [--window-id ID] [--root R]
                           (research_ledger.py: a window-change line that is no trial; count 0, no cell)
  research_cycle.py ledger-defect --ledger PATH --trial-id TID --reason TEXT [--date D] [--root R]
                           (research_ledger.py, review C-3: the ledgered cell TID is invalid; count 0, no cell)

Platform v8 (lane A) additions, each off unless the spec or the command line asks for it:
  --screen        run: fields, check, u (+ --no-composition when the IC exe offers it), fit, card, marginal (the exe's
                  marginal verb, contract K6, when offered), gate; then stop before w and write cycle_verdict.json
                  (admission and marginal rows, per-phase seconds and peak MiB) into the cycle dir
                  <out_root or build-equity>/cycle-<name>[-suffix]/; a full run writes it too (with the paired dSR,
                  DSR N and PBO blocks read from nav_summ --json / --pbo-json when the spec sets "verdict": true)
  --no-git        (contract K3) only for a --root outside any git repository: no clean check, the bounded runner gets
                  --root R --no-git, and a relative tool path (runner, builder, fit, card, monitor, summ scripts) that
                  is absent under R resolves to this worktree's copy
  clean check     scoped to the code pathspec (research_tree.CODE_PATHSPEC), before every executed phase; dirty paths
                  outside it (lane reports under .superpowers/) are listed, never a stop
  stage inputs    sec_identity_bridge + earnings_calendar + insider + sec_filings (all four or none; the three stage dirs
                  share one parent -> --sec-stages), thirteenf, ftd, regsho_threshold, security_master, short_volume_ext
                  (--<key> DIR --<key>-sha256 PIN) and reuse_fields (--reuse DIR --reuse-sha256 PIN, --reuse-hardlink
                  with fields.reuse_hardlink) reach the fields builder
  summ.dsr_n      "ledger+1" resolves at scoring time to nav_summ's N (backtest_integrity.ledger_n: the defect-rule
                  construction trials, protocol lines and window re-runs 0, + 1 for this cell when not yet ledgered;
                  research_ledger.py); summ.ledger_copy copies the ledger (into the sprint dir) after summ
  resume          (review C-13) a NAV step run by the cycle records <run dir>/cycle_binding.json (spec and argv
                  SHA-256); a done NAV is scored only when its spec digest (else its argv digest, from the binding or
                  its receipt's command) matches the current spec: a mismatch is a pin stop (exit 3) naming both
  admission lines (review C-7) the gate of a v8 cycle with a ledger first appends one chained admission line per
                  listed candidate (cycle_admission.py): the ledger's admission trials of v8 Appendix A
  summ.origin     (review C-2) prior | grid | mined, the cell's origin class (contract K5). A v8 scoring step (a
                  "verdict": true spec, or --protocol v8 in summ.extra) always runs nav_summ --protocol v8 (seed
                  20260929, 4,999 draws) and, with a ledger, --origin summ.origin; a verdict spec's summ also passes
                  --dsr-ledger <the ledger> and the verdict's DSR is nav_summ's deflated_ledger (review C-1)
  build          "equity" | "equity-rel": resolves exes (defaults or bare names) and env_path_prepend (BUILDS)
  out_root        a root-relative dir every relative output name is placed under; ic.cache and fit.work_dir, when
                  omitted, derive to <out_root or build-equity>/{candidate-cache,fit-work}/<role sha16>-<window id>
                  (shared, content-keyed; never suffixed)
  runner.phases   {phase: {seconds, max_rss_mib, min_free_mib}} per-phase caps over the runner's; absent, the OD-2 rule
                  table RUNNER_PHASE_RULES applies (u and w: 300 s / 2,560 MiB on a role of more than 1,200 dates)
  receipts        "every-phase": the direct phases (fields, check, monitor, summ) run through the bounded runner too
  ref             skipped when this cycle's fields manifest SHA equals inputs.baseline_fields (the parent's fields)
  marginal        {output, pool (input key, default reference_combined), themes (input key of the pool's composition
                  weights, e.g. reference_weights; optional), flags (--min-names N, --max-memory-mib N)}: the verb's
                  whole argv is built from the spec (--candidate-cache, --library(-sha256), --pool(-sha256), --role,
                  --themes, --fields, --output; --min-names defaults to the u pass's); a section it cannot be built
                  from is refused when the spec loads, and the pool's role and weights bindings when it is planned
  roles           (task H-1, research_roles.py) era shards: a list of roles {id, dir, begin, end, manifest_sha256,
                  universe, fields_dir, fields_manifest_sha256} replaces inputs.role; one role is exactly the
                  single-role cycle; two or more run fields, u, w and nav per era (outputs -<id>, receipts
                  --role-id), and check, the pooled fit (--era / --era-id), gate and the pooled summ (nav_summ --pool)
                  on the anchor (the last role); steps are named phase:<id>

SPEC is a JSON file (``atx.research-cycle-spec/v1``; a relative SPEC not found from the current directory is looked
up next to this script, so ``specs/v61.json`` works from the worktree root). Paths inside it are relative to --root
(default: this script's worktree). Phases, in order (a phase the spec leaves out is skipped):

  fields  prepare_research_fields.py (direct: the builder carries its own RSS/time caps), optional --reuse; then the
          fields check (baseline fields byte-identical, the expected fields added, short-volume file-set pin).
          fields.manifest_sha256 pins an AS-BUILT fields dir instead: no builder line, never rebuilt or suffixed; the
          phase is done iff DIR/manifest.json hashes to the pin (a different hash is a pin stop, exit 3; a missing
          one a hard stop), and fields.list (optional) must equal the manifest's field names in order
  check   the library static check (direct)
  ref     NAV replay of the REFERENCE construction on this cycle's fields (bounded; output ref.output, receipt
          <output>-run): the nav section's rule / leverage / flags (ref may override them) on the pinned input
          ref.combined (e.g. the reference cell's combined signal); an identity check, never a trial
  u       IC runner, unweighted pass            (bounded runner; output U-<attempt>, receipt U-run<attempt>)
  fit     fit_composition_weights.py            (bounded; output W, receipt W-run<pass>; exit 3 = documented
          "incomplete, rerun resumes" protocol -> the next pass runs, up to fit.max_passes; anything else stops)
  card    alpha_report_card.py over the u pass, its cache and this cycle's admission.json (bounded; output C,
          receipt C-run); before the gate, so a failed gate still leaves the cards
  gate    admission read-out of admission.json (internal; exit 10 when a required candidate is not admitted with
          its prior sign; gate.require "all" (default: every listed candidate) or "any" (at least one, the
          library-wave rule: print every row, stop only when none is admitted))
  w       IC runner, weighted pass              (bounded; output WT-<attempt>, receipt WT-run<attempt>)
  nav     NAV replay                            (bounded; output N, receipt N-run)
  monitor book_monitor.py --baseline: TRAIN reference distributions from the u pass daily IC, the admission, the
          cards' daily_sleeve.csv, the fit work dir and the spec's holdings_days / bias / decide paths (direct;
          output M)
  summ    nav_summ.py vs the reference cell with DSR N (direct); summ.cells (optional) lists the prior trial cells'
          NAV dirs, which go with this cycle's NAV output into one positional block right after --reference (the
          grid --effective-n dirs / --pbo / V[SR_n] read; len(cells) + 1 must equal dsr_n); summ.ledger (optional)
          is the default of --ledger. summ.cells_from_ledger: true (instead of cells; needs summ.ledger) reads the
          grid from the trial ledger at plan / run time (every line's "cell", in ledger order, this cycle's own cell
          excluded): cross-cell N = ledger lines + 1, so summ.dsr_n is the one key root changes when another cell
          was ledgered first (a mismatch stops with exit 3 and names the value)

compare   ``compare`` (optional list) adds identity checks after a named phase: each item {name, after, mode, a, b}
          runs as the internal step "<after>-compare" right after that phase, on every run and resume, and a miss is
          a HARD STOP (exit 4: no later phase runs, no trial). Operands are root-relative paths with {input:KEY}
          (a pinned input's path) and {out:PHASE} (fields, ref, u, fit, card, w, nav, monitor: this cycle's
          resolved output dir). Modes: "file" (SHA-256 of a == SHA-256 of b, bit for bit); "csv-rows" (unquoted
          CSV: same header, and the rows of b whose key column (key, default "id") is one of the compared keys are
          a's rows of those keys byte for byte, in order; b may add rows of other keys); "json-rows" (the objects of
          b's array (array, default "candidates") with a compared key are a's, in order, as canonical JSON (sorted
          keys, exact numbers); b may add objects of other keys). The compared keys are a's keys, or, with the
          operand "keys" (a library JSON, e.g. {input:baseline_library}), its candidate ids -- the parent's member
          rows, so a series of another key in both files (the IC runner's __combined__ book) is not compared; a
          must hold every one of them

Pins: every input (library, recipe, baseline library, role, identity bridge, fundamental events, SIC events (the
atx-db fundamentals stage manifest: the grp_* fields' --sic-events), baseline fields, reference admission, reference
cell, and the reference files an identity phase reads: reference combined signal, reference daily CSV, reference
orientations, reference daily IC) is pinned in the spec by ``lock``, which computes the SHA-256 from the file
(never hand-typed); ``plan``/``run`` re-hash and stop on any mismatch (exit 3). Every intermediate pin (fields
manifest, orientations, runner summary, weights, combined signal) is computed from the file the previous phase wrote.

``plan`` prints the exact command lines with every pin resolved (``# `` lines are annotations; --lines-only prints
the commands alone) and executes nothing. ``run`` executes phase by phase through scripts/run_bounded_research.py
(spec caps: 180 s / 1536 MiB / 512 MiB free), resumes from the first phase whose output is missing, never overwrites
an output (a failed attempt stays on disk: rerun with ``--attempt PHASE=N+1`` or a fresh ``--suffix``), reads every
receipt and HARD-STOPS (exit 4) on a non-zero exit or any refusal (prelaunch-memory-refusal, rss/time/system-memory
limit, runner error). The tree must be clean (the runner refuses otherwise). ``status`` shows each phase's state and
receipt. ``--suffix S`` appends ``-S`` to every output name (fields, cache, U, fit work, W, WT, N) for an identity
re-run (``--keep-fields``: every name but the fields dir, which is then reused as it is); ``--reuse-fields DIR``
passes ``--reuse DIR --reuse-sha256 <pin>`` to the fields builder; ``--ledger PATH`` (default: summ.ledger) passes
``--ledger PATH --ledger-kind <summ.ledger_kind>`` to nav_summ; ``--runner-override KEY=VALUE`` (seconds, max_rss_mib,
min_free_mib) changes a bounded-runner cap for this invocation (visible in every line; e.g. max_rss_mib=64 forces
an rss-limit refusal to check the hard stop).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cycle_admission  # noqa: E402
import cycle_resume  # noqa: E402
import research_ledger  # noqa: E402
import research_tree  # noqa: E402
from cycle_verdict import SUMM_JSON, PBO_JSON, VerdictError, step_key, write_verdict as _write_verdict  # noqa: E402

SCHEMA = "atx.research-cycle-spec/v1"
PHASES = ("fields", "check", "ref", "u", "fit", "card", "marginal", "gate", "w", "nav", "monitor", "summ")
COMPARE_SUFFIX = "-compare"            # the internal identity step after phase P is "P-compare"
STOP_PHASES = PHASES + tuple(p + COMPARE_SUFFIX for p in PHASES)
COMPARE_MODES = ("file", "csv-rows", "json-rows")
OUT_PHASES = ("fields", "ref", "u", "fit", "card", "w", "nav", "monitor")  # {out:PHASE} operands
ATTEMPT_PHASES = ("u", "fit", "w", "nav")
ERA_PHASES = ("fields", "u", "w", "nav")   # H-1: the phases each era of a roles: cycle runs (research_roles.py)
EXIT_OK, EXIT_USAGE, EXIT_PIN, EXIT_STOP, EXIT_GATE = 0, 2, 3, 4, 10
FIT_INCOMPLETE = 3                     # fit_composition_weights.py: "incomplete (rerun resumes)"
MAX_ATTEMPTS = 9
GATE_REQUIRE = ("all", "any")          # gate.require: every listed candidate admitted, or at least one
REQUIRED = {"schema", "name", "python", "runner", "inputs"}
SEC_STAGE_INPUTS = ("earnings_calendar", "insider", "sec_filings")   # dirs <ROOT>/<key>/ -> --sec-stages ROOT
SEC_INPUTS = ("sec_identity_bridge",) + SEC_STAGE_INPUTS            # all four or none (L9 design)
HOLDINGS_INPUTS = ("thirteenf", "ftd", "regsho_threshold", "security_master", "short_volume_ext")  # --<key> DIR
INPUT_KEYS = ("library", "recipe", "baseline_library", "role", "identity_bridge", "fund_events", "baseline_fields",
              "reference_admission", "reference_cell", "sic_events", "reference_combined", "reference_daily",
              "reference_orientations", "reference_daily_ic", "reference_weights") + SEC_INPUTS + HOLDINGS_INPUTS + \
    ("reuse_fields",)
# F-2's marginal IC verb (atx-impl/src/strategy_marginal_ic.cpp, dispatch_marginal_ic / run_marginal_ic): every option
# takes one value and a run without MARGINAL_REQUIRED is refused. The step builds MARGINAL_BUILT from the spec (paths and
# pins of inputs.library, inputs.<marginal.pool>, inputs.role, inputs.<marginal.themes>, this cycle's cache, fields and
# output); marginal.flags may set only MARGINAL_SPEC_FLAGS (unsigned integers; --min-names defaults to the u pass's).
MARGINAL_REQUIRED = ("--candidate-cache", "--library", "--pool", "--role", "--output")
MARGINAL_BUILT = MARGINAL_REQUIRED + ("--library-sha256", "--pool-sha256", "--themes", "--fields")
MARGINAL_SPEC_FLAGS = {"--min-names": (3, None), "--max-memory-mib": (32, 16384)}   # option: (min, max) as the verb
OPERAND = re.compile(r"\{(input|out):([A-Za-z0-9_]+)\}")
SOURCE_FLAGS = (("finra", "--finra"), ("tickerhistory", "--tickerhistory"), ("lake", "--lake"),
                ("finra_short_volume", "--finra-short-volume"))
DSR_FROM_LEDGER = "ledger+1"           # summ.dsr_n: resolved at scoring time from the trial ledger
SUMM_V8 = "v8"                         # nav_summ --protocol v8: seed 20260929, 4,999 draws, origin + window_id lines
DEFAULT_OUT_ROOT = "build-equity"      # derived cache / fit roots and the cycle dir when the spec has no out_root
RECEIPT_MODES = ("every-phase",)       # spec "receipts": the direct phases run through the bounded runner too
CAP_KEYS = ("seconds", "max_rss_mib", "min_free_mib")
# spec "build": the exe dir and the DLL dirs (env_path_prepend) of each research build tree (platform review P-4)
BUILDS = {
    "equity": {"bin": "build-equity/bin", "path": ["C:/atx-cache/vcpkg_installed/x64-windows/debug/bin",
                                                   "C:/atx-cache/vcpkg_installed/x64-windows/bin"]},
    "equity-rel": {"bin": "build-equity-rel/bin", "path": ["C:/atx-cache/vcpkg_installed/x64-windows/bin"]},
}
EXE_NAMES = {"ic": "atx-equity-strategy-ic.exe", "nav": "atx-equity-strategy-targets.exe"}
# Default per-phase runner caps, applied when the spec's runner.phases does not name the phase. OD-2 (owner ruling
# E-1, 2026-09-29): the IC passes get 2,560 MiB and 300 s on a role longer than 1,200 dates (the 3-year role has
# 1,155, the 4-year role about 1,405); every other phase keeps the runner's own caps.
RUNNER_PHASE_RULES = (
    {"phases": ("u", "w"), "when": {"role_dates_over": 1200}, "caps": {"seconds": 300, "max_rss_mib": 2560}},
)


class CycleError(Exception):
    def __init__(self, message: str, code: int = EXIT_STOP):
        super().__init__(message)
        self.code = code


def fmt_argv(argv: list[str]) -> str:
    """One command line: arguments joined by a space, an argument holding a space double-quoted (bash DRY format)."""
    return " ".join(f'"{a}"' if " " in a else a for a in argv)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------ spec
def find_spec(arg: str) -> Path:
    p = Path(arg)
    if p.exists():
        return p
    alt = Path(__file__).resolve().parent / arg
    if alt.exists():
        return alt
    raise CycleError(f"spec not found: {arg}", EXIT_USAGE)


def load_spec(path: Path) -> dict:
    try:
        spec = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CycleError(f"spec {path}: {exc}", EXIT_USAGE) from exc
    validate_spec(spec)
    return spec


def validate_spec(spec: dict) -> None:
    if not isinstance(spec, dict) or spec.get("schema") != SCHEMA:
        raise CycleError(f"spec schema must be {SCHEMA}", EXIT_USAGE)
    missing = sorted(REQUIRED - set(spec))
    if missing:
        raise CycleError(f"spec misses {', '.join(missing)}", EXIT_USAGE)
    unknown = sorted(set(spec["inputs"]) - set(INPUT_KEYS))
    if unknown:
        raise CycleError(f"spec inputs: unknown key(s) {', '.join(unknown)} (known: {', '.join(INPUT_KEYS)})",
                         EXIT_USAGE)
    for key, item in spec["inputs"].items():
        if not isinstance(item, dict) or "path" not in item or "sha256" not in item:
            raise CycleError(f"spec inputs.{key} needs path and sha256 (null until `lock`)", EXIT_USAGE)
    r = spec["runner"]
    if not all(k in r for k in ("script", "seconds", "max_rss_mib", "min_free_mib")):
        raise CycleError("spec runner needs script, seconds, max_rss_mib, min_free_mib", EXIT_USAGE)
    validate_runner_phases(r.get("phases"))
    need = {"fields": ("output",), "static_check": ("script",), "ic": ("u_output", "flags"),
            "fit": ("script", "output", "flags"), "gate": ("admitted",), "nav": ("output", "rule", "flags"),
            "summ": ("script", "dsr_n"), "card": ("script", "output"), "monitor": ("script", "output"),
            "ref": ("output", "combined"), "marginal": ("output",)}
    for section, keys in need.items():
        if section in spec and not all(k in spec[section] for k in keys):
            raise CycleError(f"spec {section} needs {', '.join(keys)}", EXIT_USAGE)
    validate_v8_keys(spec)
    f = spec.get("fields")
    if f is not None and not f.get("manifest_sha256") and not all(k in f for k in ("builder", "list")):
        raise CycleError("spec fields needs builder and list (a built fields dir) or manifest_sha256 (as built)",
                         EXIT_USAGE)
    if f is not None and "manifest_sha256" in f and (not isinstance(f["manifest_sha256"], str) or
                                                     len(f["manifest_sha256"]) != 64):
        raise CycleError("spec fields.manifest_sha256 must be a SHA-256 hex digest", EXIT_USAGE)
    if "ref" in spec:
        ref = spec["ref"]
        if "nav" not in spec or ref["combined"] not in spec["inputs"] or ref["output"] == spec["nav"]["output"]:
            raise CycleError("spec ref needs nav (its construction), inputs.<ref.combined> and an output of its own",
                             EXIT_USAGE)
    if "nav" in spec and "w_output" not in spec.get("ic", {}):
        raise CycleError("spec nav needs ic.w_output (the weighted pass feeds the NAV)", EXIT_USAGE)
    if ("fit" in spec and "ic" not in spec) or ("gate" in spec and "fit" not in spec) or \
            (("ic" in spec or "static_check" in spec) and "fields" not in spec):
        raise CycleError("spec: fit needs ic; gate needs fit; ic and static_check need fields", EXIT_USAGE)
    exes = effective_exes(spec)
    if ("ic" in spec and "ic" not in exes) or ("nav" in spec and "nav" not in exes):
        raise CycleError("spec exes needs ic (for ic) and nav (for nav)", EXIT_USAGE)
    for key in ("role", "library"):
        if "ic" in spec and key not in spec["inputs"]:
            raise CycleError(f"spec inputs needs {key}", EXIT_USAGE)
    if "summ" in spec and "nav" not in spec:
        raise CycleError("spec summ needs nav", EXIT_USAGE)
    if "card" in spec and "fit" not in spec:
        raise CycleError("spec card needs fit (it reads this cycle's admission.json)", EXIT_USAGE)
    if "monitor" in spec and ("nav" not in spec or "fit" not in spec):
        raise CycleError("spec monitor needs fit and nav", EXIT_USAGE)
    if "static_check" in spec and "baseline_library" not in spec["inputs"]:
        raise CycleError("spec static_check needs inputs.baseline_library", EXIT_USAGE)
    if spec.get("gate", {}).get("require", "all") not in GATE_REQUIRE:
        raise CycleError(f"spec gate.require must be one of {', '.join(GATE_REQUIRE)}", EXIT_USAGE)
    sm = spec.get("summ", {})
    if "cells_from_ledger" in sm and (sm["cells_from_ledger"] is not True or "cells" in sm or not sm.get("ledger")):
        raise CycleError("spec summ.cells_from_ledger must be true, excludes summ.cells and needs summ.ledger",
                         EXIT_USAGE)
    if sm.get("dsr_n") == DSR_FROM_LEDGER and (not sm.get("ledger") or "cells" in sm):
        raise CycleError(f"spec summ.dsr_n \"{DSR_FROM_LEDGER}\" needs summ.ledger and excludes summ.cells",
                         EXIT_USAGE)
    validate_compare(spec)
    cells = sm.get("cells")
    if cells is not None:
        if not isinstance(cells, list) or not cells or not all(isinstance(c, str) and c for c in cells) or \
                len(set(cells)) != len(cells) or spec["nav"]["output"] in cells:
            raise CycleError("spec summ.cells must list distinct prior NAV dirs (not this cycle's nav.output)",
                             EXIT_USAGE)
        if len(cells) + 1 != spec["summ"]["dsr_n"]:
            raise CycleError(f"spec summ.cells lists {len(cells)} prior cells + this one, dsr_n is "
                             f"{spec['summ']['dsr_n']}: the listed grid must be the declared N trials", EXIT_USAGE)


def validate_runner_phases(phases) -> None:
    if phases is None:
        return
    if not isinstance(phases, dict) or not all(p in PHASES and isinstance(c, dict) and c and set(c) <= set(CAP_KEYS)
                                               and all(type(v) in (int, float) and v > 0 for v in c.values())
                                               for p, c in phases.items()):
        raise CycleError(f"spec runner.phases must map a phase to caps {{{', '.join(CAP_KEYS)}}} (positive numbers)",
                         EXIT_USAGE)


def validate_v8_keys(spec: dict) -> None:
    """The platform v8 spec keys: build, out_root, receipts, verdict, marginal, the stage inputs, reuse_fields."""
    if "build" in spec and spec["build"] not in BUILDS:
        raise CycleError(f"spec build must be one of {', '.join(BUILDS)}", EXIT_USAGE)
    if "out_root" in spec and (not isinstance(spec["out_root"], str) or not spec["out_root"] or
                               Path(spec["out_root"]).is_absolute()):
        raise CycleError("spec out_root must be a root-relative directory (the bounded runner writes only inside "
                         "its root)", EXIT_USAGE)
    if spec.get("receipts", RECEIPT_MODES[0]) not in RECEIPT_MODES:
        raise CycleError(f"spec receipts must be one of {', '.join(RECEIPT_MODES)}", EXIT_USAGE)
    if "verdict" in spec and type(spec["verdict"]) is not bool:
        raise CycleError("spec verdict must be true or false", EXIT_USAGE)
    if "marginal" in spec:
        validate_marginal(spec)
    if "ledger_copy" in spec.get("summ", {}) and not (isinstance(spec["summ"]["ledger_copy"], str) and
                                                        spec["summ"].get("ledger")):
        raise CycleError("spec summ.ledger_copy (a path) needs summ.ledger", EXIT_USAGE)
    validate_summ_protocol(spec)
    inputs = spec["inputs"]
    sec = [k for k in SEC_INPUTS if k in inputs]
    if sec and len(sec) != len(SEC_INPUTS):
        raise CycleError(f"spec inputs: the SEC stage inputs {', '.join(SEC_INPUTS)} come together (missing "
                         f"{', '.join(k for k in SEC_INPUTS if k not in inputs)})", EXIT_USAGE)
    if sec:
        dirs = [Path(input_dir(inputs[k])) for k in SEC_STAGE_INPUTS]
        if any(d.name != k for d, k in zip(dirs, SEC_STAGE_INPUTS)) or len({d.parent.as_posix() for d in dirs}) != 1:
            raise CycleError("spec inputs: earnings_calendar, insider and sec_filings must be the directories of those "
                             "names under one alpha-panel root (--sec-stages)", EXIT_USAGE)
    if "reuse_fields" in inputs and ("fields" not in spec or spec["fields"].get("manifest_sha256")):
        raise CycleError("spec inputs.reuse_fields needs a built fields section (not an as-built manifest_sha256)",
                         EXIT_USAGE)


def option_value(flags: list, option: str) -> str | None:
    """The value after `option` in a flag list (None when absent or last)."""
    k = flags.index(option) if option in flags else -1
    return flags[k + 1] if 0 <= k < len(flags) - 1 else None


def summ_protocol(spec: dict) -> str | None:
    """"v8" when the spec's scoring step runs under the v8 pre-registration (review C-2): a verdict spec ("verdict":
    true, a v8 key) or "--protocol v8" in summ.extra. None otherwise: the v7 argv, unchanged."""
    extra = spec.get("summ", {}).get("extra", [])
    return SUMM_V8 if spec.get("verdict") is True or option_value(extra, "--protocol") == SUMM_V8 else None


def validate_summ_protocol(spec: dict) -> None:
    """summ.origin (contract K5 class of the cell, nav_summ --origin) and a verdict spec's protocol (review C-2)."""
    sm = spec.get("summ")
    if not isinstance(sm, dict):
        return
    extra = sm.get("extra", [])
    if "origin" in sm:
        origins = research_ledger.backtest_integrity().ORIGINS
        if sm["origin"] not in origins:
            raise CycleError(f"spec summ.origin must be one of {', '.join(origins)} (contract K5)", EXIT_USAGE)
        if "--origin" in extra:
            raise CycleError("spec summ: the origin is summ.origin or --origin in summ.extra, not both", EXIT_USAGE)
    if spec.get("verdict") is True and option_value(extra, "--protocol") not in (None, SUMM_V8):
        raise CycleError("spec verdict: a verdict cell is scored under nav_summ --protocol v8 (the v8 "
                         "pre-registration); summ.extra asks for another protocol", EXIT_USAGE)


def validate_marginal(spec: dict) -> None:
    """A marginal section the step can always turn into the verb's full argv (refused when the spec is loaded, so at
    plan time, never half way through a run): the pool and the themes weights are pinned inputs, flags are value pairs
    of MARGINAL_SPEC_FLAGS within the verb's ranges, and the step's own options are never repeated there."""
    m, inputs = spec["marginal"], spec["inputs"]
    if "ic" not in spec or m.get("pool", "reference_combined") not in inputs:
        raise CycleError("spec marginal needs ic and inputs.<marginal.pool> (default reference_combined: the parent's "
                         "combined signal)", EXIT_USAGE)
    themes = m.get("themes")
    if themes is not None and (not isinstance(themes, str) or themes not in inputs):
        raise CycleError("spec marginal.themes must name the pinned input holding the pool's composition weights (e.g. "
                         "reference_weights = the parent's fit output composition_weights.json: the verb's --themes "
                         "takes that file and binds it by the pool's composition_weights_sha256)", EXIT_USAGE)
    flags = m.get("flags", [])
    if not isinstance(flags, list) or len(flags) % 2 or not all(isinstance(x, str) for x in flags):
        raise CycleError("spec marginal.flags must be option/value pairs (strings); the verb's --themes takes the "
                         "weights file: name its input in marginal.themes", EXIT_USAGE)
    ic_names = option_value(spec["ic"].get("flags", []), "--min-names")
    for key, value in zip(flags[::2], flags[1::2]):
        if key in MARGINAL_BUILT:
            raise CycleError(f"spec marginal.flags: {key} is built by the step from the spec (it builds "
                             f"{', '.join(MARGINAL_BUILT)})", EXIT_USAGE)
        low, high = MARGINAL_SPEC_FLAGS.get(key, (None, None))
        if low is None or not value.isdigit() or int(value) < low or (high is not None and int(value) > high):
            raise CycleError(f"spec marginal.flags: {key} {value!r} (allowed: "
                             + ", ".join(f"{k} N >= {lo}" + (f" <= {hi}" if hi else "") for k, (lo, hi) in
                                         MARGINAL_SPEC_FLAGS.items()) + ")", EXIT_USAGE)
    if "--min-names" not in flags[::2] and ic_names is not None and not ic_names.isdigit():
        raise CycleError(f"spec marginal: the u pass's --min-names {ic_names!r} is not a count (set marginal.flags "
                         "--min-names)", EXIT_USAGE)


def input_dir(item: dict) -> str:
    """A pinned input's directory: its dir key, else the parent of its path."""
    return item.get("dir") or str(Path(item["path"]).parent).replace("\\", "/")


def effective_exes(spec: dict) -> dict:
    """spec exes with the build key applied: a missing exe defaults to the build's bin dir, a bare name goes in it."""
    exes = dict(spec.get("exes") or {})
    b = BUILDS.get(spec.get("build"))
    if b is None:
        return exes
    for key, name in EXE_NAMES.items():
        value = exes.get(key, name)
        exes[key] = value if "/" in value or "\\" in value else f"{b['bin']}/{value}"
    return exes


def phase_present(spec: dict, phase: str) -> bool:
    """Whether the spec runs `phase` (its section is present)."""
    if phase == "w":
        return "w_output" in spec.get("ic", {})
    return {"check": "static_check", "u": "ic"}.get(phase, phase) in spec


def validate_compare(spec: dict) -> None:
    items = spec.get("compare")
    if items is None:
        return
    if not isinstance(items, list) or not items:
        raise CycleError("spec compare must be a non-empty list", EXIT_USAGE)
    names = set()
    for c in items:
        if not isinstance(c, dict) or not all(isinstance(c.get(k), str) and c[k] for k in ("name", "after", "mode",
                                                                                           "a", "b")):
            raise CycleError("spec compare items need name, after, mode, a and b (strings)", EXIT_USAGE)
        if c["name"] in names:
            raise CycleError(f"spec compare: duplicate name {c['name']}", EXIT_USAGE)
        names.add(c["name"])
        if c["after"] not in PHASES or not phase_present(spec, c["after"]):
            raise CycleError(f"spec compare {c['name']}: after {c['after']!r} is not a phase this spec runs",
                             EXIT_USAGE)
        if c["mode"] not in COMPARE_MODES:
            raise CycleError(f"spec compare {c['name']}: mode must be one of {', '.join(COMPARE_MODES)}", EXIT_USAGE)
        if "keys" in c and (c["mode"] == "file" or not isinstance(c["keys"], str) or not c["keys"]):
            raise CycleError(f"spec compare {c['name']}: keys (a library JSON path) is for the row modes only",
                             EXIT_USAGE)
        for side in [s for s in ("a", "b", "keys") if s in c]:
            for kind, key in OPERAND.findall(c[side]):
                ok = key in spec["inputs"] if kind == "input" else key in OUT_PHASES and phase_present(spec, key)
                if not ok:
                    raise CycleError(f"spec compare {c['name']}: operand {{{kind}:{key}}} names no {kind} of this "
                                     "spec", EXIT_USAGE)
            if "{" in OPERAND.sub("", c[side]) or "}" in OPERAND.sub("", c[side]):
                raise CycleError(f"spec compare {c['name']}: malformed operand {c[side]!r}", EXIT_USAGE)


def parse_runner_overrides(items: list[str]) -> dict:
    out = {}
    for item in items or []:
        key, _, value = item.partition("=")
        if key not in ("seconds", "max_rss_mib", "min_free_mib") or not value.replace(".", "", 1).isdigit():
            raise CycleError(f"--runner-override {item}: expected seconds|max_rss_mib|min_free_mib=NUMBER", EXIT_USAGE)
        out[key] = int(value) if value.isdigit() else float(value)
    return out


def parse_attempts(items: list[str]) -> dict:
    out = {}
    for item in items or []:
        phase, _, n = item.partition("=")
        if phase not in ATTEMPT_PHASES or not n.isdigit() or not 1 <= int(n) <= MAX_ATTEMPTS:
            raise CycleError(f"--attempt {item}: expected PHASE=N, PHASE in {', '.join(ATTEMPT_PHASES)}, "
                             f"1 <= N <= {MAX_ATTEMPTS}", EXIT_USAGE)
        out[phase] = int(n)
    return out


# ------------------------------------------------------------------ files under the root
class Resolver:
    """Root-relative files: existence, SHA-256 (cached) and JSON. ``known`` = {relpath: sha256} stands in for files
    that are not on disk (hash-only mode for fixtures: content checks are skipped and flagged)."""

    def __init__(self, root: Path, known: dict | None = None):
        self.root = Path(root)
        self.known = dict(known or {})
        self._sha: dict = {}

    def path(self, rel: str) -> Path:
        p = Path(rel)
        return p if p.is_absolute() else self.root / p

    def exists(self, rel: str) -> bool:
        return rel in self.known or self.path(rel).is_file()

    def exists_dir(self, rel: str) -> bool:
        pre = rel.rstrip("/") + "/"
        return self.path(rel).is_dir() or any(k.startswith(pre) for k in self.known)

    def sha(self, rel: str) -> str | None:
        if rel in self.known:
            return self.known[rel]
        if rel not in self._sha:  # only existing files are cached: outputs appear while a cycle runs
            p = self.path(rel)
            if not p.is_file():
                return None
            self._sha[rel] = sha256_file(p)
        return self._sha[rel]

    def read_json(self, rel: str):
        p = self.path(rel)
        if not p.is_file():
            return None
        return json.loads(p.read_text(encoding="utf-8"))

    def hash_only(self, rel: str) -> bool:
        return rel in self.known and not self.path(rel).is_file()


def placeholder(rel: str) -> str:
    return f"<sha256:{rel}>"


# ------------------------------------------------------------------ the cycle
class Step:
    def __init__(self, phase: str, kind: str, argv=None, output=None, run_dir=None, attempt=None, state="pending",
                 note="", checks=None):
        self.phase, self.kind, self.argv, self.output, self.run_dir = phase, kind, argv, output, run_dir
        self.attempt, self.state, self.note = attempt, state, note
        self.checks = checks or []  # compare steps: the resolved comparisons
        self.role, self.cycle = None, None  # a roles: cycle's era id and era Cycle (H-1; step_key, fields_check)

    @property
    def done(self) -> bool:
        return self.state == "done"


def exe_capabilities(exe: str, root: Path, env: dict | None = None) -> frozenset:
    """What the IC exe offers beyond the v7 CLI, read from its --help text: "no-composition" (B-1's u-pass flag) and
    "marginal" (F-2's marginal IC verb, contract K6). A missing or failing exe offers nothing (never a hard stop)."""
    p = Path(exe) if Path(exe).is_absolute() else Path(root) / exe
    try:
        done = subprocess.run([str(p), "--help"], cwd=root, env=env, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return frozenset()
    text = (done.stdout or "") + (done.stderr or "")
    return frozenset(name for name, found in (("no-composition", "--no-composition" in text),
                                              ("marginal", re.search(r"\bmarginal\b", text) is not None)) if found)


class Cycle:
    def __init__(self, spec: dict, res: Resolver, *, suffix: str | None = None, attempts: dict | None = None,
                 reuse_fields: str | None = None, ledger: str | None = None, spec_path: Path | None = None,
                 keep_fields: bool = False, runner_overrides: dict | None = None, no_git: bool = False,
                 screen: bool = False, capabilities=None, verify: bool = True, role_key: str | None = None):
        if keep_fields and reuse_fields:
            raise CycleError("--keep-fields and --reuse-fields exclude each other", EXIT_USAGE)
        if reuse_fields and spec.get("fields", {}).get("manifest_sha256"):
            raise CycleError("--reuse-fields: the spec pins an as-built fields dir (fields.manifest_sha256); nothing "
                             "is built", EXIT_USAGE)
        if reuse_fields and "reuse_fields" in spec["inputs"]:
            raise CycleError("--reuse-fields: the spec pins inputs.reuse_fields already", EXIT_USAGE)
        if no_git and research_tree.no_git_refusal(res.root):
            raise CycleError(research_tree.no_git_refusal(res.root), EXIT_USAGE)
        spec = json.loads(json.dumps(spec))
        spec["runner"].update(runner_overrides or {})
        if "build" in spec:
            spec["exes"] = effective_exes(spec)
            spec.setdefault("env_path_prepend", list(BUILDS[spec["build"]]["path"]))
        self.spec, self.res, self.suffix, self.keep_fields = spec, res, suffix, keep_fields
        self.runner_overrides = dict(runner_overrides or {})
        self.attempts = dict(attempts or {})
        self.reuse_fields, self.ledger, self.spec_path = reuse_fields, ledger, spec_path
        self.no_git, self.screen = no_git, screen
        self._capabilities = capabilities   # None: probe the IC exe's --help when a step needs it (cached)
        self.py = spec["python"]
        # task H-1 (research_roles.py): an era of a roles: cycle keys its outputs and receipts by role_key; the
        # anchor era's fit and summ pool the others (fit_pool: (argv, binds); summ_pool: [(id, nav dir, weights)])
        self.role_key, self.weights_name = role_key, "composition_weights.json"
        self.fit_pool: tuple[list[str], list[str]] | None = None
        self.summ_pool: list[tuple[str, str, str]] | None = None
        # verify=False: names only (add-alpha reads a parent spec's outputs); no pin, no step
        self.pins = self.verify_inputs() if verify else {}

    # -------------------------------------------------------------- names and pins
    def out(self, name: str, keyed: bool = True) -> str:
        """An output name: placed under out_root (when set, for a relative name), keyed by the era (H-1: ``-<role>``,
        per-era outputs only), then suffixed."""
        name = self.placed(name)
        if keyed and self.role_key:
            name = f"{name}-{self.role_key}"
        return f"{name}-{self.suffix}" if self.suffix else name

    def placed(self, name: str) -> str:
        root = self.spec.get("out_root")
        return f"{root.rstrip('/')}/{name}" if root and not Path(name).is_absolute() else name

    def out_base(self) -> str:
        return self.spec.get("out_root") or DEFAULT_OUT_ROOT

    def cycle_dir(self) -> str:
        """Where the cycle's own files go: the always-run phases' receipts and cycle_verdict.json."""
        name = f"{self.out_base().rstrip('/')}/cycle-{self.spec['name']}"
        return f"{name}-{self.suffix}" if self.suffix else name

    def derived_root(self, kind: str) -> str:
        """A shared, content-keyed store derived from the role pin and the research window (never suffixed)."""
        try:
            wid = window_id()
        except LookupError as exc:
            raise CycleError(f"spec omits the {kind} root and it cannot be derived: {exc}", EXIT_USAGE) from exc
        role = self.pin("role") if "role" in self.pins else (      # verify=False: the spec's pin, else the file's
            self.spec["inputs"]["role"].get("sha256") or self.res.sha(self.ipath("role")))
        if not role:
            raise CycleError(f"spec omits the {kind} root and the role pin is unknown", EXIT_USAGE)
        return f"{self.out_base().rstrip('/')}/{kind}/{role[:16]}-{wid}"

    def cache_dir(self) -> str:
        ic = self.spec["ic"]
        return self.out(ic["cache"], keyed=False) if "cache" in ic else self.derived_root("candidate-cache")

    def fit_work_dir(self) -> str:
        fit = self.spec["fit"]
        return self.out(fit["work_dir"], keyed=False) if "work_dir" in fit else self.derived_root("fit-work")

    def tool(self, rel: str) -> str:
        """A spec script path; with --no-git a relative one absent under the root is this worktree's copy."""
        if self.no_git and not Path(rel).is_absolute() and not self.res.path(rel).exists() and \
                (research_tree.REPO / rel).is_file():
            return (research_tree.REPO / rel).as_posix()
        return rel

    def capabilities(self) -> frozenset:
        if self._capabilities is None:
            self._capabilities = exe_capabilities(self.spec["exes"]["ic"], self.res.root, self.env())
        return frozenset(self._capabilities)

    def env(self) -> dict:
        env = dict(os.environ)
        pre = self.spec.get("env_path_prepend") or []
        if pre:
            env["PATH"] = os.pathsep.join([str(Path(p)) for p in pre] + [env.get("PATH", "")])
        return env

    def verify_inputs(self) -> dict:
        """{key: (path, sha, how)}; a locked pin must equal the file's SHA-256 (else exit 3)."""
        pins = {}
        for key, item in self.spec["inputs"].items():
            rel, want = item["path"], item.get("sha256")
            got = self.res.sha(rel)
            if got is None:
                raise CycleError(f"input {key} missing: {rel}", EXIT_PIN)
            if want is None:
                pins[key] = (rel, got, "UNLOCKED (computed now; run `lock --write`)")
            elif want != got:
                raise CycleError(f"PIN MISMATCH {key}: {rel} is {got}, spec pins {want}", EXIT_PIN)
            else:
                pins[key] = (rel, got, "locked, verified" if not self.res.hash_only(rel) else "locked (hash-only)")
        role = self.spec["inputs"].get("role")
        if role and role.get("universe"):
            doc = self.res.read_json(role["path"])
            if doc is not None:
                got = (doc.get("universe") or {}).get("id")
                if got != role["universe"]:
                    raise CycleError(f"role universe is {got!r}, spec declares {role['universe']!r}", EXIT_PIN)
        return pins

    def pin(self, key: str) -> str:
        return self.pins[key][1]

    def ipath(self, key: str) -> str:
        return self.spec["inputs"][key]["path"]

    def idir(self, key: str) -> str:
        return input_dir(self.spec["inputs"][key])

    def rt_sha(self, rel: str) -> str:
        """An intermediate pin: the SHA-256 of a file an earlier phase wrote (placeholder until it exists)."""
        return self.res.sha(rel) or placeholder(rel)

    def role_counts(self) -> dict:
        """{role_dates, scored_sessions} from the role manifest ("dates", "score_begin", "score_end"); {} when the
        manifest is not on disk (hash-only mode) or lacks them."""
        doc = self.res.read_json(self.ipath("role")) if "role" in self.spec["inputs"] else None
        if not isinstance(doc, dict) or type(doc.get("dates")) is not int:
            return {}
        out = {"role_dates": doc["dates"]}
        if type(doc.get("score_begin")) is int:
            out["scored_sessions"] = doc.get("score_end", doc["dates"]) - doc["score_begin"]
        return out

    def phase_caps(self, phase: str) -> dict:
        """The runner caps of one phase: the runner's own, then the spec's runner.phases entry (else the first
        matching RUNNER_PHASE_RULES rule), then the command line's --runner-override (which wins)."""
        r = self.spec["runner"]
        caps = {k: r[k] for k in CAP_KEYS}
        spec_caps = (r.get("phases") or {}).get(phase)
        if spec_caps is None:
            counts = None
            for rule in RUNNER_PHASE_RULES:
                if phase not in rule["phases"]:
                    continue
                counts = self.role_counts() if counts is None else counts
                if all(counts.get(key[:-len("_over")], -1) > limit for key, limit in rule["when"].items()):
                    spec_caps = rule["caps"]
                    break
        caps.update(spec_caps or {})
        caps.update({k: v for k, v in self.runner_overrides.items() if k in CAP_KEYS})
        return caps

    def runner(self, run_dir: str, binds: list[str], phase: str | None = None) -> list[str]:
        r = self.spec["runner"]
        caps = self.phase_caps(phase) if phase else {k: r[k] for k in CAP_KEYS}
        argv = [self.py, self.tool(r["script"])]
        if self.no_git:
            argv += ["--root", str(self.res.root), "--no-git"]
        argv += ["--seconds", str(caps["seconds"]), "--max-rss-mib", str(caps["max_rss_mib"]),
                 "--min-free-mib", str(caps["min_free_mib"])]
        if self.role_key and phase in ERA_PHASES:   # H-1: an era's own run (the pooled fit and summ have none)
            argv += ["--role-id", self.role_key]
        argv += ["--output", run_dir]
        for b in binds:
            argv += ["--bind", b]
        return argv + ["--"]

    def always_run_dir(self, phase: str) -> str:
        """A fresh receipt dir of an always-run phase (check, summ): <cycle dir>/<phase>-run<k>, k the first unused."""
        base = f"{self.cycle_dir()}/{phase}-run"
        k = 1
        while self.res.exists_dir(f"{base}{k}"):
            k += 1
        return f"{base}{k}"

    def every_phase(self) -> bool:
        return self.spec.get("receipts") == "every-phase"

    # -------------------------------------------------------------- phase states
    def receipt(self, run_dir: str | None) -> dict | None:
        if not run_dir:
            return None
        return self.res.read_json(f"{run_dir}/receipt.json")

    def complete_summary(self, out: str) -> bool:
        rel = f"{out}/summary.json"
        if not self.res.exists(rel):
            return False
        if self.res.hash_only(rel):
            return True
        doc = self.res.read_json(rel)
        return isinstance(doc, dict) and doc.get("status") == "complete"

    def ic_attempt(self, phase: str, base: str) -> tuple[int, str, str]:
        """(attempt, state, note) of an IC pass: the explicit --attempt, else the lowest complete attempt, else a
        fresh attempt 1; a failed attempt on disk without an explicit --attempt is a stop (never auto-retried)."""
        if phase in self.attempts:
            n = self.attempts[phase]
            if self.complete_summary(f"{base}-{n}"):
                return n, "done", ""
            if self.res.exists_dir(f"{base}-{n}") or self.res.exists_dir(f"{base}-run{n}"):
                return n, "failed", self.failure_note(f"{base}-run{n}")
            return n, "pending", ""
        tried = 0
        for n in range(1, MAX_ATTEMPTS + 1):
            if self.complete_summary(f"{base}-{n}"):
                return n, "done", ""
            if self.res.exists_dir(f"{base}-{n}") or self.res.exists_dir(f"{base}-run{n}"):
                tried = n
        if tried:
            return tried, "failed", (self.failure_note(f"{base}-run{tried}") +
                                     f"; never overwritten: rerun with --attempt {phase}={tried + 1}")
        return 1, "pending", ""

    def failure_note(self, run_dir: str) -> str:
        r = self.receipt(run_dir)
        if r is None:
            return f"attempt left {run_dir} without a receipt"
        return f"receipt {run_dir}: outcome {r.get('outcome')}, exit_code {r.get('exit_code')}"

    # -------------------------------------------------------------- the steps, with every pin resolved
    def steps(self) -> list[Step]:
        s, out = self.spec, []
        role_m, role_sha = self.ipath("role"), self.pin("role")
        lib, lib_sha = self.ipath("library"), self.pin("library")
        fd = ""                                  # validate_spec: ic and the static check need fields
        if "fields" in s:
            f_out = s["fields"]["output"]
            if s["fields"].get("manifest_sha256"):  # a pinned input dir: never placed, suffixed or rebuilt
                fd = f_out
            else:
                fd = self.placed(f_out) if self.keep_fields else self.out(f_out)
        fdm = f"{fd}/manifest.json" if fd else ""
        if "fields" in s:
            out.append(self.fields_step(fd, fdm, role_sha))
        if "static_check" in s:
            sc = s["static_check"]
            argv = [self.py, self.tool(sc["script"]), "--manifest", fdm, "--library", lib, "--baseline",
                    self.ipath("baseline_library"), *sc.get("args", [])]
            if sc.get("expect_added"):
                argv += ["--expect-added", ",".join(sc["expect_added"])]
            out.append(self.always_step("check", argv, [self.tool(sc["script"])], "library static check"))
        if "ref" in s:
            out.append(self.ref_step(fdm, role_m, role_sha))
        u_out = ""
        if "ic" in s:
            ic = s["ic"]
            base = self.out(ic["u_output"])
            n, state, note = self.ic_attempt("u", base)
            u_out, run_dir = f"{base}-{n}", f"{base}-run{n}"
            flags = list(ic["flags"])
            if self.screen and "no-composition" in self.capabilities() and "--no-composition" not in flags:
                # B-1: a screen skips the u-pass blend nobody reads (and so has no combined signal to save)
                flags = [x for x in flags if x != "--save-combined"] + ["--no-composition"]
            argv = self.runner(run_dir, [s["exes"]["ic"], lib, role_m, fdm], "u") + [
                s["exes"]["ic"], "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
                role_sha, "--train-fields", fd, "--train-fields-sha256", self.rt_sha(fdm), "--output", u_out,
                *flags, "--candidate-cache", self.cache_dir()]
            out.append(Step("u", "bounded", argv, u_out, run_dir, n, state, note))
        w_dir = ""
        if "fit" in s:
            step, w_dir = self.fit_step(u_out, lib, lib_sha, role_m, role_sha)
            out.append(step)
        card_out = ""
        if "card" in s:
            step = self.card_step(u_out, w_dir, role_m, role_sha)
            card_out = step.output
            out.append(step)
        if "marginal" in s:
            out.append(self.marginal_step(lib, fd))
        if "gate" in s:
            out.append(Step("gate", "internal", None, state="always",
                            note=f"admission read-out of {w_dir}/admission.json"))
        wt_out = ""
        if "ic" in s and "w_output" in s["ic"]:
            ic = s["ic"]
            base = self.out(ic["w_output"])
            n, state, note = self.ic_attempt("w", base)
            wt_out, run_dir = f"{base}-{n}", f"{base}-run{n}"
            weights = f"{w_dir}/{self.weights_name}"
            argv = self.runner(run_dir, [s["exes"]["ic"], role_m, fdm, lib, weights], "w") + [
                s["exes"]["ic"], "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
                role_sha, "--train-fields", fd, "--train-fields-sha256", self.rt_sha(fdm), "--output", wt_out,
                *ic["flags"], "--candidate-cache", self.cache_dir(), "--composition-weights", weights,
                "--composition-weights-sha256", self.rt_sha(weights)]
            out.append(Step("w", "bounded", argv, wt_out, run_dir, n, state, note))
        n_out = ""
        if "nav" in s:
            n_out = self.out(s["nav"]["output"])
            comb = f"{wt_out}/train_combined.json"
            out.append(self.nav_step("nav", n_out, comb, self.rt_sha(comb), s["nav"], fdm, role_m, role_sha))
        if "monitor" in s:
            out.append(self.monitor_step(u_out, w_dir, card_out))
        if "summ" in s:
            out.append(self.summ_step(w_dir, n_out))
        steps = self.with_compares(out)
        if self.screen:  # u -> fit -> card -> marginal -> gate; the identity NAV and the cell wait for the full run
            for st in steps:
                if st.phase in ("ref", "ref" + COMPARE_SUFFIX, "w", "nav", "monitor", "summ") or \
                        (st.phase.endswith(COMPARE_SUFFIX) and st.phase[:-len(COMPARE_SUFFIX)] in ("w", "nav")):
                    st.kind, st.state, st.note = "skipped", "skipped", "screen: runs with the full `run`"
        return steps

    def always_step(self, phase: str, argv: list[str], binds: list[str], note: str) -> Step:
        """An always-run process phase (check, summ): direct, or bounded with a fresh receipt dir (every-phase)."""
        if not self.every_phase():
            return Step(phase, "direct", argv, state="always", note=note)
        run_dir = self.always_run_dir(phase)
        return Step(phase, "bounded", self.runner(run_dir, binds, phase) + argv, None, run_dir, None, "always", note)

    def ref_step(self, fdm: str, role_m: str, role_sha: str) -> Step:
        s, ref = self.spec, self.spec["ref"]
        nav = dict(s["nav"], **{k: ref[k] for k in ("rule", "leverage", "flags") if k in ref})
        step = self.nav_step("ref", self.out(ref["output"]), self.ipath(ref["combined"]), self.pin(ref["combined"]),
                             nav, fdm, role_m, role_sha)
        if "baseline_fields" in s["inputs"] and step.state != "done" and \
                self.res.sha(fdm) == self.pin("baseline_fields"):
            step.kind, step.state = "skipped", "skipped"
            step.note = (f"fields manifest {fdm} equals the parent's (inputs.baseline_fields): the reference "
                         "construction is identical by construction")
        return step

    def marginal_step(self, lib: str, fd: str) -> Step:
        """F-2's marginal IC verb (contract K6) over the u pass's candidate cache and fields dir, on this cycle's role:
        each candidate's IC and its IC residualised on the pool (inputs.<marginal.pool>, the parent's combined signal)
        and, with marginal.themes, on the pool's theme composites. The argv carries every option the verb requires
        (MARGINAL_REQUIRED) and every pin the spec has; skipped with a note when the IC exe does not offer the verb."""
        s, m = self.spec, self.spec["marginal"]
        ic_exe, role_m = s["exes"]["ic"], self.ipath("role")
        m_out = self.out(m["output"])
        run_dir = f"{m_out}-run"
        pool_key, themes = m.get("pool", "reference_combined"), m.get("themes")
        pool = self.ipath(pool_key)
        self.check_marginal_bindings(pool_key, themes)
        argv = [ic_exe, "marginal", "--candidate-cache", self.cache_dir(), "--library", lib, "--library-sha256",
                self.pin("library"), "--pool", pool, "--pool-sha256", self.pin(pool_key), "--role", role_m]
        binds = [ic_exe, lib, pool, role_m]
        if themes:
            argv += ["--themes", self.ipath(themes)]
            binds.append(self.ipath(themes))
        argv += ["--fields", fd]
        flags = list(m.get("flags", []))
        min_names = option_value(s["ic"]["flags"], "--min-names")
        if "--min-names" not in flags[::2] and min_names is not None:   # the u pass's name floor
            flags += ["--min-names", min_names]
        argv = self.runner(run_dir, binds + [f"{fd}/manifest.json"], "marginal") + argv + flags + ["--output", m_out]
        if "marginal" not in self.capabilities():
            return Step("marginal", "skipped", argv, m_out, run_dir, None, "skipped",
                        "the IC exe offers no marginal verb (contract K6, lane F): skipped")
        return Step("marginal", "bounded", argv, m_out, run_dir, None, *self.single_state(
            m_out, run_dir, f"{m_out}/marginal_ic.json"))

    def check_marginal_bindings(self, pool_key: str, themes: str | None) -> None:
        """The verb's bindings of the files this spec pins, checked when the step is planned (exit 3 before any phase
        runs): the pool manifest's role_manifest_sha256 is inputs.role's pin and, with themes, its
        composition_weights_sha256 is the pin of inputs.<themes>. Skipped for a pool not on disk (hash-only)."""
        rel = self.ipath(pool_key)
        try:
            doc = self.res.read_json(rel)
        except ValueError as exc:
            raise CycleError(f"marginal: pool {rel} is not a combined-signal manifest ({exc})", EXIT_PIN) from exc
        if doc is None:
            return
        doc = doc if isinstance(doc, dict) else {}
        if doc.get("role_manifest_sha256") != self.pin("role"):
            raise CycleError(f"marginal: pool {rel} was blended on role {doc.get('role_manifest_sha256')}, not on "
                             f"inputs.role {self.pin('role')} (the verb binds --role to the pool)", EXIT_PIN)
        if themes and doc.get("composition_weights_sha256") != self.pin(themes):
            raise CycleError(f"marginal: pool {rel} names composition weights {doc.get('composition_weights_sha256')}, "
                             f"not inputs.{themes} {self.pin(themes)} (the verb binds --themes to the pool)", EXIT_PIN)

    def single_state(self, out: str, run_dir: str, marker: str) -> tuple[str, str]:
        """(state, note) of a single-attempt output: done iff its marker file exists; a partial output is a stop."""
        if self.res.exists(marker):
            return "done", ""
        if self.res.exists_dir(out) or self.res.exists_dir(run_dir):
            return "failed", self.failure_note(run_dir) + "; never overwritten: use a fresh --suffix"
        return "pending", ""

    def summ_step(self, w_dir: str, n_out: str) -> Step:
        s, sm = self.spec, self.spec["summ"]
        argv = [self.py, self.tool(sm["script"])]
        if w_dir:
            argv += ["--weights", f"{w_dir}/composition_weights.json"]
        if "reference_cell" in s["inputs"]:
            argv += ["--reference", self.idir("reference_cell")]
        pool = self.summ_pool                # H-1: the eras' NAV dirs pooled by nav_summ --pool (anchor era)
        for _, _, weights in pool or []:
            if weights != f"{w_dir}/composition_weights.json":
                argv += ["--weights", weights]
        from_ledger = sm.get("cells_from_ledger") or sm["dsr_n"] == DSR_FROM_LEDGER
        prior, ledger_n = self.ledger_cells(n_out) if from_ledger else (None, None)
        cells = prior if sm.get("cells_from_ledger") else (sm.get("cells") or [])
        if cells:  # one positional block (argparse), before the options: a trailing nargs-* --pbo takes none
            argv += [*cells] + ([] if pool else [n_out])
        argv += ["--dsr-n", str(self.dsr_n(ledger_n)), *sm.get("extra", [])]
        ledger = self.ledger or sm.get("ledger")
        if summ_protocol(s) == SUMM_V8:
            argv += self.v8_summ_flags(ledger)
        if s.get("verdict"):
            argv += ["--json", f"{self.cycle_dir()}/{SUMM_JSON}"]
            if "--pbo" in sm.get("extra", []):
                argv += ["--pbo-json", f"{self.cycle_dir()}/{PBO_JSON}"]
        if ledger:
            argv += ["--ledger", ledger, "--ledger-kind", sm.get("ledger_kind", "construction")]
        if s.get("verdict"):   # review C-1: the verdict's DSR is the ledger's (N and V[SR] of v8-prereg item 3)
            if not ledger:
                raise CycleError("spec verdict: the verdict's DSR needs the sprint ledger of record (summ.ledger or "
                                 "--ledger) for nav_summ --dsr-ledger (v8-prereg item 3)", EXIT_USAGE)
            argv += ["--dsr-ledger", ledger]
        if pool:
            argv += ["--pool", *[nav for _, nav, _ in pool], "--pool-ids", ",".join(i for i, _, _ in pool)]
        elif not cells:
            argv.append(n_out)
        return self.always_step("summ", argv, [self.tool(sm["script"])], "nav_summ vs the reference cell")

    def v8_summ_flags(self, ledger: str | None) -> list[str]:
        """Review C-2: a v8 scoring step always carries nav_summ --protocol v8 (the pre-registered bootstrap seed and
        draw count, the year table, origin + window_id + chain in the ledger line), added unless summ.extra has it;
        with a ledger also --origin summ.origin (contract K5), which nav_summ requires there."""
        sm = self.spec["summ"]
        extra = sm.get("extra", [])
        out = [] if option_value(extra, "--protocol") == SUMM_V8 else ["--protocol", SUMM_V8]
        if ledger and "--origin" not in extra:
            if not sm.get("origin"):
                raise CycleError("spec summ: a v8 scoring step ledgers the cell with its origin class: set summ.origin "
                                 "(prior | grid | mined, contract K5)", EXIT_USAGE)
            out += ["--origin", sm["origin"]]
        return out

    def dsr_n(self, ledger_n: int | None) -> int:
        """summ.dsr_n: the declared integer, or "ledger+1" = the ledger's N (research_ledger.ledger_n: the defect-rule
        construction trials + 1 for this cell when not yet ledgered, nav_summ's N; resolved at scoring time)."""
        n = self.spec["summ"]["dsr_n"]
        return ledger_n if n == DSR_FROM_LEDGER else n

    def with_compares(self, steps: list[Step]) -> list[Step]:
        """The spec's compare items as one internal "<after>-compare" step right after each named phase (skipped with
        it when that phase is skipped)."""
        items = self.spec.get("compare") or []
        if not items:
            return steps
        outs = {st.phase: st.output for st in steps if st.phase in OUT_PHASES and st.output}

        def operand(text: str) -> str:
            return OPERAND.sub(lambda m: self.ipath(m[2]) if m[1] == "input" else outs[m[2]], text)
        done = []
        for st in steps:
            done.append(st)
            mine = [dict(c, **{s: operand(c[s]) for s in ("a", "b", "keys") if s in c})
                    for c in items if c["after"] == st.phase]
            if mine:
                note = "identity: " + "; ".join(f"{c['name']} [{c['mode']}]" for c in mine) + " (a miss hard-stops)"
                if st.state == "skipped":
                    done.append(Step(st.phase + COMPARE_SUFFIX, "skipped", None, state="skipped",
                                     note=f"{st.phase} skipped: {note}", checks=mine))
                else:
                    done.append(Step(st.phase + COMPARE_SUFFIX, "compare", None, state="always", note=note,
                                     checks=mine))
        return done

    def nav_step(self, phase: str, n_out: str, comb: str, comb_sha: str, nav: dict, fdm: str, role_m: str,
                 role_sha: str) -> Step:
        """A NAV replay (the cycle's cell, or the reference construction for the ref phase)."""
        s = self.spec
        k = self.attempts.get("nav", 1) if phase == "nav" else 1
        run_dir = f"{n_out}-run" if k == 1 else f"{n_out}-run{k}"
        flags = [str(nav.get("leverage")) if f == "{leverage}" else f for f in nav["flags"]]
        argv = self.runner(run_dir, [s["exes"]["nav"], comb, fdm], phase) + [
            s["exes"]["nav"], "nav", "--combined", comb, "--combined-sha256", comb_sha, "--role", role_m,
            "--role-sha256", role_sha, "--fields", fdm, "--fields-sha256", self.rt_sha(fdm), "--output", n_out,
            "--rule", nav["rule"], *flags]
        if self.res.exists(f"{n_out}/summary.json"):
            state, note = "done", ""
        elif self.res.exists_dir(n_out) or self.res.exists_dir(run_dir):
            state, note = "failed", self.failure_note(run_dir) + "; never overwritten: use a fresh --suffix"
        else:
            state, note = "pending", "" if phase == "nav" else "reference construction on this cycle's fields"
        return Step(phase, "bounded", argv, n_out, run_dir, k, state, note)

    def ledger_cells(self, n_out: str) -> tuple[list[str], int]:
        """summ.cells_from_ledger / dsr_n "ledger+1": (every trial line's cell, in ledger order, this cycle's own cell
        excluded (protocol lines are no trial: research_ledger), the ledger's N); N is backtest_integrity.ledger_n
        (the defect rule, + 1 for this cell when not yet ledgered: nav_summ's N) and an integer dsr_n must equal it."""
        sm = self.spec["summ"]
        rel = self.ledger or sm["ledger"]
        p = self.res.path(rel)
        if not p.is_file():
            raise CycleError(f"summ.cells_from_ledger: no trial ledger at {rel}", EXIT_PIN)
        navs = [nav for _, nav, _ in self.summ_pool or []]   # H-1: the pooled cell, by its pooled trial_id
        try:
            cells = research_ledger.cells(p)
            n = (research_ledger.ledger_n(p, research_ledger.pool_label(navs), pool_dirs=[self.res.path(d) for d in navs])
                 if navs else research_ledger.ledger_n(p, n_out, self.res.path(n_out)))
        except research_ledger.LedgerError as exc:
            raise CycleError(f"summ.cells_from_ledger: {exc}", EXIT_PIN) from exc
        prior = [c for c in cells if c != n_out]
        if len(set(prior)) != len(prior):
            raise CycleError(f"summ.cells_from_ledger: {rel} lists a cell twice", EXIT_PIN)
        if sm["dsr_n"] != DSR_FROM_LEDGER and n != sm["dsr_n"]:
            raise CycleError(f"summ.cells_from_ledger: {rel} lists {len(prior)} prior cells, so cross-cell N = "
                             f"{n}, but the spec declares summ.dsr_n {sm['dsr_n']}: set summ.dsr_n to {n}", EXIT_PIN)
        return prior, n

    def fields_step(self, fd: str, fdm: str, role_sha: str) -> Step:
        f, s = self.spec["fields"], self.spec
        pin = f.get("manifest_sha256")
        if pin:  # as built: an input, never (re)built by the cycle
            got = self.res.sha(fdm)
            if got is None:
                state, note = "failed", (f"pinned fields manifest missing: {fdm} (as built; the cycle never builds a "
                                         "pinned fields dir)")
            elif got != pin:
                raise CycleError(f"PIN MISMATCH fields: {fdm} is {got}, spec pins {pin}", EXIT_PIN)
            else:
                state, note = "done", f"as built, manifest sha256 {pin} (pinned)"
            return Step("fields", "pinned", None, fd, None, None, state, note)
        argv = [self.py, self.tool(f["builder"]), "--role", self.idir("role"), "--role-sha256", role_sha, "--output",
                fd, "--fields", ",".join(f["list"])]
        for key, flag in SOURCE_FLAGS:
            if key in f.get("sources", {}):
                argv += [flag, f["sources"][key]]
        if "identity_bridge" in s["inputs"]:
            argv += ["--identity-bridge", self.idir("identity_bridge"), "--identity-bridge-sha256",
                     self.pin("identity_bridge")]
        if "fund_events" in s["inputs"]:
            argv += ["--fund-events", self.idir("fund_events"), "--fund-events-sha256", self.pin("fund_events")]
        if "sic_events" in s["inputs"]:  # U2: the grp_* SIC table = the role's (atx-db fundamentals stage manifest)
            argv += ["--sic-events", self.idir("sic_events"), "--sic-events-sha256", self.pin("sic_events")]
        argv += self.stage_flags()
        if "fund_lag_sessions" in f:
            argv += ["--fund-lag-sessions", str(f["fund_lag_sessions"])]
        argv += ["--max-rss-mib", str(f.get("max_rss_mib", 1536)), "--max-seconds", str(f.get("max_seconds", 1800))]
        if "reuse_fields" in s["inputs"]:  # a pinned prior fields dir (L9 design)
            argv += ["--reuse", self.idir("reuse_fields"), "--reuse-sha256", self.pin("reuse_fields")]
            if f.get("reuse_hardlink"):
                argv.append("--reuse-hardlink")
        elif self.reuse_fields:
            rel = f"{self.reuse_fields.rstrip('/')}/manifest.json"
            sha = self.res.sha(rel)
            if sha is None:
                raise CycleError(f"--reuse-fields: no manifest at {rel}", EXIT_PIN)
            argv += ["--reuse", self.reuse_fields.rstrip("/"), "--reuse-sha256", sha]
        if self.res.exists(fdm):
            state, note = "done", ""
        elif self.res.exists_dir(fd):
            state, note = "failed", f"partial fields dir exists (never overwritten): {fd}; use a fresh --suffix"
        else:
            state, note = "pending", ""
        if self.every_phase():
            run_dir = f"{fd}-run"
            if state == "pending" and self.res.exists_dir(run_dir):
                state, note = "failed", self.failure_note(run_dir) + "; never overwritten: use a fresh --suffix"
            return Step("fields", "bounded", self.runner(run_dir, [self.tool(f["builder"]), self.ipath("role")], "fields") + argv,
                        fd, run_dir, None, state, note)
        return Step("fields", "direct", argv, fd, None, None, state, note)

    def stage_flags(self) -> list[str]:
        """The L9 stage inputs as fields-builder flags: the four SEC inputs (--sec-stages = the three stage dirs'
        common parent, --sec-identity-bridge, and one pin per stage) and the five holdings stages (--<key> DIR
        --<key>-sha256 PIN), in INPUT_KEYS order."""
        inputs, argv = self.spec["inputs"], []
        if "sec_identity_bridge" in inputs:
            argv += ["--sec-stages", str(Path(self.idir(SEC_STAGE_INPUTS[0])).parent).replace("\\", "/"),
                     "--sec-identity-bridge", self.idir("sec_identity_bridge"), "--sec-identity-bridge-sha256",
                     self.pin("sec_identity_bridge")]
            for key in SEC_STAGE_INPUTS:
                argv += [f"--{key.replace('_', '-')}-sha256", self.pin(key)]
        for key in HOLDINGS_INPUTS:
            if key in inputs:
                flag = f"--{key.replace('_', '-')}"
                argv += [flag, self.idir(key), f"{flag}-sha256", self.pin(key)]
        return argv

    def card_step(self, u_out: str, w_dir: str, role_m: str, role_sha: str) -> Step:
        c = self.spec["card"]
        c_out = self.out(c["output"])
        run_dir = f"{c_out}-run"
        adm = f"{w_dir}/admission.json"
        argv = self.runner(run_dir, [self.tool(c["script"]), role_m, f"{u_out}/summary.json", adm], "card") + [
            self.py, self.tool(c["script"]), "--u-pass", u_out, "--train", role_m, "--train-sha256", role_sha,
            "--admission", adm, "--admission-sha256", self.rt_sha(adm), *c.get("flags", []), "--output", c_out]
        return Step("card", "bounded", argv, c_out, run_dir, None, *self.single_state(c_out, run_dir,
                                                                                      f"{c_out}/index.json"))

    def monitor_step(self, u_out: str, w_dir: str, card_out: str) -> Step:
        m = self.spec["monitor"]
        m_out = self.out(m["output"])
        argv = [self.py, self.tool(m["script"]), "--baseline", "--daily-ic", f"{u_out}/train_daily_ic.csv",
                "--admission", f"{w_dir}/admission.json", "--fit-work", self.fit_work_dir()]
        if card_out:
            argv += ["--sleeve-daily", f"{card_out}/daily_sleeve.csv"]
        for key, flag in (("holdings_days", "--holdings-days"), ("bias", "--bias"), ("decide", "--decide")):
            if m.get(key):
                argv += [flag, m[key]]
        argv += [*m.get("flags", []), "--output", m_out]
        if self.every_phase():
            run_dir = f"{m_out}-run"
            return Step("monitor", "bounded", self.runner(run_dir, [self.tool(m["script"])], "monitor") + argv, m_out, run_dir,
                        None, *self.single_state(m_out, run_dir, f"{m_out}/monitor.json"))
        if self.res.exists(f"{m_out}/monitor.json"):
            state, note = "done", ""
        elif self.res.exists_dir(m_out):
            state, note = "failed", f"partial monitor dir exists (never overwritten): {m_out}; use a fresh --suffix"
        else:
            state, note = "pending", ""
        return Step("monitor", "direct", argv, m_out, None, None, state, note)

    def fit_step(self, u_out: str, lib: str, lib_sha: str, role_m: str, role_sha: str) -> tuple[Step, str]:
        s, fit = self.spec, self.spec["fit"]
        w_dir = self.out(fit["output"], keyed=False)   # H-1: one pooled fit for every era
        o, sm = f"{u_out}/orientations.json", f"{u_out}/summary.json"
        max_passes = int(fit.get("max_passes", 3))
        runs = [j for j in range(1, MAX_ATTEMPTS + 1) if self.res.exists_dir(f"{w_dir}-run{j}")]
        if "fit" in self.attempts:
            j = self.attempts["fit"]
        elif self.res.exists(f"{w_dir}/composition_weights.json"):
            j = runs[-1] if runs else 1
        elif runs:
            last = self.receipt(f"{w_dir}-run{runs[-1]}")
            j = runs[-1] + 1 if last and last.get("exit_code") == FIT_INCOMPLETE else runs[-1]
        else:
            j = 1
        run_dir = f"{w_dir}-run{j}"
        if self.res.exists(f"{w_dir}/composition_weights.json"):
            state, note = "done", ""
        elif self.res.exists_dir(run_dir):
            state, note = "failed", self.failure_note(run_dir) + "; never overwritten"
        elif j > max_passes:
            state, note = "failed", f"fit still incomplete after {max_passes} passes"
        else:
            state, note = "pending", "" if j == 1 else f"resume pass {j} (previous pass exited {FIT_INCOMPLETE})"
        pool_argv, pool_binds = self.fit_pool or ([], [])   # H-1: --era ... --era-id of the anchor era
        argv = self.runner(run_dir, [lib, self.ipath("recipe"), role_m, o, sm, *pool_binds], "fit") + [
            self.py, self.tool(fit["script"]), "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
            role_sha, "--orientations", o, "--orientations-sha256", self.rt_sha(o), "--runner-summary", sm,
            "--runner-summary-sha256", self.rt_sha(sm), *fit["flags"]]
        if "recipe" in s["inputs"]:
            argv += ["--recipe", self.ipath("recipe"), "--recipe-sha256", self.pin("recipe")]
        argv += ["--work-dir", self.fit_work_dir()]
        if "max_seconds" in fit:
            argv += ["--max-seconds", str(fit["max_seconds"])]
        argv += [*pool_argv, "--output", w_dir]
        return Step("fit", "bounded", argv, w_dir, run_dir, j, state, note), w_dir


# ------------------------------------------------------------------ internal checks
def fields_check(cycle: Cycle, fdm: str, log=print) -> None:
    f = cycle.spec["fields"]
    if f.get("manifest_sha256") and "list" in f:  # as built: the pinned manifest's rows are the declared list
        doc = cycle.res.read_json(fdm) or {}
        names = [row.get("name") for row in doc.get("fields", [])]
        log(f"fields check: pinned {fdm} sha256 {f['manifest_sha256']}: {len(names)} rows"
            f"{' == the spec list' if names == f['list'] else ' DIFFER from the spec list'}")
        if names != f["list"]:
            raise CycleError("fields check FAILED (the pinned manifest's field rows differ from fields.list)")
    chk = f.get("check")
    if not chk:
        return
    res = cycle.res
    b = res.read_json(fdm)
    if b is None:
        raise CycleError(f"fields check: no manifest {fdm}")
    base_key = chk.get("baseline", "baseline_fields")
    a = res.read_json(cycle.ipath(base_key)) if base_key in cycle.spec["inputs"] else None
    added, bad = [], []
    if a is not None:
        names = [f["name"] for f in a["fields"]]
        bad = [n for n in names if a["files"][n + ".f64"] != b["files"].get(n + ".f64")]
        added = sorted({f["name"] for f in b["fields"]} - set(names))
        log(f"fields check: {len(names) - len(bad)} of {len(names)} baseline fields byte-identical "
            f"{bad or ''}; added {added}")
    ok = not bad and (a is None or "expect_added" not in chk or added == sorted(chk["expect_added"]))
    want_sv = chk.get("short_volume_files_sha256")
    if want_sv:
        sv = next((f for f in b["fields"] if f["name"] == "sv_ratio126"), None)
        files = ((sv or {}).get("short_volume") or {}).get("files") or {}
        log(f"fields check: sv_ratio126 files {files.get('files_read')} {files.get('first_file_date')}.."
            f"{files.get('last_file_date')} sha {files.get('files_sha256')}")
        ok = ok and files.get("files_sha256") == want_sv
    reuse = b.get("reuse")
    if reuse:
        log(f"fields reuse: reused {len(reuse['reused'])}, computed {reuse['computed']} (from {reuse['from']})")
    if not ok:
        raise CycleError("fields check FAILED (baseline fields changed, unexpected added set or short-volume pin)")


def gate(cycle: Cycle, w_dir: str, log=print) -> None:
    g = cycle.spec["gate"]
    adm = cycle.res.read_json(f"{w_dir}/admission.json")
    if adm is None:
        raise CycleError(f"gate: no admission.json in {w_dir} (run fit first)")
    rows = {c["id"]: c for c in adm["candidates"]}

    def fmt(x):
        return "None" if x is None else f"{x:.4f}" if isinstance(x, float) else str(x)
    passed = []
    for cid in g["admitted"]:
        r = rows.get(cid)
        if r is None:
            log(f"gate {g.get('name', 'gate')} {cid}: not in admission.json")
            passed.append(False)
            continue
        log(f"gate {g.get('name', 'gate')} {cid}: status {r['status']} failed {r.get('failed_checks')}; runner sign "
            f"{r.get('runner_sign')} vs prior {r.get('s_k')} (agrees {r.get('sign_agrees')}); HAC t {fmt(r.get('hac_t'))}; "
            f"tau {fmt(r.get('tau'))} (limit {adm.get('rules', {}).get('tau_limit')}); max |rho| "
            f"{fmt(r.get('max_abs_rho'))} with {r.get('max_abs_rho_with')}; redundant_with {r.get('redundant_with')}; "
            f"train days {r.get('train_days')}")
        passed.append(r["status"] == "admitted" and (not g.get("sign_agrees", True) or r.get("sign_agrees") is True))
    for cid in g.get("report", []):
        if cid in rows:
            log(f"  {cid}: status {rows[cid]['status']}, max |rho| {fmt(rows[cid].get('max_abs_rho'))} with "
                f"{rows[cid].get('max_abs_rho_with')}")
    if "reference_admission" in cycle.spec["inputs"]:
        ref = cycle.res.read_json(cycle.ipath("reference_admission"))
        if ref is not None:
            prev = {c["id"]: c["status"] for c in ref["candidates"]}
            moved = {k: (prev[k], rows[k]["status"]) for k in prev if k in rows and rows[k]["status"] != prev[k]}
            log(f"  reference members vs {cycle.ipath('reference_admission')}: {len(moved)} status changes {moved or ''}")
    require = g.get("require", "all")
    ok = all(passed) if require == "all" else any(passed)
    if require != "all":
        log(f"gate {g.get('name', 'gate')}: {sum(passed)} of {len(passed)} listed candidates admitted with the prior "
            f"sign (require {require})")
    log(f"gate {g.get('name', 'gate')} " + ("PASS" if ok else "FAIL: stop (no later phase runs)"))
    if not ok:
        raise CycleError("gate failed", EXIT_GATE)


def _lines(data: bytes) -> list[bytes]:
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    return lines


def _first_diff(a: list, b: list) -> int:
    return next((k for k, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))


def compare_keys(res: Resolver, rel: str) -> list[str]:
    """The compared keys of a row compare with `keys`: the candidate ids of a library JSON (candidates[].id)."""
    p = res.path(rel)
    try:
        ids = [row["id"] for row in json.loads(p.read_bytes())["candidates"]]
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise CycleError(f"compare keys: no candidates[].id in {rel} ({exc})") from exc
    if not ids or not all(isinstance(i, str) and i for i in ids) or len(set(ids)) != len(ids):
        raise CycleError(f"compare keys: {rel} needs distinct non-empty candidate ids")
    return ids


def compare_files(res: Resolver, c: dict) -> str:
    """One identity comparison (see the module doc, `compare`); returns its summary, raises CycleError on a miss.
    Row modes compare the rows whose key is one of `keys` (a library's candidate ids) when given, else of a's keys."""
    name, mode, a, b = c["name"], c["mode"], c["a"], c["b"]

    def miss(why: str) -> CycleError:
        return CycleError(f"IDENTITY MISMATCH [{name}] ({mode}) {a} vs {b}: {why} -- hard stop, no trial")
    pa, pb = res.path(a), res.path(b)
    for rel, p in ((a, pa), (b, pb)):
        if not p.is_file():
            raise miss(f"{rel} is missing")
    if mode == "file":
        sa, sb = sha256_file(pa), sha256_file(pb)
        if sa != sb:
            raise miss(f"sha256 {sa} != {sb}")
        return f"bit for bit ({pa.stat().st_size} bytes, sha256 {sa})"
    members = compare_keys(res, c["keys"]) if c.get("keys") else None
    if mode == "csv-rows":
        la, lb = _lines(pa.read_bytes()), _lines(pb.read_bytes())
        if not la or not lb or la[0] != lb[0]:
            raise miss("headers differ")
        key = c.get("key", "id").encode()
        cols = la[0].split(b",")
        if key not in cols:
            raise miss(f"no key column {key.decode()}")
        k = cols.index(key)

        def key_of(row: bytes) -> bytes:
            parts = row.split(b",")
            return parts[k] if k < len(parts) else b""
        keys = {m.encode() for m in members} if members is not None else {key_of(row) for row in la[1:]}
        ref = [row for row in la[1:] if key_of(row) in keys]
        if {key_of(row) for row in ref} != keys:
            raise miss(f"a lacks rows of {len(keys - {key_of(row) for row in ref})} of the {len(keys)} keys")
        mine = [row for row in lb[1:] if key_of(row) in keys]
        if mine != ref:
            j = _first_diff(ref, mine)
            at = ref[j] if j < len(ref) else b"<end>"
            raise miss(f"{len(ref)} rows of {len(keys)} keys vs {len(mine)} matching rows; first difference at "
                       f"key row {j + 1} ({at[:80].decode(errors='replace')})")
        other = {key_of(row) for row in lb[1:]} - keys
        return (f"{len(ref)} rows of {len(keys)} keys byte for byte (a has {len(la) - 1 - len(ref)} rows of other "
                f"keys); b adds {len(lb) - 1 - len(mine)} rows of {len(other)} other keys")
    array, key = c.get("array", "candidates"), c.get("key", "id")
    try:
        ra, rb = json.loads(pa.read_bytes())[array], json.loads(pb.read_bytes())[array]
        ids = [r[key] for r in ra]
        wanted = set(members) if members is not None else set(ids)
    except (ValueError, KeyError, TypeError) as exc:
        raise miss(f"no {array}[].{key} rows ({exc})") from exc
    if members is None and len(wanted) != len(ids):
        raise miss(f"a lists a {key} twice")
    ra = [r for r in ra if r[key] in wanted]
    if sorted(r[key] for r in ra) != sorted(wanted):
        raise miss(f"a does not list each of the {len(wanted)} keys exactly once")
    canon = lambda r: json.dumps(r, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    sel = [r for r in rb if isinstance(r, dict) and r.get(key) in wanted]
    ca, cb = [canon(r) for r in ra], [canon(r) for r in sel]
    if ca != cb:
        j = _first_diff(ca, cb)
        raise miss(f"{len(ca)} objects vs {len(cb)} matching; first difference at {key} "
                   f"{ra[j][key] if j < len(ra) else '<end>'}")
    return f"{len(ca)} {array} objects identical (canonical JSON); b adds {len(rb) - len(sel)} objects"


def compare(cycle: Cycle, st: Step, log=print) -> None:
    for c in st.checks:
        log(f"compare {c['name']} [{c['mode']}]: {c['a']} vs {c['b']}")
        log(f"   IDENTICAL: {compare_files(cycle.res, c)}")


# ------------------------------------------------------------------ plan / status / run
def header(cycle: Cycle) -> list[str]:
    spec_sha = sha256_file(cycle.spec_path) if cycle.spec_path else None
    lines = [f"# research_cycle {cycle.spec['name']}: spec {cycle.spec_path} sha256 {spec_sha}; root {cycle.res.root}; "
             f"suffix {cycle.suffix or 'none'}{' (fields kept)' if cycle.keep_fields else ''}; attempts "
             f"{cycle.attempts or 'auto'}; runner overrides {cycle.runner_overrides or 'none'}"
             f"{'; --no-git' if cycle.no_git else ''}{'; --screen' if cycle.screen else ''}"]
    for key, (rel, sha, how) in cycle.pins.items():
        lines.append(f"# pin {key}: {rel} {sha} [{how}]")
    return lines


def plan_lines(cycle: Cycle, lines_only: bool = False) -> list[str]:
    out = [] if lines_only else header(cycle)
    for st in cycle.steps():
        if not lines_only:
            where = f" -> {st.output}" if st.output else ""
            rd = f" (receipt {st.run_dir})" if st.run_dir else ""
            out.append(f"# phase {step_key(st)} [{st.kind}; {st.state}{'; ' + st.note if st.note else ''}]{where}{rd}")
            for c in st.checks:
                out.append(f"#   compare {c['name']} [{c['mode']}]: {c['a']} vs {c['b']}")
        if st.argv and st.kind != "skipped":
            out.append(fmt_argv(st.argv))
    return out


def status_lines(cycle: Cycle) -> list[str]:
    out = header(cycle)
    for st in cycle.steps():
        line = f"{step_key(st):6s} {st.state:8s}"
        if st.output:
            line += f" {st.output}"
        r = cycle.receipt(st.run_dir)
        if r is not None:
            peak = (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20
            line += (f" | receipt {st.run_dir}: {r.get('outcome')} exit {r.get('exit_code')} "
                     f"{r.get('wall_seconds', 0):.1f} s peak {peak} MiB")
        if st.note:
            line += f" | {st.note}"
        out.append(line)
        if st.phase in ("nav", "ref") and st.done:
            summ = cycle.res.read_json(f"{st.output}/summary.json") or {}
            scen = summ.get("primary_scenario")
            csv = f"{st.output}/daily_{scen}.csv"
            if scen and cycle.res.exists(csv):
                out.append(f"       primary daily CSV {csv} sha256 {cycle.res.sha(csv)}")
    return out


def window_id() -> str:
    return research_tree.window_id()


def git_scoped(root: Path) -> tuple[list[str], list[str]]:
    """(blocking, ignored) dirty paths: inside the code pathspec, outside it. Git failing is a blocking entry."""
    try:
        return research_tree.dirty_paths(root)
    except (OSError, subprocess.CalledProcessError) as exc:
        return [f"<git status failed: {exc}>"], []


def git_clean(root: Path) -> bool:
    return not git_scoped(root)[0]


def check_clean(cycle: Cycle, clean, log, seen: set) -> None:
    """The clean-tree rule before a phase runs: `clean(root)` returns (blocking, ignored) or a bool."""
    if clean is None:
        return
    got = clean(cycle.res.root)
    blocking, ignored = (([] if got else ["<dirty>"]), []) if isinstance(got, bool) else got
    if blocking:
        raise CycleError(f"tree at {cycle.res.root} is not clean in the code pathspec: {', '.join(blocking[:8])}: "
                         "commit first (the bounded runner refuses a dirty tree)")
    new = [p for p in ignored if p not in seen]
    if new:
        seen.update(new)
        log(f"# dirty outside the code pathspec (listed, not a stop): {', '.join(new[:12])}"
            f"{' ...' if len(new) > 12 else ''}")


def execute(argv: list[str], root: Path, env: dict, capture: bool) -> subprocess.CompletedProcess:
    return subprocess.run(argv, cwd=root, env=env, capture_output=capture, text=True)


def run_cycle(cycle: Cycle, *, stop_after: str | None = None, log=print, executor=execute,
              clean=git_scoped) -> int:
    root = cycle.res.root
    clean = None if cycle.no_git else clean   # K3: a root outside any repository has no tree to check
    seen: set = set()
    check_clean(cycle, clean, log, seen)
    env = cycle.env()
    for line in header(cycle):
        log(line)
    timings: dict = {}                  # phase -> wall seconds of a direct phase run by this invocation
    phase_idx = 0
    while True:
        steps = cycle.steps()           # re-resolved after every phase: later pins come from the files just written
        if phase_idx >= len(steps):
            break
        st = steps[phase_idx]
        key = step_key(st)              # the phase, or phase:role for an era of a roles: cycle (H-1)
        if st.state == "failed":
            raise CycleError(f"HARD-STOP [{key}]: {st.note}")
        if st.state == "skipped":
            log(f"== {key}: skipped ({st.note})")
        elif st.done:
            log(f"== {key}: done ({st.output})")
            if st.phase == "fields":
                fields_check(st.cycle or cycle, f"{st.output}/manifest.json", log)
            if st.phase == "nav":           # review C-13: scored only when made from this spec
                try:
                    log(f"   binding: {cycle_resume.check_binding(cycle, st)}")
                except cycle_resume.ResumeError as exc:
                    raise CycleError(f"HARD-STOP [{key}]: {exc}", EXIT_PIN) from exc
        elif st.kind == "internal":
            w_dir = next(x.output for x in steps if x.phase == "fit")
            admission_trials(cycle, w_dir, log)
            try:
                gate(cycle, w_dir, log)
            except CycleError:
                write_verdict(cycle, timings, log)  # a failed gate still records the admission rows
                raise
        elif st.kind == "compare":
            compare(cycle, st, log)
        else:
            if "<sha256:" in " ".join(st.argv):
                raise CycleError(f"HARD-STOP [{key}]: an upstream pin is unresolved (upstream output missing)")
            if st.output and st.state == "pending" and st.phase != "fit" and cycle.res.exists_dir(st.output):
                raise CycleError(f"HARD-STOP [{key}]: output {st.output} exists (never overwritten)")
            check_clean(cycle, clean, log, seen)
            if st.phase == "summ" and cycle.spec.get("verdict"):
                cycle.res.path(cycle.cycle_dir()).mkdir(parents=True, exist_ok=True)   # nav_summ --json target
            log(f"== {key}" + (f" (attempt {st.attempt})" if st.attempt else ""))
            log(fmt_argv(st.argv))
            started = time.monotonic()
            done = executor(st.argv, root, env, st.kind == "bounded")
            timings[key] = {"seconds": time.monotonic() - started, "run_dir": st.run_dir}
            if st.kind == "bounded":
                r = cycle.receipt(st.run_dir)
                if r is None:
                    tail = (done.stderr or "")[-400:]
                    raise CycleError(f"HARD-STOP [{key}]: no receipt in {st.run_dir} (runner exit "
                                     f"{done.returncode}) {tail}")
                peak = (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20
                log(f"   receipt: outcome {r.get('outcome')} exit {r.get('exit_code')} "
                    f"{r.get('wall_seconds', 0):.1f} s peak {peak} MiB")
                if st.phase == "fit" and r.get("exit_code") == FIT_INCOMPLETE and r.get("outcome") == "process-error":
                    if st.attempt >= int(cycle.spec["fit"].get("max_passes", 3)):
                        raise CycleError(f"HARD-STOP [fit]: still incomplete after {st.attempt} passes")
                    log("   fit incomplete (exit 3, the documented resume protocol): next pass resumes")
                    continue
                if r.get("outcome") != "completed" or r.get("exit_code") != 0:
                    raise CycleError(f"HARD-STOP [{key}]: receipt {st.run_dir}: outcome {r.get('outcome')}, "
                                     f"exit_code {r.get('exit_code')}{', ' + r['error'] if r.get('error') else ''}")
            else:
                if st.kind == "direct" and done.returncode != 0:
                    raise CycleError(f"HARD-STOP [{key}]: exit {done.returncode}")
            post = cycle.steps()[phase_idx]
            if st.phase in ("u", "w", "fit", "nav", "ref", "fields", "card", "monitor", "marginal") and not post.done:
                raise CycleError(f"HARD-STOP [{key}]: exit 0 but its output is incomplete ({st.output})")
            if st.phase == "fields":
                fields_check(st.cycle or cycle, f"{st.output}/manifest.json", log)
            if st.phase == "nav":           # review C-13: what a later resume checks before scoring it
                cycle_resume.write_binding(cycle, st)
            if st.phase == "summ":
                copy_ledger(cycle, log)
        # --stop-after PHASE stops after the last step of that phase (every era's, in a roles: cycle)
        if stop_after == st.phase and not any(x.phase == st.phase for x in steps[phase_idx + 1:]):
            log(f"== stopped after {st.phase} (--stop-after)")
            return EXIT_OK
        phase_idx += 1
    write_verdict(cycle, timings, log)
    log("== screen complete: w, nav, monitor and summ run with the full `run`" if cycle.screen else
        "== cycle complete")
    return EXIT_OK


def admission_trials(cycle, w_dir: str, log) -> None:
    """Review C-7: the gate of a v8 cycle with a ledger of record (--ledger, else summ.ledger) first ledgers the
    admission trials it reads, one chained line per listed candidate (cycle_admission.py); a v7 cycle writes none."""
    rel = cycle.ledger or (cycle.spec.get("summ") or {}).get("ledger")
    if not rel or summ_protocol(cycle.spec) != SUMM_V8:
        return
    try:
        cycle_admission.ledger_admissions(cycle, rel, w_dir, log)
    except ValueError as exc:
        raise CycleError(f"HARD-STOP [gate]: admission trials not ledgered: {exc}") from exc


def write_verdict(cycle: Cycle, timings: dict, log) -> dict:
    try:
        return _write_verdict(cycle, timings, sha256_file(cycle.spec_path) if cycle.spec_path else None, log,
                              ledger_state(cycle))
    except ValueError as exc:     # review C-1: no verdict DSR from a cell count; C-6: a broken ledger chain
        raise CycleError(f"HARD-STOP [verdict]: {exc}") from exc


def ledger_state(cycle) -> dict | None:
    """{path, head, lines} of the sprint ledger of record (--ledger, else summ.ledger) when it exists: the chain head
    (backtest_integrity.ledger_head, the chain verified) every verdict records (review C-6). None without one."""
    rel = cycle.ledger or (cycle.spec.get("summ") or {}).get("ledger")
    if not rel or not cycle.res.path(rel).is_file():
        return None
    bi = research_ledger.backtest_integrity()
    p = cycle.res.path(rel)
    return {"path": rel, "head": bi.ledger_head(p), "lines": len(bi.ledger_read(p))}


def copy_ledger(cycle: Cycle, log) -> None:
    """summ.ledger_copy: the trial ledger copied (e.g. into the sprint directory) after the cycle's summ, with its chain
    head logged (review C-6)."""
    sm = cycle.spec["summ"]
    if not sm.get("ledger_copy"):
        return
    src, dst = cycle.res.path(cycle.ledger or sm["ledger"]), cycle.res.path(sm["ledger_copy"])
    if not src.is_file():
        raise CycleError(f"HARD-STOP [summ]: summ.ledger_copy: no ledger at {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    try:
        head = research_ledger.backtest_integrity().ledger_head(dst)
    except ValueError as exc:
        raise CycleError(f"HARD-STOP [summ]: summ.ledger_copy: {exc}") from exc
    log(f"   ledger copied: {src} -> {sm['ledger_copy']} (sha256 {sha256_file(dst)}, chain head {head})")


# ------------------------------------------------------------------ lock
def lock(spec_path: Path, root: Path, relock: bool = False) -> tuple[dict, list[str]]:
    spec = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    if "roles" in spec:                    # H-1: the inputs and every role's role and fields pins
        import research_roles  # noqa: PLC0415  (imports this module)
        return research_roles.lock(spec, root, relock)
    validate_spec(spec)
    res = Resolver(root)
    notes: list[str] = []
    for key, item in spec["inputs"].items():
        lock_pin(res, item, "sha256", item["path"], key, relock, notes)
    return spec, notes


def lock_pin(res: Resolver, item: dict, field: str, rel: str, key: str, relock: bool, notes: list[str]) -> None:
    """item[field] = the SHA-256 of the file ``rel``: a null pin is filled, an equal one kept, a different one
    replaced only with ``relock`` (else exit 3); a missing file is exit 3."""
    got = res.sha(rel)
    if got is None:
        raise CycleError(f"lock: input {key} missing: {rel}", EXIT_PIN)
    if item.get(field) in (None, got):
        if item.get(field) is None:
            notes.append(f"locked {key}: {rel} {got}")
        item[field] = got
    elif relock:
        notes.append(f"RELOCKED {key}: {rel} {item[field]} -> {got}")
        item[field] = got
    else:
        raise CycleError(f"lock: {key} pin {item[field]} differs from the file ({got}); --relock to replace",
                         EXIT_PIN)


# ------------------------------------------------------------------ CLI
def make_cycle(res: Resolver, *, spec_path: Path, **kw):
    """The Cycle of SPEC; a spec with ``roles`` (task H-1) is the era loop of research_roles.py (one role: that
    role's single-role Cycle)."""
    try:
        raw = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CycleError(f"spec {spec_path}: {exc}", EXIT_USAGE) from exc
    if isinstance(raw, dict) and "roles" in raw:
        import research_roles  # noqa: PLC0415  (imports this module)
        return research_roles.roles_cycle(raw, res, spec_path=spec_path, **kw)
    return Cycle(load_spec(spec_path), res, spec_path=spec_path, **kw)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] == ["ledger-protocol"]:      # a protocol (window change) line: research_ledger.py
        return research_ledger.main(argv[1:])
    if argv[:1] == ["ledger-defect"]:        # review C-3: a ledgered cell found invalid afterwards
        return research_ledger.defect_main(argv[1:])
    if argv[:2] == ["cache", "gc"]:          # unreferenced candidate caches and fit work dirs: research_gc.py
        import research_gc  # noqa: PLC0415  (imports this module)
        return research_gc.main(argv[2:])
    if argv[:1] == ["add-alpha"]:            # registry entry, library, prereg stub, derived spec, lock
        import research_add_alpha  # noqa: PLC0415  (imports this module)
        return research_add_alpha.main(argv[1:])
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("verb", choices=("plan", "run", "status", "lock"))
    ap.add_argument("spec")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--suffix", default=None)
    ap.add_argument("--attempt", action="append", default=[], help="PHASE=N (u, fit, w, nav)")
    ap.add_argument("--reuse-fields", default=None, help="prior fields dir for prepare_research_fields --reuse")
    ap.add_argument("--ledger", default=None, help="trial ledger passed to nav_summ --ledger")
    ap.add_argument("--keep-fields", action="store_true", help="--suffix leaves the fields dir name unchanged")
    ap.add_argument("--runner-override", action="append", default=[], help="seconds|max_rss_mib|min_free_mib=N")
    ap.add_argument("--lines-only", action="store_true", help="plan: the command lines only")
    ap.add_argument("--stop-after", choices=STOP_PHASES, default=None)
    ap.add_argument("--relock", action="store_true", help="lock: replace pins that differ from the files")
    ap.add_argument("--write", action="store_true", help="lock: write the pins back into SPEC")
    ap.add_argument("--no-git", action="store_true", help="K3: a --root outside any git repository (test roots)")
    ap.add_argument("--screen", action="store_true", help="u, fit, card, marginal, gate; stop before w")
    a = ap.parse_args(argv)
    try:
        spec_path = find_spec(a.spec)
        if a.verb == "lock":
            spec, notes = lock(spec_path, a.root, a.relock)
            for n in notes:
                print(n)
            text = json.dumps(spec, indent=2) + "\n"
            if a.write:
                spec_path.write_text(text, encoding="utf-8", newline="\n")
                print(f"wrote {spec_path}")
            else:
                print(text, end="")
            return EXIT_OK
        if a.suffix is not None and (not a.suffix or any(c in a.suffix for c in "/\\ ")):
            raise CycleError("--suffix must be a non-empty name without separators or spaces", EXIT_USAGE)
        cycle = make_cycle(Resolver(a.root), suffix=a.suffix, attempts=parse_attempts(a.attempt),
                           reuse_fields=a.reuse_fields, ledger=a.ledger, spec_path=spec_path, keep_fields=a.keep_fields,
                           runner_overrides=parse_runner_overrides(a.runner_override), no_git=a.no_git,
                           screen=a.screen)
        if a.verb == "plan":
            print("\n".join(plan_lines(cycle, a.lines_only)))
            return EXIT_OK
        if a.verb == "status":
            print("\n".join(status_lines(cycle)))
            return EXIT_OK
        return run_cycle(cycle, stop_after=a.stop_after)
    except CycleError as exc:
        print(f"research_cycle: {exc}", file=sys.stderr)
        return exc.code


if __name__ == "__main__":
    # the lazily imported research_roles / research_gc / research_add_alpha `import research_cycle`: this module, so
    # their CycleError is the one main() catches (H-1)
    sys.modules.setdefault("research_cycle", sys.modules[__name__])
    sys.exit(main())
