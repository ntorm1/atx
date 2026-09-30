# platform v8 status 2 (2026-09-30, PM session 2, stopped at the owner's instruction)

For: the owner, and the next parent (PM) agent. Follows `docs/plans/2026-09-30-platform-v8-handoff-1.md`. The ledger
is `.superpowers/sdd/platform-v8-20260929/progress.md`; per-integration detail is in `integration-log.md` beside it.
Figures below are from git, build receipts and test runs unless marked [lane claim] (written by a lane that could not
compile or run data) or [est].

## 0. State in one paragraph

Of the six goal steps, steps 1 and 3 are done and step 2 is about two thirds done. Steps 4, 5 and 6 (every data build
and every research cell on the 2020-2023 window, the freeze gate, the scorecard and the pitch) are **not started**.
All Wave 1 platform code and almost all research-lever code (composition v8, hold band, ADV cap, spo-v3 optimiser,
new fields, diagnostics, report components, era shards) is merged into the root branch, builds clean and passes its
test suites. Three lane branches are still unmerged (label role for B0c, cycle follow-ups and specs, mining verb).
The Wave 1 adversarial review ran only partly and found 1 important and 20 medium defects, none fixed yet. **No
role, field, IC pass or cell has been built or read on the new window. The trial count N is still 37. There is no v8
performance number of any kind.**

## 1. Where things are

