# P9 pre-registration (`p9-prereg.md`)

**ROUND 1 DRAFT - not ruled.** Lane PRE, round 1 (W3-14, W3-14a), doc agent, 2026-10-04. Nothing here binds until the
PM rules this file after round 2 and its review (W3-14). No P9 cell runs before that.

**Basis read.** `resume4-dispatch.md`; `briefs/brief-PRE.md`; `wave3-carry.md` ("Brief text superseded" PRE items,
"All lanes", "PRE (pool 13)", "PM answers to Q1-Q18", "Added after drafting"); plan
`docs/plans/2026-10-03-p9-sprint-plan.md` (sha256 `4acbde4c...`) sections 0.3, 0.5, 2.4 PRE, 4.4, 5, 6; ledger
`progress.md` (509 lines at drafting; `ledger :N` = line N) from "RESUME 5" to the end plus every ruling cited below;
status-4 and status-5; `task-AL-COMB-report.md`, `task-AL-SIG-report.md`, `task-S2-report.md`, `task-B2-report.md`;
E2's v2 manifest shape in pool-17 at `ea7d9dde` (W3-14a); B2's post-identity commits in pool-14 (`598d2199`,
`8fb17ad6`); the registration dirs `scripts/specs/p9/{candidates,rules,blocked,diagnostics,templates}/` (root has no
`kinds/` yet: E2 is unmerged); `cov-design.md` section 5; `briefs/brief-COV-MV.md`; `al-data/atx-db-request.md` A0
(flag line only); `v8y-prereg.md` and `v8x-prereg.md` (model); branch `feat/platform-v8-xpre-20261002` (OD3-TOOL).
Root heads seen while drafting: docs `3fd0bf98` then `1b8c4df2` (root ran M2g concurrently), code `42f42a07`.

**Blind.** No 2020-2023 return, IC, Sharpe, turnover or NAV statistic was read beyond the public lines of
`progress.md` and the status files; nothing dated 2024-01-01 or later; no data file, NAV dir, summary, trials file,
`wq-explore*`, `build-equity/` dir or PNG opened. Disclosure: `git log` displayed the subject and body of root commit
`1b8c4df2` (M2g results), which state two leverage-pair rho values and a COV-7 identity miss; neither number is used
here (the rho lines stay placeholders until a ledger line carries them). One `git merge-tree --write-tree` dry run
(OD3-TOOL) wrote loose tree objects to the shared object store; no ref, index or worktree changed.

**Conventions.** `[ROUND 2: <what closes it>]` = a named placeholder that round 2 fills; an item marked **Open (H-n)**
is a choice for the PM (section H). Paths are relative to `C:/atx-wt/pool-2` unless a pool is named.

---

## A. Scope, ceilings, starting counts

### A.1 Ceilings (DEC-18 as amended by COV-7 / COV-8; plan section 5.2 :911)

| quantity | registered ceiling | reachable after W3-15 (P9-S lapsed) |
|---|---|---|
| hypothesis cells | <= 8 (P9-S, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X, P9-V) | <= 7 run |
| protocol cells | <= 1 (P9-L) | <= 1 |
| N_c (construction) | <= 62 + 9 = 71 (COV-8, ledger :200) | <= 70 |
| K_a (hand-written admission strings) | <= 10 | 0 added (W3-15, OD5-4) |
| mined campaigns / M | 0 campaigns (OD-P9-9, ledger :17); M + 0 | M = 110 unchanged |
| N_tot = sum(trial_counts) + M | <= 252 | <= 241 |
| history reads (E-41, counted apart, not in N) | - | 0 now; OD-3 adds the registered lines (F.1) |

### A.2 Starting counts (public lines only)

- n_before (R0-14, ledger :190): N_c 62 (cap 62), K_a 40 hand-written (61 admission lines in all: v8 12, X hand-written
  25, X-4 re-screens 9, Y hand-written 15), M 110, N_tot 233; N_hand = N_tot - M = 123 (R0-12, ledger :184).
- Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` (ledger :190, :459, :502;
  `resume4-dispatch.md` :14). No trial since R0-11 (ledger :167).
- Public DSR at the v8 adoption print (ledger :184), printed for continuity, gating nothing: DSR_tot .6798 (N_tot 233),
  DSR_hand .7589 (N 123), DSR_v8 .8350 (N_c 62).

### A.3 E[max] arithmetic (v8y section 4 formula, gamma .5772; recomputed for this file)

E[max] factor = (1 - gamma) Z^-1(1 - 1/N) + gamma Z^-1(1 - 1/(N e)):

| N | 62 | 69 | 70 | 71 | 123 | 131 | 233 | 241 | 252 |
|---|---|---|---|---|---|---|---|---|---|
| factor | 2.3576 | 2.3972 | 2.4025 | 2.4077 | 2.6026 | 2.6242 | 2.8151 | 2.8259 | 2.8402 |

Matches plan section 5.2 :920-923 (2.357, 2.397, 2.4025, 2.4077, 2.815, 2.840). Against the v8 end state: the reachable
maxima raise the factor by +1.90% on N_c (62 -> 70), +0.83% on N_hand (123 -> 131), +0.38% on N_tot (233 -> 241); the
registered ceilings by +2.13% on N_c (62 -> 71) and +0.89% on N_tot (233 -> 252). The plan's "+0.44%" is against its
earlier 69, not against 62. V decides more than N (v8y section 4).

### A.4 Named books

- **R-2** (V8-F): the reference of G-B6 and G-B8. Spec as the v8 OD-3 protocol names it (`scripts/specs/v8/lib-v80.json`,
  v8-od3-protocol section 3.1). [ROUND 2: R-2's re-based NAV reference on the wave-3 build (H22)].
- **Y-F0**: `scripts/specs/v8/lib-v8ysb-gm.json` (sha256 `1e3ec118...`), library `fund_industry_ic_v8ysb.json`
  (`41010b0b...`, 67 candidates), L_P 1.1828 (ledger :147, :190). The first parent of P9.
- **Y-F**: Y-1 (`scripts/specs/v8/y-vol-target-y-1.json`, `faffee17...`; ledger :167).
- **P9-F0**: the last accepted P9 cell, leverage excluded (P9-X, P9-V) and P9-W excluded; equals the re-based Y-F0 if no
  P9 cell is accepted. The book of G-B1, G-B6 and G-B8.
- **P9-F**: the levered book offered to the owner (G-B2). Rule: H13.
- Parent of every cell = the last accepted (plan section 5.2 :915), except P9-W (decides nothing, never a parent),
  P9-X (parent P9-F0) and P9-V (parent and paired reference P9-X; E2-15, ledger :499).

### A.5 Windows and seal (unchanged)

TRAIN [2020-01-01, 2024-01-01), seal 2024-01-01, research-window-v2 (`atx-impl/strategies/research_window.json`).
Nothing dated 2024-01-01 or later is opened by anyone or by code they run (plan section 0.6; `resume4-dispatch.md`
:13). The OD-3 eras end on or before 2020-01-01 (`atx-engine/tools/era_pool.py:40-43` refuses a window past the seal).
`atx-impl/tools/holdout_gate.py` (the OD-1 reader of 2024+, `:1-12`) never runs in P9 (F.2 item 1).

### A.6 Trial counting (v8y section 3, carried; P9 rows)

| event | counts in | count |
|---|---|---|
| P9 construction or protocol cell scored (any row of B) | N_c | 1 |
| P9-S (no string), P9-MV / P9-V / P9-C / P9-W lapsed (B) | - | 0 |
| undefined on its parent (E-45); stopped before any return read | - | 0, logged |
| void after a return read | N_c | 1 (stays) |
| rule-7 tool or data defect: blind re-run / returns re-run | - / N_c | 0 / 1 |
| PM6-6 calibration runs, P9-B0 re-bases, identity runs, builds, ALD-DIAG, Q5, D-COV, S2R-DIAG, ucrtbase records | - | 0 |
| adoption print; OD-3 history lines | - | 0 to N; history reads counted apart (E-41) |
| any 2020-2023 statistic read for a variant not in section B | N_tot | 1 per variant; never enters P9 |

Closing rules (v8y section 3, carried): closed list (section B); a read is a trial; no retry, re-parameterisation or
re-cast of a rejected cell (E-45); voids keep their count; unused budget lapses (plan section 5.2 :918).

### A.7 Order (plan section 5.2 :913-918; never re-ordered after a read)

P9-L -> (P9-S lapsed) -> P9-H -> P9-M -> P9-C -> P9-MV -> P9-W -> P9-X -> P9-V -> adoption print (E) -> OD-3 once (F)
-> freeze (G).

### A.8 Power, stated before any result (plan section 5.2 :927-933)

MDE at 80% power = 2.49 x SE. Combination cells (SE about .2) detect only about .5; single cells are sign-level
evidence; the decision-relevant tests are G-B6 (P9-F0 vs R-2, cumulative) and G-B8 (the history read). P9-V's pair
differs only by the store's forecasts (low-variance difference; small expected effect).

---

## B. The cells

### B.0 Common to every cell

- **Registration before read** (plan section 0.6): the cell's template, rule file, constants and this block are ruled
  before its first read. ids below are <= 18 characters, so they meet W1-I1 (ledger :314) at either eval mode.
- **Manifest.** `atx.research-wave/v2` (K-P9-9) unless the block says "by hand". Kind and block as E2 defines them
  (pool-17 `scripts/wave_kinds.py@ea7d9dde:5-29`, `:58-63`): rules enter only through `rules: [{name, params}]`
  (ALCOMB-1; `:31-37`); templates carry no `rules` key and no `change.flags`; a raw owned flag is refused by the
  E2-8 guard (`:39-47`, `:431-448`, `:477-494`).
- **exes.** Every cell spec names and pins `exes.ic`, `exes.nav` and `exes.eval` by `lock --exes` (E1-REUSE-a2,
  ledger :153; B2-10 (c), :400; B2-11, :427: verify checks key and pin, the judge compares the verb hash and runs the
  pinned verb); all are the Release exes (M2c-RED-2 (e), :441). [ROUND 2: exe paths and SHA-256 from the wave-3 P9-B0
  re-pin commit (W1-I2 (b), B2-10 (b))].
- **Eval mode** = the P9-B0 reference's (PATH-1, ledger :416). Expected research-fast: no ruling switched S2's default.
  No P9 cell is registered under audit-exact, so no short cache dir is needed. [ROUND 2: read the mode from the wave-3
  P9-B0 u receipt].
- **Fit mode** = the P9-B0 parent reference's (ALCOMB-7, ledger :424): `--engine-fit` only if D3 delivered the C++
  sleeve and the wave-3 P9-B0 re-based the parent under engine-fit, else flag-absent. Stated per cell below.
- **Void** (v8y section 12, carried; P9 additions 5-7): (1) run before this file is ruled, before the wave-3 P9-B0 and
  its re-pin commit (W1-I2 (b)), or before ALD-DIAG is printed (OD5-1 (iii)); (2) any input differs from its block
  (string, constant, template or registration SHA-256, parent not the last accepted, flag, fields dir, L other than
  PM6-6's correction or the registered 2.0, store pin, exe pin, eval or fit mode); (3) a return, IC, Sharpe or NAV
  statistic of the cell read before its mechanics passed; (4) a session at or after 2024-01-01 opened; (5) an exe
  rebuilt after the re-pin without P9-B0 redone (EXE-FREEZE, ledger :406); (6) `ucrtbase.dll` version or SHA-256
  differs from the P9-B0 record (M2c-RED-2 (d)); (7) a SQLite cache index flag on any argv (SQL-11, ledger :212).
  Not voids: rule-7 defects; mechanics fail (rejected, counted); undefined on its parent (0); a driver stop.
- **Standing lines** of section C apply to every cell.

### B.1 P9-L: survival-conditioned IC labels (protocol cell; DEC-17)

- **id** `p9l-label-terminal`. **Class** protocol (B0c precedent). **Counts** N_c +1 if run; K_a 0.
- **Change.** `--label-terminal imputed-v1` with `--train-label-role <label role> --train-label-role-sha256 <pin>` on
  the IC passes (u and w), on the parent; fit, card, w and NAV follow (plan section 2.4 S2 :490-498;
  `task-S2-report.md:23`). Label role: `build-equity/train-2020-2023-lo3-dlret` (`95e16cfe...`, the parent spec's
  `inputs.label_role`, `scripts/specs/v8/lib-v8ysb-gm.json:53-57`). No constant.
- **Template / manifest.** None exists in `scripts/specs/p9/templates/`. No v2 kind carries the flag: kind `rule`
  needs a non-empty `rules` list (pool-17 `wave_kinds.py@ea7d9dde:161-162`) and `--label-terminal` is no rule row.
  **Open (H2)**: by hand through `research_cycle.py run` of a v1 template. [ROUND 2: root writes
  `scripts/specs/p9/templates/label-terminal.json` from this block; its SHA-256 recorded here].
- **Parent / paired reference.** Y-F0 as re-based at the wave-3 P9-B0. [ROUND 2: P9-B0 reference dirs and SHA-256].
- **Gross match.** **Open (H3)**: recommend `gross_match: pm6-6-engine` (DEC-10: PM6-6 binds every construction cell;
  plan section 5.2 :916-917 lists P9-S, P9-H, P9-M, P9-C only).
- **Acceptance (decides).** Mechanics only (G-B9 limits); adopted as the new parent iff mechanics pass. The driver
  has no "mechanics only" rule (`scripts/wave_rules.py:143`: `pm7-34` and `v8-prereg-5` only): the verdict is applied
  by hand and re-derived independently before the logged verdict is opened (ledger :157, :172 precedent) (H5).
- **Printed only.** Paired dSR vs the parent and both p (the pinned verb through `nav_summ`, `atx-impl/tools/
  nav_summ.py:205-210`); orientation flips (u `orientations.json`, parent vs cell); admission changes (fit
  `admission.json`, parent vs cell); the IC exe's `ic_missing_exit_v1`, `terminal_imputed_pairs`,
  `missing_exit_share` and `guard_excluded_pairs` (`task-S2-report.md:23`, `:195-204`).
- **Predictions.** None registered (protocol cell; plan section 7.2 gives -.02 to +.02 [est], not a prediction).
- **Fit mode** [ROUND 2: P9-L fit mode from the wave-3 P9-B0 parent reference (ALCOMB-7)]. Eval mode: B.0.
- **Exposure (S2-FIX1 (3), ledger :325).** Under `--label-terminal` a delisting cause may be classified up to 30 days
  after termination T, so labels whose T falls within 30 days of TRAIN end can embed post-TRAIN information.
  **Open (H4)**: recommend stating the exposure (no code change; its population is the exits ALD-DIAG prints as
  "unclassified (within 30 days of the seal)", ALD-10) rather than truncating imputation (a change in D3's IC runner
  file, W3-7).
- **Inputs (S2-6, ledger :365; `task-S2-rereview-1.md:62-68`).** Before the run root shows that no `imputed-v1`
  IC-result entry written by a pre-`110fd735` build exists under any cache root the cell reads (M2c found none,
  ledger :412); one found is deleted by name and logged, never reused. Post-fix rule, stated: the terminal step T =
  L+1 is guard-checked on the label role (|log r| > 1.5 or adjusted vs raw drift > .10 excluded and counted in
  `guard_excluded_pairs`; the builder's Shumway set {0, -.30, -.55} passes) (`task-S2-report.md:195-204`). The carry's
  "an imputed terminal return is never guard-checked" describes the pre-fix exe only.
- **Undefined (0).** The parent's IC passes refuse `--label-terminal` with its flags; no label role pinned. **Lapsed:**
  never (S2 merged).

### B.2 P9-H: `theme-cov-hedge-v1` (DEC-15; first hypothesis cell)

- **id** `p9h-cov-hedge`. **Kind** `rule`, `rules: [{name: theme-cov-hedge-v1, params: {}}]` (`params {}` = every
  registered constant). **Counts** N_c +1 if run; K_a 0.
- **Registration** `scripts/specs/p9/rules/theme-cov-hedge-v1.json` (sha256 `55f25f2f...`); template
  `scripts/specs/p9/templates/theme-cov-hedge.json` (`7c6778bb...`, = the registration's `template_sha256`). Root sets
  `parent` in the template and re-pins (registration `manifest_note`); the only allowed diff is `/parent`, checked by
  JSON diff (R0-9-PIN precedent, ledger :127). [ROUND 2: re-pinned template SHA-256].
- **Rule and constants.** Per theme t and decision d on ew-theme-std-v1's re-rank R_t: beta of each name on its own
  theme sleeve over the trailing 252 decisions ending d-2 (>= 126 pairs), orthogonalised to R_t; H_t = its centred
  rank; h_t = OLS slope of the sleeve on the hedge leg, re-estimated every 21 decisions from decision 253 (0 before);
  the theme adds W_t (R_t - h_t H_t). Constants lookback 252, lag 2, step 21, min_pairs 126 (registration
  `constants`). h_t unbounded and the hedged re-rank not re-standardised (S-2, ALCOMB-5; registration `fixed_choices`).
- **Parent.** The last accepted; its fit must be `--composition ew-theme-std-v1` or `ic-shrink-v1` (+/- `--theme-erc`,
  `--theme-tsmom`, `--mom-volman`), no theme-resid / two-speed (template `requires`; `atx-impl/tools/
  composition_theme_hedge.py:27-30`). Y-F0's fit is `ew-theme-std-v1 --theme-erc theme-erc-v1`
  (`lib-v8ysb-gm.json:205-213`): defined.
- **Registration gate.** The Q5 zero-trial diagnostic (`scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json`,
  `721889d1...`) prints q and q_lit before the PM registers P9-H (plan section 2.4 :588-590). [ROUND 2: q and q_lit,
  with each style column's share]. **Open (H6)**: the registration threshold, fixed before q is printed.
- **gross_match** `pm6-6-engine` (plan section 5.2 :916-917; C2-9, C2-11). **Acceptance (decides)** `pm7-34`: paired S2
  net dSR > 0 vs the parent AND mechanics (`scripts/wave_rules.py:143`).
- **Registered predictions and producers** (ALCOMB-9 (b), ledger :448):
  - realised vol ratio (cell / parent) < 1: the NAV exe's scenario `ann_vol` (`atx-impl/src/strategy_nav_replay.cpp:
    2001`) of the two NAV dirs, read by `scripts/wave_readers.py:129`. `atx-research-eval` prints no vol (its moments
    are sessions, sr_daily, skew, kurtosis: `atx-engine/src/research/eval/eval_report.cpp:41-46`), so ALCOMB-9's
    named producer cannot print it; the NAV exe can.
  - gross-alpha ratio about 1: no C++-printed figure. The NAV exe writes the daily `gross_return` column
    (`strategy_nav_replay.cpp:2063`, `:2080`) and prints `gross_sharpe` (`:1997`) but no gross mean; the wave reader
    sums C++ `ann_mean` and the annualised C++ cost / financing returns (`scripts/wave_readers.py:120-124`).
    **Open (H7)**.
  - "ex-ante vol ratio" (template text): cut before registration unless Q5 prints it (ALCOMB-9 (b)); Q5 prints the
    parent book's `mean_ex_ante_vol_annual` only (Q5 spec `prints.context`), so it is cut. [ROUND 2: root removes
    "ex-ante" from the template description and re-pins].
- **Printed only.** Hedge ratio per theme and block (IC exe `composition_hedge_trace`, AL-COMB S-3); turnover per unit
  gross (`scripts/wave_readers.py:131`); cost per traded dollar (`:132`; C++ `cost_bps_per_traded_dollar`,
  `atx-impl/src/strategy_nav_v7.cpp:360`); net Sharpe at 4x NAV (`capacity_curve.csv`, `strategy_nav_v7.cpp:357-382`).
- **Fit mode** [ROUND 2: P9-H fit mode (ALCOMB-7; `--theme-hedge` is accepted under `--engine-fit`, AL-COMB report
  "Combined round")]. Eval mode: B.0.
- **Undefined (0).** Parent outside the list above; the exe or fitter refuses. **Lapsed (0):** if the PM does not
  register it under H6.
- **Expected effect** +.05 to +.15 [est] (registration `predictions.expected_effect`), printed, deciding nothing.

### B.3 P9-M: `mom-volman-v1` (DEC-16: distinct from Y-2 and Y-1, ledger :79; Y-2 read at R0-8, ledger :138)

- **id** `p9m-mom-volman`. **Kind** `rule`, `rules: [{name: mom-volman-v1, params: {}}]`. **Counts** N_c +1 if run.
- **Registration** `scripts/specs/p9/rules/mom-volman-v1.json` (`8a7222fa...`); template
  `scripts/specs/p9/templates/mom-volman.json` (`7e9b26c1...`, = registration). Re-pin after `parent` as B.2.
  [ROUND 2: re-pinned template SHA-256].
- **Rule and constants.** The price_momentum theme's mass x target_b / sigma_b per block; sigma_b = sqrt(mean r(d)^2)
  over the 126 decisions ending j_b - 2; target_b = running mean of sigma_0..sigma_b; blocks start at 127 + 21 b;
  lookback 126, lag 2, step 21, no cap; a non-finite scale is refused (registration `constants`, `fixed_choices`).
- **Parent.** The last accepted with `--composition ew-theme-std-v1` or `ic-shrink-v1` (+/- `--theme-erc`), weighting
  price_momentum, no theme-tsmom / theme-resid (`atx-impl/tools/composition_mom_volman.py:41`, `:134-136`). The fitter
  does not refuse a `--theme-hedge` parent (`:134-136` checks only theme-resid, theme-tsmom, era, engine-fit), and the
  two rows do not exclude each other. ALCOMB-9 (a)'s "the P9 parent is neither" holds only if P9-H is rejected.
  **Open (H8)**.
- **gross_match** `pm6-6-engine`. **Acceptance** `pm7-34`.
- **Registered prediction** "the momentum sleeve's realised volatility falls and its Sharpe rises after scaling":
  kept only if D3 makes the IC exe write the sleeve's daily series before and after scaling and `atx-research-eval`
  prints them (ALCOMB-6, ledger :423). [ROUND 2: keep or cut the sleeve line (D3 report); if cut, root cuts the
  template sentence and re-pins].
- **Printed only.** sigma_b, target_b, scale_b per block (IC exe `composition_schedule_trace`, AL-COMB S-3); turnover
  per unit gross; net Sharpe at 4x NAV (producers as B.2).
- **Fit mode.** Flag-absent (fixed): `--engine-fit --mom-volman` is refused (`composition_mom_volman.py:53-55`,
  `:137`) until D3 adds the sleeve (ALCOMB-7). [ROUND 2: confirm the parent's P9-B0 fit mode is flag-absent; if the
  parent was re-based under engine-fit, P9-M is undefined unless D3 delivered].
- **Limit (ALCOMB-4, ledger :268).** The schedule is fixed on the fit's TRAIN sleeve: outside those sessions the
  parent's masses hold before the first recorded block and the last block's after it. It binds P9-W and OD-3 (F.1).
- **Undefined (0).** Parent outside the list (H8); price_momentum unweighted. **Expected effect** +.01 to +.03 [est].

### B.4 P9-C: `info-clock-rate-v1` (DEC-16: distinct from Y-5)

- **id** `p9c-info-clock`. **Kind** `rule` naming the trade-rate NAV row; `gross_match: pm6-6-engine`, leverage
  published by the match stage (the shape E2-13 (b) requires to work, ledger :481). **Counts** N_c +1 if run.
- **Fixed now.** theta_f = `two_speed_fast_theta(cadence)` = 1 - 2^(-C/5) (W3-16; `atx-engine/include/atx/engine/book/
  two_speed.hpp:21-23`, `:45-46`), slow rate .05 (`:39`); fast while `ea_days_since` <= 21 (plan section 2.4 :601-605).
  Acceptance `pm7-34`. Printed: fast-rate share of name-days, cost per traded dollar, 4x (plan section 5.2 :906).
  OD-3 prediction "turnover share up, gross up" (plan section 5.3 :946).
- [ROUND 2: AL-CLOCK's report: rule id and row, registration file and SHA-256, template and SHA-256, parent
  conditions, producers of the fast-rate share and of the turnover / gross predictions, fit mode, the restatement
  argument against Y-5 (DEC-16), the `source_sample_end` of [29]].
- **Lapsed (0).** **Open (H14)**: recommend lapse if AL-CLOCK is not merged and APPROVE before the P9 cells start.

### B.5 P9-MV: `mv-aim-v1` (COV-7, ledger :199; funded)

- **id** `p9mv-mv-aim`. **Kind** `rule` naming the NAV `target` row `mv-aim-v1`; `gross_match: pm6-6-engine` (plan
  section 5.2 :910); acceptance `pm7-34`. **Counts** N_c +1 if run.
- **Store.** The `atx-cov-v1` container store D-COV produces (W3-10, ledger :388). S9 / COV-9: root re-checks
  `universe.delisting.returns_applied == false` on the role before the run (ledger :354, :401).
- **Gross rule** as `brief-COV-MV.md:96-99` (W3-17): the priced block rescaled to the desired target's gross over the
  priced names; unpriced names keep their weights.
- **Predictions (plan section 5.2 :910):** vol ratio < 1 at gross-alpha ratio about 1 (producers and H7 as B.2);
  printed: turnover per unit gross, capacity curve, 4x, the rule's `mv_aim` summary block (`brief-COV-MV.md:107-108`).
- **Interface item.** E2's guard owns `--risk-model` / `--risk-model-sha256` (they are param `x-flag`s of the
  vol-target-v1 and risk-target-v1 rows, `atx-impl/src/strategy_risk_target.cpp:341-346`, `:366-382`;
  `table_flags`, pool-17 `wave_kinds.py@ea7d9dde:431-448`), so a template cannot carry them; the brief's "closed
  `params_schema` with no free parameter" plus `params: {}` (`brief-COV-MV.md:84-85`, `:119-120`) leaves no route for
  the store pin. **Open (H9)**.
- **OD-3 consequence.** If P9-MV is accepted, P9-F0 reads `--risk-model` and plan section 5.3 :954-957 makes it
  unreadable on history. **Open (H10)**.
- [ROUND 2: COV-MV merged and APPROVE (else dropped at 0, COV-7); the store pin (D-COV manifest SHA-256); registration
  and template SHA-256; `source_sample_end`; fit mode (ALCOMB-7); the PM's read of the spo-v1 / spo-v2 v7 cell outcomes
  (cov-design section 5.2 :553-555, H24)].
- **Lapsed (0)** if COV-MV is not merged and APPROVE before the P9 cells start (COV-7).

### B.6 P9-W: walk-forward refit (K-P9-8 = `atx.dsl-composition-weights/v3`, P19)

- **id** `p9w-walk-forward`. **Kind** `walk-forward` (schema only until D3: pool-17 `wave_kinds.py@ea7d9dde:28-29`,
  `:63`, `:255-262`; `lag >= 2`). **Counts** N_c +1 if run.
- **Rule.** The accepted book's fitted weights refit walk-forward: expanding, lag 2, step 21 (plan section 5.2 :907).
- **Parent / reference.** The in-sample P9-F0 candidate (the last accepted after P9-MV). Decides nothing; never
  accepted, never a parent.
- **Printed.** Paired dSR vs the in-sample parent (pinned verb). Limit: if mom-volman is in the book its schedule stays
  TRAIN-fixed (ALCOMB-4); theme-cov-hedge has no such limit (`task-AL-COMB-report.md:323-327`).
- **OD-3 prediction it feeds** (formula fixed now; number filled mechanically after P9-W): predicted history S2 net
  Sharpe of P9-F0 = its TRAIN S2 net Sharpe + P9-W's dSR (walk-forward minus in-sample). **Open (H23)**.
- [ROUND 2: D3's report: the kind's manifest shape, gross_match (H23), fit mode, the producer of the per-date
  weights; template and SHA-256].
- **Lapsed (0).** **Open (H14)**: recommend lapse if D3 is not merged and APPROVE before the P9 cells start.

### B.7 P9-X: leverage (OD5-7, ledger :467: L <= 2.0; form recommended in section D)

- **id** `p9x-leverage`. **Counts** N_c +1 if run. **Parent** P9-F0 at L_P. Undefined if L_P >= 2.0 (v8y section 6).
- **Form (recommended, D): Y-1 form.** vol-target-v1 at cap 2.0 on P9-F0: L_t = clip(2.0 x sigma_ref_t / sigma_hat_t,
  1, 2.0), sigma_hat_t = risk-target-v1's forecast on the pinned atx-risk-v1.1 store of the role, sigma_ref_t = running
  mean of the book's own estimates (current included), re-estimated every 21 sessions, cap before the first estimate;
  constants cap 2.0, floor 1, cadence 21, annualisation 252 (`scripts/specs/v8/y-vol-target.json`, `4e190f54...`,
  description; v8y section 8). In v2 terms: `rules: [{name: vol-target-v1, params: {aim_leverage: 2.0, risk_model:
  <dir>, risk_model_sha256: <pin>}}]` (row `strategy_risk_target.cpp:366-370`; `aim_leverage` range [1, 2] at
  `:337-340`); `gross_match: "none"` (PM7-20 reading B). The v8 template's raw `change.flags nav --vol-target` would be
  refused by the E2-8 guard, so a P9 template without flags is needed, or the cell runs by hand (H11). [ROUND 2:
  store-of-record pin (v1.1 rebuilt at COV's merge, ledger :450) and role-pin check; P9 template and SHA-256 if not by
  hand].
- **Alternative: X-10 form.** Kind `leverage`, `leverage: {value: "2.0"}`, `gross_match: "none"` (required:
  pool-17 `wave_kinds.py@ea7d9dde:180-189`); value range-checked against fixed-v1 and <= `LEVERAGE_MAX` 2.0 (`:93`,
  `:498-505`; E2-10).
- **Acceptance (decides), either form:** X-10's rule (PM7-34 (3)): S2 net annual return (1x) above P9-F0's AND S2 net
  Sharpe not lower than P9-F0's by more than .100 AND mechanics at the scaled limits, written in the cell's plan
  before the run: X-10 form gross [.90, 1.05] x r x the parent's matched gross ratio, r = 2.0 / L_P, |mean net| <= .02 r,
  tau mean <= .20 r, p95 <= .30 r (v8x section 8; `scripts/specs/v8/x-leverage-L2.0.json` description); Y-1 form gross
  inside [1.0, 2.0] x G_P / L_P widened by .005, the other rows at X-10's scaled limits (v8y section 8). The driver has
  neither rule nor scaling (`scripts/wave_rules.py:79`, `:143`): verdict by hand with independent re-derivation
  (ledger :157, :172) (H11).
- **Degenerate pair (B2-13, ledger :503).** If 1 - rho < 1e-6 vs P9-F0, the paired SE / t / p print "na" (B2-6) and
  the cell is decided on its non-paired lines only (the acceptance above has none paired).
- **Printed.** P9-F0 / P9-X (and Y-F0) side by side: net annual return, net Sharpe at 1x and 4x, realised vol, max
  drawdown (NAV exe scenario summary, `strategy_nav_replay.cpp:1996-2002`; 4x `strategy_nav_v7.cpp:357-382`); mean L_t
  (Y-1 form: the NAV's `vol_target.csv`); paired dSR vs P9-F0 and both p (pinned verb). [ROUND 2: rho of the P9-X pair's
  reference on the wave-3 build (B2-13)].
- **Fit / eval mode:** NAV-only cell (u, fit, card, w are P9-F0's).

### B.8 P9-V: vol-target-v1 on `atx-cov-v1` (COV-6, ledger :198)

- **id** `p9v-vol-target-cov`. **Registered only under the Y-1 form of P9-X; lapses at 0 under the X-10 form.**
  **Counts** N_c +1 if run.
- **Rule.** vol-target-v1 with its registered constants (cap = P9-X's L 2.0, floor 1, cadence 21, annualisation 252)
  on P9-F0, reading the `atx-cov-v1` store D-COV pins instead of v1.1 (plan section 5.2 :909). `rules:
  [{name: vol-target-v1, params: {aim_leverage: 2.0, risk_model: <atx-cov-v1 dir>, risk_model_sha256: <D-COV pin>}}]`
  if run as a v2 wave; lineage parent named = P9-X (E2-15). `gross_match: "none"`.
- **Acceptance (decides).** Paired dSR > 0 vs P9-X AND mechanics at P9-X's scaled limits; a rejected P9-X still serves
  as the reference (plan section 5.2 :915-917). By hand (H11). Degenerate pair: H12.
- **Registered predictions** = D-COV's printed numbers (plan section 3.4 step 6; COV-5): the book's sigma_hat bias
  closer to 1; mean L_t and its changes; D-COV's minvar / optimized bias rows beside the result; D-COV read in both
  directions (b below 1 = over-corrected; COV-11, ledger :402). [ROUND 2: D-COV's print (COV-7 Debug = Release
  container identity first; root commit `1b8c4df2` reports that identity missed and D-COV waits) and the producer of
  the book's sigma_hat bias].
- **Fixed.** Every `atx-cov-v1` parameter (a = 1, half-lives, seeds) as in brief-COV; a change after D-COV is a new
  recipe id and a P10 note (COV-5, ledger :197). S9 role check before the run (COV-9). Not read on history (plan
  section 5.3 :955-956).

### B.9 P9-S: lapsed

P9-S lapses at 0 trials; N_c and K_a unchanged (W3-15, ledger :393; OD5-4, ledger :464). Admission list: empty (0
strings; no roster, no DSL or argv SHA-256 to pin). Nothing from WQ-EXPLORE is registered (WQ-1, ledger :343).

---

## C. Standing lines (every cell, every print, the adoption print, the OD-3 print, the freeze report)

1. **Identity-bridge flag** (OD5-1 (i), ledger :461, text per ALD-9, :473): "universe `linked-operating-v3` and the
   fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in
   `al-data/atx-db-request.md` A0); size unmeasured". G-B1, G-B2 and G-B8 print with it. Absolute Sharpe / return
   figures are stated as possibly overstated by survivorship; the evidence of record is the paired, relative tests
   (OD5-1 (ii)).
2. **ALD-DIAG** (OD5-1 (iii), ALD-10, ledger :474): per year 2020-2023 on role `train-2020-2023-lo3`, the count of
   universe names and of exits through a delisting with last session on or before 2023-12-01; exits after that date
   printed as "unclassified (within 30 days of the seal)"; no return statistic; gates nothing; printed here beside the
   flag. [ROUND 2: ALD-DIAG table (M2g ran it, root commit `1b8c4df2`) once recorded in a ledger line].
3. **G-B2 hurdle** 8.0%, labelled "plan placeholder, not owner-stated" in every print (OD5-6, ledger :466).
4. **Leverage** L <= 2.0 in every P9 cell (OD5-7; E2-10, ledger :438; C3-1, ledger :507).
5. **Timings off** for every pinned run (W1-MIN M-3, ledger :316).
6. **CRT record** (M2c-RED-2 (d), ledger :441): `ucrtbase.dll` file version and SHA-256 recorded at each P9-B0 and
   before each cell; a change means P9-B0 and the re-pin are redone before any further cell. [ROUND 2: the wave-3
   P9-B0 record].
7. **Release** exes for every run of record (M2c-RED-2 (e)); the Release lit-golden test is a named skip
   (M2c-RED-2 (b)-(c); ledger :502).
8. **EXE-FREEZE** (ledger :406): after the wave-3 P9-B0 and its re-pin commit the NAV, IC and eval exes are not
   rebuilt until the freeze gate; an unavoidable rebuild redoes P9-B0 and the re-pin first, no cell in between.
9. **S2R list** (S2R-1, :468; S2R-2, :478): `ts_zscore` is used by 0 of the 67 book candidates and 0 P9 strings
   (ledger :477); `smax` and `smax5` carry the line "AuditExact NaN may be ResearchFast 0 on a flat window"; every
   cell runs in its reference's eval mode (PATH-1). S2R-DIAG (count of NaN-vs-finite cells for the two) printed if root
   ran it, else "skipped". [ROUND 2: S2R-DIAG count or "skipped"].
10. **P9-L exposure and inputs**: the 30-day line of B.1 and the S2-6 check (ledger :325, :365).
11. **Paired-test fallback** (B2-R1 (c), :310; B2-6, :347; B2-7, :348; B2-13, :503): a pair with 1 - rho < 1e-6
    prints SE / t / p "na"; root logs rho before each paired read; a printed value on a rounding boundary is listed and
    reported to the PM, never auto-accepted. Measured: X-5 vs X-3 .92027, Y-F0 vs X-5 .95560 (ledger :502). [ROUND 2:
    X-10 vs Y-F0 and Y-1 vs X-10 rho (B2-13) and the P9 pairs' references at the wave-3 P9-B0]. Fallback per cell:
    P9-X B2-13; others H12.
12. **Not queued**: `hf_crowd` (ALSIG-4, :258), `rank(russell_recon)` (dropped: ALSIG-5 :264; non-PIT: ALSIG-7 :307,
    ALSIG-8 :308; status `dropped`, final), `rank(gia_13f)` (not registered: ALSIG-2), `lazy_prices` (blocked_on_data;
    fails the bar, ALD-4), nothing from WQ-EXPLORE (WQ-1). DSL SHA-256 recomputed: `rank(russell_recon)`
    `2aad0b86...`, `rank(hf_crowd)` `1881f8ce...`, `rank(lazy_prices)` `bcbe2653...` (each = its registration).
13. **No SQLite cache index** in any cell; no `--candidate-cache-index` / `--pair-cache-index` on any argv (SQL-11,
    :212); SQL4's identity runs join the wave-3 set only if SQL4 merges before the cells (SQL-12, :213).
14. **Run caps** `--max-rss-mib 2048` unless the phase is known to need more, then root reads free memory first and
    states the figure (RUN-1, ledger :451); the parent spec's registered phase caps (u 2,560, w 3,072, card 2,560 MiB;
    `lib-v8ysb-gm.json:10-28`) are such known needs.
15. **Lineage pins** (E1-REUSE-a2, :153; W1-I2 (a)-(b), :315; B2-10 (b), :400): parents re-pinned to the P9-B0
    references after wave 2 and again after wave 3, `exes.eval` included, before the first cell; verify refuses a rule
    cell whose parent NAV came from another NAV exe.
16. **G-P11 / COV-6**: role `train-2020-2023-lo3` has `universe.delisting.returns_applied: false` (ledger :292); root
    re-checks it before every run that reads `--risk-model` (COV-9, :354; ledger :401).
17. **COV-5**: every `atx-cov-v1` parameter fixed; COV-11: D-COV read in both directions.
18. **DEC-16** (ledger :79): info-clock vs Y-5 and mom-volman vs Y-2 / Y-1 ruled distinct; AL-CLOCK's restatement
    argument recorded in B.4.
19. **M1c-RED / C2-5** (:244, :291): Release `norm-score-v1` books are evidence after C2's fix (cleared, ledger :488);
    `rel` / `rel-avx2` are not identity surfaces.
20. **Trusted-caller flags** (`task-B2-report.md:400`, B2 review S4): `--trial` / `--history` of `atx-research-eval` are
    set only by `nav_summ`; E[max] / DSR arithmetic uses the N `nav_summ` derives; house DSR uses the bisection
    `norm_ppf` (B2-9, :350).
21. **E1-STALE** (:141): never merge under a running wave.

---

## D. P9-X form (OD5-7): recommendation for the PM

**Recommendation: the Y-1 form** (vol-target-v1 at cap 2.0 on P9-F0, reading the atx-risk-v1.1 store of record).

Argued only from these public lines and the plan:
- Both forms passed X-10's rule on Y-F0 in v8: X-10 accepted (net 9.306% > 5.654%; S2 net Sharpe 1.8083 >= 1.7495;
  ledger :156), Y-1 accepted (paired dSR vs X-10 +.0215, Memmel SE .0518, p(1s) .2980; net .087990; Sharpe 1.829770;
  ledger :167); both re-derived independently (ledger :184).
- Side by side (R0-14, ledger :190): Y-F0 5.654% / 1.8495 / 1.6994 / 3.057% / 2.580%; X-10 9.306% / 1.8083 / 1.5829 /
  5.146% / 4.326%; Y-1 8.799% / 1.8298 / 1.6281 / 4.809% / 3.602% (net / 1x SR / 4x SR / vol / max DD).
- The owner's order is Sharpe, gross return, capacity, significance (plan section 0.2; header). On these points Y-1
  ranks first on Sharpe (+.0215 at 1x), capacity (+.045 at 4x) and drawdown; X-10 ranks first on net return (+.51%/yr).
  Both clear the 8.0% placeholder hurdle (OD5-6, ledger :466).
- Y-1 already replaced X-10 as Y-F, the levered book offered to the owner (ledger :167, :190): continuity.
- P9-V, the direct test of the COV recipe in the existing vol-control work, is registered only under the Y-1 form
  (COV-6, ledger :198; plan section 5.2 :909; the owner's COV directive, ledger :175).
- Honest limit: Y-1 vs X-10 is not significant (p(1s) .298, ledger :167); the choice follows the owner's order on point
  estimates, not a test.

Consequences:
- **P9-V**: registered and run after P9-X (B.8); N_c reachable 70 (A.1).
- **OD-3**: unaffected for G-B8, which reads the unlevered P9-F0 in either form. Under the Y-1 form the levered P9-F
  reads `--risk-model` and gets no history read (plan section 5.3 :954-957); P9-V is not read either (:955-956).
- **Mechanics and verdict by hand** in either form (B.7; H11).

**Alternative: the X-10 form** (fixed L 2.0, kind `leverage`). For it: higher net return (priority 2); no risk-store
dependence; the native v2 shape for a leverage change (E2-13 (b): "a leverage change is P9-X, kind `leverage`",
ledger :481); the levered book's history behaviour follows P9-F0's (a scale change), so the deployable book is not
history-blind; one trial fewer (P9-V lapses at 0; E[max] -0.22%).

What would change the recommendation: (a) the PM weighs the deployable book's net return or its history readability
above the 1x / 4x Sharpe point estimates; (b) a P9 Y-1-form cell cannot be expressed or run (no v2 route and no hand
route agreed: H11); (c) the S9 role check or the v1.1 store pin fails before P9-X; (d) a written owner statement on
production leverage (OD-P9-3) that names the fixed form.

---

## E. The adoption print (once, after the last defined cell; plan section 5.2 :925, section 4.3 phase 3)

Rule (PM7-34 (2), as v8y section 7): P9-F0 is the deployable unlevered book iff its S2 net Sharpe >= 1.0 AND its
mechanics pass. Every line below prints; only that rule decides. Run with the pinned verb (`nav_summ ... --engine-stats
<exes.eval>`; its SHA-256 printed from `nav_summ_run`; B2-11). Every line carries the identity-bridge flag where C.1
says so.

| line | content | producer | flag / label |
|---|---|---|---|
| G-B1 | S2 net Sharpe of P9-F0 >= 1.0 (target 1.85) | the summ of record (`net_moments` of the pinned verb; the NAV exe's `net_sharpe`, `strategy_nav_replay.cpp:1996`) | identity-bridge flag; "possibly overstated by survivorship" |
| G-B2 | S2 net annual return of P9-F >= 8.0% | NAV exe scenario `ann_mean` (`:2000`) | "plan placeholder, not owner-stated"; identity-bridge flag |
| G-B3 | gross-of-cost annual return; per unit of all-rows gross (floor X-5's 6.54%; target levered >= 10%) | `scripts/wave_readers.py:120-124`, `:125` (H7) | printed |
| G-B4 | net Sharpe at 4x NAV >= 1.50 and not lower than the parent's by > .10 (target 1.70) | `capacity_curve.csv` 4x row (`strategy_nav_v7.cpp:357-382`) | - |
| G-B5 | DSR_tot (N_tot), DSR_hand (N_tot - M; "n/a" rule of YP-2), DSR_v8 (N_c); gates nothing | pinned verb: DSR_tot = `dsr.tot.tot`, DSR_v8 = `dsr.house_v1` (`eval_report.cpp:128-175`, `:564-577`); DSR_hand: H15 | target >= .80, printed only |
| G-B6 | cumulative paired dSR P9-F0 vs R-2, block 21, seed 20260929, 4,999 resamples; one-sided p < .05 (target .01); both p | `nav_summ --protocol v8 --bundle <R-2 nav> <P9-F0 nav>` on the pinned verb (`paired_of(..., one_sided=True)`, `nav_summ.py:205-210`) | identity bridge shared on both sides |
| G-B7 | winner's-curse-adjusted cumulative gain (AKM conditional, threshold 0, alpha .05, SE basis Memmel) beside the raw sum | the verb's `conditional` block, `--step LABEL=DSR,SE` rows (`eval_cli.cpp:22-23`; `eval_report.cpp:495-534`); `nav_summ` does not print it (`task-B2-report.md:211-212`), so root calls the verb directly | printed; steps list H16 [ROUND 2: the step values from the ledger's accepted lines] |
| G-B8 | the OD-3 read (section F) | section F | identity-bridge flag |
| G-B9 | mechanics: all-rows gross [.90, 1.05] (x L / L_P for leverage cells), abs mean net <= .02, tau mean <= .20, p95 <= .30 | driver verify `v8-mech` (`scripts/wave_rules.py:79-101`); scaled by hand for P9-X / P9-V | - |

Printed beside, deciding nothing (v8y section 7, carried): PBO (both readings labelled, ledger :186 precedent); year
table; net annual return; turnover; Y-F0 / P9-F0 / P9-F side by side (plan section 5.2 :908); bundles Y-F0 vs P9-F0
(the P9 increment) and R-2 vs Y-F0; each cell's verdict; P9-W's dSR; the power line (A.8); ALD-DIAG (C.2).

**Appendix A accounting block** (on every cell result and on the print). Machine part from `nav_summ.py --ledger-n
build-equity/trials.jsonl --protocol v8` (`atx-impl/tools/nav_summ.py:1196-1201`; `atx-impl/tools/
backtest_integrity.py:1255-1289` `appendix_a`, `:1292-1312` `appendix_a_v8`); split filled from the ledger:

`TRAIN construction cells <N_c>; admission trials this sprint <k> (v8 12, X hand-written 25, X-4 re-screens 9,
Y hand-written 15, P9 hand-written 0 [P9-S lapsed, W3-15]); mined campaigns in P9 0 (v9-mine-c1 from v8: M 110);
N_tot <n>; N_hand <n - 110>; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation
reads before v8: 2 (2023-2024); history reads <r>; 2025+ never read; identity-bridge flag (C.1).`

---

## F. The OD-3 history read (plan section 5.3; DEC-3; OD-P9-2) and OD3-TOOL

### F.1 Runbook (registered; adapted from `v8-od3-protocol.md` @ `a7671d1a`, sha256 `640cd91e...`, section 5)

- **Once.** One read event after the adoption print, on the frozen P9-F0 with Y-F0 beside it (R0-13 read nothing,
  ledger :188; plan section 5.3 :940-942). Nothing in either book changes afterwards. No era u, w or NAV pass runs
  before the last P9 cell is ledgered (PM7-29 choice 11, carried).
- **Not via `holdout_gate.py`.** That tool reads 2024+ (OD-1; `atx-impl/tools/holdout_gate.py:1-12`;
  `v8-od3-protocol.md` section 1 "holdout_gate.py reads only 2024 and later"); plan section 5.3 :941's citation of it is
  an error (F.2).
- **Window.** Eras E1 = [2015-01-01, 2017-01-01) (504 sessions) and E2 = [2017-01-01, 2020-01-01) (754 sessions),
  warm-up from 2013-06 / 2015-06, score_begin 399 (v8-od3-protocol sections 3.2, 4 choice 2: 2013 unscorable, 2014
  dropped by source dates). E3 = `train-2020-2023-lo3` as built. **Open (H17)**: G-B8 decides on E1+E2 pooled
  (history alone); the 2015-2023 pool is printed beside (lit section 5.3: MDE about .40-.45 vs about .30).
- **Books and history lines** (E-41: each adds 0 to N, 1 to history reads): P9-F0 (the book of G-B8), Y-F0 (beside),
  R-2 (G-B8's reference), P9-F0 without the flagged members (plan section 5.3 :950). Four lines. **Open (H18, H20)**.
- **Weights.** TRAIN admission, signs and weights carried back unchanged; no re-admission, no re-fit (PM7-29 choice 1,
  carried). Rules estimated at apply time re-estimate on each era's own returns (theme-cov-hedge); mom-volman's
  schedule is TRAIN-fixed, so on 2015-2019 the parent's masses hold and the rule is inert (ALCOMB-4); info-clock reads
  each era's `ea_days_since`. Each book's L carried; no gross calibration on history; gross printed (choice 6).
- **Predictions, registered here before the read** (plan section 5.3 :945-947; lit section 5.3 caveat 2), each printed
  against its outcome:
  - P9-F0 vs Y-F0: the mechanism sign of every accepted P9 cell that is active on history (P9-H: vol ratio < 1; P9-C:
    turnover share up, gross up; P9-M: inert, no effect predicted; P9-L: labels on history come from each era's own
    label role, whose delisting stage is empty before 2018, v8-od3-protocol section 2).
  - P9-F0's history S2 net Sharpe = its TRAIN value + P9-W's dSR (B.6, H23); "lower than on 2020-2023" (PM7-28's
    registered expectation, carried).
  - ERC (review-x5 via lit section 5.3): vol ratio .80-.87 and return ratio .90-1.0. Needs X-5's pre-ERC parent on
    history (one more history line): **Open (H18)**; recommend printed "not read".
  - Producers: NAV exe `ann_vol`, `ann_mean`, `net_sharpe` per era (`strategy_nav_replay.cpp:1996-2002`); pooled rows
    `nav_summ --pool` (`nav_summ.py:1331-1337`); turnover as B.2.
- **Contamination flags** (lit section 5.2 (b); plan section 5.3 :948-951): every member of P9-F0 and every P9 rule
  carries `source_sample_end` (Appendix III). The read prints P9-F0 and P9-F0 without the flagged members. P9 rules:
  theme-cov-hedge-v1 2017, mom-volman-v1 2016 (both registrations), info-clock / mv-aim [ROUND 2]. **Open (H19)**.
- **Coverage by year** (lit section 5.3 caveat 1): `atx-impl/tools/era_data_audit.py` (in root; coverage by field and
  year, delisted share, mask exits, first valid date per candidate; opens no return). 2015-2017 tests a different book
  (FINRA short interest, FTD, Russell proxy from 2018; 13F from 2013q2; options from 2012).
- **Not readable**: any NAV that reads `--risk-model` (Y-1, P9-X in the Y-1 form, P9-V, P9-MV, risk-target). If P9-MV
  is accepted, P9-F0 itself reads `--risk-model`: **Open (H10)**.
- **Steps** (v8-od3-protocol section 5, adapted): B1 projection; B2-B5 era base, scan / repair, lo3 and dlret label
  roles (same argv, era dates); B7 era fields with P9-F0's field list (fields v15, 84 fields, the engine-kind rows
  included; not v10) [ROUND 2: the era fields argv derived from P9-F0's fields manifest]; B6 the roles specs with
  `fit.carry`; lock and plan; P1-P7 pre-read checks (pins equal TRAIN lo3's; P9-F0 / Y-F0 / R-2 reproduce their TRAIN
  NAVs byte for byte on the frozen build; score_begin 399; eras end <= 2020-01-01; >= 1,000 lo3 members per scored E1
  session; audit printed; K1 plan-only in caps; the plan lists exactly E1, E2, E3 with the carry); R0 protocol line
  (`scripts/research_cycle.py:14`, `ledger-protocol`; adds 0); R1 era runs to NAV; R2 mechanics only, E3 daily SHA-256
  = the TRAIN series; R3 summ (pooled history lines); R4 the G-B8 statistics.
- **Stop conditions** (each before any return is opened; the PM rules): v8-od3-protocol's list (pin difference,
  identity failure, score_begin != 399, era past 2020-01-01, unreviewed factor breaks, an E1 session under 1,000
  members, fields argv beyond the allowed keys, a plan with other eras or a carry refusal, K1 over caps, a book reading
  `--risk-model`, the last cell not ledgered, an R2 refusal or E3 SHA mismatch) plus: an exe or `ucrtbase.dll` record
  differing from the freeze build (C.6, C.8); a print without the identity-bridge flag.
- **Pass rule (G-B8)**: S2 net Sharpe of P9-F0 on history > 0 AND paired dSR P9-F0 vs R-2 on history > 0; both p
  printed (one-sided from the verb's `--one-sided`, `eval_cli.cpp:110-111`). A fail is reported, never re-run; the
  freeze wording is per OD5-1 (v).

### F.2 OD3-TOOL scoping (ledger :495)

1. **What "holdout_gate.py under v8x section 10's runbook" refers to.** v8x section 10 (`v8x-prereg.md:202-211`) has two
   bullets: `holdout_gate.py` is the OD-1 hidden-block gate (2024+); the OD-3 read is a separate "2013-2019 era shards,
   H-1" read. `holdout_gate.py` exists in root as `atx-impl/tools/holdout_gate.py` (not under `scripts/`; ledger :495
   looked for `scripts/holdout_gate.py`), but it is the wrong tool: it prints `pass_2024` / `pass_2025_onward`
   (`:6`) and must never run in P9. The OD-3 runbook is `v8-od3-protocol.md` section 5 (ruled PM7-29), on the xpre
   branch only (root HEAD has no `v8-od3-protocol.md`; root's `task-XPRE-report.md` is the pre-`a7671d1a` blob
   `cddc9aaa`, the branch's is `a40be229`).
2. **What the read needs, and where it is.**

| need | in root (file) | only on `feat/platform-v8-xpre-20261002` |
|---|---|---|
| era roles, label roles, fields builds | `atx-engine/tools/prepare_recent_research.py`, `atx-impl/tools/repair_role_factor_breaks.py`, `atx-engine/tools/prepare_research_fields.py` | - |
| era window checks | `atx-engine/tools/era_pool.py:40-43` | - |
| roles cycle (per-era u / w / NAV, fields_dir) | `scripts/research_roles.py` (H-1) | T4 `roles[].label_dir`; T1 `fit.carry` (+109 lines in `research_roles.py`) |
| carried weights | - | T1 `atx-impl/tools/carry_weights.py` (new, sha256 `5aebb3a7...`) |
| coverage audit | `atx-impl/tools/era_data_audit.py` | - |
| pooled rows and pooled paired dSR | `nav_summ.py --pool / --pool-ids / --pool-reference` (`:1331-1337`) | - |
| pooled one-sided p + verdict | the verb has `--one-sided` (`eval_cli.cpp:110-111`); `nav_summ` uses it only in the bundle path (`nav_summ.py:205-210`) | T2 `nav_summ --pool-one-sided` (calls Python `paired_stats(..., one_sided=True)`) |
| pooled DSR_tot (history adds no trial) | refused today: `nav_summ.py:1367-1368`; the verb's `--history` reaches `house_v1` only, not `tot` (`eval_report.cpp:566-575`) | T3 `--dsr-total` with `--pool`, `dsr_total.ledger_rows(history=)` |
| history ledger lines (E-41) | `atx-impl/tools/backtest_integrity.py:684` `history_read_lines`; `research_cycle.py ledger-protocol` (`:14`) | T5 test |
| tests | - | `atx-impl/tools/test_od3_tooling.py`, `scripts/tests/test_research_cycle_od3.py` |
| per-era risk store (only if P9-F0 reads `--risk-model`) | none (v8-od3-protocol B6 "Risk model": STOP) | none |
| P9-F0 without flagged members | none (`carry_weights.py` carries W unchanged; no drop list) | none |

   G-B8 needs T1, T4 and T2 (DSR is not a G-B8 leg); T3 is print-only for P9 and can be dropped.
3. **Would the four commits merge onto today's root?** `f68058e2` touches `carry_weights.py` (new),
   `dsr_total.py`, `nav_summ.py`, `test_dsr_total.py`, `test_od3_tooling.py` (new), `scripts/research_cycle.py`,
   `scripts/research_roles.py`, `scripts/tests/test_research_cycle_od3.py` (new); the three doc commits touch only
   `.superpowers/sdd/platform-v8-20260929/{v8-od3-protocol.md,task-XPRE-report.md}`. Merge base `da3bb239`; since then
   root changed `nav_summ.py` and `research_cycle.py` only (of those files). `git merge-tree --write-tree HEAD
   feat/platform-v8-xpre-20261002` at root head: CONFLICT in `atx-impl/tools/nav_summ.py`; `research_cycle.py`
   auto-merges today. After the remaining wave-2 merges it gets worse: E2 turns `scripts/research_cycle.py` into a
   47-line shim and moves `make_cycle` to `scripts/cycle/cli.py:474` (pool-17 at `ea7d9dde`), so the T1 refusal hunk
   (`research_cycle.py:1844-1849` on the branch) has no target; B2's deletion `598d2199` (pool-14) removes
   `dsr_total.dsr_at`, `ledger_rows`, `hand_row` (T3's target) and `nav_summ`'s Python `paired_stats` (T2's call) and
   rewrites `test_dsr_total.py`; `test_od3_tooling.py` drives `nav_summ.main`, which then needs `atx-research-eval`
   (B2-R3, fail closed). `research_roles.py` and `carry_weights.py` merge cleanly (unchanged by E2 and B2).
4. **Recommended route: port, not merge** (H21). One small Python lane (effort S, 0 trials, no C++), after E2 and B2's
   post-identity redo are in root: cherry-pick `carry_weights.py`, the T1 / T4 `research_roles.py` hunks and their
   tests; move the T1 refusal into `scripts/cycle/cli.py` `make_cycle` (3 lines, cross-lane E2 file); re-implement T2
   on the verb inside `nav_summ` (`paired_of(..., one_sided=True)` plus the verdict dict; cross-lane B2 file); drop T3
   (or add it with a one-line C++ change passing `hist` to `ledger_rows_json` at `eval_report.cpp:572`, B2's file, with
   a test); add the two P9 needs (the flagged-member drop for the carried book, H20; per-era risk stores only if H10
   needs them); commit `v8-od3-protocol.md` and the branch's `task-XPRE-report.md` as records. Root verifies:
   flag-absent `nav_summ` byte-identical on the identity argv of B2, plans byte-identical, the ported tests green, no
   G-P5 mirror added. Cost: one S lane, one review, one root merge without a build (two with T3); off the critical path
   (needed before the OD-3 read, not before the cells). Merging the branch instead re-does the same port while resolving
   conflicts against deleted Python statistics (a G-P5 risk), so it costs more.

---

## G. Freeze gate checklist (plan section 4.4, as amended by G-P11 and SQL-8; the PM ticks it)

- [ ] 1. Book: G-B1 floor met by P9-F0; G-B2 by P9-F against 8.0% (label "plan placeholder, not owner-stated");
      G-B4 floor; G-B6 one-sided p < .05; G-B9 mechanics; G-B5 and G-B7 printed; G-B1 / G-B2 / G-B8 carry the
      identity-bridge flag (OD5-1 (i)).
- [ ] 2. History: OD-3 read once under F.1; predictions printed against outcomes; contamination split printed; G-B8
      met. If G-B8 fails: not production, nothing re-run, the owner decides deployment.
- [ ] 3. Platform: G-P1..G-P9 and G-P11 ticked, each with its evidence row in the integration log (G-P11, ledger :224).
- [ ] 4. Accounting: the Appendix A block of every P9 result complete; N_c <= 71, K_a, M, N_tot <= 252 (A.1); every
      void, undefined and lapsed cell logged with its count (0 or 1).
- [ ] 5. Housekeeping: every P9 pool released; lane branches merged by SHA; no dirty path in the research pathspec.
- [ ] 6. G-P10 printed with its evidence (a)-(e), landed / partial / P10; it does not gate (SQL-8).
- [ ] 7. EXE-FREEZE held from the wave-3 re-pin to the freeze (or each break re-based first); `ucrtbase.dll` records
      equal across every cell (C.6, C.8).
- [ ] 8. Verdict wording for a pass: "passed with the identity-bridge flag"; `al-data/atx-db-request.md` handed to the
      owner with the freeze report (OD5-1 (iv)-(v)).
- [ ] 9. Owner items listed, not acted on: OD5-2 (CRT `pow` re-pin), OD5-3 (main / origin main), OD5-5 (data purchase),
      production leverage (OD5-7), the WQ theme (WQ-1).

---

## H. Open for the PM (one line each: recommendation -- why -- cost if wrong)

- H1 P9-X form: Y-1 form -- section D -- the deployable levered book has no history read; one more trial (P9-V).
- H2 P9-L shape: by hand via `research_cycle.py run` of a v1 template root writes -- no v2 kind carries
  `--label-terminal` (`wave_kinds.py@ea7d9dde:161-162`) -- the v2 guards (E2-8 / E2-10) do not run on this cell.
- H3 P9-L gross match: `pm6-6-engine` -- DEC-10 binds every construction cell; its printed dSR is then comparable --
  one calibration NAV run (0 trials).
- H4 P9-L 30-day exposure: state it, bounded by ALD-DIAG's "unclassified" exits -- truncation needs code in D3's IC
  runner file -- labels of up to the last 30 days' exits may embed post-TRAIN cause information.
- H5 P9-L verdict: by hand from verify's mechanics with independent re-derivation -- the driver has no mechanics-only
  rule (`scripts/wave_rules.py:143`) -- hand error, bounded by the re-derivation.
- H6 P9-H registration: register unless q_lit < .05, fixed before Q5 prints (Q5 runs in M2g now) -- even the lit's
  widest column list must allow the lower +.05 (Q5 spec `reading.arithmetic`) -- a hedge that works through
  non-style channels is not tried.
- H7 Gross-alpha producer (P9-H, P9-MV, G-B3): accept `wave_readers.book`'s sum of C++-printed `ann_mean` and summed
  cost / financing returns, or ask C3 (NAV owner, in flight) to print `ann_mean_gross` (one summary key, on the wave-3
  P9-B0 substitution list); else cut the line -- ALCOMB-9 named the verb, which prints no mean (`eval_report.cpp:41-46`)
  -- a Python sum stands where a C++ key was asked for.
- H8 P9-M parent if P9-H is accepted: run P9-M on the hedge parent -- the fitter and the exe accept it
  (`composition_mom_volman.py:134-136`; neither row excludes the other) and "undefined" would waste a registered cell --
  the template's description understates the accepted parents (ALCOMB-9 (a)).
- H9 P9-MV store pin: tell COV-MV now that `mv-aim-v1` must list `risk_model` / `risk_model_sha256` as params with their
  `x-flag` (as `strategy_risk_target.cpp:341-346`) and the cell names them in `params` -- E2's guard refuses the flags by
  any other route -- otherwise P9-MV cannot pin its store and lapses.
- H10 P9-MV and OD-3: register now that the OD-3 read uses per-era `atx-cov-v1` stores built and pinned before the read
  (0 trials; added to the OD3 port), falling back to "P9-F0 without P9-MV" if that tooling is not merged before the
  freeze -- plan section 5.3 makes a `--risk-model` book unreadable -- the fallback reads a book that is not the
  deployable one.
- H11 P9-X and P9-V: by hand through `research_cycle.py` (the X-10 / Y-1 precedent, ledger :156, :167) with scaled
  mechanics written in the plan before the run and independent re-derivation -- the driver has no PM7-34 (3) rule and
  no scaled limits (`scripts/wave_rules.py:79`, `:143`) -- no tool verdict; the B2-11 judge check is done by hand.
- H12 Degenerate pair for dSR-decided cells (P9-L print, P9-H/M/C/MV, P9-V): decide on the point dSR > 0 (defined in a
  degenerate pair, `atx-engine/src/research/eval/paired.cpp:179`) AND mechanics; SE / t / p "na" -- B2-13 names the
  fallback only for leverage pairs -- such a cell is skipped by G-B7 (`eval_report.cpp:503-508`).
- H13 P9-F: P9-V if it is accepted AND also passes X-10's rule against P9-F0; else P9-X if accepted; else none -- P9-V's
  own test is against P9-X only -- a levered book could be offered without having beaten the unlevered one.
- H14 P9-C and P9-W lapse at 0 if AL-CLOCK / D3 are not merged and APPROVE before the P9 cells start (COV-7's rule) --
  keeps the order fixed -- two cells may lapse.
- H15 DSR_hand producer: ask B2's redo (B2-12, in flight) for a verb row at N = N_tot - M on the ledger V; else print
  the prefix-rule DSR_hand (`eval_report.cpp:579-604`) labelled -- v8y's definition used Python `dsr_total.dsr_at`,
  deleted in `598d2199` -- G-B5's DSR_hand changes definition.
- H16 G-B7 steps: every accepted construction cell from R-2 on (R-2, X-2, X-3, X-5, Y-S) plus P9's accepted cells, each
  with its ledgered dSR and Memmel SE -- lit section 5.2 (a)'s lineage -- a different list changes the adjusted sum.
- H17 G-B8 sample: E1+E2 (2015-2019) decides; the 2015-2023 pool printed -- the pool includes the in-sample window --
  lower power (MDE .40-.45 vs .30).
- H18 OD-3 books: P9-F0, Y-F0, R-2, P9-F0-unflagged (4 history lines); the ERC prediction printed "not read" --
  each extra book is one more read of the single-use block -- the ERC claim gets no history evidence.
- H19 Contamination rule: flag a member when its primary source (the paper whose construction it implements) has a
  sample ending in 2015 or later; unverified = flagged; round 2 verifies the 31 by reading the papers (ALSIG-1 method)
  -- lit section 5.2 (b) -- over-flagging shrinks the unflagged book.
- H20 Unflagged book: flagged members treated as absent on history (the coded present-member rule, TRAIN weights
  carried) -- no re-fit, no new construction -- needs a drop list in the carry tool (port lane).
- H21 OD3-TOOL: port lane (F.2 item 4) -- smaller than a merge, no G-P5 risk -- one S lane before the freeze.
- H22 Re-base R-2 and Y-F0 at the wave-3 P9-B0 -- G-B6 and the OD-3 P2 identity compare like with like (EXE-FREEZE) --
  two more zero-trial NAV runs.
- H23 P9-W: `gross_match: pm6-6-engine` and the OD-3 prediction formula of B.6 -- comparable dSR; a mechanical bridge
  from P9-W to OD-3 -- a different formula changes one printed prediction.
- H24 Before registering P9-MV the PM reads the spo-v1 / spo-v2 v7 cell outcomes (cov-design section 5.2 :553-555) --
  the design asks it of the PM, not a lane -- none.
- H25 Print "about 1" ratios against a band [.90, 1.10] -- makes the print checkable -- none (prints decide nothing).
- H26 P9-H on history: state that the hedge is 0 in each era's first 252 decisions (h_t is 0 before decision 253) --
  E1 has 504 scored sessions -- E1's hedge effect is understated.

---

## Appendix I. SHA-256 recomputed (sha256 of the bytes at root; HEAD blob = working tree for each)

| file | sha256 | note |
|---|---|---|
| `scripts/specs/p9/rules/theme-cov-hedge-v1.json` | `55f25f2fd1976fab2138b1130da5c6968addbd4e333b75fbf9c78330cf2b8ed0` | - |
| `scripts/specs/p9/rules/mom-volman-v1.json` | `8a7222fa7d99b853abb102f87538fadf2ea4a17b8d22b698a913b64c58e2b493` | - |
| `scripts/specs/p9/templates/theme-cov-hedge.json` | `7c6778bbd6c5759e3c431bae3cf301700240e672ea8406926b549927688dfb15` | = registration `template_sha256` |
| `scripts/specs/p9/templates/mom-volman.json` | `7e9b26c1bc4a6453447ccbf9fee2612ecb50a293330f239cfc93d035e45db6fb` | = registration `template_sha256` |
| `scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json` | `721889d19ad5fe19cb09546530e30c04fdc7d4f2012f42c63bbfe72005a8ba74` | - |
| `scripts/specs/p9/candidates/russell_recon.json` | `509e1834af4e8f9b184ab7bb37d15c9058d37a4085c97897ccaf48dca9f088ae` | status dropped |
| `scripts/specs/p9/blocked/hf_crowd.json` | `3cbd5169e5495f70e88a5160f03a62924966cc79bffb9934a3de54d4b5fec4de` | moved at `30b9c9d2` (ALD-12 (c)) while drafting; was `9b62a71a...` |
| `scripts/specs/p9/blocked/lazy_prices.json` | `49b2292ab84847516138d09bc769cf6dd6245973d1530fd630bbc4b945b285b0` | moved at `30b9c9d2`; was `97fe4f73...` |
| `atx-impl/strategies/fund_industry_ic_v8ysb.json` | `41010b0bbc662922e6e873057e1fc39d046d6f01a0a5124dd8a601c25a1308f5` | = Y-F0 spec pin |
| `scripts/specs/v8/lib-v8ysb-gm.json` | `1e3ec118273cb52d7248568011e7dc24a211e7204c25b1116f5a5bd7deaa8626` | Y-F0 |
| `scripts/specs/v8/y-vol-target.json` | `4e190f5434dfc13aced1459721cde476faf3560ef99aa41e05bfe492e2f141b4` | = v8y section 14 pin |
| `scripts/specs/v8/y-vol-target-y-1.json` | `faffee1795ba500fbebaa5a52e2449acaf02c92f7ee972011a267a52ba265f77` | Y-1 |
| `scripts/specs/v8/x-leverage-L2.0.json` | `ebe68e6e4de3ad5b0dc5b86e38d8f749cf77d3bd637c55d5453e9c1e4a674975` | = ledger :156 `ebe68e6e` |
| `v8-od3-protocol.md` @ `a7671d1a` | `640cd91ecfc0dd8e6b0b3a3da13e87e52b6e361c42fc2347d099b9da6976c9bb` | xpre branch only |
| `atx-impl/tools/carry_weights.py` @ `f68058e2` | `5aebb3a7ce8ff3f845d3151bf44d5b1648a14705be4421b3fa0768a3db33227c` | xpre branch only |
| DSL `rank(russell_recon)` / `rank(hf_crowd)` / `rank(lazy_prices)` | `2aad0b86...` / `1881f8ce...` / `bcbe2653...` | = registrations |

## Appendix II. Inconsistencies found across the lane files and the plan

1. Plan section 5.3 :941 routes OD-3 "through `holdout_gate.py`"; that tool is the OD-1 2024+ reader (F.2 item 1).
2. ALCOMB-9 (b) names `atx-research-eval` as the producer of P9-H's realised vol and gross-alpha ratios; the verb prints
   neither (`eval_report.cpp:41-46`) (B.2, H7).
3. The theme-cov-hedge template promises an "ex-ante" vol ratio that only Q5 could print, and Q5 prints the parent's
   level only (B.2).
4. ALCOMB-9 (a) "the P9 parent is neither" holds only if P9-H is rejected (B.3, H8).
5. `brief-COV-MV.md` (closed `params_schema`, `params: {}`) leaves no route for `--risk-model` past E2's guard (H9).
6. Carry PRE item "an imputed terminal return is never guard-checked" predates S2-FIX1 (1), which guards it (B.1).
7. Plan section 5.2 :916-917's PM6-6 list omits P9-L and P9-W while DEC-10 binds every construction cell (H3, H23).
8. The v2 driver has no PM7-34 (3) rule and no scaled mechanics (`wave_rules.py:79`, `:143`), so P9-X / P9-V cannot be
   judged by it as registered (H11); the v8 Y-1 template's raw `--vol-target` flag would be refused under v2 (B.7).
9. The verb's `--history` reaches `house_v1` but not `tot` (`eval_report.cpp:571-572`) (F.2).
10. P9-MV accepted would make P9-F0 unreadable on history under plan section 5.3 (H10).
11. Plan section 5.2's N_c / N_tot ceilings assume P9-S's cell and 10 strings; reachable maxima are 70 / 241 (A.1); the
    full N_c ceiling is +2.13% on E[max] against v8's 62, above lit section 5.2 (c)'s "about 2%" (A.3).
12. v8y's DSR_hand used `dsr_total.dsr_at`, deleted by B2 `598d2199`; the verb's `--hand` is v8x's prefix rule (H15).
13. The v8 OD-3 runbook builds era fields v10; P9-F0 reads fields v15 with engine-kind rows (F.1 steps).
14. The blocked registrations' bytes changed during this round (`30b9c9d2`); Appendix I holds the new SHA-256.

## Appendix III. Contamination flags (`source_sample_end`) of the book's 67 members and the P9 rules

No member of `fund_industry_ic_v8ysb.json` records `source_sample_end` (only the P9 files do: `git grep` over
`scripts/specs`). A paper's sample cannot end after its publication year, so every member whose cited papers are all
dated 2014 or earlier is clean by that bound. The rest need the paper's sample read (H19).

- **Clean by publication year (36):** ep, cfp, fcfp, ebit_ev_f49, net_payout, sp, rd_me, roa, accruals_f49, fscore,
  asset_growth_f49, noa_f49, issuance_xbrl, issuance_vendor, ear, ind_mom_12_1, within_ind_mom, high_52w, si_ratio,
  seasonality_same_month, iv_rv_spread, opex_at_f49, ins_opp_buy, pct_accruals, fip_id, si_low_io, inst_persist,
  div_season, vol_beta, so_wang_rev, mom_turn, ea_uvol, dato, fscore_hbm, stio_trade, deal_target.
- **Verify, first-cited paper 2015 or later (19):** opbe (2015), cbop_f49 (2016), roe_q (2015), bac (2020), smax
  (2020), dtc (2015), si_change (2016), sv_flow (2020), qmj_safety (2019), q5_eg_f49g (2021), smax5 (2020), ea_overdue
  (2018), ear_mom_12m (2025; Gerard-Jehl sample to 2024 per lit section 5.2 (b): flagged), op_rd (2025; Novy-Marx-
  Medhat, named by lit section 5.2 (b): flagged), dtc_slow (2015), k8_intensity (2017), nt_late (2017), iv_vol_of_vol
  (2018), day_rev_freq (2022).
- **Verify, a later citation 2015 or later beside an earlier source (12):** value_composite (1992; 2021), bm (1985;
  2020), gpa_f49 (2013; 2020), res_mom_12_1 (2011; 2020), mom_12_1 (1993; 2016), ind_adj_rev_5 (2014; 2015), nincr
  (1999; 2018), res_mom_ind (2011; 2020), inst_best_ideas (2010; 2022), ftd_fail (2009; 2016), earn_surprise_comp
  (1989; 2022), season_y2_5 (2008; 2016). Under H19's primary-source rule most stay clean if the later paper is a
  robustness or crash-risk citation.
- **P9 rules:** theme-cov-hedge-v1 2017 (DMRS 1963/07-2017/06), mom-volman-v1 2016 (Cederburg et al.; BSC 2011),
  vol-target-v1 (not read on history), info-clock-rate-v1 and mv-aim-v1 [ROUND 2: from AL-CLOCK's and COV-MV's
  registrations].
- [ROUND 2: verified `source_sample_end` for the 31 members above and the final flag list].

## Appendix IV. Round-2 placeholders (index)

Every `[ROUND 2: ...]` tag above, in order: exes pins (B.0); eval mode (B.0); P9-L template; P9-L parent references;
P9-L fit mode; P9-H template re-pin; Q5 numbers; P9-H fit mode; P9-H "ex-ante" cut; P9-M template re-pin; P9-M sleeve
line (ALCOMB-6); P9-M parent fit mode; P9-C block (AL-CLOCK); P9-MV block (COV-MV, D-COV store pin, W3-10); P9-W block
(D3); P9-X store pin and template; P9-X pair rho; P9-V predictions (D-COV); R-2 re-based reference (A.4); ALD-DIAG
(C.2); CRT record (C.6); S2R-DIAG (C.9); leverage-pair and P9 rho lines (C.11); G-B7 step values (E); era fields argv
(F.1); P9 rule `source_sample_end` (Appendix III); the 31 members' verification (Appendix III).
