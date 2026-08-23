# Why the top of the OPRA book fails to fit — diagnosis, 2026-08-23

Status: **investigation only.** No C++ was changed. Every number below was
produced by a command recorded in §7.

Scope: the 2026-08-21 full-OPRA populate run and the tier-50 / tier-250
rosters. Binaries under test: `build/bin/atx-vol-surface-db-build.exe` (debug,
built 2026-08-23 07:52) and `build/bin/universe_autofit.exe` (built during this
investigation from the unmodified `universe_autofit` target).

---

## 0. Executive summary

There are **three independent mechanisms**, not one, and they act on three
disjoint populations. The leading hypothesis brought into this investigation —
that the PCP borrow fixed point fails because the surface-db path feeds no
dividend schedule — is **refuted for the top of the book and unsupported for the
tail**; the tail failure *is* a carry failure, but its cause is missing
co-terminal quotes, not missing dividends.

| # | Mechanism | Population | Cells |
|---|---|---|---|
| 1 | Risk-geometry oracle + populate publish floor refuse an otherwise-serviceable surface | top of the book | 25/25 of tier-250; 302 of 2,431 at full universe |
| 2 | Carry-confidence gate finds fewer than 3 quotable co-terminal ATM pairs on every expiry | long tail | 2,129 of 2,431 at full universe |
| 3 | Panel cannot imply a spot by put-call parity, so the board never reaches the fitter | long tail | 943 (`n_load_errors`) |

**The single decisive result for the emitter (§4): a healthy MARK surface exists
for 25 of the 26 probed boards, including every top-of-book name the risk gate
refuses.** Today it is never built, because the populate path requests
`SurfaceOutputs::Risk` only.

`carry=failed` in the `risk surface rejected:` string is a **red herring**. It
reports `SessionDiagnostics::carry_confident`, which on these boards resolves to
`ValidationFailure::CarryGap` (bit 11) — the one bit that *publishes* a
candidate as Degraded rather than rejecting it
(`src/fitting/surface_policy.cpp:33-44`). SPY passes with carry-failed slices
(`t-50.csv` slice_drop rows). It is set on 24 of the 25 tier-250 failures and
explains **zero** of them.

---

## 1. Mechanism 1 — the risk-geometry oracle (top of the book)

### 1.1 Reading the rejection string

The string is formatted at `src/fitting/pricer_fitter.cpp:2041-2068`. Field by
field:

| field | source |
|---|---|
| `mask=` | `ValidationDigest::failures`, a `ValidationFailure` bitmask (`include/atx/vol/api/fitting/surface_policy.hpp:66-105`) |
| `admission=` / `admission_failed=` | `SurfaceAdmissionDecision` from `evaluate_surface_admission` (`src/fitting/fit_policy.cpp:102-207`), i.e. the **populate publish policy**, not the oracle |
| `butterfly=` / `butterfly_slack=` / `slopes=` | `src/fitting/risk_surface_validation.cpp:353-373` |
| `calendar=` / `calendar_slack=` / `calendar_w=` | `src/fitting/risk_surface_validation.cpp:393-433` |
| `carry=` | `SessionDiagnostics::carry_confident` — **diagnostic only** |
| `inversion=` | `SessionDiagnostics::inversion_certified` -> `ValidationFailure::InversionResidual` (bit 7) |

Bit values: `InvalidDomain=1`, `NonFinite=2`, `PriceBounds=4`,
`StrikeMonotonicity=8`, `Butterfly=16`, `Calendar=32`, `Wing=64`,
`InversionResidual=128`, `TimedOut=256`, `StaleInput=512`,
`InsufficientData=1024`, `CarryGap=2048`, `SubstituteUnderserve=4096`.

Two bits are **publish-with-Degraded**, never rejections: `CarryGap` and
`SubstituteUnderserve` (`src/fitting/surface_policy.cpp:33-44`). Every other set
bit rejects.

`mask` bit 0 (`InvalidDomain`) is overloaded: `pricer_fitter.cpp:1801-1803`
folds *any* `FitAdmissionPolicy` refusal into it, which is why the code emits
the separate `admission=` term. On these boards bit 0 always means
`QualityBelowFloor` — `evidence.worst_frac_within_bidask < 0.35`
(`src/fitting/fit_policy.cpp:172-178`, floor at
`tools/include/atx/vol/tools/surface_db_populate.hpp:74`).

### 1.2 Attribution of all 25 tier-250 failures

Every cell is accounted for; there is no residual.