| item | value |
|---|---|
| root worktree | `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, base main `7fbfc379`, tree clean |
| root HEAD | the commit that adds this file (code head `237486fe`) |
| size of the branch | over 140 commits since `ef11f462`; 160 code files, +38,772 / -3,197 lines (docs and sprint directory excluded) |
| last good build | tag `v8-5` (source `37e84d81`), preset `equity-dev`, 51 s, no warnings-as-errors |
| executables (v8-5) | ic `4b4ffb7b…d68f`, targets `0d0a6921…a8f1`, risk `f45e8870…0392` |
| next free build tag | `v8-5a` (or `v8-6`) |
| trial ledger | `build-equity/trials.jsonl`, 37 lines; no `protocol` line for the new window yet |
| pre-registration | `v8-prereg.md`; pins still unfilled (no artifact built) |
| disk | C: 43 GB free (was 70 GB on 2026-09-29). Wave 0 plus B0a, B0b needs about 16 GiB; B0c about 7 GiB |
| pushed / merged to main | nothing. Nothing committed in `C:/atx` |

## 2. Goal steps: done and not done

| step | content | state |
|---|---|---|
| 1 | finish integration 3 identities; fold pending ledger files | **done** (identities a-f; no pending files were left) |
| 2a | integration 4: merge R1, R45, G, R6, F3, REPORT, H3, H1; fix the F-1 reuse defect; build; suites | **done** (plus A2, R6 part 2, F-C/F-D, H1 tooling, REPORT tasks 2-4) |
| 2b | identities of handoff section 6 step 2 (R-1, R-4, R-5, H3 warm pass) | **not started** (stopped); 7 runs listed in section 7 |
| 2c | one adversarial review since `ef11f462`; fix I and M; one scoped re-review | review **partial** (section 6); fixes and re-review **not started** |
| 3 | rulings on S_prior, NAV in the ADV cap, hold-band decide parity | **done** (E-14, E-15, E-16), before any read |
| 4 | Wave 0 data build, overlap reports, pins, protocol line, cells B0a, B0b, B0c, diagnostics, Release A/B | **not started** |
| 5 | research cells R-1 .. R-7 | **not started** (code for all seven levers is written) |
| 6 | V8-F and freeze gate; Wave 5 tooling acceptance; scorecard, pitch, handoff 2, merge command | **not started**, except that the Wave 5 and Wave 6 *code* is written (H-1 merged; H-3 unmerged; report components merged) |

## 3. What this session verified in root

### Integration 3 Part 4 (identities on the existing 3-year role, commit `c5492e4a`)

| id | check | result |
|---|---|---|
| a | W0-1: accepted v7.1 NAV cell with the new window code | PASS, 12 files byte-identical |
| b | what `context_sha256` hashes | data only; the code SHA moved to the code fingerprint (Ruling E-23 accepts) |
| c | C-3 field reuse | FINDING: 62 of 63 payloads equal fields-v9 (`regsho_threshold_days63` differs: stage republished, known). Reuse copied 0 of 63 because W0-1 changed every builder fingerprint once. The copy path itself is still untested |
| d | B-2: u pass at 4 and 12 workers | PASS, daily IC, targets and combined files byte-identical to each other and to the accepted pass. 12 workers are 23 s slower overall on this host (Ruling E-24: stay at 4) |
| e | D-1 stage timers | PASS, only `stage_seconds` is added |
| f | F-0: lo1 role rebuilt with delisting options off | payloads byte-identical; manifest differs in 19 explained paths (accepted) |

### Integration 4 (parts A and B; builds v8-4 .. v8-4e and v8-5)

- Merged: R1 (composition `ew-theme-std-v1`), R45 (hold band, ADV cap, hold-band state in the holdings export), G
  (diagnostics), R6 parts 1 and 2 plus Ruling E-26 (engine solver `solve_tracking`, rule `spo-v3`), F3 (F-1 reuse
  fix, `grp_ff12f49`, `k8_item402_63`, `gscore7_lowbm`, `eps_consist_4y`), REPORT (seal exemption, `mega_report/v8.py`,
  v8 pitch config, scorecard template), H3 part 1 (role panel lifted to the engine), H1 (era shards tooling), A2
  (marginal step argv, plan-rows test).
- Source pins recomputed once: `dsl_vm_sources` 33 paths, digest `f24cfbbe…`; `ic_result_sources` unchanged; no
  semantics bump (the move was verbatim).
- The two `FactoryOos` failures of handoff 1 were triaged as pre-sprint (commit `a187e2fe` on main changed the
  record CRC) and confirmed by one probe build; re-pinned in `c41d401e` (Ruling E-22a). Factory tests 377 / 377.
- Test counts on the last builds:

| suite | result |
|---|---|
| atx-engine data / combine / book / factory | 284 passed 14 skipped / 220 / 147 / 377 |
| atx-impl strategy-ic / strategy / target | 101 / 45 / 222 passed 1 skipped |
| atx-impl-tests | 940 passed, 6 skipped, 1 failed (`ConfigJsonNotInDiscoverDigest`, from before the sprint) |
| Python: strategies / engine tools / impl tools / scripts | 163 / 240 / 452 (2 skipped) / 121 (3 skipped) |
| tiny_world end to end | passes; no golden moved |

- Compile fixes made by the integrator on lane code: 3 (`21b498d9`, `09bb1ad4`, `d5e5510a`). No design error found
  at integration.
- Open from integration: the spo-v2 digest pin is still a placeholder (`SpoV3.V1AndV2DigestsUnchanged` checks v1,
  then skips); capture needs a build of the pre-R6 tree. `ParallelLockstepGrid` tests need the `parallel` test group
  configured. Build provenance of v8-4e names `5c6efcd4`, not its source.

## 4. Lanes at the stop (every tree clean)

| lane | pool | branch head | content | merged in root? |
|---|---|---|---|---|
| R1 | - | `ec2dfd16` | composition v8 | yes |
| G | - | `fea9b6e9` | `book_diagnostics.py` (G-1a .. G-3d) | yes |
| F3 | 9 | `fc8ff96c` | F-1 reuse fix, four new fields; 147 field tests pass | yes |
| H1 | 3 | `cf757c97` | era pooling (fitter, nav_summ, ledger), `roles:` loop, `era_data_audit.py`; 83 new tests | yes |
| REPORT | 10 | `a9a244f6` | v8 report blocks, pitch config, scorecard template, legacy book blocks, header, `final` check | yes |
| R6 | 7 | `f60a524e` | spo-v3 (S_prior 20), shaping flags under spo-v3 | yes |
| R45 | 11 | `126a5f5f` | Ruling E-25: NAV `--label-role` for B0c. **Work in progress** (`ec7356a4`): the check that membership differs only at the declared delisting cells, its help text and its test remain. Never compiled | **no** (4 commits) |
| A2 | 8 | `79440cfa` | add-alpha on a v8 parent spec, `cache gc` fix, `ew-theme-std-aim-v1` (R-3), v8 base specs and cell templates with caps per E-28 | **no** (9 commits; the first 2 are in) |
| H3 | 4 | `339c07b1` | mining verb parts 2-3 (`atx-equity-strategy-mine`, rule `mined-v1`). **Never compiled; fixture tests never run** [lane claim] | **no** (3 commits) |

Merge order for integration 5: R45 (after the lane finishes the remaining three items), then A2 (it needs R45's
`label_role` spec key; until then `plan` of `base-b0c` and `r1..r7` stops with "unknown key label_role"), then H3
(own build-fix pass and its own review, by the owner's rule for the mining verb).

## 5. Rulings made this session (all in `progress.md`, all before any read)

| id | decision | why |
|---|---|---|
| E-14 | spo-v3 `S_prior` 1.0 -> 20; criterion: mean correlation of the traded book with the aim >= .9 | 1.0 was a guess; on the lane's synthetic prototype it tracks factor loadings only |
| E-15 | the ADV cap uses the run's initial NAV at every capacity multiple | it is a $1bn rule; the 4x figure is a stress. Disclosed on the cell |
| E-16 | the holdings export carries the hold-band state | decide parity; does not block the R-4 cell |
| E-17 | "frozen book" for a history read = frozen rules, pooled re-admission; no runner change | rebinding weights to another role defeats the role binding |
| E-18 | diagnostics run in splits under 600 s / 2,560 MiB | not trials |
| E-19 | `--plan-only` is the checker of record; the v7.1 recipe bytes stay | the test now passes on the real plan |
| E-20, E-30 | definitions of `gscore7_lowbm` and `eps_consist_4y` | fixed blind, one variant per hypothesis |
| E-21 | lane F3 fixes the F-1 reuse defect | testable in the lane |
| E-22, E-22a | FactoryOos: re-pin only after a confirmation build | done |
| E-23 | `context_sha256` accepted as coded | data and code identity are separate keys |
| E-24 | IC passes stay at 4 workers | measured |
| E-25 | NAV `--label-role`: decisions on the winner's role, marking on the delisting-returns role | E-10 could not be run otherwise |
| E-26 | spo-v3's aim includes an accepted hold band / ADV cap | the registration says "the accepted rule" |
| E-27 | R-3 gains multiply tier weights inside each theme; theme share stays 1 / T | "on top of the R-1 weights" |
| E-28 | weighted pass with the theme block runs under 3,072 MiB | admission about 2,606 MiB [est] |
| E-29 | capacity curve on B0c, report only | every cell reports 4x against its parent |
| E-31 | spo-v3: no early exit; `limits_unmet > 0` is a mechanics defect, fixed blind, rerun without a new trial | pre-registration rule 7 |
| E-32, E-33 | `mined-v1` confirm statistic; ledger kind `mining-campaign` adds 0 | binds only under OD-7 |
| E-34 | the freeze gate's bootstrap p is one-sided | the hypothesis is directional; both p values are printed |

Trial accounting: TRAIN construction cells 37; admission trials this sprint 0; validation reads 2 (before v8); 2025+
never read. Hidden-data disclosures this session: none.

## 6. Wave 1 adversarial review: partial, not yet acted on

Three read-only readers on the most capable model, fixed commit `7af37e9d`, one area each. Stopped early by the owner
stop: **coverage is partial and no test file was read.** Full text: `review-w1-A.md`, `review-w1-B.md`,
`review-w1-C.md` in the sprint directory. Totals: **I 1, M 20, m 23.** Nothing is fixed.

Checked and found clean: the IC runner split is verbatim; the signal and result cache keys admit no stale hit;
results do not depend on worker count; the C++ seal holds; `ew-theme-std-v1` matches its five registered rules in
C++ and Python; price fields read session t-1 or earlier; no floating-point change with every new flag off; the
statistics formulas in `nav_summ.py` were derived and match.

Findings that block a number from being trusted:

| id | sev | where | problem | blocks |
|---|---|---|---|---|
| C-1 | **I** | `scripts/cycle_verdict.py:55`, `scripts/research_cycle.py:913` | the cycle's verdict and headline DSR do not use the pre-registered cross-trial variance (`--dsr-ledger` is never passed). Example: Sharpe 1.0, N 41 gives DSR .42 against .91 | every cell verdict and the freeze gate |
| C-2 | M | `research_cycle.py:913` | the summ step never adds `--protocol v8`; add-alpha specs inherit the v7 bootstrap seed and draw count | every paired test |
| C-9, C-10 | M | `compare_window_overlap.py:224-239` | `bit_identical: true` when zero cells were compared; NaN-against-value cells ignored | the W0-a overlap reports |
| C-3, C-4, C-5 | M | `backtest_integrity.py:657-690` | defect and rerun flags skipped silently on a ledgered cell; `rerun_of` unchecked; a blind rerun can remove any cell from N | the trial count |
| C-6, C-7 | M | `backtest_integrity.py:636, 758` | the hash chain misses the 37 legacy lines; Appendix A prints 0 admission trials | Appendix A block |
| C-13 | M | `research_cycle.py:967` | resume by marker file only: a stale NAV can be scored under a new spec | every re-run |
| B-4 | M | `strategy_ic_library.cpp:44` | the fields manifest is read under a 1 MiB limit, so fields v10 / v11 (70 / 73 rows) may be refused [unverified threshold] | R-2 re-screens, R-7 |
| B-2 | M | `strategy_marginal_ic.cpp:194` | the marginal verb does not read themes from `theme_standardise` [partly unverified] | R-2 screen after R-1 |
| B-3 | M | `strategy_data.cpp:89` | nothing refuses a delisting-returns role as the signal role (E-10 holds by procedure only) | B0c |
| A-2 | M | `strategy_spo.cpp:1454` | under a warm start, gamma is calibrated on the first warm-up decision; spo-v3 may stop on a warm-start parent if the risk store has no earlier rows [unverified] | R-6 |
| A-3 | M | `strategy_nav_replay.cpp:1185` | a warm start that builds no book is neither refused nor reported | B0c |
| A-4 | M | `strategy_spo.cpp:1420` | the E-14 criterion is computed on planned weights, not the traded book | R-6 |

Other medium findings (no cell depends on them): A-1 (grid with ADV cap uses the first variant's leverage), B-1
(price-field reuse fingerprint misses the session calendar), C-8 (ruled: E-34), C-11 (`holdout_gate` ruling file
unauthenticated), C-12 (`cache gc --under` path spelling can delete referenced stores).

Not reviewed at all: every test file; `research_fields_sec.py`, `research_fields_holdings.py`,
`research_fields_v8.py`; `fit_composition_weights.py`; `alpha_report_card.py`; `generate_library.py`; most of
`strategy_live`, `strategy_holdings`, `strategy_nav_v7`; and everything merged after `7af37e9d` (F-C, F-D, era
shards, report blocks, E-26), plus the three unmerged lanes.

## 7. Remaining work, in order

1. **Fix lane** for C-1, C-2, C-9, C-10, C-3..C-7, C-13, B-2, B-3, B-4, A-2, A-3, A-4 (and the cheap remainder),
   then one scoped re-review. Finish the review of the files in "not reviewed".
2. **Lane R45** finishes E-25 (three items). **Integration 5**: merge R45, A2, H3; build; suites; mining-verb
   fixture acceptance; one review of the mining verb.
3. **Identities** on the 3-year role (none run yet): NAV with every new flag off; `--hold-band 0`;
   `--adv-hold-q 1e9`; composition v8 with re-rank and cap off; H3 warm u pass (48 / 48 hits); field reuse step 2
   (expect 49 reused, 14 recomputed); spo-v2 side files; `--label-role` equal to `--role`. Capture the spo-v2 pin.
4. **Wave 0** (`w0-2-runbook.md` R1-R13): projection, base role, repair, bridge, fundamental events, roles lo1 and
   lo3, fields v9 on both, plan-only, cold u pass, overlap reports under ruling W0-a. Fill the pins; write the chained
   `protocol` ledger line. About 15 bounded runs, under one hour of compute [est].
5. **Cells B0a, B0b, B0c** (N 38, 39, 40) with year tables and the Appendix A block; diagnostics on B0c; Release A/B.
6. **Cells R-1 .. R-7** (N 41 .. 47), identity first, one cell each; R-2 through `add-alpha` and `run --screen`
   (command sequence in `task-A2-report.md` on the A2 branch); risk model on the 4-year role before R-6; fields
   v10 / v11 before the R-2 re-screens and R-7.
7. **V8-F** cumulative test and freeze gate; H-2 measurement; scorecard v8; pitch render; handoff 2; the merge
   command for the owner.

Open decisions for the next PM: whether the pooled (era) fit must support `ew-theme-std-v1` before any history read
(today it neither refuses nor supports it); the width of the fields-manifest limit (B-4).

## 8. Risks that changed this session

- Disk fell from 70 GB to 43 GB free while another session worked. Wave 0 through B0c needs about 23 GiB. Deleting
  the superseded caches (about 25.6 GiB) is an owner decision.
- The host has about 3.5 GB of free memory while the atx-db session runs; the R-1 weighted pass needs about 2.6 GiB
  of admission [est] (Ruling E-28). It may be refused until the host is quiet.
- About 4,700 lines of lane C++ (mining verb 3,821; label role 926) have never been compiled.
- The review was partial. The one important finding was in code that decides accept or reject; more may exist in
  the unread files.

## 9. Determination: is a v8 pitch with improved metrics justified?

**No. There is no v8 metric. A pitch that shows improved numbers cannot be written from the present state.**

Reasons, in order of weight:

1. **Nothing has been measured.** No role, field, signal or cell exists on the 2020-2023 window. The ledger still has
   37 cells. Every number available today is either a v7.1 figure on 2020-2022 (net Sharpe 1.405, gross 1.809,
   turnover .038 per day, 13.5 bps per traded dollar, net Sharpe 1.236 at 4x NAV, cell-count DSR .8247) or an
   estimate from the plan. The v8 pitch config as committed has every verdict "pending run" and renders nine refused blocks [lane claim].
2. **The tools that would produce the numbers are not yet trustworthy.** Review finding C-1 shows the cycle's verdict
   and DSR use the wrong variance; C-9 and C-10 show the overlap tool can report identity on zero compared cells.
   These must be fixed before the first cell, or the first v8 numbers would be wrong in the pitch's favour or against it.
3. **The baseline moves before any lever does, and probably downward.** B0a re-bases the book on a window that adds
   2023 (partly selected in earlier sprints). B0c then adds delisting returns and a 60-session warm start, which is a
   protocol correction, not an improvement. The plan's borrow stress is -.04 to -.12 Sharpe [est]. The v8 headline
   can therefore be below v7.1's 1.405 before any research cell runs. "Improved" has to mean improved against B0c,
   and B0c does not exist.
4. **Even after all cells run, a Sharpe improvement is unlikely to be demonstrable.** By the plan's own power table
   a single cell is sign-level evidence (paired SE about .10 on four years); only the cumulative test V8-F against B0c
   has power, and only for a total gain of about +.20. The plan's summed midpoints are about +.15 net Sharpe at $1bn
   and +.35 at $4bn [est], and it rates "the freeze gate stays unmet" as a high-likelihood risk. The metrics most
   likely to improve measurably are mechanical: turnover (about -35% [est]), cost per traded dollar, and net Sharpe
   at 4x NAV.

What can be stated honestly today is a **platform** status, not a performance pitch: the 4-year window and seal are
coded in one place and enforced in every tool; the IC runner is split, incremental and capped; composition v8, the
hold band, the ADV cap and a reusable target-tracking solver exist in atx-engine and atx-impl behind flags with
flag-off identity; four new fields and seven library candidates are registered blind; diagnostics, era-shard and
report tooling are merged; the mining verb is written. All of it is tested on synthetic data and none of it on the book.

Earliest point at which a v8 pitch is justified: after step 5 of section 7 for a **re-based** pitch (B0c with year
tables and diagnostics, stated as the new baseline, with no improvement claim), and after step 7 for an
**improvement** pitch, and then only for the metrics the cumulative test or the mechanical criteria support.
Remaining effort to the re-based pitch is about one PM session (fix lane, integration 5, identities, Wave 0, three
cells); to the improvement pitch about two more [est].

## 10. Standing rules (unchanged)

Root alone builds and runs real data; children never build or run data. Never build, switch or commit in `C:/atx`;
never touch `atx-db/`. Nothing dated 2024-01-01 or later is opened by any tool or agent. Every ruling is written
before the measurement it could bias. A rejected cell is never retried with other parameters. Budget: N at most 51
construction cells; admission trials at most 15 plus the 8 re-screens. No pushes; the merge to main is the owner's.
