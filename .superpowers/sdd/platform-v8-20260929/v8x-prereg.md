# v8 expansion X pre-registration (draft by lane XPRE, 2026-10-02; binds once the PM rules, before any X measurement)

Basis: PM7-1..PM7-5, v8-prereg.md (rules 1-11 and its rulings bind X except where this file amends them by name), E-34,
E-38, E-45, PM5-23, PM6-6, the MINE-RUN registration as joined on `1bd448cd`. Nothing here was derived from a return,
IC, Sharpe, turnover or NAV output of 2020-2023 beyond numbers printed in status 6 and progress.md; nothing dated
2024-01-01 or later was opened. Open choices are marked OC-k (section 13): recommendation and cost if wrong.

## 1. Baseline and named books

- **V8-F** (X baseline): the last accepted v8 cell at the V8-F freeze gate, whatever the gate's outcome. The PM's ruling
  pins its spec digest, ledger trial_id, S2 daily net series SHA-256, L and fields dir. B0c is printed beside it.
- **H-F**: the last accepted hand-written X cell (after X-8); the campaign's pool and the book of DSR_hand.
- **X-F0**: last accepted after X-9 (leverage excluded), the book of every claim; **X-F**: X-10 if accepted, else X-F0.

## 2. Windows and seal (unchanged)

TRAIN [2020-01-01, 2024-01-01), seal 2024-01-01, from `atx-impl/strategies/research_window.json` (sha256 `62cf2cfa...`,
research-window-v2), never typed. Disclosure carried: 2023 and 2024 were read twice at book level before v8; every v8
and X cell reads 2023; 2025+ never read. The campaign's discover / confirm split lies inside TRAIN (section 9).

## 3. Trial counting (continues from the ledger at V8-F; no reset)

Ledger of record: root `build-equity/trials.jsonl`, chain-verified (`backtest_integrity.ledger_read`).

| event | counts in | count | ledger evidence |
|---|---|---|---|
| X construction cell scored (X-1..X-10) | N_c | 1 | construction line; defect rule (`trial_counts`) |
| candidate listed in a wave's `gate.admitted` (new, refinement, mined), admitted or not | K_a | 1 each | admission line per candidate (`cycle_admission.py`), origin |
| campaign evaluation (registry record, any status) | M | 1 each | campaign line `registry.count` (E-33a) = budget |
| repair member inside X-1; re-screen of an unchanged string (report-only row) | - | 0 | OC-2 (X-1 counts 1); R2-e |
| PM6-6 calibration run (mechanics only); field build, plan-only, probe, fixture, identity run | - | 0 | integration log (W0-i) |
| W0-4 window re-run; rule-7 blind re-run; undefined cell; candidate withdrawn before any read | - | 0 | `rerun_basis`; ruling |
| rule-7 returns re-run | N_c | 1 | `rerun_basis` returns |
| history read (OD-3); hidden-block read (OD-1) | - | 0 | counted apart (history / validation lines) |
| any 2020-2023 statistic read for a variant not on the X list | N_tot | 1 per variant | disclosure ruling; the variant never enters X |

Code names (root): `N_c = ledger_n(records, True, "construction")`; K_a = admission-kind trials by `trial_counts`;
`M = campaign_registry_count(records)`; **N_tot = sum(trial_counts(records)) + M** (every kind the defect rule counts,
legacy kinds included). What closes the search: (1) **closed list**: only items of the PM's pinned X list (P4) are
measured, each one frozen string or rule with its constants; after the first X read nothing joins it (an idea from a
result is a v9 note); (2) **a read is a trial**: every path that reads a 2020-2023 statistic writes a ledger line or is
the table's last row; (3) **no retry**: a rejected cell or member is never retried, re-parameterised or re-cast as
another lane's item (E-45); (4) **voids keep their count** (section 11), only a tool or data defect leaves N (rule 7);
(5) **hard ceilings** (section 5): unused budget lapses and is never spent on a new idea.

## 4. Deflated Sharpe

