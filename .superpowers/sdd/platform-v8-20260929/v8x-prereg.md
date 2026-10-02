# v8 expansion X pre-registration (lane XPRE, 2026-10-02; ruled by the PM in PM7-6..PM7-14 before any X measurement)

Basis: PM7-1..PM7-14, v8-prereg.md (rules 1-11 and its rulings bind X except where this file amends them by name), E-34,
E-38, E-45, PM5-23, PM6-6, the MINE-RUN registration as joined on `1bd448cd`. Nothing here was derived from a return,
IC, Sharpe, turnover or NAV output of 2020-2023 beyond numbers printed in status 6 and progress.md; nothing dated
2024-01-01 or later was opened. The draft (`71a68d60`) left eleven choices to the PM; they are ruled (section 13).

## 1. Baseline and named books

- **V8-F** (X baseline): the last accepted v8 cell at the V8-F freeze gate, whatever the gate's outcome. The PM's ruling
  pins its spec digest, ledger trial_id, S2 daily net series SHA-256, L and fields dir. B0c is printed beside it.
- **H-F**: the last accepted hand-written X cell (after X-8); the campaign's pool and the book of DSR_hand.
- **X-F0**: last accepted after X-9 (leverage excluded), the book of every claim; **X-F**: X-10 if accepted, else X-F0.

## 2. Windows and seal (unchanged)

TRAIN [2020-01-01, 2024-01-01), seal 2024-01-01, from `atx-impl/strategies/research_window.json` (sha256 `62cf2cfa...`,
research-window-v2), never typed. Disclosure carried: 2023 and 2024 were read twice at book level before v8; every v8
and X cell reads 2023; 2025+ never read. The campaign's discover / confirm split lies inside TRAIN (section 9).

## 3. Trial counting (continues from the ledger at V8-F; no reset; PM7-7)

Ledger of record: root `build-equity/trials.jsonl`, chain-verified (`backtest_integrity.ledger_read`).

| event | counts in | count | ledger evidence |
|---|---|---|---|
| X construction cell scored (X-1..X-10) | N_c | 1 | construction line; defect rule (`trial_counts`) |
| candidate listed in a wave's `gate.admitted` (new, refinement, mined), admitted or not | K_a | 1 each | admission line per candidate (`cycle_admission.py`), origin |
| campaign evaluation (registry record, any status) | M | 1 each | campaign line `registry.count` (E-33a) = budget |
| repair member inside X-1 (PM-confirmed defect only); re-screen of an unchanged string | - | 0 | PM7-9 (X-1 counts 1); R2-e |
| repair the PM does not confirm: moves to X-2 as a refinement | K_a | 1 | PM7-9 |
| PM6-6 calibration run (mechanics only); field build, plan-only, probe, fixture, identity run | - | 0 | integration log (W0-i) |
| W0-4 window re-run; rule-7 blind re-run; undefined cell; candidate withdrawn before any read | - | 0 | `rerun_basis`; ruling |
| rule-7 returns re-run | N_c | 1 | `rerun_basis` returns |
| history read (OD-3); hidden-block read (OD-1) | - | 0 | counted apart (history / validation lines) |
| any 2020-2023 statistic read for a variant not on the X list | N_tot | 1 per variant | disclosure ruling; the variant never enters X |

Code names (root): `N_c = ledger_n(records, True, "construction")`; K_a = admission-kind trials by `trial_counts`;
`M = campaign_registry_count(records)`; **N_tot = sum(trial_counts(records)) + M** (every kind the defect rule counts,
legacy kinds included; no netting, PM7-7). What closes the search: (1) **closed list**: only items of the PM's pinned
X list (P4) are measured, each one frozen string or rule with its constants; after the first X read nothing joins it
(an idea from a result is a v9 note); (2) **a read is a trial**: every path that reads a 2020-2023 statistic writes a
ledger line or is the table's last row; (3) **no retry**: a rejected cell or member is never retried,
re-parameterised or re-cast as another lane's item (E-45); (4) **voids keep their count** (section 11), only a tool or
data defect leaves N (rule 7); (5) **hard ceilings** (section 5): unused budget lapses, never spent on a new idea.

## 4. Deflated Sharpe (PM7-7)

Formula (Bailey and Lopez de Prado 2014; `backtest_integrity.expected_max_sr`, `deflated_sharpe`, `moments`):
- SR, T, g3, g4: per-session S2 net Sharpe (mean / sd ddof 1), sessions, skewness and non-excess kurtosis of the book's
  daily S2 net series (the file its ledger line pins by SHA-256).
