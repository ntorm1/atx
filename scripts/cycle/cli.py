"""One research cycle driven by a spec file (platform v7 lane L2): replaces the hand-copied vNN_train.sh ladders.

  research_cycle.py plan   SPEC [--root R] [--suffix S [--keep-fields]] [--attempt PHASE=N ...] [--reuse-fields DIR]
                                [--ledger PATH] [--runner-override KEY=VALUE ...] [--lines-only] [--no-git] [--screen]
  research_cycle.py run    SPEC [same options] [--stop-after PHASE] [--auto-attempt]
                                [--admission-wait S [--host-budget-mib N]]
  research_cycle.py status SPEC [same options]
  research_cycle.py lock   SPEC [--root R] [--relock] [--write] [--exes]
  research_cycle.py add-alpha --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation "..." --origin prior
                              --parent v71 [--name v72] [--plan-json PATH] [--root R]   (research_add_alpha.py)
  research_cycle.py cache gc --keep-referenced-by SPEC [SPEC ...] [--under DIR] [--root R] [--apply]
                           (research_gc.py: stores no listed spec uses; deleted only with --apply)
  research_cycle.py ledger-protocol --ledger PATH --owner-ruling TEXT --date D [--window-id ID] [--root R]
                           (research_ledger.py: a window-change line that is no trial; count 0, no cell)
  research_cycle.py ledger-defect --ledger PATH --trial-id TID --reason TEXT --ruling ID --date D [--root R]
                           (research_ledger.py, review C-3: the ledgered cell TID is invalid; count 0, no cell; reviews
                           F-1, F-5: the owner ruling's id and date; a blind or returns re-run of TID needs the ruling)
  research_cycle.py ledger-campaign --ledger PATH --campaign DIR [--date D] [--root R]
                           (research_ledger.py, Ruling E-33: a mine output's campaign line; count 0, registry count)
  research_cycle.py mine {lock,pool,probe,plan,run} SPEC [--root R] [...]
                           (research_mine.py, v9 MINE-RUN: one mined campaign as a pinned cell, ledgered on completion)

Platform v8 (lane A) additions, each off unless the spec or the command line asks for it:
  --screen        run: fields, check, u (+ --no-composition when the IC exe offers it), fit, card, marginal (the exe's
                  marginal verb, contract K6, when offered), gate; then stop before w and write cycle_verdict.json
                  (admission and marginal rows, per-phase seconds and peak MiB) into the cycle dir
                  <out_root or build-equity>/cycle-<name>[-suffix]/; a full run writes it too (with the paired dSR,
                  DSR N and PBO blocks read from nav_summ --json / --pbo-json when the spec sets "verdict": true)
  --auto-attempt  (P9 OR-4) a bounded step whose latest attempt the host refused (cycle_resume.AUTO_OUTCOMES: the
                  memory floor at launch or while running, a launch-admission timeout) and whose output is absent runs
                  again in <run dir>/attempt-k (k = 2, 3, ... <= MAX_ATTEMPTS) on the next invocation; without the flag
                  that state is a stop, as before. A step whose run dir holds attempt-k sub-dirs is read from the
                  latest (its receipt and NAV binding); attempt 1 keeps the run dir of before
  resume argv     (P9 OR-3, K-P9-10) a done bounded output (u, fit, w, card, marginal, every-phase fields / monitor) is
                  reused only when its run receipt's command equals the one this spec would run now
                  (cycle_resume.REUSE_NEUTRAL options aside); a receipt written before K-P9-10 is reused as before.
                  Rulings E1-REUSE (a), E1-REUSE-a2: a done bounded output (nav, ref too) whose K-P9-10 receipt names
                  another executable SHA than the spec's exes_sha256 pin of that exe (for an output a template
                  inherited: the parent's pin) is refused (exit 3); without a pin, a mismatch with the exe on disk is
                  reused and logged (`receipt exe NOTE:`)
  --admission-wait S  (P9 F-5 (a)) every bounded process waits, at most S seconds, before its launch for free memory
                  >= its declared peak + floor and no compiler (run_bounded_research --admission-wait-seconds)
  --host-budget-mib N (P9 OR section 5; needs --admission-wait) the host memory semaphore over declared caps (runner
                  --host-budget-mib), and the steps that read nothing of each other run side by side under it
                  (PARALLEL: ref || u, card || marginal); a phase cap above N is refused when planned. Each launch
                  checks free memory inside the semaphore's lock, net of the other claims' unallocated caps, so a
                  pair never over-commits; N bounds the sum of declared caps: set it no higher than the host's free
                  memory at wave start
  --keep-verdicts (P9 OR section 3) every cycle_verdict.json write also leaves its bytes in
                  <cycle dir>/verdicts/<mode>-<k>.json, never overwritten (a receipt's verdict pin never dangles)
  exes_sha256     (P9 OR-2) {exe key: SHA-256} of the exes the cell runs (effective exes: ic, nav), written by
                  `lock --exes` (a template's into change.set, over the pins its parent's spec carries); plan and run stop
                  (exit 3) when an exe no longer hashes to its pin; absent, exes are pinned by path only, as before
  marginal.candidates (P9 ruling P4) [member id, ...]: the marginal verb gets --candidates FILE, the ids one per line
                  (UTF-8) in <cycle dir>/marginal-candidates-<sha12>.txt, written when the step starts and bound in its
                  receipt; set by a wave whose manifest's marginal ruling has candidates_only; absent, as before
  --no-git       (contract K3) only for a --root outside any git repository: no clean check, the bounded runner gets
                  --root R --no-git, and a relative tool path (runner, builder, fit, card, monitor, summ scripts) that
                  is absent under R resolves to this worktree's copy
  clean check     scoped to the code pathspec (research_tree.CODE_PATHSPEC), before every executed phase; dirty paths
                  outside it (lane reports under .superpowers/) are listed, never a stop
  stage inputs    sec_identity_bridge + earnings_calendar + insider + sec_filings (all four or none; the three stage dirs
                  share one parent -> --sec-stages), thirteenf, ftd, regsho_threshold, security_master, short_volume_ext
                  (--<key> DIR --<key>-sha256 PIN) and reuse_fields (--reuse DIR --reuse-sha256 PIN, --reuse-hardlink
                  with fields.reuse_hardlink) reach the fields builder
  summ.dsr_n      "ledger+1" resolves at scoring time to nav_summ's N (backtest_integrity.ledger_n: the defect-rule
                  construction trials, protocol lines and window re-runs 0, + 1 for this cell when not yet ledgered,
                  0 for a history read, Ruling E-41; research_ledger.py); summ.ledger_copy copies the ledger (into the sprint dir) after summ
  resume         (review C-13) a NAV step run by the cycle records <run dir>/cycle_binding.json (spec digest and argv
                  SHA-256); a done NAV is scored only when its spec digest matches the current spec's AND its argv
                  digest (from the binding or its receipt's command) the NAV command the spec runs now (review F-9: a
                  template's spec digest, also the verdict's spec_sha256, covers every parent up its chain,
                  research_spec.spec_digest): a mismatch is a pin stop (exit 3) naming both; the ref phase's NAV
                  records and is checked on its argv digest alone (review F-6)
  admission lines (review C-7) the gate of a v8 cycle with a ledger first appends one chained admission line per
                  listed candidate (cycle_admission.py): the ledger's admission trials of v8 Appendix A
  summ.origin     (review C-2) prior | grid | mined, the cell's origin class (contract K5). A v8 scoring step (a
                  "verdict": true spec, or --protocol v8 in summ.extra) always runs nav_summ --protocol v8 (seed
                  20260929, 4,999 draws, block 21: summ.extra may not set --seed, --draws or --block, review F-2)
                  and, with a ledger, --origin summ.origin; a verdict spec's summ also passes
                  --dsr-ledger <the ledger> and the verdict's DSR is nav_summ's deflated_ledger (review C-1)
  build          "equity" | "equity-rel": resolves exes (defaults or bare names) and env_path_prepend (BUILDS)
  out_root        a root-relative dir every relative output name is placed under; ic.cache and fit.work_dir, when
                  omitted, derive to <out_root or build-equity>/{candidate-cache,fit-work}/<role sha16>-<window id>
                  (shared, content-keyed; never suffixed)
  runner.phases   {phase: {seconds, max_rss_mib, min_free_mib}} per-phase caps over the runner's; absent, the OD-2 rule
                  table RUNNER_PHASE_RULES applies (u and w: 300 s / 2,560 MiB on a role of more than 1,200 dates); a
                  time cap (here or runner.seconds) above research_tree.RUNNER_MAX_SECONDS is refused at load (P9 OR-1)
  receipts        "every-phase": the direct phases (fields, check, monitor, summ) run through the bounded runner too
  ref             skipped when this cycle's fields manifest SHA equals inputs.baseline_fields (the parent's fields),
                  unless the NAV exe's SHA-256 differs from the executable_sha256 of the parent NAV's completed run
                  receipt (inputs.reference_cell; P9 OR-2): then the reference construction runs
  nav, ref        done when summary.json exists and, for an argv with --capacity-curve, capacity_curve.csv and
                  v7_extras.json too (written after the summary; P9 NV-4); a summary without them is a failed attempt
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
  templates      a SPEC of schema atx.research-cycle-template/v1 is a cell written as its parent's spec plus the
                  registered change (research_spec.py: parent, nominal_parent, requires, change, locked); plan works on
                  the nominal parent while parent is null; run refuses a null parent, an open "requires" up the chain,
                  or a "<fill:...>" value; lock pins the inputs the template adds or derives
  fields pin      fields.manifest_sha256 null (an as-built dir not yet locked): computed from the file at plan time
                  (UNLOCKED), written by `lock --write`
  ic.w_flags      {"--opt": "value" | true | false} applied to ic.flags for the weighted (w) pass only (research_spec
                  apply_flags), e.g. Ruling E-28's --max-memory-mib 3072 for a theme_standardise composition
  fit.flags       (Ruling E-27b) a v8 spec refuses --composition ew-theme-aim-v1 (the v5 rule); R-3 on an ew-theme-v1
                  parent fits ew-theme-aim-v2

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
          must hold every one of them. "keys_in" (a second library JSON, with "keys") keeps the keys it also lists
          (add-alpha --replaces: the parent's members the child still holds)

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
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import time

import cycle_resume
import research_ledger
import research_spec
import research_tree
from cycle.compare import compare
from cycle.gate import admission_trials, copy_ledger, fields_check, gate, write_verdict
from cycle.lock import lock
from cycle.phases import Cycle, Step
from cycle.resolver import Resolver
from cycle.spec import (COMPARE_SUFFIX, EXES_PIN, EXIT_OK, EXIT_PIN, EXIT_USAGE, FIT_INCOMPLETE, PARALLEL, STOP_PHASES,
                        SCRIPTS_DIR, CycleError, find_spec, load_spec, parse_attempts, parse_runner_overrides)
from cycle_verdict import step_key
from research_common import dirty_or_error, execute, fmt_argv, sha256_file


# ------------------------------------------------------------------ plan / status / run
def header(cycle: Cycle) -> list[str]:
    spec_sha = sha256_file(cycle.spec_path) if cycle.spec_path else None
    cell = cycle_resume.spec_digest(cycle)      # review F-9: a template's digest covers its parent chain
    chain = f" (spec digest over the template chain {cell})" if cell and cell != spec_sha else ""
    lines = [f"# research_cycle {cycle.spec['name']}: spec {cycle.spec_path} sha256 {spec_sha}{chain}; "
             f"root {cycle.res.root}; "
             f"suffix {cycle.suffix or 'none'}{' (fields kept)' if cycle.keep_fields else ''}; attempts "
             f"{cycle.attempts or 'auto'}; runner overrides {cycle.runner_overrides or 'none'}"
             f"{'; --no-git' if cycle.no_git else ''}{'; --screen' if cycle.screen else ''}"
             f"{'; --auto-attempt' if getattr(cycle, 'auto_attempt', False) else ''}"
             f"{'; --keep-verdicts' if getattr(cycle, 'keep_verdicts', False) else ''}"
             + "".join(f"; {k} {v}" for k, v in (getattr(cycle, "launch", None) or {}).items())]
    for key, (rel, sha, how) in cycle.pins.items():
        lines.append(f"# pin {key}: {rel} {sha} [{how}]")
    for key, (rel, sha) in (getattr(cycle, "exe_pins", None) or {}).items():   # P9 OR-2 (none without the pins)
        lines.append(f"# pin exe {key}: {rel} {sha} [{EXES_PIN}, verified]")
    return lines + research_spec.header_lines(cycle.spec, cycle.spec_path, research_tree.REPO)


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


def git_scoped(root: Path) -> tuple[list[str], list[str]]:
    """(blocking, ignored) dirty paths: inside the code pathspec, outside it. Git failing is a blocking entry."""
    blocking, ignored, error = dirty_or_error(root)
    return ([f"<git status failed: {error}>"], []) if error is not None else (blocking, ignored)


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


def parallel_partner(cycle: Cycle, steps: list[Step], idx: int, stop_after: str | None) -> int | None:
    """P9 OR section 5: the index of the step that runs side by side with ``steps[idx]``, or None. Only under a host
    memory budget (--host-budget-mib: every bounded process holds its declared cap in the host semaphore), only for a
    pending bounded step of a PARALLEL key phase whose next step that is not one of its own compare steps (nor skipped)
    is the pending bounded step of the paired phase, of the same era, with every pin resolved; never past --stop-after."""
    st = steps[idx]
    want = PARALLEL.get(st.phase)
    if not cycle.launch.get("host_budget_mib") or want is None or st.kind != "bounded" or st.state != "pending" or \
            stop_after in (st.phase, st.phase + COMPARE_SUFFIX):
        return None
    for j in range(idx + 1, len(steps)):
        x = steps[j]
        if x.phase == st.phase + COMPARE_SUFFIX or x.state == "skipped":
            continue
        ok = x.phase == want and x.kind == "bounded" and x.state == "pending" and x.role == st.role and \
            "<sha256:" not in " ".join(x.argv)
        return j if ok else None
    return None


def start_step(cycle: Cycle, st: Step, key: str, clean, log, seen: set) -> None:
    """Before a process step runs: no unresolved upstream pin, no existing output (never overwritten), a clean tree;
    then its log lines."""
    if "<sha256:" in " ".join(st.argv):
        raise CycleError(f"HARD-STOP [{key}]: an upstream pin is unresolved (upstream output missing)")
    if st.output and st.state == "pending" and st.phase != "fit" and cycle.res.exists_dir(st.output):
        raise CycleError(f"HARD-STOP [{key}]: output {st.output} exists (never overwritten)")
    check_clean(cycle, clean, log, seen)
    if st.phase == "summ" and cycle.spec.get("verdict"):
        cycle.res.path(cycle.cycle_dir()).mkdir(parents=True, exist_ok=True)   # nav_summ --json target
    for rel, text in st.writes.items():   # e.g. the marginal --candidates FILE (P9 ruling P4): its name carries its SHA
        dest = cycle.res.path(rel)
        if dest.is_file() and dest.read_bytes() != text.encode("utf-8"):
            raise CycleError(f"HARD-STOP [{key}]: {rel} exists with other bytes (never overwritten)")
        if not dest.is_file():
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(text.encode("utf-8"))
    log(f"== {key}" + (f" (attempt {st.attempt})" if st.attempt else ""))
    log(fmt_argv(st.argv))


def run_processes(pair: list, root: Path, env: dict, executor) -> list[tuple]:
    """[(CompletedProcess, wall seconds)] of each (step, key): one step runs in this thread, as always; a parallel
    pair in two threads (each bounded process waits for its own claim in the host semaphore)."""
    def one(item) -> tuple:
        started = time.monotonic()
        done = executor(item[0].argv, root, env, item[0].kind == "bounded")
        return done, time.monotonic() - started
    if len(pair) == 1:
        return [one(pair[0])]
    with ThreadPoolExecutor(max_workers=len(pair)) as pool:
        return list(pool.map(one, pair))


def step_outcome(cycle: Cycle, st: Step, key: str, done, log) -> bool:
    """A process step's result from its receipt (bounded) or exit code (direct); raises CycleError (HARD-STOP) on a
    refusal or failure; True when the fit exited "incomplete" and its next pass runs (FIT_INCOMPLETE)."""
    if st.kind != "bounded":
        if st.kind == "direct" and done.returncode != 0:
            raise CycleError(f"HARD-STOP [{key}]: exit {done.returncode}")
        return False
    r = cycle.receipt(st.run_dir)
    if r is None:
        tail = (done.stderr or "")[-400:]
        raise CycleError(f"HARD-STOP [{key}]: no receipt in {st.run_dir} (runner exit {done.returncode}) {tail}")
    peak = (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20
    log(f"   receipt: outcome {r.get('outcome')} exit {r.get('exit_code')} {r.get('wall_seconds', 0):.1f} s peak "
        f"{peak} MiB")
    if st.phase == "fit" and r.get("exit_code") == FIT_INCOMPLETE and r.get("outcome") == "process-error":
        if st.attempt >= int(cycle.spec["fit"].get("max_passes", 3)):
            raise CycleError(f"HARD-STOP [fit]: still incomplete after {st.attempt} passes")
        log("   fit incomplete (exit 3, the documented resume protocol): next pass resumes")
        return True
    if r.get("outcome") != "completed" or r.get("exit_code") != 0:
        retry = r.get("outcome") in cycle_resume.AUTO_OUTCOMES and st.output and \
            not cycle.res.exists_dir(st.output)   # P9 OR-4: the host refused it, nothing was written
        raise CycleError(f"HARD-STOP [{key}]: receipt {st.run_dir}: outcome {r.get('outcome')}, "
                         f"exit_code {r.get('exit_code')}{', ' + r['error'] if r.get('error') else ''}"
                         + ("; nothing was written: a resume with --auto-attempt runs the next attempt"
                            if retry else ""))
    return False


def finish_step(cycle: Cycle, st: Step, key: str, idx: int, log) -> None:
    """After a process step succeeded: its output is complete (the step re-resolved at its index ``idx``), the fields
    check, a NAV's binding (review C-13, F-6), the summ's ledger copy."""
    post = cycle.steps()[idx]
    if st.phase in ("u", "w", "fit", "nav", "ref", "fields", "card", "monitor", "marginal") and not post.done:
        raise CycleError(f"HARD-STOP [{key}]: exit 0 but its output is incomplete ({st.output})")
    if st.phase == "fields":
        fields_check(st.cycle or cycle, f"{st.output}/manifest.json", log)
    if st.phase in ("nav", "ref"):  # review C-13 (F-6: ref too): what a later resume checks
        cycle_resume.write_binding(cycle, st)
    if st.phase == "summ":
        copy_ledger(cycle, log)


def run_cycle(cycle: Cycle, *, stop_after: str | None = None, log=print, executor=execute,
              clean=git_scoped) -> int:
    root = cycle.res.root
    refusal = research_spec.run_refusal(cycle.spec, cycle.spec_path, research_tree.REPO)
    if refusal:                               # a template's null parent, open precondition or value to fill
        raise CycleError(refusal, EXIT_PIN)
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
            try:
                if st.phase in ("nav", "ref"):  # review C-13: scored only when made from this spec (F-6: ref
                    log(f"   binding: {cycle_resume.check_binding(cycle, st)}")   # compared only when made by the
                elif st.kind == "bounded":      # NAV command the spec runs now); P9 OR-3: every other bounded output
                    how = cycle_resume.check_receipt_argv(cycle, st)               # reused only on the same argv
                    if how:
                        log(f"   receipt argv: {how}")
                if st.kind == "bounded":        # E1-REUSE-a2: a pinned exe refuses another; unpinned: noted
                    exe, note = cycle_resume.check_receipt_exe(cycle, st)
                    if exe:
                        log(f"   receipt exe: {exe}")
                    if note:
                        log(f"   receipt exe NOTE: {note}")
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
            pair = [(st, key, phase_idx)]   # P9 OR section 5: with its PARALLEL partner under a host memory budget
            j = parallel_partner(cycle, steps, phase_idx, stop_after)
            if j is not None:
                pair.append((steps[j], step_key(steps[j]), j))
            for x, k, _ in pair:
                start_step(cycle, x, k, clean, log, seen)
            if j is not None:
                log(f"   {key} || {pair[1][1]}: side by side under the host memory budget "
                    f"{cycle.launch['host_budget_mib']} MiB")
            results = run_processes(pair, root, env, executor)
            for (x, k, _), (done, seconds) in zip(pair, results):
                timings[k] = {"seconds": seconds, "run_dir": x.run_dir}
            again = False
            for (x, k, i), (done, _) in zip(pair, results):
                if step_outcome(cycle, x, k, done, log):
                    again = True        # the fit's next pass (the fit is never paired)
                    break
                finish_step(cycle, x, k, i, log)
            if again:
                continue
        # --stop-after PHASE stops after the last step of that phase (every era's, in a roles: cycle)
        if stop_after == st.phase and not any(x.phase == st.phase for x in steps[phase_idx + 1:]):
            log(f"== stopped after {st.phase} (--stop-after)")
            return EXIT_OK
        phase_idx += 1
    write_verdict(cycle, timings, log)
    log("== screen complete: w, nav, monitor and summ run with the full `run`" if cycle.screen else
        "== cycle complete")
    return EXIT_OK


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
    if argv[:1] == ["ledger-campaign"]:      # Ruling E-33: a mining campaign's line (atx-equity-strategy-mine)
        return research_ledger.campaign_main(argv[1:])
    if argv[:2] == ["cache", "gc"]:         # unreferenced candidate caches and fit work dirs: research_gc.py
        import research_gc  # noqa: PLC0415  (imports this module)
        return research_gc.main(argv[2:])
    if argv[:1] == ["add-alpha"]:            # registry entry, library, prereg stub, derived spec, lock
        import research_add_alpha  # noqa: PLC0415  (imports this module)
        return research_add_alpha.main(argv[1:])
    if argv[:1] == ["mine"]:                 # v9 MINE-RUN: a mined campaign cell (atx-equity-strategy-mine)
        import research_mine  # noqa: PLC0415  (imports this module)
        return research_mine.main(argv[1:])
    if argv[:1] == ["wave"]:                 # v8 YINFRA: one research wave, one command (research_wave.py)
        import research_wave  # noqa: PLC0415  (imports this module)
        return research_wave.main(argv[1:])
    if argv[:1] == ["candidates"]:           # v8 YINFRA: the candidate queue (wave_queue.py)
        import wave_queue  # noqa: PLC0415
        return wave_queue.main(argv[1:])
    if argv[:1] == ["scoreboard"]:           # v8 YINFRA: the lineage of accepted books (wave_scoreboard.py)
        import wave_scoreboard  # noqa: PLC0415
        return wave_scoreboard.main(argv[1:])
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("verb", choices=("plan", "run", "status", "lock"))
    ap.add_argument("spec")
    ap.add_argument("--root", type=Path, default=SCRIPTS_DIR.parent)
    ap.add_argument("--suffix", default=None)
    ap.add_argument("--attempt", action="append", default=[], help="PHASE=N (u, fit, w, nav)")
    ap.add_argument("--reuse-fields", default=None, help="prior fields dir for prepare_research_fields --reuse")
    ap.add_argument("--ledger", default=None, help="trial ledger passed to nav_summ --ledger")
    ap.add_argument("--keep-fields", action="store_true", help="--suffix leaves the fields dir name unchanged")
    ap.add_argument("--runner-override", action="append", default=[], help="seconds|max_rss_mib|min_free_mib=N")
    ap.add_argument("--lines-only", action="store_true", help="plan: the command lines only")
    ap.add_argument("--stop-after", choices=STOP_PHASES, default=None)
    ap.add_argument("--auto-attempt", action="store_true",
                    help="P9 OR-4: a step the host refused (memory) with nothing written runs again in <run dir>/"
                         "attempt-k (the wave passes it when its manifest's driver.auto_attempt is true)")
    ap.add_argument("--keep-verdicts", action="store_true",
                    help="P9 OR section 3: also write each verdict to <cycle dir>/verdicts/<mode>-<k>.json (never "
                         "overwritten)")
    ap.add_argument("--admission-wait", type=float, default=None, metavar="SECONDS",
                    help="P9 F-5 (a): every bounded process waits (bounded) for free memory and no compiler")
    ap.add_argument("--host-budget-mib", type=int, default=None, metavar="N",
                    help="P9 OR section 5: the host memory semaphore (needs --admission-wait); PARALLEL steps side by "
                         "side")
    ap.add_argument("--relock", action="store_true", help="lock: replace pins that differ from the files")
    ap.add_argument("--write", action="store_true", help="lock: write the pins back into SPEC")
    ap.add_argument("--exes", action="store_true", help="lock: also pin the exes' SHA-256 (exes_sha256, P9 OR-2)")
    ap.add_argument("--no-git", action="store_true", help="K3: a --root outside any git repository (test roots)")
    ap.add_argument("--screen", action="store_true", help="u, fit, card, marginal, gate; stop before w")
    a = ap.parse_args(argv)
    try:
        spec_path = find_spec(a.spec)
        if a.verb == "lock":
            spec, notes = lock(spec_path, a.root, a.relock, a.exes)
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
        refusal = research_tree.launch_refusal(a.admission_wait, a.host_budget_mib)
        if refusal:
            raise CycleError(f"--admission-wait / --host-budget-mib: {refusal}", EXIT_USAGE)
        launch = {"admission_wait_seconds": a.admission_wait, "host_budget_mib": a.host_budget_mib}
        cycle = make_cycle(Resolver(a.root), suffix=a.suffix, attempts=parse_attempts(a.attempt),
                           reuse_fields=a.reuse_fields, ledger=a.ledger, spec_path=spec_path, keep_fields=a.keep_fields,
                           runner_overrides=parse_runner_overrides(a.runner_override), no_git=a.no_git,
                           screen=a.screen, auto_attempt=a.auto_attempt, launch=launch,
                           keep_verdicts=a.keep_verdicts)
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