| mask | decode | binding gate(s) | symbols | n |
|---|---|---|---|---|
| 2049 | CarryGap + InvalidDomain | **QualityBelowFloor** (worst in-band < 0.35) | AAPL AMZN GOOGL LQD MSFT | 5 |
| 2064 | CarryGap + Butterfly | **butterfly** | BABA LULU QQQ SOXX USO | 5 |
| 2096 | CarryGap + Butterfly + Calendar | **butterfly + calendar** | AMD IBM IWM SMH XLV | 5 |
| 2176 | CarryGap + InversionResidual | **inversion certificate** | EWZ MAR SIVR W | 4 |
| 2192 | CarryGap + Butterfly + InversionResidual | **butterfly + inversion** | ADP COP LITE TQQQ VRTX | 5 |
| 16 | Butterfly | **butterfly** | RUT | 1 |
| | | | **total** | **25** |

Rolled up by gate: butterfly/calendar geometry binds on **16**, the inversion
certificate on **9** (5 of them jointly with butterfly), the 0.35 publish floor
on **5**. `CarryGap` is set on 24 and binds on **0**.

### 1.3 The butterfly violations are numerically negligible

`convexity_slope_tolerance = 1e-8`
(`src/fitting/risk_surface_validation.hpp:62`). The test is a three-point finite
difference on the Black-76 call price reconstructed from `w`, with the forward
normalised to 1 (`risk_surface_validation.cpp:353-364`):

```
slope_left  = (p[i]   - p[i-1]) / (K[i]   - K[i-1])
slope_right = (p[i+1] - p[i]  ) / (K[i+1] - K[i]  )
slack       = slope_left - slope_right          // > 1e-8 => violation
```

Observed slacks split cleanly by curve family (per-attempt values from
`ua-prod-attempts.csv`):

* **ConvexDense rungs — pure numerics.** SMH `1.34e-8`, USO `1.47e-8`,
  QQQ `2.28e-8`, RUT `3.46e-8`, RUT (round 0) `5.01e-8`, XLV `5.16e-8`,
  SOXX `7.06e-8`, USO `8.37e-8`, IBM `1.60e-7`, IBM `4.72e-7`.
  These are 1.3x to 47x a 1e-8 tolerance on a price normalised to F = 1.
  A slack of 1.3e-8 is a butterfly mispricing of order 1e-10 of the forward.
* **eSSVI primaries — real but economically negligible.** SOXX `4.41e-5`,
  SMH `5.23e-5`, TQQQ `7.1e-5`, LITE `1.74e-4`, VRTX `2.48e-4`, USO `2.66e-4`,
  ADP `2.89e-4`, COP `4.23e-4`, IBM `4.46e-4`, BABA `9.28e-4`, XLV `1.10e-3`,
  LULU `2.27e-3`. At LITE's `butterfly_k = -0.385417`,
  `slack / dK = 1.74e-4 / 0.00708 ~= -0.025` of implied risk-neutral density —
  roughly 0.6 % of peak density, negative. Real negative gamma, but worth
  ~1e-6 of the forward in butterfly value.

**Correction to `docs/LEDGER.md:154`.** The `slopes=-0.98..-0.999` values are
**not** the Lee wing bound. `first_butterfly_slope_left/right` are `dC/dK` of
the normalised call price (`risk_surface_validation.cpp:346-349`); `dC/dK -> -1`
is the deep-in-the-money limit. The paired reading `slopes=-0.000108/-0.000112`
(AMD) is the deep-out-of-the-money limit of the same quantity. The Lee bound
lives in `max_abs_wing_total_variance_slope = 2.0` and fires
`ValidationFailure::Wing` (bit 64), which is set on **none** of the 25.

### 1.4 The violations sit in extrapolated territory

`FitQualityMode::Balanced` certifies `k` in `[-0.50, +0.50]` on a 97-point grid
(`pricer_fitter.cpp:2104-2109`). Measured two-sided quote coverage on
2026-08-21, per underlier, as the fraction of expiries whose two-sided quotes
actually reach `|k| = 0.50` and `0.35`:

| symbol | expiries | cover abs(k)<=0.50 | cover abs(k)<=0.35 | first violation k |
|---|---|---|---|---|
| XLV | 14 | **0.0 %** | 7.1 % | butterfly -0.479, calendar +0.469 |
| COP | 18 | **0.0 %** | 66.7 % | butterfly -0.469 |
| VRTX | 15 | **0.0 %** | 26.7 % | butterfly -0.458 |
| QQQ | 32 | **0.0 %** | 40.6 % | butterfly +0.369 |
| ADP | 17 | 11.8 % | 58.8 % | butterfly -0.479 |
| TQQQ | 16 | 25.0 % | 43.8 % | butterfly -0.458 |
| SPY (passes) | 33 | 24.2 % | 24.2 % | — |
| SMH | 27 | 51.9 % | 70.4 % | butterfly +0.344, calendar +0.453 |
| IBM | 20 | 65.0 % | 80.0 % | butterfly -0.240, calendar +0.500 |