- SR0 = sqrt(V) x [(1 - c) PhiInv(1 - 1/N) + c PhiInv(1 - 1/(N e))], c = .5772 (Euler).
- DSR = Phi((SR - SR0) sqrt(T - 1) / sqrt(1 - g3 SR + (g4 - 1) SR^2 / 4)).
- **V** = `dsr_variance(records, "research-window-v2")["variance_sr"]`: ddof-1 variance of `s2_net_sr / sqrt(252)` over
  the construction lines with window_id research-window-v2 (W0-4 re-runs, B0a..V8-F, X cells, the leverage cell and
  procedural voids; defect-rule exclusions and history lines out). Admission trials and campaign evaluations have no
  book Sharpe: they enter N, not V; a candidate run as a book variant is taken to disperse like a ledgered cell. The
  effective-N DSR (ONC), Lo null and legacy variance are printed beside it and gate nothing.
- **N**: three counts from the same chain-verified ledger; each print carries N, V, cells in V, SR0, chain head.

| name | book | N | role |
|---|---|---|---|
| DSR_tot | X-F0 | N_tot of the whole ledger at the X gate | gates (section 7); PM7-1, PM7-7 |
| DSR_hand | H-F | N_tot and V of the ledger prefix ending at H-F's construction line (before the campaign line) | PM7-2, so the owner can drop the campaign; gates nothing |
| DSR_v8 | X-F0 | N_c (v8 rule 3, `nav_summ --dsr-ledger`) | continuity with the V8-F gate; gates nothing |

