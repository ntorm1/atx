# Oracle v2 — session handoff, 2026-08-19

Written at a hard stop. Two lanes were **still running** when this was written; their
results are NOT in here and must not be assumed. See [§5](#5-in-flight-do-not-assume-results).

Goal in force: *"run oracle-v2 loop to target completion."* **Not met.** Bootstrap is
complete and the loop can now, in principle, iterate — but no ratchet iteration has ever
run, and no accuracy target is met.

---

## 1. One-paragraph state

The oracle bootstrap state machine reached **`ready`** (`next_iter = iter-001`) for the
first time: all four capability receipts valid. Stage 3 (conventions) and Stage 4 (Mode B)
both landed and were independently gate-verified. Mode B gave us the **first real
volatility measurement** in the project's history — 442.79 bp against a 10 bp target,
where Mode A's 0 bp was an identity that measured nothing. A diagnostic lane then found
that **89.4% of the price error is a missing exercise-style dimension**, not a tuning
problem, with a measured counterfactual of 376.06 → 40.54 ticks. Two lanes were in flight
at the stop: one closing a holdout-leakage hole in the broker, one adding the
exercise-style axis.

---

## 2. Refs, and what they mean

| Ref | SHA at stop | Meaning |
|---|---|---|
| `refs/heads/oracle/canonical` | `abed0a9c` | The ONLY thing the capability probe reads. Stage 4 head. |
| `main` | `1cbc3946` | Contains canonical. **Moves under you** — another session commits here concurrently. |
| `backup/oracle-canonical-20260817` | `e232a118` | Pre-Stage-3 canonical. |
| `backup/oracle-canonical-stage4-20260818` | `a1c984e5` | Pre-Stage-4 canonical. |

Oracle line, oldest → newest:

```
d2d4c0c6  Stage 1 receipts (recovered)
e232a118  Stage 2 Mode A bootstrap          <- old canonical
7124c823  merge main into canonical (two-headed history reconciled)
8ca07919  v2 design docs
e79f2b8d  oracle-bench CLI contract (the six frozen gate strings now parse)
602d7bfa  Stage 3 receipts
a1c984e5  Stage 3 exact-token fix
abed0a9c  Stage 4 Mode B                    <- canonical NOW
```

**Standing authorization (granted by the user this session):** advance
`refs/heads/oracle/canonical` whenever a lane's gates pass **and** the move is a verified
fast-forward **and** a backup ref is written first. Anything divergent, any force, any
non-fast-forward → stop and ask.

---

## 3. What landed, verified

All numbers below were reproduced by the PM independently, not taken from agent reports.

### Stage 3 — conventions RESOLVED
Map: `discrete_forward_pv__rate__sdiv_yield`. Five gates PASS. 277,952 rows, 0 engine
errors, 100% selection coverage. Pinned floor at `atx-vol/bench/oracle/scorecards/iter-000.json`.
Speed pin 3122 rows/s = `floor(3469.47 * 0.90)`, re-measured 3857.54 on a quiet host.

One accepted regression, published: `mode_a_vega_rel` +0.29% of baseline (bound is 1%).

### Stage 4 — Mode B, vol MEASURED from NBBO
Fits per `underlier × expiry × bucket` via `american_implied_vol`. 241,052 of 299,798 rows
fitted; 36,900 refused and counted. Gates: `mode_b_targeted_tests` 1/1,
`mode_a_targeted_tests` 53/53, `mode_b_smoke_tune` PASS.

### The numbers (honest)

| metric | Mode A floor | Mode B measured | target | met |
|---|---:|---:|---:|:--:|
| price_mae | 376.06 ticks | 35.52 | ≤ 2 | NO |
| vol_mae | 0 *(identity)* | **442.79 bp** | ≤ 10 | NO |
| delta_rel | 0.0133 | 0.0610 | ≤ 0.02 | NO |
| gamma_rel | 0.0788 | 0.4024 | ≤ 0.02 | NO |
| theta_rel | 12.68 | 2619.93 | ≤ 0.02 | NO |
| vega_rel | 0.2134 | 9.5797 | ≤ 0.02 | NO |
| rho_rel | 0.8489 | 5.2989 | ≤ 0.02 | NO |
| phi_rel | 1.1989 | 5.0967 | ≤ 0.02 | NO |
| volga_rel | 0.5228 | 1.7194 | ≤ 0.02 | NO |
| vanna_rel | 0.1573 | 0.5340 | ≤ 0.02 | NO |
| delta_decay_rel | 1.3189 | 15.2330 | ≤ 0.02 | NO |

---

## 4. Numbers that will mislead you — read before citing anything

1. **`mode_a_vol_mae = 0` is an IDENTITY.** Mode A prices *at* `srVol`, so it reports back
   the vol it was handed. It is not vol accuracy and never was.
2. **`mode_b_price_mae = 35.52` is nearly an identity too.** Mode B re-prices the mid it
   inverted, so it measures `|mid − srPrc|` — a property of SpiderRock's smoothing, not our
   accuracy.
3. **The "10× worse than the market mid" claim was WRONG and is corrected.** It compared
   different populations (277,952 vs 241,052 rows). On the same admitted set it is **2.8×**.
   Mode B's refusal screen uses the *American* zero-vol bound, so it structurally discards
   exactly the rows where the exercise-style bug lives — 76.7% of total price error sits in
   the 12.8% of rows Mode B drops.
4. **"Vol is floored by the price leg" was too strong.** That arithmetic divided a
   tail-dominated *mean* by a *median* vega. Row-level median implied `|dvol|` is **4.79 bp**,
   not 442. True for the tail; false for the body. Direction survives (fixing exercise style
   drops mean implied `|dvol|` 345 → 72 bp), the strong form does not.
5. **`theta_rel = 2619` is not informative.** The relative metric's 1e-4 denominator floor
   explodes on near-zero oracle greeks. `iter-000`'s own baseline shows `volga_rel = 132.2`
   for the same reason.
6. **Price MAE is an unweighted dollar mean** over prices from $9 (MULL) to $8,135 (SPX).
   SPX is 31% of rows and 81.5% of the error.

---

## 5. IN FLIGHT — do not assume results

Two agents were running at the stop. **Check their worktrees before doing anything else.**

### `ready-migration` — pool-14, `lane/oracle-ready-migration-20260818`, base `abed0a9c`
Resumed with instructions to (a) close the holdout-visibility hole scoped by
`operation_id`, (b) close the unconsumed finalize capability on REJECT, (c) commit in four
separate commits, (d) add a CHANGELOG entry.

At the stop its work was **uncommitted** (~1,182 insertions across 6 files + a new
`scripts/tests/oracle-ready-contracts.test.mjs`). If the process died, the tree is dirty
but intact — inspect, do not restart from scratch.

### `exercise-style` — pool-13, `lane/oracle-exercise-style-20260819`, base `abed0a9c`
Adding an exercise-style axis to `ConventionMap` and letting the sanctioned sweep measure
it. Explicitly forbidden from committing `iter-000.json`, `conventions.json`, or
`CONVENTIONS.md` — those are the ratchet's artifacts, and re-pinning outside a ratchet
would destroy the baseline and skip holdout validation.

---

## 6. Open blockers and decisions

### 6a. HOLDOUT LEAK — highest priority, fix in flight
`#repoFiles()` (`scripts/oracle-lane-broker.mjs:742-745`) filters only cohort JSON and
parquet, then serves everything under `atx-vol`/`.claude`/`scripts`/`docs` — including
`bench/oracle/scorecards/*.json`, `docs/LEDGER.md`, `docs/oracle/NORTHSTAR.md`. Ratchet
memory carries holdout aggregates. Improve is the **tuning** stage and its planner and
builder both hold `repo_read`.

Left unfixed, every ratchet after the first is tuned against the test set — silently, with
all gates green. **Verify the fix landed before running any iteration.** The prescribed
shape is scoped by `operation_id`: invisible to `sprint_build`/`sprint_integration`/`measure`,
visible to `ratchet` (must append) and to bootstrap (preserves Stage 3 re-run). Applied to
**both** `repoSearch` and `repoRead`.

### 6b. Five pre-existing bugs in the retired ready path
Found during migration; all latent because the path never executed.
- **The ACCEPT path could never have landed.** `casReceiptError` compares
  `receipt.new_tree !== expected.new_tree`, but the retired ratchet built `expected` with
  no `new_tree` → "CAS identity mismatch" on every ACCEPT.
- The digest receipt was unsatisfiable (demanded a digest the publishing command
  deliberately never emits).
- `metric_evidence` was unsatisfiable (required a pasted line no broker command emits).
- Suspect exclusion unreachable (needs a parameterized market-check gate that
  `GATE_REGISTRY` does not contain). Retired → `oracle_suspects_excluded` must be `[]`,
  so the whole holdout is benchmarked. Conservative direction; changes no verdict.
- `scripts/tests/oracle-receipt-adoption.test.mjs` is **8/8 red at pristine HEAD** —
  orphaned by the Stage 1 broker hard-cut. Pre-existing, not new breakage.

### 6c. Unconsumed finalize capability on REJECT
`lane_release` mints the ratchet finalize token unconditionally; on REJECT nothing consumes
it, leaving a durable token that can move canonical to a SHA the verdict rejected. Goes
inert once canonical moves, but that is a race, not a guarantee. Fix in flight.

### 6d. Ingest gaps that bound what is achievable
- **No exercise-style / `calcEngine` column.** SPX/XSP are defensible from contract spec;
  MGTN is tagged `EQT`/`NMS`/`Stock` yet reproduces European at 94.9% — **not derivable
  from any column we ingest**. A ticker list is a stand-in for data we do not have.
- **No dividend ex-date.** `ddiv` is a scalar, so the escrowed-spot model cannot be
  replaced by a discrete-dividend tree. This blocks the 10.5% error bucket.

---

## 7. The diagnosis to act on (measured, committed at `1e44b56f`)

Tool: `atx-vol/examples/oracle_price_leg_diag.cpp` on `lane/oracle-price-leg-20260818`
(reads rows from **stdin** as CSV; it is not a standalone runner).

- **Our Andersen-Lake pricer is NOT the bug** — mean abs $0.0035 against an independent
  CRR-Richardson binomial. A thousand times finer than the $3.76 being explained.
- **89.4% of error = exercise style.** `srPrc` *is* the European price on our exact inputs
  (XSP and MGTN to $0.0001, SPX to $0.023). Counterfactuals: baseline **376.06** → European
  for SPX+XSP **44.40** → +MGTN **40.54**, against an oracle-cheat `min(Amer,Euro)` bound of
  **40.28**. Routing by exercise style captures essentially all of it.
- **10.5% = escrowed-spot dividend model.** `spot = uPrc − PV(ddiv)` with `q = sdiv`
  destroys the call's dividend-capture early exercise and inflates the put's premium ~18%.
- **Clean-American residual = 2.68 ticks**, so the irreducible `calcEngine` floor is
  **≈3 ticks, not hundreds**. Charter target is 1 tick — so the target may be unreachable
  without `calcEngine`, and that is worth deciding early.
- **Volatility clock FALSIFIED as a cause** — `mode_a_inputs()` consumes SpiderRock's own
  `row.years` verbatim. `day_count: BUS_252` only scales theta/delta-decay; it never touches
  pricing.
- **α recovered anyway, verified by independent arithmetic:** **α = 0.710606**, 7.5 h
  trading day (7.5 × 252 = 1890 exactly). Weekday step `7.5α/1890 + 16.5(1−α)/6870` =
  0.003515 vs measured 0.003514925; weekend `24(1−α)/6870` = 0.0010109 vs 0.001010988;
  `252a + 113b = 1.000003`. **Pin this** for anything constructing its own year fraction.

Error is a violent tail, not a bias: top 1% of rows carry 64.3% of total error; ITM
long-dated **puts** dominate.

---

## 8. Recommended next steps, in order

1. **Check the two in-flight lanes.** Inspect worktrees; do not restart.
2. **Verify the holdout fix landed** (§6a) and that its tests fail when reverted. Nothing
   else matters if this is open.
3. **Land the exercise-style axis** through the sanctioned sweep. Expect ~376 → ~40 ticks.
   Still misses the 2-tick target.
4. **Run iteration 1 for real** — the first ever. Watch for the §6b bugs resurfacing, and
   for `sprint_build` scope-path friction on untracked files (untested against a real lane).
5. **Decide on ingest** (§6d). Both remaining error buckets need data we do not collect.
6. **Decide whether a 1-tick target is reachable at all** given a ~3-tick measured floor.

---

## 9. Traps that cost real time this session

- **Receipts must preserve EXACT digit tokens, not just values.** Round-tripping through a
  float rewrites `783.64806090884758` → `783.6480609088476` (same double, one digit shorter)
  and PowerShell 5.1's `ConvertFrom-Json` does not type them identically. `residual_floor`
  fails closed with "digits that do not round-trip". Use a `Decimal`-preserving writer and
  verify at the **byte** level; Python value-equality will lie to you.
- **The gate runner refuses any untracked/uncommitted file** under `.claude`/`atx-vol`/`scripts`
  and keys its artifact filename to HEAD's SHA. Every receipt edit forces a commit **and** a
  full ~6 min sweep re-run. Renaming an artifact to match would be fabricated evidence.
- **`main` moves under you.** It moved three times mid-session. Always `git merge-tree`
  dry-run before integrating; expect merges, not fast-forwards.
- **The `rtk` Bash hook can silently truncate `head` output into a redirect** (injecting a
  literal `[N more lines]`) *and* still report `SYNTAX OK`. Do not use shell redirection for
  file edits. `grep` output is also frequently garbled — prefer `sed -n 'A,Bp'` for exact
  ranges.
- **`american_implied_vol` returns SUCCESS at the floor.** It screens only *immediate*
  intrinsic and returns `Ok(kIvMin)` for a quote below the true bound — so a naive caller
  publishes `0.005` as a *measured* volatility. Correct bracket is
  `max(immediate intrinsic, DISCOUNTED FORWARD intrinsic)`; the forward leg is larger for a
  call OTM on spot but ITM on the forward, the everyday case on hard-to-borrow names.
- **Jäckel "Let's Be Rational" is the WRONG inversion for American premia.** It is exact for
  the Black map, so it charges the early-exercise premium to volatility.
- **`ORACLE_BENCH_TEST_COUNT` (currently 53) and `$script:OracleBenchTestIds` must move
  together** or `mode_a_targeted_tests` fails closed. That is the registry working.
- Never edit source with PowerShell `Get-Content`/`Set-Content` — it BOM-corrupted a file here.

---

## 10. Standing constraints (non-negotiable)

- **Never tune against holdout.** `vol-analyst` sees smoke/tune only. Membership frozen
  after iteration 0. Never run a command containing `--cohort holdout` outside Ratchet.
- **Evidence discipline.** Relay only numbers backed by pasted command output. No output,
  no claim. Agent reports are not self-verifying — reproduce the load-bearing ones.
- **Oracle is north star, not truth.** Cells where the market sides with atx-vol against
  SpiderRock get excluded from the ratchet, not chased. `oracle_suspect_candidates` stays
  empty until evidence fills it.
- **Gates are non-negotiable.** A REJECT means the branch stays unmerged. Report it with the
  refuted hypotheses; do not retry the same hypothesis without new evidence.
- **One `vol-oracle-iter` run at a time.** Never two concurrently.
- **STOP AND ASK** on: 3 consecutive REJECTs, a bootstrap stage failing twice, disk or
  licensed-data blockers, destructive/outward-facing actions, or all targets met.
- Work in `C:\atx-wt\pool-N`, never `C:\atx`. Lease via `scripts/lease-worktree.ps1`, never
  raw `git worktree add`.

---

## 11. Housekeeping left undone

- **pool-15** (`lane/vrp-features-20260817`) and **pool-16** (`lane/vrp-book-20260817`) hold
  VRP lanes with **dead keepers**. Not oracle work, not mine to release — but they are
  holding pool slots and their work may be uncommitted. Worth a look.
- pool-7 and pool-9 are flagged `CORRUPT/LEGACY`; pool-12 is `QUARANTINED (preserved for audit)`.
- Untracked strays in `C:\atx`: `qdfp.cpp/.hpp`, `qdplus.cpp/.hpp` (QuantLib Andersen-Lake
  reference — useful, read in place), `tmp/`, `atx-datalogsxsec-fit/`,
  `scripts/sweep-worktree-builds.ps1`, plus alpha-layer files from the concurrent session.
- LEDGER/NORTHSTAR were updated in `C:\atx` rather than a pool worktree, which departs from
  the letter of "ledger only in pool-N". Flagged deliberately.