Formula (Bailey and Lopez de Prado 2014; `backtest_integrity.expected_max_sr`, `deflated_sharpe`, `moments`):
- SR, T, g3, g4: per-session S2 net Sharpe (mean / sd ddof 1), sessions, skewness and non-excess kurtosis of the book's
  daily S2 net series (the file its ledger line pins by SHA-256).
- SR0 = sqrt(V) x [(1 - c) PhiInv(1 - 1/N) + c PhiInv(1 - 1/(N e))], c = .5772 (Euler).
- DSR = Phi((SR - SR0) sqrt(T - 1) / sqrt(1 - g3 SR + (g4 - 1) SR^2 / 4)).
- **V** = `dsr_variance(records, "research-window-v2")["variance_sr"]`: ddof-1 variance of `s2_net_sr / sqrt(252)` over
  the construction lines with window_id research-window-v2 (W0-4 re-runs, B0a..V8-F, X cells, the leverage cell and
  procedural voids; defect-rule exclusions and history lines out). Admission trials and campaign evaluations have no
  book Sharpe: they enter N, not V. Stated assumption: a candidate run as a book variant disperses like a ledgered
  cell (OC-4). The effective-N DSR (ONC), Lo null and legacy variance are printed beside it and gate nothing.
- **N**: three counts from the same chain-verified ledger; each print carries N, V, cells in V, SR0, chain head.

| name | book | N | role |
|---|---|---|---|
| DSR_tot | X-F0 | N_tot of the whole ledger at the X gate | gates (section 7); PM7-1 |
| DSR_hand | H-F | N_tot and V of the ledger prefix ending at H-F's construction line (before the campaign line) | PM7-2, so the owner can drop the campaign; gates nothing |
| DSR_v8 | X-F0 | N_c (v8 rule 3, `nav_summ --dsr-ledger`) | continuity with the V8-F gate; gates nothing |

"DSR up" = DSR_tot of X-F0 at the X gate above DSR_tot of V8-F at V8-F's ledger state; V8-F recomputed with the X
gate's N and V is printed beside. Tool (P5): `nav_summ.py --dsr-total`, a Python flag adding
`backtest_integrity.total_trials(records)` and the three rows; output byte-identical without the flag; tested on a
synthetic ledger (construction, admission, campaign, defect, window re-run lines; N_tot computed by hand). It lands
after V8-F (PM5-21 holds until then). No X DSR is printed before it exists.
Scale (arithmetic, gates nothing): E[max] factor 2.38 at N 66 (V8-F: 51 cells + 15 admissions), 2.52 at N 97 (+ hand
X), 2.83 at N 247 (+ campaign): the campaign raises SR0 by 12% at any V. Illustration from public numbers: the seven
cells ledgered on 2020-2023 so far (B0a..R-4) give sd .057 annual; with SR 1.256, T 1,005 and normal moments DSR is
.987 / .987 / .985 at N 66 / 97 / 247; at a hypothetical sd .30, .860 / .841 / .791. V decides the gate, not the count.

## 5. The X budget

| source (lane cap) | construction cells | admission trials | campaign evaluations |
|---|---|---|---|
| XIMP-A defect repairs (every PM-ruled defect) | 1 (X-1) | 0 | - |
| XIMP-B refinements (<= 8) | 1 (X-2) | <= 8 | - |
| XSIG new signals (<= 12) and XDATA candidate signals (<= 3) | 1 (X-3) | <= 15 | - |
| XIMP-C library-wide processing variants (<= 2) | <= 2 (X-4, X-5) | 0 | - |
| XCOMB rules (<= 3) | <= 3 (X-6..X-8) | 0 | - |
| campaign v9-mine-c1 | 0 | 0 | B = 11 x fields <= 132 |
| mined wave | 1 (X-9) | <= 16 (shortlist cap) | - |
| leverage (PM7-3) | 1 (X-10) | 0 | - |
| **X ceiling** | **<= 10** | **<= 39 (23 hand-written + 16 mined)** | **<= 132** |

