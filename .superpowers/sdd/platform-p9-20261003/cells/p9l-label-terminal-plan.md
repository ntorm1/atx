# P9-L (`p9l-label-terminal`) plan: limits, verdict arithmetic, route (committed before any run of the cell)

Cells root agent 1 (`r14-prompts/cells-prompt-1.md`), pool-2, branch `feat/platform-p9-20261003`, 2026-10-16. The
registration is `p9-prereg.md` B.1 (round 2b-ii, ruled PRE-2BII-RULED) with Rulings PRE-2 .. PRE-5, PRE-27, C2-6,
CELLS-1 .. CELLS-6; the per-cell steps are `r12-post-gate.md` "CELLS" 0-5. This file is the R0-10-form plan of a hand
cell (CELLS step 1): the limits and the verdict arithmetic, fixed here before any read of the cell. Nothing in it is
read from a P9-L output (none exists at this commit).

## 1. Registration (code commits, before any run)

| what | commit | SHA-256 |
|---|---|---|
| the template `scripts/specs/p9/templates/label-terminal.json` (atx.research-cycle-template/v1; parent and nominal_parent `../../v8/lib-v8ysb-gm.json`; `lock --write --exes`) | `0bba6c89` | `7483f4b10513d9000cfc89036c3f8f7f49626a9dd9c57999bcce11a258754219` (spec digest over the chain `ab5685d0fdcd21459bc7fa3809949ee18ca32334e2867daacd7145ec06b7fc6a`) |
| the run recipe `scripts/specs/p9/templates/label-terminal-cal.json` (`wave_stage_cell.calibrated_doc` of the template, written by that function; `lock` dry exit 0) | `6ea79e5a` | `7783967471f5b06dc1407d4425605156f7a3e647f28edd2a3ca5f2fb1a989d37` (chain digest `71262bc87eedaa315710b3478a4ef496f028d4b572d4cdced3eae69fc1a24e0c`) |
| the lineage parent `scripts/specs/v8/lib-v8ysb-gm.json` (at `f8d852f3`; was `3631679c...edea` at the re-pin `2078f877`) | - | `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a` |
| the paired parent NAV `build-equity/p9-b0g-yf0-nav` (listing = `gate-logs/b0-files-p9-b0g-yf0-nav.txt`) | - | listing `e205d6ecb16e75cba02194b90b313849404c2052b3e1738c4bf020e486fcf797`; `summary.json` `826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38` (= the template's locked `reference_cell`) |
| the parent fit admission (locked `reference_admission`) `build-equity/mega-weights-v8x-theme-erc-v8ysb/admission.json` | - | `121f70466884030f97d98ec3d965760e7df376c160385072d0abc8428b9bf255` |
| exes (`exes_sha256`, = the re-pin table, EXE-FREEZE): eval / IC / NAV | - | `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c` / `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705` / `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565` |
| label role (`--train-label-role`, the parent's `inputs.label_role`) `build-equity/train-2020-2023-lo3-dlret/manifest.json` | - | `95e16cfe3ad0e0c9e7cf94acb6df4004e6f286ae59dff7c0e725ea33aace5069` |

The registered change (B.1 verbatim): `change.flags.ic` = `--label-terminal imputed-v1 --train-label-role <label role>
--train-label-role-sha256 <pin>` on the u and w IC argv; fit, card, w, NAV and monitor follow (outputs renamed). The
template names no `nav.flags`: it INHERITS the parent's, so the planned NAV argv carries `--ann-mean-gross` once and
`--info-clock-print` once (FLAGS-ROUTE; alike with `p9-b0g-yf0-nav`, which carries both). Planned argv flag counts
(`research_cycle.py plan`): u / w `--label-terminal` x1, `--train-label-role` x1, `--train-label-role-sha256` x1,
`--candidate-cache build-equity/mega-candidate-cache-v8-lo3`; NAV `--ann-mean-gross` x1, `--info-clock-print` x1,
`--aim-leverage 1.1828`, the `-cal` NAV `--calibrate-gross 0.9862134133265011` x1 and `--calibrate-tolerance 0.005` x1;
on no phase a cache-index flag, a `--risk-model`, a rule flag, a timing or seal flag.

G_parent (PRE-3, the `match_engine` form): `0.9862134133265011` = the parent NAV's `mean_gross_leverage_all_rows` by the
mechanics reader (`scripts/wave_readers.py mechanics`, the driver's `wave_steps.reader_argv` form under the bounded runner
with the readers' caps 180 s / 1536 MiB / 512 MiB free; tool `cells/tools/hand_readers.py`; output
`build-equity/p9l-label-terminal-readers/mech-parent.json` `7bd04ca02e3ce6a17ead49d7a03ef3cc1e00f84f5338943f4dbee7f43e8d483e`,
run dir `mech-parent-run1`, outcome completed). Tolerance `0.005` = `wave_rules.GROSS_MATCH["pm6-6-engine"]`.

## 2. Limits (G-B9, UNSCALED: r = 1; PRE-5) and the verdict arithmetic

The mechanics rule of record is `v8-mech` (`scripts/wave_rules.py` `MECHANICS["v8-mech"]` / `mechanics_check`), read
from the mechanics reader of the PUBLISHED NAV (the `-gm` NAV dir) at its primary scenario (S2):

| row | value read | limit (unscaled) |
|---|---|---|
| 1 gross_all_rows | `mean_gross_leverage_all_rows` | in [0.90, 1.05] |
| 2 abs_net_all_rows | abs(`mean_net_leverage_all_rows`) | <= 0.02 |
| 3 tau_mean | `tau_gmv_mean` | <= 0.20 |
| 4 tau_p95 | `tau_gmv_p95` | <= 0.30 |
| 5 max_return_identity_error | `accounting.max_return_identity_error` | <= the NAV's own `accounting.tolerance` |
| 6 max_cash_book_relative_error | `accounting.max_cash_book_relative_error` | <= the NAV's own `accounting.tolerance` |

Verdict (PRE-5: mechanics only; derived by hand from the output files, then re-derived by the DERIV agent, CELLS-2):

- **ACCEPTED** iff all six rows pass (each value finite and within its limit). Adopted as the new parent iff accepted
  (B.1 "Acceptance"); the cell of record a child names is `label-terminal-gm.json` (section 3 step 4).
- **REJECTED** iff any row fails (mechanics fail: rejected, counted, not a void; B.0).
- **No verdict, STOP** (hand back, the PM rules; no ledger line): the calibration record incomplete or not at L 1.1828
  toward G_parent bit for bit; `matched` false; the mechanics reader's G of the published NAV not equal to the exe's
  `calibration.gross` bit for bit; a run failed / refused; any void condition of B.0 (section 5).
- No dSR, p, IC, Sharpe, return or NAV figure enters the verdict (they are printed only, section 4).

## 3. Route and order (one bounded run at a time; trial ledger re-hashed after each)

Every run below goes through the cycle's own bounded runner (`scripts/run_bounded_research.py`), with the parent spec's
registered phase caps (C.14: u 300 s / 2,560 MiB, w 300 s / 3,072 MiB, card 300 s / 2,560 MiB; every other phase
180 s / 1,536 MiB; `--min-free-mib 512`), the free-RAM figure stated before each, `--admission-wait 1800` (HOST-6a), C:
at least 15 GB free. Each console goes to `cells/logs/` and is read through a filter (the gate's admission lines and
the step states only).

1. `research_cycle.py run scripts/specs/p9/templates/label-terminal.json --stop-after w --admission-wait 1800`: fields
   pinned as built (v15, done); u, fit, card, the internal gate, w. The gate appends no admission line: the 9 listed
   candidates' admission trial ids (`cycle_admission.trial_id`: candidate, DSL, role pin, window) are all in the
   ledger already (9 / 9 recomputed at this commit; `ledger_append` skips a present trial_id). Expected ledger after:
   133 / `27e40f9f` unchanged (a change = STOP, recorded).
2. `research_cycle.py run scripts/specs/p9/templates/label-terminal-cal.json --stop-after nav --admission-wait 1800`: u,
   fit, card and w resolve to step 1's outputs (done); ONE NAV into `build-equity/mega-nav-p9l-label-terminal-gm`
   (pass 1 at L 1.1828, at most one correction, published at the final L; `match_engine`). Checks, in the driver's
   order: `summary.json` `calibration` holds every CALIBRATION_KEY; `leverage_requested == 1.1828` and
   `target == 0.9862134133265011`; the mechanics reader (`mech-matched`, the same reader form) of the published NAV
   gives `mean_gross_leverage_all_rows == calibration.gross` bit for bit; `matched` true.
3. L-prime = `f"{calibration.leverage:.4f}"` if `corrected`, else `1.1828` (the driver's rule).
4. The cell of record `scripts/specs/p9/templates/label-terminal-gm.json` = `wave_stage_cell.record_doc` (written by
   that function): the plain recipe at L-prime, no `--calibrate-*` flag, `nav.output` = the same `-gm` NAV dir, so the
   same cycle dir (`build-equity/cycle-p9l-label-terminal-gm`); `lock` dry; its own code commit. No NAV re-run: the exe
   published exactly the plain run at L-prime (C2-R2; `summary.json` adds `calibration`). For DERIV: the cell of
   record's planned argv (`research_cycle.py plan label-terminal-gm.json`) carries `--aim-leverage <L-prime>` and no
   `--calibrate-*`; the NAV dir's receipt carries the `-cal` argv (the run recipe that ran), and its L-prime is the one
   in `summary.json` `calibration.leverage` (4 dp); both recorded in the cell record.
5. The hand seal scan (section 5 item 8) and the cache listing after (section 5 item 9).
6. The mechanics verdict (section 2) written in `cells/p9l-label-terminal-record.md` and COMMITTED before any return,
   IC, Sharpe or NAV statistic of the cell is read (B.0 void (3)).
7. Then, in this order: rho (B2-13) of the pair (cell `-gm` NAV vs `p9-b0g-yf0-nav`) by
   `gate-logs/tools/rho_pairs.py <exes.eval> P9L=<cell>,<parent>` under the bounded runner (value and the
   "1 - rho < 1e-6" flag; the fallback of C.11 / PRE-12 as fixed); the printed-only lines of B.1: the paired dSR vs the
   parent with its Memmel SE and both p (`nav_summ.py --engine-stats <exes.eval> --protocol v8 --bundle <parent>
   <cell>`, the driver's `bundle_argv` form, bounded; B2-11: `nav_summ_run.engine_exe_sha256` = `e2b48c46...d962c`
   checked), orientation flips (u `orientations.json`, `p9-b0g-yf0-u` vs the cell's u), admission status changes (fit
   `admission.json`, the locked `reference_admission` vs the cell's), the IC exe's `ic_missing_exit_v1`,
   `terminal_imputed_pairs`, `missing_exit_share`, `guard_excluded_pairs` (the cell's u / w `summary.json`).
8. Record and hand back; STOP for DERIV (`r14-prompts/deriv-p9l-prompt.md`). Nothing is written to the trial ledger.
9. Only on the PM's "DERIV agrees": the trial line through the hand cycle, `research_cycle.py run
   label-terminal-cal.json` (the recipe that ran; monitor, then summ: `nav_summ --ledger build-equity/trials.jsonl
   --ledger-kind construction`, receipt `build-equity/cycle-p9l-label-terminal-gm/summ-run1`); the count / SHA after
   and the new line's trial id recorded.

Output dirs of the cell (all new; none exists at this commit): u `build-equity/mega-p9l-train-u-label-terminal-1`, fit
`build-equity/mega-weights-p9l-label-terminal`, card `build-equity/mega-cards-p9l-label-terminal`, w
`build-equity/mega-p9lw-train-label-terminal-1`, the NAV of record `build-equity/mega-nav-p9l-label-terminal-gm` (run
dir `-gm-run`), readers / bundle / rho `build-equity/p9l-label-terminal-readers/`, monitor and summ only after DERIV.
No separate `-cal` NAV dir exists: the one calibrated NAV is the NAV of record (route of `match_engine`).

## 4. Printed only (decide nothing)

Paired dSR vs the parent, its SE and both p; orientation flips; admission changes; the four IC exe label keys; rho.
Predictions: none registered (B.1). Exposure (PRE-4): stated, no truncation code. Appendix A block and the
identity-bridge flag line as the record form requires.

## 5. Pre-cell checks (CELLS step 0; B.0 "Void" 1-7) and void checklist

1. Ruled / P9-B0 / re-pin / ALD-DIAG before the run: PRE-2BII-RULED, P9-B0 of the gate, re-pin `2078f877`, ALD-DIAG
   printed at M2g (void 1).
2. Inputs = the block (void 2): the template / recipe SHA-256s above; parent = the lineage parent (the first cell);
   flags as section 1 (`--ann-mean-gross` and `--info-clock-print` alike by inheritance); fields v15 as built (pinned);
   L = 1.1828 or PM6-6's correction only; exes = the table; eval mode research-fast; fit mode flag-absent.
3. No return / IC / Sharpe / NAV statistic of the cell read before its mechanics passed (void 3): section 3 order.
4. No session at or after 2024-01-01 opened (void 4).
5. No exe rebuilt (void 5): every exe re-hashed = the table before the run.
6. `ucrtbase.dll` 10.0.26100.7623 `2C08E77F...65EF` (void 6): re-checked before the run.
7. No SQLite cache index flag on any argv; `index.sqlite` absent (void 7, SQL-11).
8. Seal: `--seal-allow` has no object on a v1 hand cycle (no seal scan in `research_cycle.py run`; the flag and the
   scan are `research_wave.py`'s: `wave_seal.py`), as on the exe-direct references (SEAL-REF ground); disclosed. In its
   place a hand scan of every log of the cell's runs (the run dirs' `stdout.log` / `stderr.log`, the readers' run dirs,
   `cells/logs/` consoles) with `wave_seal`'s FORMS, SEEDS and seal-reference rule, each hit classified by the
   `tools/sealsrc.py` sources (A the untracked owner PNG name; B a `"started_utc"` stamp; OTHER anything else);
   OTHER > 0 = STOP before any read.
9. Real cache (GATE-RELCACHE form, `gate-logs/tools/dir_listing.py`): before = 552 files, listing `c9ee9291f5ddc0c2`
   (`cells/logs/p9l-cache-listing-before.txt`, = `gate-logs/cache-lo3-listing-agent4-final.txt`); after: the 552
   original rows unchanged (name, size, mtime) and the additions listed (P9-L writes new `imputed-v1` IC-result scope
   dirs: additions, never a change of an existing entry).
10. S2-6 (B.1 "Inputs"): HOLDS: `cells/tools/s2_6_scan.py` over `build-equity/mega-candidate-cache-v8-lo3`: 2 IC-result
    scope dirs, 184 records, every key flag-absent (no `label_terminal`), 0 `imputed-v1` records
    (`cells/logs/p9l-s2-6-scan.txt`).
11. Trial ledger before: 133 lines, `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`; free RAM and C:
    stated before each run; timings off (no timing flag on any argv).

## 6. Disclosures fixed before the run

- The parent's u resume root (`mega-v8-b0b-train-u-v8ysb-1`, and the reference `p9-b0g-yf0-u`) was produced on the
  screen route (B-1: `--no-composition`, the u-pass blend nobody reads), while P9-L's full-run u carries the spec's
  `ic.flags` `--save-combined` (`scripts/cycle/phases.py` u step: only `--screen` swaps them). Route, not an input of
  the block: the u-pass blend is not read by fit, card, gate or w.
- The u / w passes write new IC-result scope dirs (`label_terminal` in the key) into the real cache: additions only.
- The cell's NAV is one calibrated run (`-cal`, the run recipe that ran); the cell of record `-gm` is bound to it.

## 7. Pre-resume resource amendment (2026-10-17; P9L-WBUDGET)

Code registration `f560f741` changes only `change.set.ic.w_flags.--max-memory-mib` to `"3584"`
and `change.set.runner.phases.w.max_rss_mib` to 3584 in the template and regenerated calibration recipe.
Template SHA-256 `1d15513e74851e776a7e40d538f528690fbab04c4a92bb8afdcc240a805e3170`;
run recipe SHA-256 `1f00e19ec9dc3092ebe570b9645700910062881835dc9cd391623cff76c00062`.
The template was re-locked with `--write --exes`; the calibration recipe was regenerated with
`wave_stage_cell.calibrated_doc`, the same note, G_parent and tolerance; dry lock succeeded.
The complete JSON diff is four added lines, two per file. No research input changes.

Resume exactly `research_cycle.py run scripts/specs/p9/templates/label-terminal.json --stop-after w --attempt w=2
--admission-wait 1800`, preserving completed u / fit / card and failed w run1. Require free RAM >= 4608 MiB
and C: >= 15 GB immediately before launch; if RAM is low, poll every 60 seconds for at most 180 minutes.
A second refusal stops. The remainder of section 3 stands; B.0 void (3) requires mechanics PASS before
return / IC / Sharpe reads. Commit own derivation, then stop for fresh DERIV before any trial-ledger write.
SEAL-HAND supplies the hand scan after each stage. No separate -cal NAV directory or second NAV is planned.

[Resource-dispatch restatement, R15-RAM-NOW, 2026-10-17: the owner directs no RAM waits.
The free-RAM >= 4608 MiB gate and `--admission-wait 1800` above are superseded. The immediate command is
`research_cycle.py run scripts/specs/p9/templates/label-terminal.json --stop-after w --attempt w=2`.
Omit admission-wait and host-budget options for subsequent stages too. The child argv is unchanged;
the runner retains w 300 s / RSS 3584 MiB and its 512 MiB launch/runtime free-memory floor.
C: >= 15 GB and one bounded run at a time still apply. Any actual resource stop requires immediate
evidence and handback; no automatic retry. No source/spec mutation, re-lock or rebuild accompanies this ruling.]