**All five calendar violations** (AMD -0.375, IBM +0.500, IWM +0.500,
SMH +0.453, XLV +0.469) and **10 of the 16 butterfly violations** are at
`|k| > 0.35`, i.e. in the region the oracle certifies but the board does not
quote. The oracle is adjudicating no-arbitrage on extrapolation.

### 1.5 The fallback machinery is genuinely exhausted

`PricerFitter::fit` already runs (a) a family-substitution ladder
(`pricer_fitter.cpp:1831-1913`) and (b) up to three rounds of strict ConvexDense
recovery with the oracle's exact grid at 0.1x its tolerance
(`pricer_fitter.cpp:1933-2024`, `src/fitting/convex_recovery.cpp`). Every rung
of every failing board is recorded in `ua-prod-attempts.csv`. Representative:

```
SMH   0 essvi         mask 2096  butterfly=3  slack 5.23e-5
SMH   1 svi           mask 2080  calendar=24
SMH   2 convex-dense  mask 2064  butterfly=2  slack 1.34e-8
SMH   3 convex-dense  mask 2064  butterfly=2  slack 1.34e-8
SMH   4 convex-dense  mask 2064  butterfly=4  slack 2.78e-8
SMH   5 convex-dense  mask 2064  butterfly=8  slack 4.28e-8
```

Four distinct curve families and three repair rounds, and the strict-repair
rounds *increase* the violation count. Each family fails a different constraint —
SVI fails calendar (16-181 violations), ConvexDense fails butterfly at 1e-8
scale — which is the signature of a tolerance-scale problem, not a data problem.
The strict-recovery rung is also structurally barred from 9 of the 25 (masks
2176/2192 carry `InversionResidual`, and mask 2049 carries bit 0;
`should_attempt_strict_recovery` admits only Butterfly|Calendar|CarryGap,
`src/fitting/convex_recovery.cpp:9-15`).

`SurfaceFallback::LastKnownGood` cannot help either: `fit_board` constructs a
fresh `PricerFitter` per board (`src/marketdata/corpus_board_fit.cpp:295`), so
`last_admitted_generation == 0` and the LKG arm of
`decide_risk_surface_admission` is unreachable in populate.

### 1.6 The inversion certificate binds on the thinnest boards

`inversion_certified` requires **every** fitted slice to be certified
(`src/fitting/session.cpp:632-640`), and per slice `deam_inversion_certified`
(`src/fitting/calib.cpp:2008-2032`) fails when the de-Am node drop **fraction**
exceeds `max_certified_deam_drop_fraction = 0.10`
(`include/atx/vol/api/fitting/calib.hpp:344`). The predicate is a fraction with
no absolute floor, so 2 dropped rows out of 12 sinks a slice while 2 out of 60
does not — and one bad slice sinks the board. The 9 inversion failures are
almost exactly the thin end of the sample:

```
n_slices (mark run):  SIVR 1, MAR 6, VRTX 8, EWZ 9, ADP 9, RUT 10, W 12,
                      XLV 13, LULU 14, TQQQ 14, COP 15, LITE 16, IBM 18, ...
inversion=failed:     SIVR, MAR, VRTX, EWZ, ADP, W, TQQQ, COP, LITE
                      -> the 9 failures are 9 of the 12 thinnest boards
```

`inversion_certified` is only reachable at all because `apply_risk_policy` sets
`in.deam.audit_fit_inversions = true` (`pricer_fitter.cpp:1484`); the mark path
never audits, never certifies, and is not gated on it.

### 1.7 RUT is not a European/American routing problem

The cash-settled-index hypothesis (`docs/LEDGER.md:103`, `:139`) does **not**
explain RUT. RUT's mask is `16` — butterfly only, `carry=ok inversion=ok`,
`butterfly_k = +0.031250` (at the money), slack `3.46e-8` on a ConvexDense fit.
An early-exercise convention error biases the IV *level*; it does not produce a
3e-8 convexity blip at the money. Routing European index options through the
American map remains a real, separately-documented defect — it is just not the
cause of this rejection.

---

## 2. Mechanism 2 — the carry-confidence gate (long tail)

At full universe the picture inverts. Of 2,431 failed cells in `full.csv`:

```
1,267  "fit_curve_surface: no expiry produced a usable slice"        (kind=svi, prep=configured)
  862  "run_surface_parity: no expiry produced a usable eSSVI slice" (prep=permissive)
  302  "risk surface rejected: ..."
```