N_c <= N_c(V8-F) + 10 (<= 61); N_tot <= N_tot(V8-F) + 181; N_hand <= N_tot(V8-F) + 31. Caps: X cells keep W0-c (IC
phases 2,560 MiB / 300 s, others 1,536 MiB / 180 s); field builds 2,560 MiB / 600 s (W0-i); campaign OC-6. Argument:
waves, not one cell per member: the smallest detectable paired gain on 4 years is .165 (plan 12.2), so one member's
effect is noise at cell level; per-member cells would add up to 23 to N_c and decide nothing, while the admission gate
screens and counts each member (R-2 precedent). Three alpha waves, not one: repairs, refinements and additions are
different hypothesis classes and one rejection must not drop the others (two extra cells raise E[max] under 1%). One
cell per rule: it acts on the whole library or aim. One stage-1 campaign: a closed list, budget = capacity (draft D4),
one confirm year (D1); it costs 12% on SR0 and DSR_hand shows what dropping it would give.

## 6. Order of X cells and acceptance (fixed now; never re-ordered after a read)

Unless a row says otherwise: parent = last accepted; fields v13 (P6); PM6-6 gross matching; rule 5 (paired S2 net dSR
> 0 against the parent AND mechanics AND the criterion); both p printed (E-34, PM5-23); year table and 4x row (E-29).
X-2, X-3 and X-9 list every new string in `gate.admitted` (one admission line each); a refinement the gate does not
admit leaves the wave and its member keeps its pre-X string (declared now, so restoring it is no choice after a read).

