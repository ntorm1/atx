# atx platform v8 -- status 4 (PM session 4, owner stop, 2026-10-01)

Root `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`. Code head `9c5cfa0c` (build `v8-10`); the commits
after it are ledger, log and brief commits only. Tree clean. Ledger: `.superpowers/sdd/platform-v8-20260929/progress.md`
(rulings E-1..E-45, W0-a..n, R2-a..h, R7-a..c, PM3-1..10, PM4-1..15, all binding). Every merge, build, test and data
run: `integration-log.md`. Previous status: status 3 (`2026-09-30-platform-v8-status-3.md`). Goal prompt for the
next PM: `2026-10-01-platform-v8-next-goal-prompt-4.md`.

## 0. State in one paragraph

The owner asked to stop at the next logical break point and to build the v8 report only if enough information
existed. It does not: **no v8 cell has run, so no v8 metric exists and the scorecard and pitch cannot be built.**
This session did steps 1-3 of goal prompt 3 and half of step 4. Integration 6 part B is merged and green: about
6,000 lines of never-compiled lane C++ compiled first time under /W4 /WX with 0 fixes, every suite passes and
identities 1, 4, 7, 8 pass. The scoped re-review (three readers) found 0 I, 6 M, 11 m; every rule matches its
registration; the M findings were ruled before any read and went to fix round FIX-4 (two lanes, section 2). The
mining verb is finished (MINE-FIX, all ten findings) and waits for integration 7. Wave 0 part 2a is done: fields
v9 exist on both 4-year roles and the field overlap is explained to the last cell. **No statistic from the
2020-2023 window has been read**: no signal, IC pass or cell exists. N is still 37, admission trials 0.

## 1. What this session did (in order)

| step | result | where |
|---|---|---|
| MINE-FIX resumed (PM4-1) | MINE-7, 8, 6, 9, 10 and minors 11, 12, 17, 18 done; round 1 = budget ceiling 1,000 (PM4-13) | task-MINE-FIX-report.md (pool 8) |
| Integration 6 part B | FIX-2, FIX-3, COMB2, ORTH merged; part-B edits; builds v8-10, v8-10a; 0 compile fixes, 3 test-premise fixes | log "integration 6 part B" |
| Identities 1, 4, 7, 8 after part B (PM4-3) | all PASS | same |
| Scoped re-review SPO / COMB / ORTH (PM4-6) | I 0, M 6 (1 fixed in 6B), m 11; 0 likely compile failures | review-6b-spo.md, review-6b-comb.md, review-6b-orth.md |
| Rulings on the review (PM4-7..12) | pooled fit covers every V8-F composition; ladder criteria for R-8..R-12; theme order; tie rule | progress.md |
| Fix round FIX-4a / FIX-4b | see section 2 | task-FIX-4-brief.md, task-FIX-4a/4b-report.md |
| Wave 0 part 2a (PM4-14): R10, R11, field overlap | fields v9 lo1 and lo3 built, 63 / 63 each; 62 / 63 fields bit-identical to v7.1 | log "Wave 0 part 2a" |
| regsho_threshold_days63 cause test (PM4-15) | outcome (a): bit-identical to the post-republish build; cause = republished stage | log "Wave 0 part 2a: regsho decomposition" |
| Cells brief written | procedure and registered criteria for B0a..R-12 in one file | task-CELLS-brief.md |

## 2. Lanes at the stop

| lane | pool | branch | head | content | merged |
|---|---|---|---|---|---|
| FIX-4a | 4 | feat/platform-v8-fix4a-20261001 | `b8b68f4e` | STOPPED MID-WAY. Done: R6B-O-1 test (d9726cfa), O-2 theme order PM4-11 (e8997d64), O-3 tie rule PM4-12 (72696785), O-4 order check (263077f8). In progress: O-5 parent weights check, saved as the WIP commit b8b68f4e (incomplete, untested). Not started: O-6, O-7, R6B-C-1 (pooled fit, PM4-7), R6B-C-5 (runner rule / block check), report | no |
| FIX-4b | 10 | feat/platform-v8-fix4b-20261001 | `cf15052d` | STOPPED MID-WAY. Done: R6B-C-2 ladder criteria R-8..R-12 (f408a6f1), R6B-C-4 E-27b guard on parsed argv (cf15052d). Not started: R6B-S-1, R6B-S-2 (spo-v3 tests), report | no |
| MINE-FIX | 8 | feat/platform-v8-minefix-20260930 | `20e7bd19` | MINE-1..10, minors 11, 12, 17, 18, PM4-13 ceiling; MINE-14, 15, 16 deferred to v9 | no (integration 7, after the freeze gate, PM4-2) |
| FIX-2, FIX-3, COMB2, ORTH | 7, 10, 11, 4 | - | - | merged in integration 6 part B | yes |