Summing the per-chain cause counters embedded in those 2,129 messages:

```
carry_failed       11,190 chains  (99.7 %)   2,128 of 2,129 cells have >=1
starved                20 chains  ( 0.2 %)      18 cells
fit_failed             17 chains  ( 0.2 %)       7 cells
uncovered / prep_failed / calendar_refused / skipped:  0
```

So the tail is **one** mechanism: the carry solve fails on essentially every
expiry of the board.

### 2.1 The gate

`apply_risk_policy` arms `in.deam.require_carry_confidence = true`
(`pricer_fitter.cpp:1484`). Under it, an expiry whose carry is not confident is
deferred to a phase-1.5 term-structure repair rather than used
(`src/fitting/curve_fit.cpp:652-690`); a board with **no** confident expiry has
no anchor for that repair, so every chain is finally stamped `CarryFailed`
(`curve_fit.cpp:918-920`).

Confidence is (`src/fitting/deamer.cpp:837-839`):

```cpp
diag.confident = diag.n_retained >= opts.min_confident_borrow_pairs        // 3
              && diag.dispersion <= opts.max_carry_dispersion              // 0.02
              && diag.max_leave_one_out_shift <= opts.max_carry_leave_one_out; // 0.005
```

Defaults at `include/atx/vol/api/fitting/deamer.hpp:412-417` (`n_atm = 3`,
`max_borrow_pairs = 5`, `min_confident_borrow_pairs = 3`,
`carry_atm_band = 0.06`). A *pair* is one strike carrying a two-sided, unflagged
call **and** a two-sided, unflagged put (`deamer.cpp:426-437`, `:543-580`); with
none, the solve returns `Unavailable, "no near-ATM co-terminal pair for borrow"`
(`deamer.cpp:806-808`).

### 2.2 The measurement

Best-expiry count of two-sided co-terminal pairs, 2026-08-21, measured directly
from the hive parquet:

| pairs on best expiry | carry-dead cells (n=2,129) | fit-ok cells (n=2,815) |
|---|---|---|
| 0 | 0 (0.0 %) | 0 (0.0 %) |
| 1-2 | **938 (44.1 %)** | **0 (0.0 %)** |
| 3-5 | 705 (33.1 %) | 120 (4.3 %) |
| 6+ | 486 (22.8 %) | 2,695 (95.7 %) |
| median | **3** | **21** |

The separation at the `min_confident_borrow_pairs = 3` boundary is clean: no
successful board has fewer than 3 pairs on its best expiry, and 44 % of dead
boards have fewer than 3 on *every* expiry. Of the 705 dead boards in the 3-5
bucket the count is raw, whereas `n_retained` is post-trim, and the +/-0.06 ATM
band holds only 1-2 of them on nearly every expiry (per-expiry detail in a
40-name random sample: `v3/b1`, `v6/b1`, `v10/b1` are typical, where `v` is
valid pairs on the chain and `b` is pairs inside the band).

### 2.3 Missing dividends are not the cause

`cash_divs` enters only the **starting point** of the fixed point —
`hybrid_forward_base(S, r, T, cash_divs, ...)` at `deamer.cpp:788-789`. The
fixed point then implies `borrow` from put-call parity and rebuilds
`F = hybrid_forward_from_base(forward_base, borrow, T)` (`deamer.cpp:846`). An
un-modelled discrete dividend is therefore **absorbed into the implied borrow
level**, uniformly across every pair of that expiry.

All three confidence predicates are **level-invariant**: a count
(`n_retained`), a spread (`dispersion`), and a jackknife spread
(`max_leave_one_out_shift`). None bounds the borrow *level*. There is no
borrow-magnitude clamp anywhere on the fit path (`kBorrowMaxIter` at
`deamer.cpp:64` is a loop bound, not a value bound), and `HtbDetector`
(`include/atx/vol/api/pricing/rates_curve.hpp:247`) has exactly one caller,
`ForwardCurve::detect_htb` (`src/pricing/rates_curve.cpp:210`), which nothing in
the fitting path invokes. **A missing dividend schedule cannot fail this gate.**

This does not make the missing dividend file harmless — it biases the American
early-exercise premium decomposition, exactly as `docs/LEDGER.md:144` already
records — it just is not what is failing here.

### 2.4 Why the liquid ETFs are not a counterexample

QQQ, IWM, SMH, AMD, USO, RUT, IBM and BABA are liquid and carry `carry=failed`
in their message — but they are in **Mechanism 1**, not Mechanism 2. Their
`mask` is 2064/2096/16, i.e. butterfly/calendar; the `carry=failed` term on
those lines is the `CarryGap` diagnostic of §0, which a *published* board
carries too (SPY). Mechanism 2 is defined by the `"no expiry produced a usable
slice"` message, which none of those names emits. The tail carry mechanism is
thin-book-specific; the liquid failures are a different mechanism. The two do
not conflict.