"DSR up" = DSR_tot of X-F0 at the X gate above DSR_tot of V8-F at V8-F's ledger state; V8-F recomputed with the X
gate's N and V is printed beside. Tool (P5; written in lane XPRE at `44a27d5d`): `nav_summ.py <book dirs> --dsr-total
<ledger> [--dsr-hand <dir> ...]`, logic in `atx-impl/tools/dsr_total.py`. It prints DSR_tot and DSR_v8 per listed dir
and DSR_hand per `--dsr-hand` dir; the same flag on V8-F's dir gives V8-F at its own ledger state. It refuses a missing
ledger, a broken chain, a missing or unledgered `--dsr-hand` dir and `--pool`. Without the flags the output is
byte-identical (stdout, stderr, JSON without `nav_summ_run`). It lands at integration 8 (PM5-21 holds until V8-F); no X
DSR is printed before it is merged.
Scale (arithmetic, gates nothing): E[max] factor 2.38 at N 66 (V8-F: 51 cells + 15 admissions), 2.52 at N 97 (+ hand
X), 2.83 at N 247 (+ campaign): the campaign raises SR0 by 12% at any V. Illustration from public numbers: the seven
cells ledgered on 2020-2023 so far (B0a..R-4) give sd .057 annual; with SR 1.256, T 1,005 and normal moments DSR is
.987 / .987 / .985 at N 66 / 97 / 247; at a hypothetical sd .30, .860 / .841 / .791. V decides the gate, not the count.

## 5. The X budget (PM7-6)

| source (lane cap) | construction cells | admission trials | campaign evaluations |
|---|---|---|---|
| XIMP-A defect repairs (every PM-confirmed defect, PM7-9) | 1 (X-1) | 0 | - |
| XIMP-B refinements (<= 8; unconfirmed repairs join here) | 1 (X-2) | <= 8 | - |
| XSIG new signals (<= 12) and XDATA candidate signals (<= 3) | 1 (X-3) | <= 15 | - |
| XIMP-C library-wide processing variants (<= 2) | <= 2 (X-4, X-5) | 0 | - |
| XCOMB rules (<= 3) | <= 3 (X-6..X-8) | 0 | - |
| campaign v9-mine-c1 | 0 | 0 | B = 11 x fields <= 132 |
| mined wave | 1 (X-9) | <= 16 (shortlist cap) | - |
| leverage (PM7-3, PM7-11) | 1 (X-10) | 0 | - |
| **X ceiling** | **<= 10** | **<= 39 (23 hand-written + 16 mined)** | **<= 132** |

N_c <= N_c(V8-F) + 10 (<= 61); N_tot <= N_tot(V8-F) + 181; N_hand <= N_tot(V8-F) + 31. Caps: X cells keep W0-c (IC
phases 2,560 MiB / 300 s, others 1,536 MiB / 180 s; the IC cap re-probed before X-3, PM7-13); field builds 2,560 MiB /
600 s (W0-i); campaign PM7-12. Argument: waves, not one cell per member: the smallest detectable paired gain on 4 years
is .165 (plan 12.2), so one member's effect is noise at cell level; per-member cells would add up to 23 to N_c and
decide nothing, while the admission gate screens and counts each member (R-2 precedent). Three alpha waves, not one:
repairs, refinements and additions are different hypothesis classes and one rejection must not drop the others (two
extra cells raise E[max] under 1%). One cell per rule: it acts on the whole library or aim. One stage-1 campaign: a
closed list, budget = capacity (draft D4), one confirm year (D1); it costs 12% on SR0 and DSR_hand shows what dropping
it would give. A refinement that exceeds the cap of 8 (an unconfirmed repair included) waits for v9 at 0 trials.

## 6. Order of X cells and acceptance (fixed now; never re-ordered after a read)

Unless a row says otherwise: parent = last accepted; fields v13 (P6); PM6-6 gross matching; rule 5 (paired S2 net dSR
> 0 against the parent AND mechanics AND the criterion); both p printed (E-34, PM5-23); year table, 4x row (E-29) and
turnover printed. X-2, X-3 and X-9 list every new string in `gate.admitted` (one admission line each); a refinement the
gate does not admit leaves the wave and its member keeps its pre-X string (declared now: restoring it is no choice).

| cell | content | acceptance besides mechanics |
|---|---|---|
| X-1 | XIMP-A repairs the PM confirmed from XIMP's proof, one wave, in place, members not re-screened; parent V8-F | mechanics alone (PM7-9); run, counted, dSR printed, gates nothing |
| X-2 | XIMP-B refinements (and unconfirmed repairs), one wave, in place | dSR > 0 AND net Sharpe at 4x NAV not lower (PM7-10) |
| X-3 | XSIG + XDATA additions, one add-alpha wave (PM7-14) | dSR > 0 AND net Sharpe at 4x NAV not lower (PM7-10) |
| X-4, X-5 | XIMP-C processing variants, one cell each | dSR > 0 AND XIMP's registered criterion (default: turnover per unit gross not higher, PM5-11) |
| X-6, X-7 | XCOMB-1 combination rule; XCOMB-3 if a combination rule | dSR > 0 AND turnover per unit gross not higher (PM5-11), unless XCOMB registers another |
| X-8 | XCOMB-2 capacity rule (XCOMB-3 here if a capacity rule) | dSR > 0 AND net Sharpe at 4x NAV higher AND cost per traded dollar not higher (default; XCOMB's registered criterion governs) |
| - | campaign v9-mine-c1 on pool H-F (section 9); not a cell | campaign mechanics |
| X-9 | mined wave; parent H-F | dSR > 0 AND net Sharpe at 4x NAV not lower (amends draft D14, PM7-10) |
| X-10 | leverage on X-F0 (section 8); not gross-matched | section 8 (PM7-11) |

Why this order: repairs first (every later paired test on correct definitions); refinements before additions (new
members judged against the improved roster); processing after additions (it acts on every member); combination, then
capacity (it acts on the aim); the campaign after every hand-written cell (D15: priors before mined; the pool is the
book the mined members enter; H-F is the book PM7-2 needs); leverage last (PM7-3: it scales whatever X built).
Order rules: a cell whose condition is unmet or whose rule is undefined on its parent (E-45, PM4-10) is undefined (0)
and the next runs on the same parent; a member gets one change in X (a refinement of a repaired member is written on
the repaired string; an XSIG candidate duplicating an XIMP change: the PM keeps one before any read, PM7-14); if XIMP-C
and XCOMB deliver one mechanism, XCOMB's is kept and the other withdrawn at 0 (PM7-14); a rule ruled a restatement of
R-3, R-4 or any rejected v8 / X rule is withdrawn at 0; plan section 13 binds X (PM7-14); caps PM7-13.

## 7. The X gate (cumulative test) and the claims (PM7-8)

Run once, after X-10 (after X-9 if X-10 is undefined). Pass = (a) `nav_summ.py --protocol v8 --bundle <V8-F nav>
<X-F0 nav>`: paired dSR > 0 with one-sided bootstrap p < .10 (rule 4: block 21, seed 20260929, 4,999 resamples; both
p printed) AND (b) DSR_tot(X-F0) >= .95 AND (c) mechanics of X-F0. Printed beside, gating nothing: bundle X-F0 vs B0c,
DSR_hand, DSR_v8, PBO, year table, net Sharpe at 4x, net annual return, turnover, the Appendix A block. The X book
replaces the V8-F book only on a pass; otherwise the V8-F book stays the deployable book, the X report says so, every
per-cell verdict stands, nothing is re-run, and the X book is carried to v9 with its count.
Claims: **Sharpe up** only under (a)-(c). **DSR up** as defined in section 4. **Capacity up**: X-F0's 4x net Sharpe
above V8-F's, stated as sign-level. **Return up**, split in two: X-F0 vs V8-F (matched gross: signal and construction)
and X-10 vs X-F0 (leverage: the owner's risk decision, never called alpha; section 8).

## 8. Leverage cell X-10 (PM7-3, PM7-11)

- Parent X-F0 at L_P. **L = 2.0**, the executable's maximum: one value, no grid; r = 2.0 / L_P. In dollars the cell's
  1x book lies between the parent's registered 1x and 2x curve points (E-29). Spec: `change.set {"nav.leverage",
  "nav.output"}`; not gross-matched.
- Mechanics restated: the all-rows gross limit is [.90, 1.05] x (2.0 / L_P) x the parent's matched gross ratio,
  computed and written in the cell's plan before the run (PM7-11); |mean net| <= .02 r; tau mean <= .20 r, p95 <= .30 r;
  every other row unchanged.
- Acceptance (replaces rule 5's dSR > 0 for this cell only): net annual return (S2, 1x) above X-F0's AND net Sharpe at
  4x NAV not lower than X-F0's by more than .100 AND mechanics. ".100" is "one SE": plan 12.2's registered SE of a
  paired dSR on 4 years, fixed before any result (a scale-only change has a measured paired SE near 0; the iid SE of
  one Sharpe, about .5, guards nothing).
- Report: the unlevered book (X-F0) and the levered book (X-10) side by side, each with net annual return, net Sharpe
  at 1x and at 4x NAV, and maximum drawdown. Which one is deployed is the owner's risk decision (PM7-3). The cell counts
  in N_c and V; its dSR and both p are printed; it is never in the Sharpe claim.

## 9. Mined campaign (OD-7 granted by PM7-2; PM7-12): amendments and runbook

Registration of record: `docs/plans/2026-10-01-v9-mine-campaign-prereg.md`, `scripts/specs/v9/mine-c1.json` and its
runbook, as joined at `1bd448cd` (merged at integration 8). Amendments:
- **A1 counting.** Every evaluation enters N_tot as M (PM7-7, PM7-12; amends draft item 10 and E-33 for the X DSR only;
  `trial_counts` and N_c unchanged).
- **A2 source.** H-F, the last hand-written X book (PM7-12, amends D15); role `build-equity/train-2020-2023-lo3/
  manifest.json` (B0b won); fields = H-F's.
- **A3 fields.** The field rule (item 4) is re-applied at the lock to the registry at H-F: a field read by any registry
  alpha, every X-listed member included (admitted or not), leaves the list; B = 11 x fields; F = 1.47 if B <= 100,
  1.54 if 101..1,000. XSIG's preference (1) targets these fields; the hand-written prior keeps the field.
- **A4 memory and isolation (PM7-12).** `max_memory_mib` = the registered probe's value (expected 4,032 MiB at 4
  workers). The campaign runs alone: no other data process, no build. If free physical memory at the start is below
  max_memory_mib + 1 GiB, the integrator lowers the workers (the next of 4, 2, 1; max_memory_mib re-set to that probe
  value; spec re-committed), never the budget, and stops if 1 worker does not fit.
- **A5** wave criterion PM7-10; **A6** pool cap 80 (PM7-13); **A7** id, spec path, output dirs unchanged (tests pin
  them). Disclosure: H-F was chosen with 2023 in view; the mined expressions are first read by the campaign.

Runbook (root pool-2, after X-8):
```
PY="C:/Program Files/Python312/python.exe"; RC="$PY scripts/research_cycle.py"; SPEC=scripts/specs/v9/mine-c1.json
# 0 integration 8 receipt: mine + factory tests pass; golden 0x889874a3b9b29c55 at 1 and 4 workers; rung_failed 0
# 1 edit SPEC: pool_source.combined / .summary = <H-F ic.w_output>-<k>/train_combined.json / summary.json;
#   pool_source.weights = <H-F fit.output>/composition_weights.json; inputs.role = lo3 manifest; inputs.fields = H-F's
"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_research_mine.py -k fields_are_the_rule   # A3: fields, budget
$RC mine lock $SPEC --write; $RC mine pool $SPEC; $RC mine lock $SPEC --write   # pins role, fields, pool source, pool
$RC mine probe $SPEC     # W = largest of 4, 2, 1 with required <= 7,680 MiB; max_memory_mib = required at W, up to 64
# delete the four requires lines (OD-7: PM7-2, PM7-12); commit SPEC and the draft
$RC mine plan $SPEC      # header: locked pins, capacity = budget = B, z(B), F(B), Fc 1.77, windows, caps
$RC mine run $SPEC --date <YYYY-MM-DD>   # alone; free physical >= max_memory_mib + 1,024 MiB, else A4
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
  only reader of 2024+ and outputs two bits. Not run in X without an owner ruling (PM7-14). If ruled: at most once, on
  one book chosen before the read by the X gate (X-F if the gate passed, else V8-F), never on both, never on a
  candidate; thresholds pinned in the ruling file before; adds 0 to N (validation event line), 1 to validation reads;
  nothing in the book changes after it; a failed bit is an owner decision, never a re-run.
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
(id, class, cell, string or rule, constants, lane report path@SHA, SHA-256 of the DSL or rule text; the defects the PM
confirmed for X-1, PM7-9) and records the file's SHA-256 in progress.md. P5 the DSR tool (section 4, `44a27d5d`)
merged at integration 8 with its tests passing. P6 fields v13 = V8-F's fields + every X field, one build (0 trials),
every reused payload bit-identical, reuse counts as the lanes predicted. P7 PM7-13 at integration 8: roster and pool
caps 64 -> 80 (regenerated recipe re-pinned; identity by the golden and the pinned bench); the IC-pass memory cap
re-probed before X-3.