The owner said "stop here" while both FIX-4 lanes were working; both agents were stopped at once, not at a
boundary of their choosing. Neither lane wrote a report, and neither lane's final test run is known: the committed
findings are unverified by the PM (the lanes run pytest before each commit by rule, but no result reached the
ledger). Pool 4's uncommitted work was saved as a WIP commit so no tree is dirty and nothing was discarded; the
next agent must review that diff before building on it. Both lanes branch from root `43a0447d`. Their C++ is
uncompiled until integration 6 part C. Every tree is clean.

## 3. Rulings made this session (all in progress.md, all before any read)

| id | decision |
|---|---|
| PM4-1 | MINE-FIX resumed and finished (isolated lane, no build, no data) |
| PM4-2 | MINE-FIX merges (integration 7) only after the V8-F freeze gate: it changes an engine header and would move executable pins under locked specs |
| PM4-3 | identities 1, 4, 7, 8 re-run after the part-B build |
| PM4-4 | r11.json records R-1's turnover criterion beside rule 5 (E-44) |
| PM4-5 | E-15 stands: the aim's ADV cap reads the run's initial NAV at every capacity multiple |
| PM4-6 | the re-review ran beside integration 6 part B on the frozen lane heads |
| PM4-7 | the pooled (era) fit must implement every composition a V8-F can carry: ic-shrink-v1, ic-shrink-aim-v1, theme-resid-v1 |
| PM4-8 | the ladder machine-checks R-8 (E-43 band), R-9 (report-only), R-10 / R-11 (R-1's turnover criterion), R-12 |
| PM4-9 | R-12 acceptance = rule 5 plus book turnover not higher than the parent (as R-2 and R-7) |
| PM4-10 | a fit refused on an infeasible 1/(2T) cap is an undefined cell (adds 0), not a rejected trial |
| PM4-11 | theme-resid order = registry order of PRIOR_THEMES, later themes appended; `filing_events` last |
| PM4-12 | theme-resid keeps a theme's own tie blocks tied (block-mean residual, then re-rank); no-tie result unchanged bit for bit; id stays theme-resid-v1 (never run) |
| PM4-13 | a mined campaign's --budget above 1,000 is refused until the overlap factor is re-derived (binds only under OD-7) |
| PM4-14 | Wave 0 part 2 split: 2a (fields v9, field overlap) before FIX-4 merges, 2b (plan-only, u pass, signal and IC overlap, pins, locks) after integration 6 part C |
| PM4-15 | regsho_threshold_days63 overlap difference: cause test against the post-republish build; outcome (a), re-base continues |

Trial accounting: TRAIN construction cells 37; admission trials 0; validation reads 2 (before v8); history reads 0;
2024+ never opened. One disclosure this session: the Wave 0 integrator read 20 progress lines of the R10 build that
carry counts of rows past the seal (no value, no statistic).

## 4. Verified in root this session

Build v8-10 (twelve targets) and v8-10a clean, 0 warnings, 0 compile fixes. Suites at `9c5cfa0c`: target 256, book
155, strategy 46, ic 139, mine 18, factory 387, combine 233, impl 1003 / 5 skipped / 1 known failure
(`ConfigJsonNotInDiscoverDigest`); Python strategies 163 + 9, engine tools 253, impl tools 529 / 1 skipped, scripts
178 / 3 skipped; tiny_world unmoved; spo-v2 pin `3bfd293e` holds; ERA's one-era equality test un-skipped and passing
for ew-theme-std-v1, ew-theme-std-aim-v1, ew-theme-aim-v2. Identities 1, 4, 7, 8 PASS (identity 4: the weights file
differs only in `module_sha256` because FIX-3 changed `composition_rules.py`; data files identical).

Wave 0 part 2a: fields v9 lo1 manifest `888e6616e441e863a9f91234124e1aebc907db11e18d9789d3c583cf447b8695` (63 / 63,
166.8 s, 960 MiB); lo3 manifest `9f1563638b5e4f7ead7be686803b96a0707ada2c608fcbc6dc084179bd9021ef` (63 / 63, 154.6 s,
1,014 MiB). The 11 live stage hashes equal the runbook pins and the R8 values (W0-n). Field overlap lo1 against
v7.1: 409,448,655 cells, 62 / 63 fields bit-identical. `regsho_threshold_days63` differs on 3,006,158 cells
(3,006,153 a value against an old NaN; 5 value changes, largest 5.0) and is bit-identical to the post-republish
build `v8-i3p4-c-fields2` (stage pin `fb073c62`), so the republished stage is the whole cause; no registered or
drafted v8 candidate reads the field. Disk 50 GB free.

## 5. Open findings and risks

- FIX-4a and FIX-4b are half done, unmerged, uncompiled and without reports (section 2). Until FIX-4a finishes
  R6B-C-1, an OD-3 history read of a V8-F that carries an accepted R-10 or R-11 would be refused; until O-5..O-7
  land, R-11's checks are weaker than ruled. None of this touches B0a..R-8, so the cells up to R-8 do not
  depend on FIX-4a's remaining items; they do depend on integration 6 part C merging what exists.
- The tie rule (PM4-12) is implemented twice (Python fitter, C++ kernel) and needs the scoped diff read at
  integration 6 part C.
- The E-31a void exit path (exit 3, `voided` keys) is correct by reading and untested end to end until FIX-4b's
  test is built (R6B-S-1). A B0c warm-up book whose beta starts outside the limit would void R-6 at its first
  scored decision; E-31a then applies (void, blind fix, re-run, no new trial).
- Minor findings open: R6B-C-7 (r10 forces the w cap to 3,072 MiB), R6B-C-8 (Python and C++ sum in different
  member orders; the runner refuses loudly), SPO-3, SPO-6, F-4, F-7, F-11, F-12, F-13, T-10, C-14..C-22.
- R-8: realised volatility is expected near 5% / 1.247 under aim-partial-v5, so the E-43 band may fail; the rule
  is not re-parameterised. Check the risk store's capped_specific count first.
- Mining: the overlap factor 1.55 is validated to a budget of about 1,000 at the 504-row floor (PM4-13); a
  default campaign on the 4-year role is estimated at 10.85 GiB (an OD-7 precondition). Nothing in v8 runs one.
- Host memory: the R-1 weighted pass needs about 2.6 GiB (E-28); 6 GB was free at the start of this session.
- Pool leases still show dead owners from 2026-09-25..27 (PM3-2 stands: pools are used directly).

## 6. Report and pitch determination

No v8 report was generated. A scorecard or pitch needs at least B0c (the re-based baseline) and an improvement
claim needs the V8-F cumulative test; neither exists. What can be stated honestly: the platform is merged, green
and reviewed through integration 6 part B; fields v9 exist on both 4-year roles; every registered lever (R-1..R-12)
is behind a flag with a registration fixed before any read. Earliest re-based pitch: after B0c. Earliest
improvement pitch: after V8-F.

## 7. Next steps (detail in goal prompt 4)

0. Resume FIX-4a (pool 4, from b8b68f4e: review the WIP diff, finish O-5, then O-6, O-7, C-1, C-5, report) and
   FIX-4b (pool 10, from cf15052d: S-1, S-2, report), per `task-FIX-4-brief.md`.
1. Integration 6 part C (tag prefix `v8-11`): merge FIX-4a and FIX-4b by SHA, build, the gtest filters of both
   reports, every suite, identity 4 (and identity 1 if a NAV source changed), one scoped read-only diff review.
2. Wave 0 part 2b: R12 plan-only, the cold u pass, the signal overlap (must be bit-identical, PM4-15) and the
   daily IC overlap under W0-a, R14 pins, the chained `protocol` line, `lock --write` on base-lo1 / base-lo3.
3. Cells in registered order per `task-CELLS-brief.md`: B0a, B0b, B0c, then R-1..R-8 and R-10..R-12 or R-9.
4. V8-F cumulative test and freeze gate, H-2, scorecard v8, pitch, v7 pitch re-render, handoff 2.
5. Integration 7 (MINE-FIX `20e7bd19`) after the freeze gate.