---

## 3. Mechanism 3 — the 943 load errors are a spot problem

`n_load_errors 943`. These never reach the fitter and appear in neither
`cells_ok` nor `cells_failed`.

Measured against the hive for 2026-08-21:

```
distinct underlyings in hive                     6,189
roster                                           6,189   (unique)
configured                                       5,246
missing                                            943
  ... present in hive but not configured           943   (100 %)
  ... absent from hive                               0
  ... with an OSI root != `underlying` column        0
  ... with an unparseable OSI symbol                 0
  ... with NO co-terminal pair (both mids > 0)
      on ANY expiry                                942   (99.9 %)
```

So the panel's uid-namespace hard `InvalidArgument`
(`include/atx/vol/api/marketdata/opra_panel.hpp:351-367`) and the row-level OSI
drop rules (`:337-340`) fire on **zero** of the 943. The adjusted-deliverable
roots (`AIV1`, `ANY1`, `QGEN1`, ...) are only 128 of the 943 and are not failing
for that reason — `NOAH1` and `QGEN1` both load and fail later, at carry.

The actual refusal is the spot implication, `opra_panel.hpp:368-371` /
`src/marketdata/opra_panel.cpp:296-297, 394-397`:

```
no strike carries a two-sided call and a two-sided put on any expiry,
so put-call parity cannot imply a spot; pass spot_override
```

