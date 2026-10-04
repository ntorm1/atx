# P9 resume rulings (PM, 2026-10-03, after owner re-issued the goal)

Binding. Format: `Ruling <ID>: decision -- why -- cost if wrong`. These answer every open question in `status-3.md`.
Standing rules in `status-3.md` "How the owner wants it run" still apply to every agent.

## All agents

- Ruling RESUME: owner stop is lifted by the new /goal; all parked work resumes from its handoff file, nothing restarts
  -- the handoffs are the recovery map -- a restarted lane wastes a day.
- Ruling W2-BUILD: every wave-2 lane may build its own named targets through `scripts/research-build.ps1` in its own
  pool (tag `p9-<lane>-<letter>`), one target-scoped build at a time, only when the memory gate admits; if the gate
  refuses, wait and retry (bounded, up to ~30 min total), never bypass, never raw cmake / ninja. Root's builds in
  pool-2 have priority: a lane that sees the gate busy backs off -- uncompiled C++ arriving at merge costs root far
  more than a serialized lane build -- builds queue and lanes slow down.
- Ruling DISK: an agent deletes its own regenerable scratch when done (pytest tmp dirs, `__pycache__`, superseded
  build-tag object dirs it created, review-package temp files). Never delete: anything under `C:/atx`, `atx-db/`,
  any `build-equity/trials.jsonl`, any pinned / expected / golden artefact, the X-5 / Y-1 reference outputs, caches
  named by a pin, another agent's tree -- owner asked for disk hygiene -- a wrongly deleted regenerable costs a rebuild.
- No TDD: implement first, then the tests the brief names. Commit trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. No subagents. Kill your own child processes. PY-HYG:
  pytest with explicit paths only.

## Root M1d / P9-B0

- Ruling B0-Y1: the P9-B0 substitution list is extended, before the run and by code reading only (no output read), with
  (a) Y-1 `vol_target` rows and (b) the L1-L4 book blocks, but only those files / JSON paths root shows are computed
  through the C1 sqrt kernel; root writes the enumerated paths into `integration-log.md` first. Comparison stays SHA +
  JSON path, no statistic read. A difference outside the written list is a stop -- C1's sqrt change legitimately
  reaches every vol-target book, the original list only named the X-5 NAV -- a too-wide list could hide a regression,
  bounded by the code-path proof per entry.

## Lanes

- Ruling SQL2-CLS: root adds new `atx.<name>/v<n>` schema literals to `classes.json` at merge, one commit per merge
  slot; lanes only list their new literals in the report -- one writer avoids N-way conflicts -- root forgets one and
  the classes guard goes red (cheap, visible).
- Ruling B2-1: Python-statistics deletion is a separate final commit on the lane branch, merged by root only after
  root's identity run passes -- keeps the identity comparison possible -- none beyond one extra merge step.
- Ruling B2-2: `paired.se` in C++ is whatever the Python statistic it replaces computes today (read the code, do not
  infer from the name); the port must agree to the P12 tolerance (1e-12) and decisions byte-equal. If the plan's
  K-P9-5 text names an estimator that differs from the Python code, implement the Python behaviour and flag it.
- Ruling B2-3: yes, the tie test adds `schema` to the eval_tie ledger records it writes (test fixtures only; the real
  trial ledger is untouched).
- Ruling C2-1: calibration record goes in `summary.json` (C1-PROD precedent), not `recipe.json`.
- Ruling C2-2: in engine mode the run stage may stop before the NAV, behind an explicit flag; flag-absent output stays
  byte-identical.
- Ruling C2-3: pass 2 missing tolerance publishes `matched: false` and the match stage stops (fail closed).
- Ruling C2-RED: M1c-RED fix is the contraction-free polynomial in `stats_ext.hpp`; no expected hash is edited; the
  two Release reds must go green in both Debug and Release.
- Ruling S2-1: re-pin both source digests without a semantics version bump, recorded in the report (S1-PIN precedent).
- Ruling S2-2: advertise label-terminal / min-coverage / ic-hac-rule in `exe_capabilities`; append-only cross-lane
  edit, listed in the report.
- Ruling S2-3: cache the missing-exit-v1 IC that runs beside `--label-terminal`, under its own cache identity.
- Ruling ALCOMB-1: follow E2: `rules:` live in the v2 wave manifest; the template schema is not widened.
- Ruling ALCOMB-2: no cross-lane edits beyond P17's three; anything else is listed in the report as a merge request
  for root.
- Ruling E2-1: do the K-P9-10 `--attempt k` argv change now.
- Ruling E2-2/3: the composition fixture (needs D2) and the NAV-rule / horizon fixtures (need C1) are root post-merge
  items; list the exact commands in the report; do not weaken or skip-mark other tests.
- Ruling ALSIG-1: verify WYZ `source_sample_end` against the paper citation (web lookup of the published paper is
  allowed); if it cannot be verified, mark the row `unverified` and leave it out of registration.
- Ruling ALSIG-2: `gia_13f` (literature rho .15 > .1 bar) is not registered; record it as excluded by the bar.
- Ruling ALSIG-3: `ENGINE_FIELDS` routing of engine-only rows is out of lane; note for root / A3.
- Ruling D2-1: `--engine-fit` writes the C++ values; the summary reports agreement with the Python fit under the P12
  tolerance; flag-absent stays byte-identical -- owner priority is core C++ -- a C++ / Python gap larger than P12 is a
  stop, not a tolerance change.
- Ruling D2-2: yes, CM-5 also fingerprints the composition modules' `module_sha256`.
- Ruling D2-3: yes, ew-theme-aim-v2 and the resid diagnostics need a C++ path before `composition_*.py` is deleted;
  if not reached in D2, the deletion moves to D3 (wave 3).
- Ruling A3-1: A3 may make the minimal routing edit in `prepare_research_fields_engine` for its six fields
  (cross-lane, listed in the report). A3-2: P13 untouched is accepted. A3-3: per-field re-hash under `--reuse`
  accepted, noted for wave 3. A3-4: ~320 KB fixture accepted.
- Ruling A3-RED: A3's claim that both M1a-RED gtests were wrong is not accepted on its word; its reviewer re-derives
  the expected values independently (numpy 1.26.4 + field spec).
- Ruling COV-1: no lane split (one worktree); order: engine recipe + kernel first, then the atx-impl verb and link.
- Ruling COV-2: memory limit is the verb's `--max-bytes`, passed into the kernel; kernel default when absent.
- Ruling COV-3: "asset-level MRAD" is per asset series. The COV-4 as-of trace (does the adjusted close embed anything
  after session d) is mandatory in the report (G-P11).
- Ruling T2-MIN: `t2_gold.py` outside the file list is accepted; the reviewer judges whether the bulk script edits are
  mechanical (accept) or behavioural (required fix).