## 13. Rulings (PM7-6..PM7-14, declared before any X measurement; full text in progress.md)

- **PM7-6 budget**: <= 10 construction cells, <= 39 admission trials (23 hand-written, 16 mined), one campaign <= 132.
- **PM7-7 count and deflation**: N_tot = every trial the defect rule counts + campaign registry counts, no netting; V
  ddof 1 over construction lines on research-window-v2; DSR_tot gates; DSR_hand and DSR_v8 beside (sections 3, 4).
- **PM7-8 X gate**: the X book replaces V8-F only if DSR_tot >= .95 AND paired one-sided p < .10 (section 7).
- **PM7-9 repairs**: mechanics alone, not re-screened, only for defects the PM confirmed from XIMP's proof (the string
  does not compute its own stated canonical definition, from DSL semantics); else a refinement in X-2 (sections 3, 6).
- **PM7-10 alpha waves**: X-2, X-3, X-9 on dSR > 0, mechanics and 4x net Sharpe not lower; turnover printed (section 6).
- **PM7-11 leverage**: L 2.0, one SE .100, net return + 4x guard + mechanics; gross limit in the cell's plan before the
  run; unlevered and levered books side by side with maximum drawdown; the owner chooses (section 8).
- **PM7-12 campaign**: memory = probe value; runs alone; workers lowered below cap + 1 GiB free, never the budget;
  source H-F; every evaluation in N_tot (section 9).