**This confirms the coordinator's reading.** `OpraLoadSpec::spot_override` is
the documented escape (`opra_panel.hpp:369`: "spec.spot_override if > 0, else
imply ..."). If 626 of the 943 have an underlier NBBO in
`C:/atx-data/underlier-hive/date=2026-08-21/underlier.parquet`, wiring that feed
into the surface-db build path and setting `spot_override` would let those 626
reach the fitter. Two caveats, neither of which I resolved: (a) a board that
cannot produce one two-sided call/put pair anywhere is very likely to fail
Mechanism 2 immediately afterwards, so the *recovered fit* count will be well
below 626 even though the *load* count is fixed; (b) I did not verify the
`underlier-hive` schema or that `atx-vol-chain-export` and
`atx-vol-surface-db-build` agree on the spot convention.

### `--max-failures` elides the LOG, not the report

`coverage.failed_cells_elided 2399` is printed at
`tools/surface_db_build_main.cpp:440` and governs the `failed_cell` lines on
stdout only. `full.csv` carries **all 2,431** failure rows
(`date,symbol,code,detail` section, lines 5277-7707). No diagnostic was lost;
`--report` is already the complete record.

---

## 4. Does a mark-quality surface exist? — **Yes, and it is never built**

This is the decisive question for the emitter, and it has a clean answer.

### 4.1 Today the populate path asks for a risk surface only

`symbol_config_from_preset(FitPreset::Populate)` maps through
`map_legacy_fit_preset` to `{Balanced, SurfacePurpose::Risk}`
(`include/atx/vol/api/fitting/surface_policy.hpp:222-230`) and therefore sets
`surface_policy.outputs = SurfaceOutputs::Risk`
(`src/storage/surface_db.cpp:521-523`). `pricer_config_for_symbol` copies it
verbatim (`src/storage/surface_db_populate.cpp:82`). Confirmed at runtime — the
harness prints the resolved contract:

```
[fit-path] production  qm=balanced,out=risk,ra=required,fb=lkg,floor=populate,knobs=on
```

Consequently `PricerFitter::fit` never enters the mark arm
(`pricer_fitter.cpp:1365-1366`: `if (has_output(requested_outputs,
SurfacePurpose::MarketMark))`), `mark_future` is never engaged, and
`finalize_mark()` returns `Ok()` immediately. **No mark surface is produced on
the populate path at all.**

Had the contract been `MarketMarkAndRisk`, the mark *would* be built and
published — `finalize_mark()` runs before the risk `Err` return at
`pricer_fitter.cpp:2029`, and its comment says so explicitly ("Mark publication
is independent of risk admission") — but `fit_board` would still discard it: it
returns on `!st` at `src/marketdata/corpus_board_fit.cpp:296-305` and never
calls `PricerFitter::market_mark_surface()`. So there are two separate cuts to
make, and both must be made.

### 4.2 The same boards fit as marks

Same date, same boards, same `populate` preset, same 0.35 publish floor, only
`--outputs mark --risk-admission na`:

| run | ok | failed |
|---|---|---|
| production (`out=risk`) | 6 | 20 |
| **mark-only (`out=mark`, floor still on)** | **25** | **1** |

Per-board worst-slice in-band fraction on the mark run (`worst_in_band`, the
same quantity the 0.35 floor reads):

```
AMD 1.000  MSFT 1.000  GOOGL 1.000  SOXX 1.000  USO 1.000  SMH 1.000
LITE 1.000 RUT 1.000   EWZ 1.000   ADP 1.000   W 1.000     MAR 1.000
SIVR 1.000 TQQQ 1.000  VRTX 1.000  BABA 1.000  LULU 1.000
IWM 0.896  COP 0.929   XLV 0.915   IBM 0.875   AMZN 0.643
QQQ 0.563  AAPL 0.500  SPY 0.417
LQD  --  (the only failure: QualityBelowFloor even as a mark)
```

Every one is `mm_state = healthy`. Note MSFT and GOOGL: **1.000 in-band as a
mark**, versus `QualityBelowFloor` (< 0.35 on at least one slice) as a risk
candidate. The floor refusal is a property of the *risk* fit (eSSVI at Robust
budget with `CalendarRepair::Project` and audited inversions), not of the board.

### 4.3 Even the rejected risk candidate mostly meets the mark contract

For **20 of the 25** tier-250 failures the rejection line reads
`admission=ok admission_failed=none`. That verdict is `evaluate_surface_admission`
run with `populate_admission_policy()` — the WP12 Mark-serving contract plus the
0.35 floor (`src/storage/surface_db_populate.cpp:1432-1436`). So the surface
being thrown away already satisfies the publish contract a mark consumer is held
to; only `RiskAdmission::Required` refuses it.

**Bottom line for the emitter: fair values and greeks are recoverable for
essentially every one of these names today. The work is a contract change on the
populate path, not a fitting improvement.**

---

## 5. Ranked candidate fixes

Recovery estimates are against the 26-name probe set (20 of the 25 tier-250
failures reproduce in it) unless stated. **None of these is implemented.**

### R1 — Request and persist a MARK surface alongside the risk surface
**Evidence:** §4. 25/26 healthy marks including every risk-refused name.
**Recovers:** ~24 of 25 tier-250 failures for *mark-grade* use (fair value and
greeks). Does **not** produce a risk-admitted surface.
**Shape:** set `surface_policy.outputs = MarketMarkAndRisk` for the populate
tier, and teach `fit_board` to keep `PricerFitter::market_mark_surface()` when
`fit()` returns the risk `Err` (`corpus_board_fit.cpp:296-305`).
**Risk:** the mark is a pinned `LinearVariance`/Hft fit — market-following and
*not* arbitrage-free. It must be stored with a purpose tag so nothing reads it
as a risk surface. The storage layer already models this (`SurfacePurpose`,
`SurfaceProvenance`).
**Cost:** one extra `VolaSession::build` per board, run concurrently
(`pricer_fitter.cpp:1376`). ~0.9 s/board median in the debug harness.

### R2 — Certify the risk band the board actually quotes
**Evidence:** §1.4. All 5 calendar violations and 10/16 butterfly violations sit
outside `|k| = 0.35`; XLV/COP/VRTX/QQQ have **zero** expiries whose two-sided
quotes reach `|k| = 0.50`. Measured A/B at `qm=latency` (band +/-0.35, 65
points): **19 ok vs 6 baseline, +13 recovered**, -2 regressions (SPY, AMD).
**Recovers:** ~13 of 20 in-probe, i.e. roughly 16 of the 25.
**Shape:** derive `RiskSurfaceValidationConfig::k_min/k_max` from the board's own
measured quote coverage rather than from `FitQualityMode`, and record the
certified band in provenance so a consumer knows where the guarantee stops.
**Risk:** this narrows a real guarantee — it must be recorded, not silently
shrunk. The +/-0.35 A/B is confounded with `FitPreset::Fast`, so the +13 is
directional, not the exact yield of a band-only change.

### R3 — Give the butterfly test a scale-relative tolerance
**Evidence:** §1.3. ConvexDense strict-recovery rungs fail at 1.34e-8-4.72e-7
against a 1e-8 absolute tolerance, after three repair rounds explicitly designed
to cure this class (`convex_recovery.hpp:1-10` names the same root cause from
the 2026-08 SPY backfill). RUT's entire rejection is a 3.46e-8 slack at the
money.
**Recovers:** RUT outright; removes the ConvexDense rung failure for
QQQ/SMH/USO/XLV/SOXX/IBM, which is what blocks the ladder from adopting a
substitute. Estimated 6-11 of the 25.
**Shape:** the current test compares a second difference of a price to a fixed
1e-8. A relative form — scaled by local price magnitude and by `dK` — would
separate a 1e-8 solver artefact from LITE's 1.74e-4 real negative density.
**Risk:** this loosens a correctness gate. It needs a measured floor derived
from `ConvexSliceFit::iv`'s bisection bracket
(`src/fitting/dense_slice.cpp:496-521`, `1e-12 * max(1, hi)`) and the
`safe_call_price` projection (`src/fitting/dense_slice_price.hpp:37-55`), not a
guessed constant. **I did not derive that floor — see §6.**

### R4 — Stop the publish floor from vetoing fallback rungs
**Evidence:** measured A/B with `--publish-floor off`: **13 ok vs 6 baseline,
+7 recovered.** Only 4 of those 7 are the QualityBelowFloor group (AMZN, GOOGL,
MSFT, LQD); the other 3 (LITE, SOXX, LULU) recovered because a *ladder rung*
that had been refused `QualityBelowFloor` became adoptable. The attempts CSV
shows the veto directly — e.g. `SOXX 2 convex-dense ... QualityBelowFloor`,
`LULU 2/3 convex-dense ... QualityBelowFloor`.
**Recovers:** ~7 of 20 in-probe, ~9 of 25.
**Shape:** apply the publish floor to the *published* candidate only, not to
every ladder rung's admissibility; or make it a demote-to-Degraded reason rather
than a rejection, the way `CarryGap` already is.
**Risk:** the floor exists to block the demonstrated 2025-04-10 publish
(`surface_db_populate.hpp:66-73`). Removing it wholesale is not acceptable;
scoping it to publication is the narrower change.

### R5 — Make the de-Am certificate robust on thin slices
**Evidence:** §1.6. `deam_inversion_certified` is a fraction with no absolute
floor, and `inversion_certified` is an all-slices AND. The 9 inversion failures
are 9 of the 12 thinnest boards.
**Recovers:** up to 9 of 25 (4 of which — EWZ, MAR, SIVR, W — fail on *nothing
else*, mask 2176).
**Shape:** add an absolute drop floor beside the 10 % fraction, or let a small
number of uncertified slices demote to Degraded (a `CarryGap`-style
publish-with-reason) instead of failing the board.
**Risk:** this is a §8.1 correctness certificate. Weakening it needs the charter
owner, not a fitting-side decision.

### R6 — Supply `spot_override` from the underlier hive
**Evidence:** §3. 942 of 943 load errors are the PCP spot implication.
**Recovers:** up to 943 *loads*; far fewer *fits* — a board with no two-sided
co-terminal pair anywhere will very likely die at Mechanism 2 immediately after.
Contract-weighted this is 1.7 % of the board.
**Shape:** wire the `underlier-hive` feed `atx-vol-chain-export` already reads
into the surface-db build path and set `OpraLoadSpec::spot_override`.
**Risk:** low, but the spot convention must match; an implied-spot mismatch
would silently move every forward.

### R7 — Relax the carry gate for thin boards (tail only)
**Evidence:** §2.2. 44 % of carry-dead boards never reach
`min_confident_borrow_pairs = 3`; the fit-ok population has a median of 21.
**Recovers:** unknown — a 1-pair borrow is genuinely uncertain, and the correct
answer may be to publish it Degraded rather than to call it confident. The
machinery for that already exists (`CarrySource::MoneynessBounded`, second-tier
anchors, `max_carry_moneyness_shift`, `deamer.hpp:419-434`).
**Risk:** high. This is the gate that stops the library serving a fabricated
forward. Do not touch it before R1/R2/R4, which recover the *valuable* names.

### Not recommended
* **`ATX_VOL_FIT_LINEAR_FALLBACK` / `ATX_VOL_FIT_UNCOVERED_PARAMETRIC`.** Both
  target *dropped slices*, which surface as `CarryGap` — the one bit that
  publishes. They explain **0** of the 25 and cannot recover any of them. The
  linear-fallback gate additionally only reaches the polymorphic-driver lane, so
  an eSSVI-served board is unaffected however it is set
  (`pricer_fitter.cpp:226-250`, working-tree addition). This also explains why
  `rem-baseline.csv`, `rem-linear.csv`, `rem-uncovered.csv` and `rem-both.csv`
  are byte-identical (md5 `5cdf072e50536d3d2a67cc019329a623`) — the `build-rel`
  binary is from 2026-08-16 and contains none of the three env strings, so those
  A/Bs never armed anything.
* **A dividend schedule, as a fix for fit coverage.** §2.3. Worth having for the
  American premium decomposition; recovers no cell here.

---

## 6. What I could not determine

1. **The exact numerical floor of the oracle's butterfly stencil.** I showed
   empirically that ConvexDense rungs fail at 1.3e-8-4.7e-7, and identified the
   two candidate amplifiers (the `iv()` bisection bracket `1e-12*max(1,hi)` at
   `dense_slice.cpp:517`, and the near-coincident samples the node-union grid can
   produce at `risk_surface_validation.cpp:80-86`, deduplicated only at 1e-9
   relative). I did **not** derive or measure the resulting slope-noise
   distribution, so R3's tolerance form is not yet specified.
2. **Whether a mark surface would pass the 0.35 floor for LQD.** It is the one
   board that fails as a mark. Not investigated.
3. **The exact yield of R2 as a band-only change.** The +/-0.35 A/B also switched
   `FitPreset` to `Fast`; +13/-2 is directional. Isolating the band needs a
   `quality_mode`-only knob, which no CLI exposes today.
4. **Whether the 626 underlier-hive quotes actually rescue fits.** §3, caveat
   (a). I verified the *cause* of the load errors, not the yield of the fix.
5. **Build-to-build stability of the marginal cases.** EWZ fails in the
   2026-08-16 release report (`t-250.csv`) and *passes* in the fresh debug
   surface-db-build run with the same inputs; QQQ's `butterfly_slice` moves
   28 -> 29. Cells whose slack sits within ~5x of the tolerance are not stable
   across builds. I did not quantify how many of the 25 are in that regime.
6. **Why AAPL/MSFT/GOOGL pass on 2026-08-11 and fail on 2026-08-21.** The
   populate-path family selection differs between the two dates
   (`curve_fit.cpp:1166-1182` documents the underlying Fitted/Missing
   instability), but I did not run the 08-11 comparison under this harness.
7. **Whether `universe_autofit` is a faithful populate mirror for family
   selection.** It reproduces 20 of the 25 rejection strings byte-for-byte, but
   AAPL/AMD/IWM/BABA/W route to ConvexDense there and pass, where populate routes
   them to eSSVI and fails. The harness deliberately omits `fit_board`'s
   `session_overlay` (`apply_symbol_config`: `band_k`, `calendar_repair`) and the
   index pin (`examples/universe_autofit.cpp:356-399`). All A/B deltas in §5 are
   within-harness and therefore sound; the absolute counts are not populate's.

---

## 7. Reproduction

```powershell
# Reproduce the top-of-book failures (fresh debug binary; ~3 min for 7 symbols)
build\bin\atx-vol-surface-db-build.exe --db <tmp> --hive C:/atx-data/opra-all `
  --from 2026-08-21 --to 2026-08-21 --symbols SPY,AAPL,QQQ,AMD,EWZ,LITE,RUT `
  --index SPY --preset populate --r 0.043000 --snapshot-suffix T19:55:00Z --report base.csv
# -> 2 ok / 5 failed; AAPL 2049, AMD 2096, LITE 2192, QQQ 2064, RUT 16

# Gate attribution A/B (universe_autofit; needs a {symbol}/{date}.parquet split
# of the hive -- filter the date file by the `underlying` column, one dir per symbol)
build\bin\universe_autofit.exe --opra-root <perseq> --date 2026-08-21 `
  --symbols-file syms.txt --snapshot-suffix T19:55:00Z --r 0.043 --preset populate `
  --fit-path production --no-value --out ua-prod.csv --attempts-out ua-prod-attempts.csv
#   baseline                                      ->  6 ok / 20 fail
#   ... --outputs mark --risk-admission na        -> 25 ok /  1 fail
#   ... --publish-floor off                       -> 13 ok / 13 fail
#   ... --preset fast            (band +/-0.35)   -> 19 ok /  7 fail

# Stale-binary check (always run before trusting an env-var A/B)
strings build-rel/bin/atx-vol-surface-db-build.exe | grep ATX_VOL_FIT_   # -> nothing
strings build/bin/atx-vol-surface-db-build.exe     | grep ATX_VOL_FIT_   # -> both gates
```

Artefacts (session scratchpad `.../6315bf90-f92d-40a2-b76e-726fef3e975e/scratchpad/`):
`dbprobe/t-50.csv`, `dbprobe/t-250.csv`, `dbprobe/full.csv`,
`diag/full_failures.txt`, `diag/carrydead_all.txt`, `diag/missing_symbols.txt`,
`diag/ua-prod.csv`, `diag/ua-prod-attempts.csv`, `diag/ua-mark.csv`,
`diag/ua-nofloor.csv`, `diag/ua-latency.csv`.