| cell | content | acceptance besides mechanics |
|---|---|---|
| X-1 | XIMP-A repairs, one wave, strings replaced in place; parent V8-F | by declaration (OC-2); dSR printed, gates nothing |
| X-2 | XIMP-B refinements, one wave, in place | dSR > 0 AND net Sharpe at 4x NAV not lower (OC-3) |
| X-3 | XSIG + XDATA additions, one add-alpha wave | dSR > 0 AND net Sharpe at 4x NAV not lower (OC-3) |
| X-4, X-5 | XIMP-C processing variants, one cell each | dSR > 0 AND XIMP's registered criterion (default: turnover per unit gross not higher, PM5-11) |
| X-6, X-7 | XCOMB-1 combination rule; XCOMB-3 if a combination rule | dSR > 0 AND turnover per unit gross not higher (PM5-11), unless XCOMB registers another |
| X-8 | XCOMB-2 capacity rule (XCOMB-3 here if a capacity rule) | dSR > 0 AND net Sharpe at 4x NAV higher AND cost per traded dollar not higher (default; XCOMB's registered criterion governs) |
| - | campaign v9-mine-c1 on pool H-F (section 9); not a cell | campaign mechanics |
| X-9 | mined wave; parent H-F | dSR > 0 AND net Sharpe at 4x NAV not lower (amends draft D14, OC-3) |
| X-10 | leverage on X-F0 (section 8); not gross-matched | section 8 |

Why this order: repairs first (every later paired test on correct definitions); refinements before additions (new
members judged against the improved roster); processing after additions (it acts on every member); combination, then
capacity (it acts on the aim); the campaign after every hand-written cell (D15: priors before mined; the pool is the
book the mined members enter; H-F is the book PM7-2 needs); leverage last (PM7-3: it scales whatever X built).
Order rules: a cell whose condition is unmet or whose rule is undefined on its parent (E-45, PM4-10) is undefined (0)
and the next runs on the same parent; a member gets one change in X (a refinement of a repaired member is written on
the repaired string; an XSIG candidate duplicating an XIMP change: the PM keeps one before any read); no hypothesis is
registered twice (OC-9), and a rule ruled a restatement of R-3, R-4 or any rejected v8 / X rule is withdrawn at 0;
plan section 13 binds X unless lifted by name (OC-8); caps OC-1.

## 7. The X gate (cumulative test) and the claims

Run once, after X-10 (after X-9 if X-10 is undefined). Pass = (a) `nav_summ.py --protocol v8 --bundle <V8-F nav>
<X-F0 nav>`: paired dSR > 0 with one-sided bootstrap p < .10 (rule 4: block 21, seed 20260929, 4,999 resamples; both
p printed) AND (b) DSR_tot(X-F0) >= .95 AND (c) mechanics of X-F0. Printed beside, gating nothing: bundle X-F0 vs B0c,
DSR_hand, DSR_v8, PBO, year table, net Sharpe at 4x, net annual return, turnover, the Appendix A block. If unmet: the
scorecard says so, the deployable book stays V8-F (OC-7), every per-cell verdict stands, nothing is re-run.
Claims: **Sharpe up** only under (a)-(c). **DSR up** as defined in section 4. **Capacity up**: X-F0's 4x net Sharpe
above V8-F's, stated as sign-level. **Return up**, split in two: X-F0 vs V8-F (matched gross: signal and construction)
and X-10 vs X-F0 (leverage: the owner's risk decision, never called alpha).

## 8. Leverage cell X-10 (PM7-3)

- Parent X-F0 at L_P. **L = 2.0**, the executable's maximum; r = 2.0 / L_P. In dollars the cell's 1x book lies between
  the parent's registered 1x and 2x curve points (E-29). Spec: `change.set {"nav.leverage", "nav.output"}`.
- Mechanics restated (gross linear in L to .1%, PM6-6): all-rows gross in [.90 r, 1.05 r]; |mean net| <= .02 r; tau
  mean <= .20 r, p95 <= .30 r; every other row unchanged.
- Acceptance (replaces rule 5's dSR > 0 for this cell only): net annual return (S2, 1x) above X-F0's AND net Sharpe at
  4x NAV >= X-F0's - .100 AND mechanics. ".100" is "one SE": plan 12.2's registered SE of a paired dSR on 4 years,
  fixed before any result. A scale-only change has a measured paired SE near 0 (the series differ only by costs), so
  that reading rejects any raise for a deterministic cost; the iid SE of one Sharpe (about .5) guards nothing (OC-5).
- The cell counts in N_c and V; its dSR and both p are printed; it is never in the Sharpe claim.

## 9. Mined campaign (OD-7 granted by PM7-2): amendments and runbook

Registration of record: `docs/plans/2026-10-01-v9-mine-campaign-prereg.md`, `scripts/specs/v9/mine-c1.json` and its
runbook, as joined at `1bd448cd` (merged at integration 8). Amendments:
- **A1 counting.** M enters N_tot (amends draft item 10 and E-33 for the X DSR only; `trial_counts`, N_c unchanged).
- **A2 source.** H-F (amends D15); role `build-equity/train-2020-2023-lo3/manifest.json` (B0b won); fields = H-F's.
- **A3 fields.** The field rule (item 4) is re-applied at the lock to the registry at H-F: a field read by any registry
  alpha, every X-listed member included (admitted or not), leaves the list; B = 11 x fields; F = 1.47 if B <= 100,
  1.54 if 101..1,000. XSIG's preference (1) targets these fields; the hand-written prior keeps the field.
- **A4** memory OC-6; **A5** wave criterion OC-3; **A6** pool cap OC-1; **A7** id, spec path, output dirs unchanged
  (tests pin them). Disclosure: H-F was chosen with 2023 in view; the mined expressions are first read by the campaign.

Runbook (root pool-2, after X-8; nothing else running, E-6):
```
PY="C:/Program Files/Python312/python.exe"; RC="$PY scripts/research_cycle.py"; SPEC=scripts/specs/v9/mine-c1.json
# 0 integration 8 receipt: mine + factory tests pass; golden 0x889874a3b9b29c55 at 1 and 4 workers; rung_failed 0
# 1 edit SPEC: pool_source.combined / .summary = <H-F ic.w_output>-<k>/train_combined.json / summary.json;
#   pool_source.weights = <H-F fit.output>/composition_weights.json; inputs.role = lo3 manifest; inputs.fields = H-F's
"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_research_mine.py -k fields_are_the_rule   # A3: fields, budget
$RC mine lock $SPEC --write; $RC mine pool $SPEC; $RC mine lock $SPEC --write   # pins role, fields, pool source, pool
$RC mine probe $SPEC     # W = largest of 4, 2, 1 with required <= 7,680 MiB; max_memory_mib = required at W, up to 64
# delete the four requires lines (OD-7: PM7-2 and the ruling on this file); commit SPEC and the draft
$RC mine plan $SPEC      # header: locked pins, capacity = budget = B, z(B), F(B), Fc 1.77, windows, caps
$RC mine run $SPEC --date <YYYY-MM-DD>   # free memory >= max_memory_mib + 1,536 MiB at launch
$RC ledger-campaign --ledger build-equity/trials.jsonl --campaign build-equity/mine-v9-c1   # must exit 2 (shares trial_id)
```
Checked before any statistic, in order: (1) receipt completed, exit 0; (2) campaign line appended (chain head
recorded), N_c unchanged, registry count = B; (3) the mechanics `mine run` prints: distinct = evaluated + screen-rejected
+ racing-rejected + rung-failed + failed = B, racing-rejected 0, rung-failed 0, registry new records = B, hurdle z =
z(B), overlap factor F(B), every confirm read at Fc 1.77, recipe pins, windows (discover [2020-01-01, 2023-01-01),
confirm [2023-01-01, 2024-01-01)) and fields = SPEC. `stdout.log` stays closed until (3) passes. Then: counts (runbook
11a), promotions and `mined_members.json` (11b), `trials.csv` numbers last (diagnostic; select nothing). A failed run
(no `campaign.json`, nothing read) is a blind re-run adding nothing; a complete campaign ruled void keeps its line and
its M. Mined wave: `$RC mine wave $SPEC --parent <H-F library id> --name <id>m1 --parent-spec <H-F spec>`, the printed
add-alpha lines, `$RC run <wave spec> --screen`, then `$RC run <wave spec>` (X-9).

## 10. Hidden-block gate (OD-1, `holdout_gate.py`) and the history read (OD-3)

- `holdout_gate.py --deploy MANIFEST --thresholds FILE --owner-ruling FILE --ledger build-equity/trials.jsonl` is the
  only reader of 2024+ and outputs two bits. Not run in X unless the owner rules. If ruled: once, on one book chosen
  before the read by the X gate (X-F if the gate passed, else V8-F), never on both, never on a candidate; thresholds
  pinned in the ruling file before; adds 0 to N (validation event line), 1 to validation reads; nothing in the book
  changes after it; a failed bit is an owner decision, never a re-run.
- The OD-3 read (2013-2019 era shards, H-1) adds 0 to N and 1 to history reads (E-41); only on a frozen book (V8-F or
  X-F) after the X gate, never to choose among X items; the pooled fit knows ew-theme-v1, v6, std-v1, std-aim, aim-v2
  only, so an X-F with an XCOMB rule first needs it extended (blind, synthetic tests).

## 11. What voids a cell

Void = never accepted, never a parent, never retried. If any return statistic of it was seen, it is ledgered normally
and stays in N_tot and V; a cell stopped before any return read is not ledgered and adds 0. Void when: (1) it ran
before the PM ruled on this file, before integration 8 or before the X list was pinned; (2) any input differs from the
X list or its template (string, constant, parent not the last accepted, flag, fields dir, L other than PM6-6's
mechanics correction); (3) a return, IC, Sharpe or NAV statistic of the cell was read before its mechanics passed
(cells-brief order); (4) a session at or after 2024-01-01 was opened (also a hidden-data disclosure).
Not voids: a tool or data defect is rule 7 (invalid, out of N and V; blind re-run 0, returns re-run a new trial); gross
not matched after two corrections, or L < 1, is a mechanics fail (rejected, counted); undefined on its parent is 0.

## 12. Preconditions before the first X measurement (in order)

P1 V8-F ledgered and its freeze gate evaluated; the W0-4 re-runs ledgered (they set V). P2 integration 8 (`1bd448cd`,
`834d5a05`, R6C-3 / R6C-7 C++, `exe_plan --max-memory-mib`; golden at 1 and 4 workers; fixture rung_failed 0;
`AlphaVmSlotReuse.*`). P3 X lanes merged by SHA on one build tag; identity: V8-F re-run with every X flag absent equals
its ledgered S2 daily SHA-256; Python suites 0 failed. P4 the X list pinned: the PM's ruling appends a table to this file
(id, class, cell, string or rule, constants, lane report path@SHA, SHA-256 of the DSL or rule text) and records the
file's SHA-256 in progress.md. P5 the DSR tool (section 4) merged with its tests. P6 fields v13 = V8-F's fields + every
X field, one build (0 trials), every reused payload bit-identical, reuse counts as the lanes predicted. P7 OC-1
applied (regenerated recipe re-pinned) before X-3.

## 13. Open choices (the PM rules; recommendation -- cost if wrong)

- **OC-1 caps.** V8-F holds up to 60 members vs the roster cap 64 (R7-a) and `mine pool`'s 64; X-3 lists up to 15.
  Raise both to 80 before X-3 (R7-a's route: regenerated v7.1 slim recipe re-pinned; `kMaxMinePoolMembers` and its
  Python mirror at integration 8; members are streamed, so the probe's memory holds) -- one re-pin and one constant;
  fallback: X-3 lists only 64 - roster in registered prior rank, the rest wait for v9.
- **OC-2 repairs by declaration.** X-1 accepted on mechanics alone, members ungated, each repair ruled a defect before
  any read (a named element of the cited definition differs); this selects nothing and makes a disguised refinement
  costly (it cannot be rejected) -- if repairs lower Sharpe, X starts lower and the X gate shows it.
- **OC-3 alpha-wave criterion.** Net Sharpe at 4x NAV not lower (X-2, X-3, X-9), replacing "turnover not higher" (R-2,
  draft D14): it prices turnover at the owner's capacity point -- turnover may drift up while 4x holds.
- **OC-4 count and V.** N_tot = every ledger trial + M, no netting (a mined member counts in M and K_a); V from book cells
  -- N_tot high by at most 16 (SR0 +.5%); if candidates disperse more than cells, SR0 is low (effective-N DSR shows it).
- **OC-5 leverage.** L 2.0; tolerance .100; net return + 4x guard + mechanics -- if 2.0 fails where 1.5 would pass, X
  has no leverage step; the measured paired SE would almost surely reject; the iid SE guards nothing.
- **OC-6 campaign memory.** Probe value at the largest of 4 / 2 / 1 workers, up to 64, at most 7,680 MiB (expected
  4,032 at 4), above OD-2's 2,560 (PM5-10 unmet); runner 600 s -- refusals precede any payload; a time stop re-runs blind.
- **OC-7 X gate.** DSR_tot >= .95 and the paired test vs V8-F; if unmet, V8-F stays the deployable book -- X gains under
  the test's power (.165) are not deployed.
- **OC-8 plan section 13.** Binds X; a bar is lifted by name only for a rule whose weights read no return (e.g. ERC on
  the risk store's covariance) -- a barred rule is withdrawn at 0, or one counted trial enters that v8 meant to exclude.
- **OC-9 overlaps.** XIMP-C and XCOMB-2 with one mechanism (e.g. per-theme half-life): keep XCOMB-2's (it carries a
  capacity criterion); XSIG and XIMP on one member: keep one -- the dropped variant waits for v9.
- **OC-10 one wave for XSIG + XDATA** (shared fields build) -- a weak XDATA family drags XSIG's wave; two cost +1 cell.
- **OC-11 hidden block.** No read unless the owner rules; one at most, on the book chosen first -- no OOS bit till then.

Appendix A block for X results: `TRAIN construction cells <N_c>; admission trials this sprint <k> (v8 <k8>, X
hand-written <kx>, mined <km>); mined campaigns <0|1> (v9-mine-c1: budget <B>, registry count <C>, admitted <a>); N_tot
<n>; N_hand <h>; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8:
2 (2023-2024); history reads <r>; 2025+ never read.`