- **PM7-13 caps**: roster and pool 64 -> 80 at integration 8; IC-pass memory cap re-probed before X-3 (P7).
- **PM7-14**: plan section 13 binds X (lifted by name only for risk-only rules); XIMP-C / XCOMB on one mechanism keeps
  XCOMB's; one change per member; XSIG + XDATA in one wave; hidden-block read only by owner ruling, once at most, on a
  book chosen before the read (sections 6, 10).

Appendix A block for X results: `TRAIN construction cells <N_c>; admission trials this sprint <k> (v8 <k8>, X
hand-written <kx>, mined <km>); mined campaigns <0|1> (v9-mine-c1: budget <B>, registry count <C>, admitted <a>); N_tot
<n>; N_hand <h>; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8:
2 (2023-2024); history reads <r>; 2025+ never read.`

## 14. The X list (precondition P4; pinned 2026-10-02 by root at the PM's dispatch, from PM7-18, PM7-24, PM7-32)

Pinned before any X measurement (no X cell, screen or campaign has run; no X statistic exists). Basis: PM7-16 (a),
PM7-18 (a)-(c), PM7-23, PM7-24 (the cell list and each criterion), PM7-32 (XDATA registration), PM7-15 (a) (theme
`filing_events` last in the fitter, done at integration 8). Confirmed defects for X-1 (PM7-9): **none** (PM7-18 (a): A-1
is a variant, not a repair); X-1, X-7, X-8 are unused and not refilled (PM7-24). Lane reports (path @ commit, file
SHA-256): XIMP `task-XIMP-report.md` @ `3db253d5` (`c31428f9...9620`); XSIG `task-XSIG-report.md` @ `7467448f`
(`377265d4...caff7`); XDATA `task-XDATA-report.md` @ `e3654b93` (`72f2a27a...00c6a2` incl. task GOLD); XCOMB
`task-XCOMB-report.md` @ `914f9944` (`945a7b09...c6a2`). Fields of every X cell: v13 lo3, manifest
`e5f7f28c465a92885d55d55a439eb2f217b9f5f15d51017b4293ebc55935e9b2` (P6; 75 rows = v10's 70 + the 5 X fields).
Every add-alpha call carries the lane's frozen argv; only `--parent`, `--name`, `--parent-spec`, `--fields` and
`--plan-json` (PM6-9) are filled in by root. Hand-written admission trials: 13 (X-2 5, X-3 8); the other 10 lapse.

| cell | id | class | string or rule | constants (theme, tier, sign; flags) | source | SHA-256 of the DSL / rule text |
|---|---|---|---|---|---|---|
| X-2 | `q5_eg_f49g` | refinement (A-1, admission trial, PM7-18 a) | XIMP A-1 string | investment_issuance, B, +1; `--replaces q5_eg_f49` (no `--rescreen`) | XIMP A-1 | `07a61a9eeedf47ba8743086054475298ff892f5444cf66dd47316e6ab35cbf41` |
| X-2 | `iv_rv_spread_xe` | refinement B-1 (TRAIN statistics in view, PM7-18 b) | XIMP B-1 | options_implied, B, +1; `--replaces iv_rv_spread` | XIMP B.4 | `29e7d9cf564cacf2424ce917718613c3e1787e0c8fdc77fec19c41369054e0ae` |
| X-2 | `ind_adj_rev_5_nx` | refinement B-2 | XIMP B-2 | reversal_seasonality, B-, +1; `--replaces ind_adj_rev_5` | XIMP B.4 | `9c1d051d4a6ff79a34c4a7057cd48ec64c9d5f6957ea317f5a3f6254e052b46f` |
| X-2 | `ins_opp_buy` | refinement B-3 (TRAIN statistics in view) | XIMP B-3 | ownership_flow, B-, +1; `--replaces ins_opp` | XIMP B.4 | `981d01b22ba05cfa768ae1b7a009001d18172dad81a9f2bbeb9a6f3c1f75a68a` |
| X-2 | `bac_vq` | refinement B-4 (TRAIN statistics in view) | XIMP B-4 | low_risk, B+, +1; `--replaces bac` | XIMP B.4 | `116135c031f95c4655f9e7e6ae7efae71162c622eeb9887697a670e3a1123ecc` |
| X-3 | `stmom` | new (XSIG 1) | XSIG section 5 | price_momentum, B-, +1 | XSIG 5 | `06dc6238d7e94089391ca1405731ea8ebfe000eb8c9ea45d131fbecf53ab907d` |
| X-3 | `earn_season` | new (XSIG 2) | XSIG section 5 | reversal_seasonality, B-, +1 | XSIG 5 | `64a0be8f2c71efd1f7a2de920bb5163e99b9dfcef3ea3f4da15f673bc0b328e2` |
| X-3 | `k8_intensity` | new (XSIG 3) | XSIG section 5 | filing_events, C+, +1 | XSIG 5 | `0b7d6cdeb6c2f86e328543eb7c996c5d4939fd960631cabce65b4a11e1c7cb05` |
| X-3 | `inst_persist` | new (XSIG 4) | XSIG section 5 | reversal_seasonality, C+, +1 | XSIG 5 | `ea338c04bdff78ef224eb4244568fa672b051eb2c94020749add9f3ba184ab9c` |
| X-3 | `nt_late` | new (XSIG 5) | XSIG section 5 | filing_events, B-, +1 | XSIG 5 | `bafc4e3a93204e59b4a455e7c88335167ed612f01072192526551eea58a7aeed` |
| X-3 | `div_season` | new (XDATA) | `rank(div_month_pred)` | reversal_seasonality, C+, +1 (PM7-32) | XDATA 4b; argv below | `5a1380a1d482c34aca88450b8de9a065150afeaa0c3c8cd359956d4cfd06d0dc` |
| X-3 | `vol_beta` | new (XDATA) | `rank(decay_linear((-1 * beta_dvol_21), 21))` | low_risk, C+, +1 (PM7-32) | XDATA 4b; argv below | `91583af314a4f9dcec2c6a2c2fc1a507fbfdd39a60068bfe744e8158b6458e94` |
| X-3 | `season_y2_5` | new (XDATA) | `rank(season_y2_5)` | reversal_seasonality, C+, +1 (PM7-32) | XDATA 4b; argv below | `13fe28b662804e2d4daaad505475d65837cd8aaa95dc57650835e3c5a5d818eb` |
| X-4 | `value_composite_v49` | re-screen (C-1) | XIMP C-1 table | the replaced member's; `--replaces value_composite --rescreen` | XIMP C-1 | `09fb156c56c6de79548fe94d905db524717b8fa561867a79965d0585f56b864f` |
| X-4 | `bm_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces bm --rescreen` | XIMP C-1 | `a37c3ecabe83b6982017e247a7a802a1546712b078596d68b9d4b0d62473d09d` |
| X-4 | `ep_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces ep --rescreen` | XIMP C-1 | `0d1975e5a8f11aa523aaf7d7db62f0c6424edb465f6cd78b7149653cd7e976e4` |
| X-4 | `cfp_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces cfp --rescreen` | XIMP C-1 | `86aed3229acc7590722d6eb5172844ebf2e0ba79059dd2e5b2487417893aaf7d` |
| X-4 | `fcfp_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces fcfp --rescreen` | XIMP C-1 | `408ba943fe971124d44c2eb4948a4d0dded6965f49c617cb8043fa71ef604a4d` |
| X-4 | `ebit_ev_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces ebit_ev_f49 --rescreen` | XIMP C-1 | `4a2b9ba77881892f947213d3929fbf7aa21c57011940f637b5b592c926fb78cb` |
| X-4 | `net_payout_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces net_payout --rescreen` | XIMP C-1 | `8b6e8423c1997896f0d2ed88e865b36de2245e53e9d093df85c31f7ef7021f8a` |
| X-4 | `sp_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces sp --rescreen` | XIMP C-1 | `1153ac469a8e35fc89df89dd7f504203df006c81c8a3647befed0a1a36e9c962` |
| X-4 | `rd_me_v49` | re-screen (C-1) | XIMP C-1 table | `--replaces rd_me --rescreen` | XIMP C-1 | `bf89d6a635c81096135fcfe25236067d091f0447189c63846f59ca40d70df07d` |
| X-5 | `theme-erc-v1` | combination rule (XCOMB 1) | template `scripts/specs/v8/x-theme-erc.json` @ `30b719e0` | 10000 sweeps, dispersion 1e-10, cap 1/(2T) | XCOMB 1 | file `c71e8b576030b9df66ca758d6966387600abe0b32c25118e4cd40055b8b79ae2` |
| X-6 | `inv-vol-v1` | capacity rule (XCOMB 2) | template `scripts/specs/v8/x-inv-vol.json` @ `068b4a4d` | floor fraction .25, median, fill-session sigma | XCOMB 2 | file `26c971023bc51da246c0f5d4a142335e5fadea9346c46ed3d376a65d6dc36592` |

X-4 calls: the replaced member's theme, tier, sign, prior-sign source, form and notes from the registry, citation +
"; within FF49: Ehsani, Harvey and Li 2023, FAJ" (XIMP C-1 "Code path"), roster order. Acceptance per cell (section 6,
PM7-10, PM7-24, PM5-11): X-2 / X-3 dSR > 0 AND mechanics AND net Sharpe at 4x NAV not lower than the parent's (turnover
printed); X-4 dSR > 0 AND mechanics AND turnover per unit gross (PM5-11: executed tau_gmv_mean / all-rows gross, S2) not
higher, the value members' FF49 covered cells printed; X-5 dSR > 0 AND mechanics AND turnover per unit gross not higher;
X-6 dSR > 0 AND mechanics AND net Sharpe at 4x NAV higher AND S2 cost per traded dollar lower. PM6-6 on every cell.

XDATA add-alpha argv (PM7-32; form, formula, domain, deviation from report sections 4a / 4b):

```bash
"$PY" scripts/research_cycle.py add-alpha --id div_season --dsl "rank(div_month_pred)" --theme reversal_seasonality --tier C+ --prior-sign 1 --citation "Hartzmark and Solomon (2013, JFE) The dividend month premium" --origin prior --prior-sign-source "Hartzmark-Solomon 2013" --form "R(x)" --formula "div_month_pred (hs-divseason-q3-6-9-12-v1): Ex-date ledger: an observed session s of a line whose step from its previous observation p has y = 1 - F_p/F_s in [1 bp, 4%], no kept-gap step in (p, s], and is not a factor-break-v1 jump cell (a factor step with no matching raw drop). Row t, M = the month of session t: n_paid = months of M-12..M-1 holding an ex-date; a quarterly payer has 3 <= n_paid <= 6 and a first observation at or before month M-12's first session; value 1 if an ex-date is in M-3, M-6, M-9 or M-12, else 0; NaN for non-payers, annual, semiannual and monthly payers, short history" --domain "NaN for non-payers, annual, semiannual and monthly payers, short history" --deviation "DIV_PRED_MONTHS 3, 6, 9, 12: Hartzmark-Solomon (2013) (the lag set of the CZ DivSeason replication); DIV_PAYER_MONTHS, DIV_MIN_PAID, DIV_MONTHLY_MAX 12, 3, 6: the lag set predicts a quarterly cycle; a quarterly payer shows 4 paid months a year, 3 when one is skipped or moved across a window edge, at most two extras; annual and semiannual payers (1-2) would be flagged in months they never pay, monthly payers (7+) have no off month; DIV_YIELD_MIN 1e-4, DIV_YIELD_MAX 0.04: ex-dates from the vendor factor, above its print precision and below any regular payment; specials, spin-offs and stock dividends of 5% or more fall outside" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id vol_beta --dsl "rank(decay_linear((-1 * beta_dvol_21), 21))" --theme low_risk --tier C+ --prior-sign 1 --citation "Ang, Hodrick, Xing and Zhang (2006, JF) The cross-section of volatility and expected returns" --origin prior --prior-sign-source "Ang-Hodrick-Xing-Zhang 2006" --form "R(decay_linear(x, 21))" --formula "-beta_dvol_21 (ahxz-beta-dvol-spy21-v1), decayed 21: OLS of the line's daily adjusted return on [1, r_SPY, dIV_SPY] over sessions t-21..t-1 (at least 17 usable days, regressors not collinear); value = the dIV slope. SPY = the unique securityID with ticker_tk 'SPY' (else refused); r_SPY its adjusted return (house guard, jump cells excluded); dIV the daily change of its atmCenI_21d inside IV_DOMAIN; line returns with the house guard and no kept-gap step; long low beta" --domain "NaN with fewer than 17 usable days in t-21..t-1 or collinear regressors (DVOL_MIN_DAYS 17, DVOL_DET_TOL 1e-10)" --deviation "VOL_LINE_TICKER, VOL_COLUMN SPY, atmCenI_21d: AHXZ use VXO (S&P 100, 30-day ATM implied volatility); SPY's 30-day (21 trading days) ATM IV is the in-house analogue; SPY's adjusted return for the CRSP value-weighted market; DVOL_WINDOW 21: AHXZ: daily returns within one month; DVOL_MIN_DAYS 17: declared: about 80% of the window, the house ratio of F-1's 48 of 60; AHXZ's own minimum not verified" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id season_y2_5 --dsl "rank(season_y2_5)" --theme reversal_seasonality --tier C+ --prior-sign 1 --citation "Heston and Sadka (2008, JFE) Seasonality in the cross-section of stock returns; Keloharju, Linnainmaa and Nyberg (2016, JF) Return seasonalities" --origin prior --prior-sign-source "Heston-Sadka 2008; Keloharju-Linnainmaa-Nyberg 2016" --form "R(x)" --formula "season_y2_5 (hs-season-y2-5-v1): for k = 2..5: b = e - 252k, a = b + 21 (e = row t on the extended axis); r_k = P_a/P_b - 1 with both observations, no kept-gap step, the monthly divergence guard; value = mean of the finite r_k when at least 3" --domain "NaN with fewer than 3 finite r_k (SEASON_MIN_YEARS 3)" --deviation "SEASON_YEARS 2, 3, 4, 5: Heston-Sadka (2008); the CZ MomSeason years 2 to 5 form; SEASON_YEAR_SESSIONS, SEASON_WINDOW 252, 21: the alignment of the roster member seasonality_same_month (delay 252 / 231); SEASON_MIN_YEARS 3: declared: a majority of the four windows (one halt or listing gap does not drop the name)" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
```
