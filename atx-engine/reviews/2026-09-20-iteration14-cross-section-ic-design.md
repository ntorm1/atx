# Iteration 14: cross-sectional forecast evaluation on the identified real panel

Design only. No implementation, build, test or native run was performed for this note. All
paths are relative to the isolated worktree `C:\atx\.worktrees\equity-platform`, branch
`feat/equity-platform-20260920`. Authored by a read-only design subagent.

Adopted checkpoint: `D-1` of
`.superpowers/sdd/equity-platform-parent-goal/research-alpha-pipeline.md` (§D-1, §E), Stage 1
only.

**Revision 4, 2026-09-20.** Revised in place three times: after the independent design review
(`cp14-design-review.md`, **Fix required**: 8 Critical, 12 Important, 15 Minor); after the
scoped re-review of Revision 2 (`cp14-design-rereview.md`, **Fix required narrowly**: all 20
prior findings resolved, 1 new Critical, 3 Important, 5 Minor); and after re-review round 2
(`cp14-design-rereview2.md`, **passed**, 4 Minors + 1 informational). All under
`.superpowers/sdd/equity-platform-parent-goal/`. §11.1 carries the original rulings
R-1..R-8 / OQ-1..OQ-4; **§11.2 carries AR-1..AR-10; §11.3 carries AR-11 / R-A / R-B / R-C and
five Minors; §11.4 carries NEW-1..NEW-4 and the PCS-membership runtime check; §11.5 carries
RR-1; §11.6 carries A-1..A-9 from the T7a lane. All six are binding.** Everything in this
document is pre-run: no native
command has been executed for checkpoint 14, which is why the priced book size (§3.12), the
block length (§4.3), the terminal-evidence partition (§2.3), the common-sample definition
(§3.13) and the bootstrap stream key (§3.10) could all still be corrected without opening a new
trial.

---

## 1. Purpose and non-goals

### 1.1 Purpose

Produce the first forecast-quality measurement in this program's history: per-date
cross-sectional Pearson and Spearman information coefficients, an IC decay curve over a
pre-registered horizon set, ICIR with circular-block-bootstrap intervals, signal
autocorrelation and implied turnover, decile spreads gross and net of the frozen cost
convention, and a per-date coverage table — computed on the already-identified, already
independently verified 2013 evaluation slice.

The gap this closes is stated by the repository in its own comments. `eval/breadth.hpp:71`
takes `ic` as a caller-supplied parameter; `combine/combiner.hpp:39-47` records that the
IC-from-signal form "is not computable here"; and the research inventory finds "**zero IC of
any kind in `atx-impl/src`**" and "**no decile or quantile spread anywhere**"
(research §A row 5, §C-1). Thirteen checkpoints have made the accounting around two frozen
momentum expressions exact to 1e-19 without ever estimating whether those expressions predict
anything (research §C-1).

### 1.2 Scope, frozen

- **Stage 1 only.** The existing frozen 2013 context
  `C:/atx/data/tickerhistory_training_native_20260919/context.bin` (445 x 1,661 x 12, context
  ID `ec572b82…`), evaluation window 2013-04-04..2013-12-31, **189 evaluation observations**
  (handoff "Frozen research context"; research §B.2). **Zero new data preparation.**
- One new pure engine unit: `atx-engine/include/atx/engine/eval/cross_section_ic.hpp` +
  `atx-engine/src/eval/cross_section_ic.cpp` + one test file.
- One new `atx-impl` subcommand, `equity-ic`, reusing `evaluate_equity_baseline` **unchanged**.
- One new `atx-impl` unit: `trial_ledger.{hpp,cpp}` (append-only, hash-chained), writing
  `atx-engine/reviews/trial-ledger.jsonl`.
- Structural exclusion of the 2023-01-01..2025-12-31 sealed period **by code**.
- Every statistic reported four ways — full sample and common sample (§3.13, ruling AR-4), each
  unrestricted and restricted to exclude the 34 audited-terminal-or-gap IDs (§3.8, ruling
  AR-7). These are additional statistics of the same 30 configurations, not additional trials.

### 1.3 Non-goals — explicitly out of scope

1. **Stage 2 (the 2013-04-04..2019-12-31 panel).** Research §D-1 "Window, in two stages — do
   not conflate them": the full training block needs a new bounded panel build; the 445-date
   context measured 1,026,482,176 bytes peak working set against a conservative
   2,775,826,250-byte admission score and a 3 GB budget, and ~1,950 dates is roughly 4.4x
   (`reviews/2026-09-19-annual-panel-memory-review.md:92-97,47-50`). Follow-on checkpoint.
2. **Any fitting.** No `fit_linear`, `fit_gbt`, `elastic_net`, PCA, interaction selection or
   ensemble is wired. Research §C-9 and `reviews/2026-09-19-learning-availability-audit.md:8-22`
   name two unfixed leakage defects; wiring `learn/` to the equity path before they are fixed
   would import leakage into the only clean pipeline.
3. **Any change to the book, the allocator or the replay.** No edit is planned to
   `atx-engine/include/atx/engine/book/replay.hpp`, `atx-engine/src/book/replay.cpp`,
   `atx-engine/tests/book/book_claims_replay_test.cpp`, `atx-engine/src/book/claims_state.cpp`
   (checkpoint 13 in flight), nor to `atx-impl/src/equity_allocation.cpp` or
   `atx-impl/src/stage_equity_book.cpp` (reserved for the later cadence checkpoint, research
   §D-3).
4. **Cadence, hysteresis or turnover-budget selection.** Research §D-3; it consumes this
   checkpoint's decay curve and is a separate trial family.
5. **Net P&L, Sharpe, capacity or any investment claim.** Section 12 states the bound.
6. **No live trading, no broker action, no order.** Handoff, "Goal and working priorities".

---

## 2. Inputs and what is reused unchanged

### 2.1 Reused unchanged — no edit planned to any file in this table

| Artifact | Location | What is reused |
| --- | --- | --- |
| Frozen signal DSLs | `atx-impl/src/equity_baseline_views.hpp:15-19` | The two hard-coded momentum expressions, verbatim |
| Signal names | `atx-impl/src/equity_baseline_views.hpp:18-19` | `momentum_252`, `momentum_126` |
| Warmup constant | `atx-impl/src/equity_baseline_views.hpp:14` | `kEquityBaselineWarmup = 256` |
| Evaluation view builder | `atx-impl/src/equity_baseline_views.hpp:90` `evaluate_equity_baseline` | Called unchanged; returns the NaN-gated, readiness-gated, eligibility-gated `alpha::Panel` + `SignalSet` + `session_keys` + `context_rows` |
| Plan/budget | `atx-impl/src/equity_baseline_views.hpp:77` `plan_equity_baseline` | Called unchanged for the memory-admission estimate |
| Panel fields | `atx-impl/src/equity_baseline_views.cpp:27` `kFields{"close","raw_close","volume"}` | `close` is the price series for forward returns |
| Rank transform | `atx-core/include/atx/core/stats/cross_section.hpp:102` (scratch overload) | Ties averaged, normalized [0,1]; the no-allocation form |
| Pearson | `atx-engine/include/atx/engine/learn/latent.hpp:151` `detail::pearson` | Definition restated in §3.4; the engine unit gets its own copy in `eval::` (see ruling R-6) |
| IC walk shape | `atx-engine/include/atx/engine/learn/linear_alpha.hpp:139` / `atx-engine/src/learn/linear_alpha.cpp:119-147` `detail::oof_ic_series` | Generalized: single forward walk over ascending dates, `>= 2` covered rows, no map |
| Lockbox | `atx-engine/include/atx/engine/eval/lockbox.hpp:72` `SealedReservation`, `:209` `SealedPanel`, `:243` `field_cross_section_or_trap`, `:268` `reserve_lockbox`, `:346` `reserve_window` | The structural seal (§5) |
| Identity binding pattern | `atx-impl/src/stage_equity_book.cpp:187-194` | `require_panel_parent` / `require_same_panel_axes` calls, mirrored |
| Publication pattern | `atx-impl/src/stage_equity_baseline.cpp:352-369` (`write_text`, hard-link publish), `:41-45` (`number()` via `std::to_chars`) | Copied idiom, not a shared helper (both are TU-local statics) |
| Digest line | `atx-impl/src/dispatch.cpp:35-44` `emit_digest_line` | Called unchanged |
| RNG | `atx-core/include/atx/core/random.hpp:83` `Xoshiro256pp`, `:57` `splitmix64_next` | Bootstrap resampling, fixed seed |
| Error model | `atx-core/include/atx/core/error.hpp:29-54` `ErrorCode`, `Result`, `Status`, `ATX_TRY` | Exactly as `book/` and `eval/` use it |

### 2.2 Inputs the run consumes

```text
--panel        C:/atx/data/tickerhistory_training_native_20260919/context.bin
               context ID  ec572b826dce65fbd4cd391921f57f01e1ce084864ec84b3e6a944003ead8079
               payload SHA c219dc237a58574e4b55df8ee51e9f239ef9d5963903e54aa253bd07eafb85ef
--baseline-dir C:/atx/data/equity_baseline_training_2013_20260919
               evaluation ID 57b7c21c9320e98c1dee6c97e5f3b8cde211c5efdd688c2daf6fb28daa3cd475
               combo ID      def31e5ca03dc2329bb05a2526c4470e77028fea060d0c3743634d6b4f86a692
--out          a fresh directory (must not exist)
```

(All four digests from the handoff, "Frozen research context" and "Baseline intermediates used
by `equity-book`".)

`--baseline-dir` is required, not optional: it supplies the **deployed** equal blend as a
published, identity-bound artifact (`combo.bin`) rather than a re-derivation. The stage
therefore evaluates the exact preference stream `equity-book` consumes
(`atx-impl/src/stage_equity_book.cpp:374`), with no second implementation of the blend.

### 2.3 Terminal-coverage evidence input

`atx-engine/reviews/2026-09-20-equity-required-marks-audit.md` supplies the only admitted
evidence: 110 required date/ID cells across 34 IDs, 104 absent from the archive, 6 quarantined
(`:5`). Its evidence table names **exactly three** 2013 events with consideration amounts,
which it labels a **terminal-cash-event hypothesis** subject to validated instrument mapping —
not a settled classification (`:13-15`):

| Ticker | ID | Last observation | Last close (raw) | Consideration | Special dividend |
| --- | --- | --- | --- | --- | --- |
| HNZ | `37648` | 2013-06-07 | 72.49 | USD 72.50 | — |
| DELL | `35715` | 2013-10-29 | 13.86 | USD 13.75 | USD 0.13, **record date 2013-10-28** |
| MOLX | `39970` | 2013-12-06 | 38.68 | USD 38.68 (incl. a USD 0.18 adjustment to a USD 38.50 base) | — |

Two further gaps are demonstrably **not** terminal — GNW `150340` and MA `351548`, both with an
intervening session (`:19`), which the audit calls an unresolved intervening observation.

**The partition of the 34 required-mark IDs, stated once (ruling R-A).** The audit classifies
**five**; it declines to classify the rest:

```text
34 required-mark IDs
 =  3  terminal-cash-event HYPOTHESIS, with consideration   (HNZ 37648, DELL 35715, MOLX 39970)
 +  2  evidenced NON-terminal, intervening session          (GNW 150340, MA 351548)
 + 29  UNCLASSIFIED — the audit makes no terminal finding about them
```

The audit classifies 110 **cells** as 104 archive-absent and 6 QA-quarantined; it does **not**
classify the remaining 29 IDs as terminal. This design therefore does not either. Cells of the
29 drop into `n_dropped_missing_forward` like any other missing mark, and the `_ex34`
restriction (§3.8, ruling AR-7) still excludes **all 34** regardless of classification.

**PCS is never applied** (ruling AR-1, re-issued under R-A). PCS appears nowhere in the
required-marks audit's evidence table, and the handoff forbids it twice: "Actual PCS historical
admission remains rejected until evidence is sufficient" and "**Do not apply a real PCS event**
until vendor predecessor/successor mapping, TRI component coverage, entitlement basis, evidence
availability, exact boundary, successor marks, fractions, payment allocation and stock-loan
handling are established". The MetroPCS/T-Mobile terms are moreover **stock plus cash** — a
one-for-two reverse split plus USD 4.0491 per pre-split share, continuing as TMUS (handoff,
"Primary research and why PCS is not patched yet") — and §3.8's terminal-leg formula has no
representation for a successor-share leg at all, so applying it would fabricate a cash-only
return for a stock-and-cash transaction. PCS is the one ID for which the stage can cite a
terminal event without admissible consideration, so it is flagged `terminal = 1,
terminal_evidenced = 0` and counted in `n_terminal_unevidenced`.

**`n_terminal_unevidenced` is a MEASURED output, not a pre-registered constant** (ruling R-A).
It is defined as the count of cells the caller flags terminal without evidence, i.e. cells of an
ID that (i) is one of the 34, (ii) is not one of the three evidenced events, and (iii) has a
citable terminal event whose last observed session falls inside the evaluation window. Under
AR-1 that is PCS. **Expected value: a small count of PCS cells; the 29 unclassified IDs are
NOT included**, because nothing licenses flagging them terminal. The design predicts the
partition (3 / 2 / 29), not the cell count — the run measures the count and the receipt records
what was observed, against the derivation above. The stage writes the full partition to
`request.json`.

The audit's own limit is binding on this design: "**neither absence nor the following row
authorizes terminal settlement, a carried price, a zero return**" (`:19`), and "This
establishes the classifications, **not economic correctness or usable replacement prices**"
(`:5`). The engine therefore never derives a terminal return; the caller supplies one per cell
or the cell is counted as unevidenced and dropped (§3.8).

---

## 3. Definitions and formulas

All indices are **panel row indices** (evaluation observations), never calendar days. `I` is
the instrument count (1,661 canonical for this context), `T` the evaluation date count (189).
Storage is date-major: cell `(t, i)` is at flat index `t * I + i`, matching
`atx-engine/include/atx/engine/alpha/panel.hpp:69-71` and `:265-273`.

### 3.1 Admission mask

A cell `(t, i)` is **admitted** iff `panel.in_universe(t, i)` on the Panel returned by
`evaluate_equity_baseline`. That mask is already the intersection of the inherited context
eligibility with common signal readiness, and the signals are NaN-gated against the same gate
(`atx-impl/src/equity_baseline_views.hpp:82-87`). The engine takes the mask as a `u8` span and
does not recompute eligibility.

**Guard (research §D-1, `annual-panel-memory-review.md:60-70`):** `extract_streams` builds a
static full-size `Universe`, so a panel mask alone does not enforce eligibility downstream. The
stage passes the mask explicitly into the engine call; the engine asserts `mask.size() == T*I`
and treats a zero mask byte as exclusion regardless of value finiteness.

### 3.2 Signal values

Three signal series, each a date-major `f64` span of length `T*I`:

| Index | Name | Source |
| --- | --- | --- |
| 0 | `momentum_252` | `EquityBaselineEvaluation::signals` alpha 0 (`equity_baseline_views.hpp:15,18`) |
| 1 | `momentum_126` | `EquityBaselineEvaluation::signals` alpha 1 (`equity_baseline_views.hpp:16,19`) |
| 2 | `blend_equal` | the single `alpha` column of the published `combo.bin` artifact (`stage_equity_baseline.cpp:378-383`) |

Signals 0 and 1 are **raw expression outputs**.

**Signal 2 is a rank-space blend, not a monotone image of a raw average.** The producing code is
`extract_streams(view.signals, atx::engine::WeightPolicy{}, view.panel, frictionless_sim())`
followed by `0.5 * positions(0,d)[i] + 0.5 * positions(1,d)[i]`
(`atx-impl/src/stage_equity_baseline.cpp:341-352`). `extract_streams` applies the
rank / winsorize-0.025 / gross-1.0 policy **per alpha stream**, so

```text
blend_equal_i(t) = 0.5 * g( rank_w(s0(t)) )_i + 0.5 * g( rank_w(s1(t)) )_i
```

where `rank_w` is the per-date rank-then-winsorize transform and `g` the per-date gross
normalization. This is **not** a monotone transform of `0.5*s0 + 0.5*s1`: the two orderings
differ wherever the two signals disagree in rank-versus-level spacing, which is generically
everywhere, not only at winsorization ties. `blend_equal` is therefore a **distinct signal**,
and neither its rank IC nor its Pearson IC is derivable from a raw equal average's. Its decile
buckets also differ.

Two consequences, both reported rather than corrected:

- `g` (per-date gross normalization) is a per-date **affine** transform, so it leaves Pearson
  IC, rank IC and decile ordering unchanged. The confound is the per-alpha **rank** step, not
  the normalization.
- Signal 2's Pearson IC is a correlation between a bounded, near-uniform variable and a
  heavy-tailed return. It is reported beside the rank IC and the confound is carried into §12
  (ruling OQ-1). Per OQ-1 no raw-equal-average fourth signal is added; `N_14` stays 30.

### 3.3 Forward return

`P_i(t)` is the `close` field — `raw_close * cumulReturnFactor`, pointwise, per the context
recipe assertion at `atx-impl/src/stage_equity_baseline.cpp:246` (`"research_ohlc" ==
"raw-OHLC*cumulReturnFactor-pointwise"`).

```text
r_i(t, h) = P_i(t + h) / P_i(t) - 1
```

Defined iff `t + h < T`, `P_i(t)` finite and `> 0`, and `P_i(t+h)` finite and `> 0`. No other
construction is admitted: no log return, no winsorization of the return, no de-meaning, no
excess-over-market, no volatility scaling. A negative or zero price is not repaired; it is a
missing forward return.

**No forward eligibility test.** The name's admission at `t+h` is deliberately **not** required.
Requiring it would condition the date-`t` cross-section on `t+h` universe membership, which is
lookahead of exactly the class `report-holding-period-audit.md:59-67` documents on the factory
path ("applies the **last** book retrospectively to every historical raw-close return").

**Signal/label overlap.** Both expressions are strictly backward-looking at `t`:
`ts_mean(delay(close, 21) / delay(close, 252) - 1, 5)` reads `close` at `t-21` and earlier
(`equity_baseline_views.hpp:15-16`). The signal window and the `(t, t+h]` return window are
disjoint, so there is no same-bar contamination. This is a property of these two expressions
and does not generalize.

**Alignment convention, and its offset from the deployed book.** The return is indexed from
`t` — the date the signal is observed. The deployed profile executes with a **one-observation
delay**: the decision at `t` is executed at `t+1` (handoff, "one-observation execution delay";
`atx-engine/include/atx/engine/book/replay.hpp:21` `execution_delay_periods{1}`). So
`r_i(t,h)` is the return a hypothetical instantaneous executor would earn, and the deployed book
earns approximately `r_i(t+1, h)` over an overlapping but shifted window.

This is a **forecast statistic**, not a book return, and it is not lookahead — the signal and
label windows are disjoint either way. But it means the decay curve is indexed one observation
earlier than the book it is meant to inform, and D-3 (cadence selection) consumes this curve.
The offset is disclosed in §12 and carried in the output schema as
`"alignment": "signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1"`. A second
execution-delayed alignment variant is **not** added: it would be a new trial axis and change
`N_14` (ruling I-3 / AR scope).

### 3.4 Pearson IC

Per `(t, h, signal, variant)`, over the **used set** `U(t,h)` of §3.7:

```text
n = |U|
if (n < 2) return 0.0                                  # the n < 2U early return
ma = mean(x),  mb = mean(r)                            # means accumulated first, then divided
cov = Σ (x_i - ma)(r_i - mb)
va  = Σ (x_i - ma)^2 ,  vb = Σ (r_i - mb)^2
IC  = (va == 0.0 || vb == 0.0) ? 0.0 : cov / sqrt(va * vb)
```

Exactly the definition, the `n < 2U` early return and the constant-series convention of
`atx-engine/include/atx/engine/learn/latent.hpp:151-178`: a degenerate or constant
cross-section returns `0.0`, not NaN. The restatement is complete — `eval::detail::pearson` is
a byte-for-byte algorithmic copy including both guard branches, and §9.1 case 2a pins exact
`f64` equality against `learn::detail::pearson` on a fixed vector (ruling R-6). Accumulation
order is ascending instrument index — fixed, so the result is bit-reproducible run to run.

Emitted only when `|U(t,h)| >= min_names_per_date` (pre-registered 2, matching
`linear_alpha.cpp:141` `pv.size() >= 2U`). Dates below the threshold are counted, not
silently skipped.

### 3.5 Rank (Spearman) IC

```text
rank(x over U) -> rx    via core::stats::rank (cross_section.hpp:102, scratch overload)
rank(r over U) -> rr
rank_IC = pearson(rx, rr)
```

**Tie handling, exact.** `core::stats::rank` assigns each run of bit-equal values the mean of
the positions it occupies, normalized by `n-1` (`cross_section.hpp:123-136`). Ordering inside
the sort is `in[left] < in[right]`, with `left < right` as the total-order tie-break
(`:114-121`) — so the permutation is deterministic, and averaging removes any dependence on it.
`n == 1` yields `0.0` (`:108-111`), which cannot occur here because `min_names_per_date >= 2`.
Equality is **bitwise `==` on `f64`**, not a tolerance: two values differing by one ulp are two
ranks. This matches the same definition `learn/latent.hpp:183-191` uses for `spearman`.

Ranks are computed over `U(t,h)` **only** — never over the full cross-section with missing
cells held out afterwards. Because `U` differs by horizon (forward coverage differs), the rank
transform is recomputed per `(t, h)`.

### 3.6 NaN policy

One rule, applied everywhere: **a non-finite value excludes its cell from the statistic and
increments a named counter. Nothing is imputed, forward-filled, zero-filled, carried or
winsorized into range.** `0.0` is never used as a stand-in for "missing"; it is a legitimate
signal value. `equity_curve`'s missing-return-to-zero behaviour, pinned by the `NoSurvivorship`
test, is explicitly **not** adopted here — `report-holding-period-audit.md:31` records that
"the name does not establish a valid delisting policy."

### 3.7 Coverage counters, per `(t, h, signal, variant)`

| Counter | Definition |
| --- | --- |
| `n_eligible` | `#{ i : mask(t,i) == 1 }` |
| `n_signal_finite` | `#{ i in eligible : isfinite(x_i(t)) }` |
| `n_with_forward` | `#{ i in signal_finite : r_i(t,h) defined per §3.3 }` |
| `n_dropped_missing_forward` | `n_signal_finite - n_with_forward` |
| `n_terminal_applied` | cells where the variant supplied an evidenced terminal return (§3.8) |
| `n_terminal_unevidenced` | cells whose gap is audited-terminal but carries no evidenced consideration (includes PCS, §2.3) — **dropped** |
| `n_excluded_audited` | cells excluded by the `_ex34` restriction (§3.8, ruling AR-7) |
| `n_used` = `|U(t,h)|` | `n_with_forward + n_terminal_applied` |

`coverage.csv` carries every counter for every date. **No IC number is ever reported without
the coverage table beside it** (research §D-1 risk 2).

### 3.8 Forward-return variants — both always computed, never selected between

**Variant A — `DropMissingForward`.** A cell with no `P_i(t+h)` drops. This is the Shumway
selection: a name that vanishes because it terminated loses its final, often large, often
negative return, and the bias is **upward** (research §C-8, §F[9];
`data/universe.hpp` SURVIVORSHIP CAVEAT).

**Variant B — `IncludeAuditedTerminalV1`.** For a cell whose forward window crosses a date/ID
pair classified terminal by `2026-09-20-equity-required-marks-audit.md`, and for which that
audit records an evidenced consideration amount (the **three** IDs of §2.3 — HNZ, DELL, MOLX —
and no others), the forward return is

**Precedence rule — the terminal leg is a fallback, never an overlay (ruling A-6).** The
terminal cash leg is applied **only when the ordinary §3.3 forward return is undefined because
the name has no close at `t+h`**, i.e. its last observed session lies inside `(t, t+h]`. If a
finite positive `P_i(t+h)` exists, the **ordinary** return `P_i(t+h)/P_i(t) - 1` is used, no
terminal leg is applied, and `n_terminal_applied` does not increment — even for a
terminal-flagged, evidenced ID. A terminated name has no later marks; applying a settlement leg
on top of a live price would be economically wrong and would double-count the consideration.
Read literally the formula below would set `t_last = t+h` in that case, which is exactly the
reading this rule forbids. §9.1 case 8c pins it.

Subject to that precedence,

```text
t_last(i, t, h) = max{ u : t < u <= t+h AND P_i(u) finite and > 0 }     # bounded by (t, t+h]
r_i(t, h)       = (P_i(t_last) / P_i(t)) * (1 + terminal_leg_i(t)) - 1
terminal_leg_i(t) = ( consideration_i + special_i(t) ) / raw_close_i(t_last) - 1
special_i(t)      = special_dividend_i   if  session_keys[t] <= record_date_ns_i
                    0.0                  otherwise
```

**`t_last` scope (M-8).** The search is bounded by the half-open forward window `(t, t+h]`, not
by the whole panel. If no valid mark exists in that window the cell has no terminal path either
and drops as an ordinary missing forward return.

**Special-dividend entitlement (ruling AR-2).** The special dividend is included **only** when
the cell's observation date is at or before the event's record date. For DELL the audit states
the USD 0.13 "applied to holders of record at the 2013-10-28 close … **Its entitlement must not
be inferred from holdings at a later missing mark**" (`:13-15`). A cell at `t = 2013-10-29`
buys at that day's close — after the record date — and receives the consideration alone.
`record_date_ns` is a per-event field of the caller-supplied evidence table, carried verbatim in
`request.json`; an event with no record date carries `special_dividend = 0.0` and the condition
is vacuous. §9.1 cases 8a/8b pin both sides of the boundary.

`TerminalHoldToSettlementV1`: the proceeds are **not reinvested** for the remainder of the
horizon, and the settlement lag is **not discounted**. `terminal_leg` is computed against
`raw_close` because the consideration is a cash amount per as-traded share; the audit records
the corresponding closes as raw (`:13-15`).

For a cell flagged terminal with **no** evidenced consideration — under ruling AR-1/R-A that is
PCS (§2.3) — the cell drops and increments `n_terminal_unevidenced`, a **measured** count. The
**29 unclassified** required-mark IDs are *not* flagged terminal at all: the audit makes no
terminal finding about them, so their missing cells drop into `n_dropped_missing_forward` like
any other missing mark. The audit's line "neither absence nor the following row authorizes
terminal settlement, a carried price, a zero return" (`:19`) forbids anything else, and
**no stock-plus-cash event is representable by this formula at all**.

The engine **never derives** `terminal_leg`. It receives, per cell, a `terminal` flag byte, an
`terminal_evidenced` flag byte and a `terminal_value` amount; the atx-impl stage builds those
arrays from the audit's evidence table, which is checked into `request.json` verbatim with its
source line references and its record dates.

**What variant B actually measures — stated honestly (I-2).** The three evidenced legs are
`terminal_leg ∈ {+0.0138%, +0.144% or −0.794%, 0.000%}` for HNZ, DELL (with / without the
special) and MOLX. Variant B therefore corrects the **evidenced fraction** of the terminal
problem, which is small. The upward Shumway bias lives overwhelmingly in the *dropped* names,
which **both variants drop identically**. Variant A-vs-B is not a survivorship sensitivity and
is not presented as one.

**Audited-ID restriction (ruling AR-7).** The measure research §D-1 risk 2 actually prescribes —
"report IC with and without the 34 IDs named by the required-marks audit" — is implemented as a
**reported restriction of the same 30 configurations**, not a new signal and not a new trial:
every statistic is additionally emitted over the sub-universe that excludes all 34
audited-terminal-or-gap IDs for the whole window. Columns are suffixed `_ex34` (e.g.
`ic_mean_ex34`, `rank_ic_mean_ex34`, `spread_gross_mean_ex34`) and the excluded-ID count per
date is emitted as `n_excluded_audited`. `N_14` stays 30 (§4.4 rule: different statistics of one
configuration, not a search).

Reporting rule: the two variants are **always both emitted**, side by side, in every output,
each with and without the `_ex34` restriction. Choosing the more favourable of any of them, at
any later point, is a new trial.

### 3.9 IC mean, ICIR, naive t

Over the `n_h` emitted dates of a `(signal, horizon, variant)` series:

```text
if (n == 0) -> summary_reportable = 0; ic_mean = ic_sd = icir = naive_t = 0.0
if (n == 1) -> summary_reportable = 0; ic_mean = IC_0; ic_sd = icir = naive_t = 0.0
otherwise   -> summary_reportable = 1
               ic_mean = (1/n) Σ IC_t
               ic_sd   = sqrt( (1/(n-1)) Σ (IC_t - ic_mean)^2 )     # sample sd, n-1
               icir    = (ic_sd == 0.0) ? 0.0 : ic_mean / ic_sd
               naive_t = icir * sqrt(n)
```

**`n < 2` is explicitly defined, never NaN (I-11).** The `n-1` divisor is undefined at `n = 1`
and `0/0` would produce a NaN that the `ic_sd == 0.0` test does not catch. `summary_reportable`
is a first-class field of `IcHorizonSummary`; when it is `0` the serializers emit `ic_sd`,
`icir` and `naive_t` as JSON `null` / CSV `""` with `unreportable_reason == 1`
(`"series-shorter-than-twenty"` — the `n < 2` case is the degenerate end of the same
short-series condition, §3.10's frozen enum, ruling A-3), and every dispersion-dependent
bootstrap is also unreportable. Stage 1 will not reach this branch, but §6 ships a general
engine unit and §9.1 case 29 pins it.

`naive_t` **is emitted and is explicitly labeled invalid** in the schema
(`"naive_t_validity": "invalid-under-overlapping-horizons"`). For `h > 1` the daily IC series
is serially correlated by construction and the naive standard error is understated (research
§D-1 risk 5). The reportable uncertainty is §3.10 only.

### 3.10 Circular block bootstrap

Frozen procedure, pinned to the byte (rulings AR-6 and R-C). Every step below is reproducible
by the Python oracle from this text **plus the `Xoshiro256pp` definition at
`atx-core/include/atx/core/random.hpp:83`** — its four-step splitmix seeding and its `++`
output transform live there, not here, and the oracle may read that header because it is not
the unit under test (T7a bars reading `cross_section_ic.cpp` and `stage_equity_ic.cpp` only;
N-7). §9.2 requires the oracle to reproduce the **first 8 draw start indices** of two named
streams bit-exactly, one per sample id.

**Block length and reportability.** For a series `S` of length `n` at horizon `h`:

```text
L_h        = max(block_len_floor, ceil(h / 2))       # block_len_floor = 5, pre-registered
b          = ceil(n / L_h)                           # blocks per draw
reportable = (bootstrap_draws >= 1) && (n >= 20) && (n / L_h >= 10)
```

`ceil(h / 2)` is integer arithmetic: `(h + 1) / 2` in `usize`. `n / L_h >= 10` is **integer
division** — `floor(n / L_h) >= 10` (M-7). `bootstrap_draws >= 1` is part of the rule, not an
afterthought: with `draws == 0` the percentile step would read an empty span, and
`quantile_sorted`'s guard is `ATX_ASSERT`, which compiles to `((void)0)` outside the checked
build (`atx-core/include/atx/core/macro.hpp`) — an out-of-bounds read in `rel`.
`.agents/cpp/agent.md` §0 forbids it. Validation additionally rejects a configuration that sets
`bootstrap_draws == 0` while any horizon would otherwise be reportable, so "disabled intervals"
is an explicit whole-run choice rather than a silently degraded one.

**Statistic identifiers — explicit integer values, frozen:**

```cpp
enum class BootstrapStatisticId : atx::u16 {
  IcMean       = 0,
  Icir         = 1,
  RankIcMean   = 2,
  RankIcir     = 3,
  SpreadGross  = 4,
  SpreadNet    = 5
};
```

**Seed derivation.** One `u64` per stream, derived with no ambiguity:

**Exact bit layout of the key (ruling R-C).** Six fields, each in its own disjoint byte-aligned
range, so the XOR with `kBootstrapSeed` stays injective:

| Field | Shift | Range | Values |
| --- | --- | --- | --- |
| `statistic_id` | `<< 8` | bits 8-15 | `BootstrapStatisticId` 0..5 |
| `horizon_index` | `<< 16` | bits 16-23 | **index** into `H`, 0..4 — never the value |
| `signal_index` | `<< 24` | bits 24-31 | 0..2 |
| `variant_id` | `<< 32` | bits 32-39 | 0 = `DropMissingForward`, 1 = `IncludeAuditedTerminalV1` |
| `restriction_id` | `<< 40` | bits 40-47 | 0 = full, 1 = `_ex34` |
| `sample_id` | `<< 48` | bits 48-55 | **0 = full, 1 = common** (new, ruling R-C) |

```text
seed_run    = kBootstrapSeed                                       # 20260920
stream_key  = seed_run
              ^ (u64(statistic_id)   <<  8)
              ^ (u64(horizon_index)  << 16)
              ^ (u64(signal_index)   << 24)
              ^ (u64(variant_id)     << 32)
              ^ (u64(restriction_id) << 40)
              ^ (u64(sample_id)      << 48)
state       = stream_key
seed_x      = splitmix64_next(state)          # splitmix64_next ADVANCES `state` FIRST and
                                              # RETURNS the mixed value of the advanced state;
                                              # the RETURN VALUE is used, the mutated `state`
                                              # is discarded
rng         = Xoshiro256pp(seed_x)            # random.hpp:83; its ctor applies splitmix64
                                              # four more times
```

`horizon_index` moves from bits 0-7 to bits 16-23 so every field sits on its own byte and the
layout reads unambiguously; bits 0-7 and 56-63 are unused. **Without `sample_id` the
full-sample and common-sample intervals of one configuration would be drawn from the identical
stream** — reproducible, but 12 pairs of maximally correlated intervals per configuration, and
an implementer who noticed the gap would invent a sixth field and stop matching this document
(N-4).

`horizon_index`, `signal_index`, `variant_id`, `restriction_id` and `sample_id` are positions in
the frozen §4 lists, so adding a horizon later does not renumber existing streams' inputs at
the same index.

**Bounded uniform draw (Lemire, nearly divisionless), frozen — and spelled in C++, not
pseudocode** (N-5). Two constructs need a concrete spelling to survive `/W4 /permissive- /WX`:

- **The 128-bit product.** `atx-core` already ships the helper:
  `atx::core::detail::umul_64_to_128(u64 a, u64 b, u64& hi, u64& lo)` at
  `atx-core/include/atx/core/decimal.hpp:120-133`, which uses `unsigned __int128` under
  `#if defined(__SIZEOF_INT128__)` (clang-cl 18 takes this branch) and `_umul128` from
  `<intrin.h>` otherwise. Per ruling R-6's rationale the eval unit does **not** reach into
  another module's `detail` namespace: it restates the same helper locally as
  `eval::detail::umul_64_to_128`, byte-for-byte the algorithm of `decimal.hpp:120-133`
  including the `#if` split and the `<intrin.h>` include, with a `// SAFETY:` comment naming
  `decimal.hpp:120` as the source and noting that `__int128` is a compiler extension used for
  a widening multiply only (never a 128-bit divide).
- **The rejection threshold.** `-n` on an unsigned operand is MSVC warning **C4146**, which
  `/WX` turns into an error, so the design does not write it. The frozen spelling is
  `(0ULL - n) % n` on `std::uint64_t`, which is warning-proof and numerically identical to
  `(2^64 - n) mod n`.

```cpp
// eval::detail — returns a uniform value in [0, n), n >= 1.
[[nodiscard]] atx::u64 draw_below(atx::core::Xoshiro256pp& rng, atx::u64 n,
                                  atx::usize& modulo_fallbacks) noexcept {
  atx::u64 hi = 0, lo = 0;
  atx::u64 x = rng.next_u64();          // ONE 64-bit output per attempt
  umul_64_to_128(x, n, hi, lo);         // hi = high 64 bits, lo = low 64 bits
  if (lo < n) {
    const atx::u64 thresh = (0ULL - n) % n;   // (2^64 - n) mod n; no unary minus on unsigned
    for (atx::usize tries = 0; lo < thresh; ++tries) {
      if (tries == 64U) {               // statically bounded (JPL rule 2)
        ++modulo_fallbacks;
        return x % n;
      }
      x = rng.next_u64();               // one further 64-bit output per rejection
      umul_64_to_128(x, n, hi, lo);
    }
  }
  return hi;                            // the HIGH 64 bits are the result
}
```

For `n <= 189` the rejection probability per attempt is below `2^-56`, so the 64-attempt
fallback is unreachable in practice — the counter exists so "unreachable" is **measured**, not
asserted, and the receipt asserts it is 0.

**Resampling.** For each of exactly `B = bootstrap_draws` draws, in order:
`b` start indices `s_1..s_b` are drawn by `b` consecutive `draw_below(rng, n)` calls; block `j`
contributes `S[(s_j + k) mod n]` for `k = 0..L_h-1` (circular wrap); the blocks are concatenated
in draw order and the concatenation is truncated to exactly `n` elements. The statistic is
recomputed on the resample by the same §3.9 code path (so an `n < 2` resample is impossible —
resamples have length `n`).

**Interval.** The `B` draw statistics are sorted ascending and the 2.5 / 97.5 percentiles are
taken with the nearest-rank, round-half-up convention of
`atx-core/include/atx/core/stats/cross_section.hpp:75-86` `quantile_sorted`. That helper lives
in `atx::core::stats::detail`; per ruling R-6's rationale the eval unit **restates** the same
five-line rule locally as `eval::detail::quantile_sorted_asc` rather than reaching across into
another module's `detail` namespace (M-2), with a comment naming the source. `splitmix64_next`
(`atx::core::detail`) is likewise restated locally as `eval::detail::splitmix64_next`, with the
in/out convention above written next to it.

**Unreportable output, and the `unreportable_reason` enum — pinned once, here (ruling A-3).**
When `reportable == 0` the interval fields are emitted as `null` (JSON) / `""` (CSV) with an
integer `unreportable_reason` and its matching string. The **frozen** mapping, which §6, §3.9,
§3.13, §7.3 and every §9.1 case use without restating it:

| Code | String | Condition |
| --- | --- | --- |
| `0` | — | reportable |
| `1` | `"series-shorter-than-twenty"` | `n < 20` |
| `2` | `"common-prefix-gap"` | `common_prefix_gaps > 0` on a common block (§3.13) |
| `3` | `"series-too-short-for-block-length"` | `floor(n / L_h) < 10` |
| `4` | `"bootstrap-draws-zero"` | `bootstrap_draws == 0` |

**Precedence when several conditions hold: the lowest nonzero code wins.** So a common block
that is both prefix-gapped and short reports `2`, and a series that is both `n < 20` and
block-short reports `1`. Code `4` is only reachable through a configuration §6.2 already
rejects when any horizon would otherwise be reportable, so it exists for the deliberately
interval-free configuration and for nothing else. §3.9's `summary_reportable == 0` at `n < 2`
carries code `1` (it is the degenerate end of the same short-series condition).

**Implementer note:** the header comment sketched in §6 lists these codes; if T1's draft header
(around the `BootstrapInterval` / `IcSampleStats` declarations) carries an older two-code
comment, T2/T3 align it to this table — the table is authoritative, the comment is not.

§4.4 predicts exactly which horizons fire code `3`.

### 3.11 Signal autocorrelation and implied turnover

Cross-sectional, per signal, lag 1 (the pre-registered lag set is `{1}`):

```text
for each t in [1, T):
    A(t) = { i : mask(t-1,i) == 1 && mask(t,i) == 1
                 && isfinite(x_i(t-1)) && isfinite(x_i(t)) }
    rho_pearson(t) = pearson( x(t-1)|A , x(t)|A )        # emitted iff |A| >= 2
    rho_rank(t)    = pearson( rank(x(t-1)|A), rank(x(t)|A) )
rho_pearson = mean of rho_pearson(t) over emitted t        # REPORTED ONLY
rho_rank    = mean of rho_rank(t)    over emitted t        # DEFINES the turnover proxy
implied_one_way_turnover = 1 - rho_rank
```

**Which rho defines the turnover proxy is pinned (ruling AR-5): `rho_rank`.** The deployed
preference is a rank-space object (§3.2), so a rank autocorrelation is the defensible input to a
turnover proxy; `rho_pearson` is emitted for reference and defines nothing. Both columns appear
in `signal_autocorr.csv` and the turnover column names its source explicitly
(`"turnover_source": "rho_rank"`).

`implied_one_way_turnover = 1 - rho(1)` is a **practitioner rule of thumb**
(research §F[11]), not an accounting identity, and is labeled as such in the output. It is not
comparable to the replay's realized turnover, and it is not comparable to the measured
9.2187 x-NAV cumulative *target change* (research §B.3), which the same section qualifies as
"target changes, not realized turnover".

### 3.12 Quantile (decile) spread

Per `(t, h, signal, variant)`, over `U(t,h)` with `n = |U|`:

1. Reject the date for spread purposes if `n < Q` (`Q = 10`, pre-registered); increment
   `dates_below_quantile_count` (the §6 field name; N-9 naming nit — there is one spelling).
2. Order the names by `(x_i descending, instrument_index ascending)` — a total order, so the
   assignment is deterministic under ties. This mirrors the stable-index tie-break of
   `core::stats::rank` (`cross_section.hpp:114-121`) and the ascending-index tie rule
   `learn/latent.hpp:33` already uses.
3. Name at ordered position `p` (0-based) goes to quantile `q = floor(p * Q / n)`, so
   `q = 0` is the highest-signal decile and `q = Q-1` the lowest. Bucket sizes differ by at most
   one when `Q` does not divide `n`; the sizes are emitted per date.
4. Equal weight inside each bucket. `mean_q(t,h) = mean{ r_i(t,h) : i in bucket q }`.
5. `spread(t,h) = mean_0(t,h) - mean_{Q-1}(t,h)` — long the top decile, short the bottom.

**The book this describes is gross 2.0** (ruling AR-11): `mean_0 - mean_{Q-1}` is the return of a
portfolio long the top decile at total weight `+1.0` and short the bottom at total weight `-1.0`.
The gross-spread definition is **unchanged** — it is the conventional decile spread the
literature and research §D-1 use — and the **cost side is scaled to it**, so the drag prices the
same book the spread earns.

**Gross** spread statistics are the mean, sd and bootstrap interval of `spread(t,h)` over
emitted dates, using §3.10 with the same `L_h`.

**Net** spread — per-date, on exactly the same dates as gross (ruling AR-3). The rebalance-grid
construction of the previous draft is **deleted**; there is no grid, no `k` index and no
`spread_over_period_k`. The net series is the gross series minus a **per-date** drag:

```text
net_h(t) = gross_h(t) - cost_drag_h(t)                    # same t set, same length as gross

cost_drag_h(t) = trade_drag_h + borrow_drag_h(t)
trade_drag_h    = (trade_bps / 1e4) * 2 * turnover_h                       # constant per horizon
borrow_drag_h(t)= (annual_borrow_bps / 1e4) * (days_h(t) / 365.0) * short_leg_gross

days_h(t) = (session_keys[t + h] - session_keys[t]) / 86'400'000'000'000   # ns -> days,
                                                                           # INTEGER division
```

Every symbol, defined:

| Symbol | Definition |
| --- | --- |
| `gross_h(t)` | `spread(t,h)` from step 5 above |
| `trade_bps` | `5.0`, obtained as `constexpr EquityAllocationConfig{}.trade_bps` (ruling AR-10; `atx-impl/src/equity_allocation.hpp:40`) — a compile-time reference, **no file edited** |
| `annual_borrow_bps` | `365.0`, a literal with a comment naming its source: the deployed equity-book run configuration recorded in the handoff ("365bps annual simple short borrow on ACT/365", handoff "Current working book and latest completed evidence"). It is **not** taken from `replay.hpp`, whose `annual_borrow_bps{0.0}` default is `0.0` |
| `2 *` | converts a one-way turnover to full-L1 traded dollars (a round trip touches both the exited and the entered side), matching the full-L1 convention of `equity_allocation.hpp:39` `turnover_limit{0.2} // Full L1` |
| `turnover_h` | the **decile-membership one-way turnover at lag `h`**, defined below; emitted as `IcHorizonSummary::decile_one_way_turnover` |
| `short_leg_gross` | **`1.0`** (ruling AR-11) — the short decile leg of the **gross-2.0** book the spread describes (long 1.0, short 1.0); the borrow drag applies to this leg only |
| `days_h(t)` | **actual calendar days** spanned by the forward window, taken from the panel's own session timestamps: `(session_keys[t+h] - session_keys[t])` nanoseconds, integer-divided by `86'400'000'000'000`. Both operands are `i64` session keys of the evaluated panel, validated strictly increasing (§6.2), so the numerator is positive and the quotient is exact whenever the keys are midnight-aligned, as this archive's are |

Because `days_h(t)` varies with the calendar — weekends, holidays and the December
1-to-3-session gaps — `borrow_drag_h(t)`, and therefore `cost_drag_h(t)`, is **per-date**, not a
horizon constant. `trade_drag_h` remains a horizon constant. Both are emitted: the per-date
values on `IcDatePoint`, their means on `IcHorizonSummary` (§6).

`turnover_h` is measured on the actual decile portfolios, at the **same gross-2.0 weights the
spread earns** (ruling AR-11), not assumed:

```text
w_i(t)     = +1.0 / n_top(t)   for i in decile 0 at date t          # decile 0 totals +1.0
             -1.0 / n_bot(t)   for i in decile Q-1 at date t        # decile Q-1 totals -1.0
              0                otherwise
oneway(t)  = 0.5 * Σ_i | w_i(t) - w_i(t - h) |            # emitted iff both t and t-h emit deciles
turnover_h = mean of oneway(t) over emitted t
```

**Worked check — the scale is now consistent.** On a *complete* decile turnover (no name held
from `t-h` to `t` in either leg), the book exits `+1.0` long and `-1.0` short and enters a fresh
`+1.0` and `-1.0`, so

```text
Σ_i | w_i(t) - w_i(t-h) |  =  1.0 + 1.0 + 1.0 + 1.0  =  4.0     # full L1
oneway(t)                  =  0.5 * 4.0              =  2.0
trade_drag_h               =  (5.0 / 1e4) * 2 * 2.0  =  0.0020  =  20 bps
```

20 bps is the round-trip cost of trading a gross-2.0 book completely at 5 bps per absolute
dollar. The previous draft's `w = ±0.5/n` gave `Σ|Δw| = 2.0`, `oneway = 1.0` and `10 bps` — the
cost of a gross-1.0 book, i.e. **half** the cost of the book whose return `gross_h(t)` reports.
That 2× understatement is what AR-11 removes; the borrow leg is corrected by the same factor
via `short_leg_gross = 1.0`.

**Name collision resolved (C-4 item 4).** `IcHorizonSummary::decile_one_way_turnover` (this
quantity, horizon-indexed, measured from decile membership) and
`AutocorrSummary::implied_one_way_turnover` (§3.11, lag-1, derived from `rho_rank`) are
different quantities with different names. `cost_drag_h(t)` uses the **former**. This is the
single frozen reading (ruling AR-3, accepted); there is no alternative substitution.

**Series length and reportability.** The drag is subtracted date by date from a series of the
same length, so the net series has exactly the same length and the same emitted-date set as the
gross series, `reportable` is identical for `spread_gross_ci` and `spread_net_ci`, and both
follow §4.4's table. No net interval is lost that a gross interval keeps.

**How this compares with the replay's own borrow accrual — what `replay.cpp` actually does.**
Read-only inspection of `atx-engine/src/book/replay.cpp`:

- `borrow_charge` (`:251-259`) computes
  `short_dollars * ((annual_borrow_bps * kBpsToFraction / borrow_day_basis) * days)` — **simple,
  non-compounding**, with `borrow_day_basis` the configured 360 or 365 (365 deployed), and
  returns `0.0` when either the short dollars or the rate is zero.
- `days` comes from `elapsed_days(start, end)` (`:70-82`): `(end - start)` nanoseconds divided
  by `kNanosPerDay` as an **`f64`, fractional, not truncated**, rejecting non-increasing keys.
- The call site charges over **consecutive** observations, on **post-trade marked short
  dollars** for the whole observed interval.

So the design's convention matches the replay's on three points — ACT day count taken from the
same session-key axis, `/365`, simple and non-compounding — and, after AR-11, on the **book
size** as well: `short_leg_gross = 1.0` is the actual short leg of the portfolio whose return
`gross_h(t)` reports, so there is no longer a notional-book mismatch. Two differences remain,
both stated in §12 rather than papered over: (a) the replay accrues on the **actual marked
short dollars of each interval**, which drift with prices between rebalances, while this design
holds the short leg at its rebalance weight of `1.0` for the whole window; (b) this design's
`days_h(t)` is an **integer** day count while `elapsed_days` is fractional — identical for
midnight-aligned keys, divergent if a key ever carried an intraday time. Consecutive
`elapsed_days` telescope, so the total day count over `(t, t+h]` agrees with `days_h(t)` under
(b)'s condition.

Schema: `"borrow_day_convention": "calendar_days_from_session_keys"`.

**Reachability statement, mandatory in the output.** `borrow_charge` is defined at
`atx-engine/src/book/replay.cpp:251` and is **not** declared in
`atx-engine/include/atx/engine/book/replay.hpp` — it is file-local and unreachable. The net
spread above therefore **restates two constants** — one by compile-time reference to atx-impl's
own header, one as a documented literal from the deployed run configuration — and is **not** a
reuse of the replay's cost code. No agreement with the replay's accounting is claimed or
implied. The schema carries

```json
"cost_model_provenance":
  "trade_bps=constexpr EquityAllocationConfig{}.trade_bps (equity_allocation.hpp:40); annual_borrow_bps=365 literal from the deployed equity-book run config (handoff), NOT replay.hpp whose default is 0.0; no call into replay.cpp borrow_charge, which is file-local"
```

Both gross and net are emitted for every `(signal, horizon, variant)`, each also under the
`_ex34` restriction of §3.8.

### 3.13 Common-sample decay column (ruling AR-4)

**The confound.** The frozen context ends 2013-12-31 and has no rows after the evaluation
window, so `IC(h)` is necessarily computed on dates `[0, 189 - h)`: `h = 1` uses 188 dates
running through late December, `h = 63` uses 126 dates ending around 2013-09-30. The horizons
are measured on **nested, different calendar samples**. A falling decay curve is therefore not
distinguishable, as specified, from a 2013-H2-versus-H1 regime difference — and the decay curve
is this checkpoint's headline deliverable and D-3's input.

**The fix, pre-registered before any run.** Every horizon statistic is emitted a second time,
restricted to the **common sample**. **One reading, no intersection clause** (ruling R-B):

```text
common_sample_dates = T - max(H)                       # a PREFIX LENGTH, 189 - 63 = 126
in_common_sample(t) = (t < common_sample_dates)        # the whole definition
```

The common sample is the date prefix `[0, T - max(H))` and nothing else. It is a single scalar,
which is exactly what `CrossSectionIcConfig::common_sample_dates` carries (§6), what
`IcDatePoint::in_common_sample` flags, and what §9.1 case 20c asserts — three statements of one
definition rather than three definitions.

**Failure rule, in place of the deleted intersection — all-or-nothing per horizon (ruling
NEW-1).** The prefix keeps all five horizons on one sample *by construction* only while every
prefix date actually emits. `common_prefix_gaps` is **one `atx::usize` per `(horizon, sample)`,
shared by that block's IC and spread families**: it counts prefix dates that failed to emit at
that horizon for **either** reason — a cross-section below `min_names_per_date` (no IC) or
below `Q` (no spread). There is no spread-only gap counter, and no per-family split; the
previous draft's "counts toward `common_prefix_gaps` for the spread statistics only" clause is
**deleted**, because a single scalar cannot express it.

**Any gap voids the ENTIRE common block for that horizon.** If `common_prefix_gaps > 0` then,
for that `(horizon, sample = common)` block, **every** `*_common` column — IC and spread alike:
`ic_mean_common`, `ic_sd_common`, `icir_common`, `naive_t_common`, the rank equivalents,
`spread_gross_mean_common`, `spread_net_mean_common`, `spread_*_sd_common` and all six
bootstrap intervals — is emitted as **`null` (JSON) / `""` (CSV)** with
`summary_reportable = 0` and `unreportable_reason == 2` (`"common-prefix-gap"`, §3.10's frozen
enum; it wins over a co-occurring code 3 by the lowest-nonzero rule). The means are **nulled,
not emitted**: a point estimate
over an incomplete prefix is exactly the number a reader would mistake for a common-sample
result, and the whole purpose of AR-4 is that the five horizons share one sample. The full-sample
block for the same horizon is unaffected. Silently falling back to a per-horizon intersection —
which would put the five horizons back on five different samples — is forbidden.

**Spread columns use the same prefix**, even though spread emission needs `n >= Q` rather than
`n >= min_names_per_date`; under the rule above a date that emits an IC but not a spread is a
gap for the whole block, so the IC and spread `_common` families can never be computed over
different date sets. Stage 1 expects no gap at all (200-400 admitted names against
`min_names_per_date = 2` and `Q = 10`); §10.4's `predictions_confirmed` records the observed
`common_prefix_gaps` per horizon.

Columns are suffixed `_common` (`ic_mean_common`, `rank_ic_mean_common`,
`spread_gross_mean_common`, and their bootstrap intervals) and appear beside the full-sample
values in `ic_decay.csv` and `ic_summary.json`. The common-sample series length is
`n_common = 126` for **every** horizon, so §4.4's reportability rule is evaluated separately
for the common-sample intervals (see §4.4's second table). Each `_common` interval is drawn
from its **own** bootstrap stream via the `sample_id = 1` field of the §3.10 key (ruling R-C).

**This does not change `N_14`.** Per §4.4's own rule, these are different statistics of the same
30 configurations, not a search. The reader is expected to read the `_common` column for shape
and the full-sample column for the longest available series, and the schema says so. The
confound itself is carried into §12 regardless, because the common-sample restriction narrows
it rather than removing it: all five horizons then share one sample, but that sample is
2013-H1-weighted.

---

## 4. Pre-registered trial set and trial ledger

These values are **frozen by this document before any run**. Changing any of them produces a
**new trial**, appended to the ledger. Nothing is overwritten (handoff: "Data-only repairs
receive a new dataset version and linked rerun, never erase old trials",
`atx-impl/docs/EQUITY_BOOK_BASELINE.md:76`).

### 4.1 Signals — exactly three, no others

| idx | name | definition |
| --- | --- | --- |
| 0 | `momentum_252` | `ts_mean(delay(close, 21) / delay(close, 252) - 1, 5)` (`equity_baseline_views.hpp:15`) |
| 1 | `momentum_126` | `ts_mean(delay(close, 21) / delay(close, 126) - 1, 5)` (`equity_baseline_views.hpp:16`) |
| 2 | `blend_equal` | the published `combo.bin` alpha column: `extract_streams(view.signals, atx::engine::WeightPolicy{}, view.panel, frictionless_sim())` then `0.5 * positions(0,d)[i] + 0.5 * positions(1,d)[i]` (`stage_equity_baseline.cpp:341-352`) — a **rank-space** blend, §3.2 |

No other expression, no sign flip, no parameter variant, no residualized or neutralized form.

### 4.2 Horizon set — exactly five, in panel rows

```text
H = { 1, 5, 10, 21, 63 }
```

Rationale, recorded so it is not re-derived: 1 is the shortest measurable; 5 is the deployed
decision cadence, which `report-holding-period-audit.md:29` records is "every **five stored
panel rows**, starting at row zero … **not an exchange-calendar weekly schedule**"; 21 and 63
match the signals' own `delay(close, 21)` offset and the allocator's 63-adjacent-return risk
window (handoff, "Allocation uses 63 adjacent decision-time returns"). 10 fills the gap. No
horizon may be added, removed or re-spaced without a new ledger entry.

### 4.3 Remaining frozen values

| Parameter | Frozen value |
| --- | --- |
| Quantile count `Q` | `10` (deciles) |
| `min_names_per_date` | `2` |
| Bootstrap draws `B` | `2000` |
| Bootstrap seed `kBootstrapSeed` | `20260920` (decimal, `u64`); stream derivation §3.10 |
| Block length rule | `L_h = max(5, ceil(h/2))` → `{5, 5, 5, 11, 32}` (re-pinned pre-run, ruling AR-3) |
| Bootstrap percentiles | `2.5 / 97.5`, nearest-rank, round-half-up |
| Reportability rule | `bootstrap_draws >= 1 && n >= 20 && floor(n / L_h) >= 10` |
| Statistic ids | `BootstrapStatisticId` §3.10, explicit integer values 0..5 |
| Autocorrelation lags | `{1}`; `rho_rank` defines the turnover proxy (ruling AR-5) |
| Forward-return variants | `DropMissingForward`, `IncludeAuditedTerminalV1` — both, always |
| Evidenced terminal events | exactly three: HNZ `37648`, DELL `35715`, MOLX `39970` (ruling AR-1). PCS is never applied |
| DELL special dividend | included only when `session_keys[t] <= 2013-10-28` (ruling AR-2) |
| Audited-ID restriction | every statistic also emitted as `_ex34` (ruling AR-7); not a new trial |
| Common sample | every statistic also emitted as `_common` over the **prefix** `t < T - max(H)` = `[0, 126)`, one scalar `common_sample_dates` (rulings AR-4 and R-B); not a new trial. An incomplete prefix voids the whole `_common` block for that horizon — every `*_common` column null, `unreportable_reason == 2` (NEW-1) |
| Cost convention | `trade_bps` = `constexpr EquityAllocationConfig{}.trade_bps` (= 5.0); `annual_borrow_bps` = `365.0` literal from the deployed run config |
| Priced book (ruling AR-11) | the **gross-2.0** book the spread describes: decile weights `±1.0/n`, `short_leg_gross = 1.0`. A complete decile turnover costs **20 bps** |
| Bootstrap stream key | six fields, byte-aligned: `statistic_id<<8`, `horizon_index<<16`, `signal_index<<24`, `variant_id<<32`, `restriction_id<<40`, `sample_id<<48` (ruling R-C) |
| Borrow day count | **actual calendar days from the panel's session keys**: `days_h(t) = (session_keys[t+h] - session_keys[t]) / 86'400'000'000'000`, integer division (ruling AR-3, second part). Observation-count days are **rejected** |
| Net-spread construction | per-date `net_h(t) = gross_h(t) - cost_drag_h(t)`, with `borrow_drag_h(t)` varying by calendar span (ruling AR-3). **No rebalance grid** |
| Tie handling | averaged ranks (§3.5); quantile assignment by `(signal desc, index asc)` |
| Seal policy | `RejectSealedV1` (§5) |
| Alignment | signal at `t`, return from `t`; deployed book executes at `t+1` (§3.3) |

### 4.4 Trial count `N` for deflated-Sharpe accounting

```text
N_14 = |signals| x |H| x |variants| = 3 x 5 x 2 = 30
```

The forward-return variant counts as a trial axis because a reader could select the more
favourable of the two; counting it is the conservative choice. The IC family and the
quantile-spread family computed from one `(signal, horizon, variant)` configuration are **one**
trial, not two — they are different statistics of one configuration, not a search.

**The `_ex34` restriction (AR-7) and the `_common` sample (AR-4) likewise do not raise `N`.**
Both are reported **for every** configuration, unconditionally and side by side with the
unrestricted full-sample value; neither is a candidate to be chosen between. They are
additional statistics of the same 30 configurations under the rule above. If a later checkpoint
ever *selects* on one of them, that selection is a new trial and must be ledgered as such.
`N_14 = 30` stands.

`N_14 = 30` is appended to the ledger **before** the run and is the number
`eval::deflated_sharpe` (`atx-engine/include/atx/engine/eval/deflated_sharpe.hpp:138`) must be
fed when any Sharpe is eventually deflated. Research §C-2: "There is no trial ledger, so the
`N` for deflation is unknown and every future equity Sharpe is undeflatable in principle."

**Stage-1 reportability, computed in advance, with the arithmetic shown.** `T = 189`, and a
date `t` emits at horizon `h` iff `t + h < T` and `|U(t,h)| >= 2`, so `n_h = 189 - h` when every
date emits (§12 and the receipt assert `dates_below_min_names == 0`, which is expected at
200-400 admitted names against `min_names_per_date = 2`).

Full-sample series, under the re-pinned rule `L_h = max(5, ceil(h/2))`:

| `h` | `ceil(h/2)` | `L_h` | `n_h = 189 - h` | `floor(n_h / L_h)` | `>= 10`? | `n_h >= 20`? | **reportable** |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 1 | 5 | 188 | 37 | yes | yes | **yes** |
| 5 | 3 | 5 | 184 | 36 | yes | yes | **yes** |
| 10 | 5 | 5 | 179 | 35 | yes | yes | **yes** |
| 21 | 11 | 11 | 168 | 15 | yes | yes | **yes** |
| 63 | 32 | 32 | 126 | 3 | **no** | yes | **no → `null`** |

Common-sample series (§3.13), `n_common = 189 - max(H) = 126` for **every** horizon:

| `h` | `L_h` | `n_common` | `floor(n_common / L_h)` | **reportable** |
| --- | --- | --- | --- | --- |
| 1 | 5 | 126 | 25 | **yes** |
| 5 | 5 | 126 | 25 | **yes** |
| 10 | 5 | 126 | 25 | **yes** |
| 21 | 11 | 126 | 11 | **yes** |
| 63 | 32 | 126 | 3 | **no → `null`** |

**Prediction: exactly one horizon, `h = 63`, comes back with a `null` interval, in both the
full-sample and common-sample columns, for every signal, variant and restriction. Everything
else is reportable, including every net spread** (§3.12: the net series has the same length as
the gross series).

**Why the block rule was re-pinned.** The previous draft's `L_h = max(5, h)` gives
`floor(168/21) = 8 < 10` at `h = 21` and `floor(126/63) = 2` at `h = 63` — two of five horizons
unreportable, including the signals' own `delay(close, 21)` offset, i.e. 40% of the decay curve
as bare point estimates. The change is made **now, before any run**, which is the only time it
is free; the ledger's pre-registration line carries the final values and the reason. `L_h` still
exceeds half the overlap length at every horizon, which is the stated dependence scale, and
§12 records that a block shorter than the full overlap leaves the interval optimistically
narrow.

### 4.5 Trial ledger — file, schema, append discipline

**File (ruling OQ-3):** `atx-engine/reviews/trial-ledger.jsonl` — the tracked reviews directory
that holds the receipts, **not** `build-equity/audits/`, which is build-adjacent and could be
cleaned, taking every prior checkpoint's declared `N` with it. One JSON object per line, UTF-8,
LF endings, no trailing whitespace, **append-only**. Sidecar
`atx-engine/reviews/trial-ledger.manifest.json` carries `{"lines": n, "head_sha256": …,
"created_utc": …}`. Every other reference in this design to a `build-equity/audits/`
trial-ledger path is superseded by this one.

**Hash chain.** Each line's `prev_sha256` is the SHA-256 of the *previous line's bytes
including its LF*; the first line uses 64 zeros. `head_sha256` in the sidecar is the SHA-256 of
the last line. A deletion or in-place edit breaks the chain and is detectable, so "append-only"
is a verifiable property rather than a promise.

**Schema (every key required; unknown keys rejected on read):**

```json
{
  "schema": "atx-trial-ledger-v1",
  "trial_id": "iteration14-cross-section-ic-0001",
  "appended_utc": "2026-09-20T00:00:00Z",
  "prev_sha256": "0000…0000",
  "checkpoint": 14,
  "purpose": "training-only-forecast-evaluation",
  "status": "pre-registered",
  "trial_count_declared": 30,
  "parents": [
    {"role": "source-context",     "artifact_id": "ec572b82…"},
    {"role": "baseline-evaluation","artifact_id": "57b7c21c…"},
    {"role": "baseline-combo",     "artifact_id": "def31e5c…"},
    {"role": "design-note",        "sha256": "<sha256 of this file>"}
  ],
  "recipe": {
    "signals": [{"name":"momentum_252","dsl":"…","dsl_sha256":"…"}, …],
    "horizons": [1,5,10,21,63],
    "quantiles": 10,
    "bootstrap": {"draws":2000,"seed":20260920,
                  "block_len_rule":"max(5, ceil(h/2))","block_lens":[5,5,5,11,32],
                  "percentiles":[2.5,97.5],
                  "reportable_rule":"draws>=1 && n>=20 && floor(n/L)>=10",
                  "statistic_ids":{"IcMean":0,"Icir":1,"RankIcMean":2,"RankIcir":3,
                                   "SpreadGross":4,"SpreadNet":5},
                  "stream_key":"seed ^ (stat<<8) ^ (horizon_index<<16) ^ (signal<<24) ^ (variant_id<<32) ^ (restriction_id<<40) ^ (sample_id<<48)",
                  "sample_ids":{"full":0,"common":1},
                  "predicted_null_horizons":[63]},
    "autocorr_lags": [1], "turnover_source": "rho_rank",
    "forward_variants": ["DropMissingForward","IncludeAuditedTerminalV1"],
    "restrictions": ["full","_ex34"], "samples": ["full","_common"],
    "common_sample_rule": "prefix t < (T - max(H)); incomplete prefix => _common unreportable",
    "alignment": "signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1",
    "net_spread_rule": "per-date net_h(t) = gross_h(t) - cost_drag_h(t); no rebalance grid",
    "cost": {"trade_bps":5.0,"trade_bps_source":"constexpr EquityAllocationConfig{}.trade_bps",
             "annual_borrow_bps":365,"annual_borrow_bps_source":"deployed equity-book run config (handoff)",
             "short_leg_gross":1.0,"priced_book_gross":2.0,
             "decile_weights":"+1.0/n_top, -1.0/n_bottom",
             "borrow_day_convention":"calendar_days_from_session_keys",
             "borrow_day_formula":"(session_keys[t+h]-session_keys[t])/86400000000000 integer division",
             "provenance":"constants-restated-not-a-replay-call; replay.cpp borrow_charge is file-local"},
    "seal": {"policy":"RejectSealedV1","validation_begin":"2020-01-01",
             "sealed_begin":"2023-01-01"}
  },
  "window": {"start":"2013-04-04","end_exclusive":"2014-01-01","observations":189},
  "source_exclusions": {
    "required_mark_id_count": 34,
    "terminal_hypothesis_ids": [37648, 35715, 39970],
    "terminal_hypothesis_note": "terminal-cash-event hypothesis per the audit's wording, not a settled classification",
    "terminal_evidenced_record_dates": {"35715": "2013-10-28"},
    "evidenced_non_terminal_ids": [150340, 351548],
    "unclassified_id_count": 29,
    "terminal_unevidenced_ids": [146189],
    "ex34_restriction_ids": ["… all 34 required-mark IDs …"],
    "pcs_statement": "PCS is never applied; admission remains rejected"},
  "fit_boundary": {"fit_kind":"unfit-no-fitting-performed","fitted_observations":0},
  "runtime": {"wall_seconds": null, "peak_working_set_bytes": null},
  "result": {"outcome":"pending","manifest_sha256":null,"failure_sha256":null},
  "producer_executable_sha256": "…",
  "notes": "Stage 1 of D-1; sign-and-shape only; not accepted alpha."
}
```

**Two lines per run, never one.** A `"status":"pre-registered"` line is appended **before** the
native run, with `result.outcome == "pending"`. A second line with the same `trial_id`,
`"status":"completed"` or `"failed"`, `runtime` filled and `result.manifest_sha256` or
`result.failure_sha256` set, is appended **after**. A failed run keeps both lines; the retry is
`…-0002`. Nothing is rewritten. This satisfies `EQUITY_BOOK_BASELINE.md:76`: "Every attempted
variant, failure, rejected run and prior inspected strategy goes into an append-only trial
ledger with parent IDs, exact recipe, source exclusions, fit boundaries, runtime/RSS and
result/failure hashes."

**API** (`atx-impl/src/trial_ledger.hpp`):

```cpp
[[nodiscard]] atx::core::Result<TrialLedgerEntry>
append_trial(const std::string& ledger_path, const TrialLedgerEntry& entry);   // verifies chain, then appends
[[nodiscard]] atx::core::Result<TrialLedgerHead>
verify_trial_ledger(const std::string& ledger_path);                           // full chain walk
```

`append_trial` re-verifies the whole chain before writing and fails with `ErrorCode::Internal`
on a break; it never truncates, never opens with `std::ios::trunc`, and uses `std::ios::app`
exclusively.

---

## 5. Lockbox and lookahead guards

### 5.1 The seal must be code, not prose

Research §C-2: the 2013-2019 / 2020-2022 / 2023-2025 split "exists only as prose at
`EQUITY_BOOK_BASELINE.md:72`", and "The equity path uses none of" `eval/lockbox.hpp`. Include
lists of all four equity TUs contain no `eval/` header (research §B.7).

### 5.2 `apply_calendar_seal` — the new structural gate

A pure function in the same new engine unit. **`SealPolicy`, `CalendarSeal`, `SealReport` and
`apply_calendar_seal` are declared exactly once, in the §6 header sketch** (M-3); this section
specifies their *behaviour* and does not re-spell their members. An implementer takes the
declaration from §6 only.

Behaviour:

1. `policy == Unknown` → `Err(InvalidArgument)`. No default.
2. `session_keys` must be strictly increasing → else `Err(InvalidArgument)`.
3. Count `dates_at_or_after_sealed = #{ t : session_keys[t] >= sealed_begin_ns }` and the
   validation equivalent, always — the counts are recorded even when zero.
4. `policy == RejectSealedV1` **and** `dates_at_or_after_sealed > 0` →
   `Err(PermissionDenied, "cross_section_ic: <n> input observations at or after the sealed
   boundary")`. The count is in the message and in `SealReport` (the stage records the report
   on the failure path before propagating).
5. `policy == MaskSealedV1` → `dates_visible = first sealed index`; the caller must truncate.
   **Not used in checkpoint 14**; present so the enum has a rejectable alternative rather than
   a silent one.
6. Whenever a boundary falls strictly inside the panel, the stage additionally routes the
   Panel through `eval::reserve_window(panel, holdout_begin, holdout_len, embargo_len)`
   (`lockbox.hpp:346`) and records the returned `SealedReservation::content_address`
   (`lockbox.hpp:72`), so the truncation is the same audited carve the factory path uses, and
   the receipt carries its content address.

**Honest statement about Stage 1, required in the receipt.** The 2013 slice contains no date
at or after either boundary, so `dates_at_or_after_validation == 0` and
`dates_at_or_after_sealed == 0`, and `reserve_window` is **not** called (`holdout_len` would be
0, which `lockbox.hpp:348-351` rejects). The seal is therefore **non-vacuous by code and
vacuous by data**: it is a live gate that this input does not trip. The receipt says exactly
that. Claiming a lockbox carve occurred on a 2013-only panel would be false, and carving the
terminal 20% of 2013 by index — which `reserve_lockbox(panel)` (`lockbox.hpp:319`, default frac
0.20) would do — would reserve 2013 dates, not 2023-2025, and is therefore **not** used. See
ruling R-2.

### 5.3 Window guard at the CLI boundary

`equity-ic` repeats the baseline stage's own date validation verbatim:
`*begin >= *date_to_nanos("2013-04-01")` and `*end <= *date_to_nanos("2020-01-01")`
(`atx-impl/src/stage_equity_baseline.cpp:99-100`), and the context-recipe assertion
`end > training_end` → reject (`:260-263`, "context must prove requested end coverage and
exclude sealed dates"). Both are TU-local in `stage_equity_baseline.cpp`, so the new stage
restates them; the test plan pins that the two stages reject the same boundary dates (§9.4).

### 5.4 Lookahead hazards this design guards, with citations

| Hazard | Source | Guard here |
| --- | --- | --- |
| Forward eligibility conditioning | `report-holding-period-audit.md:59-67` — `stage_report.cpp` "applies the **last** book retrospectively to every historical raw-close return" | §3.3: admission is tested at `t` only, never at `t+h` |
| Backward return applied to a forward book | `report-holding-period-audit.md:79-92` — `stage_metabook.cpp:581-594` supplies the **backward** daily return to a book formed at `t` | §3.3 uses `P(t+h)/P(t)`, strictly forward; the engine never reads a return ending at or before `t` |
| Label maturity vs embargo | `learning-availability-audit.md:8-14` — "a finite historical forward label can still depend on prices after `t`"; `FeatureMatrix` "cannot enforce label maturity" | No fitting occurs, so no selector consumes a label. The horizon is carried explicitly per statistic (`h` is a field of every emitted row), which is precisely the metadata the audit says `FeatureMatrix` lacks |
| Fold contamination via shared augmentation | `learning-availability-audit.md:15-22` | No folds, no PCA, no interaction selection |
| Mask not enforced downstream | `annual-panel-memory-review.md:60-70` — `extract_streams` builds a static full instrument `Universe` | §3.1: the mask is passed as an explicit span and asserted to size `T*I` |
| Warmup rows entering evaluation | `bounded-equity-panel-design.md:68` — "`fit_begin` alone does not prevent warmup orders"; "generic `run_optimize` schedules from row zero" | The stage evaluates only `EquityBaselineEvaluation::panel`, which begins at `plan.evaluation_begin` with the 256-observation warmup already consumed (`equity_baseline_views.hpp:38-41`) |
| Bar release before interval end | `bar-availability-audit.md:6-12,30-32` — completed Databento bars carry an interval-**start** timestamp, and `DatabentoPipelineE2E.LoaderBridgeFeedRoundTrip` pins it | **Not on this path.** This panel comes from the `tbltickerhistory` archive, not the Databento bar feed. The analogous hazard here is the vendor's 05:00 CT T+1 full-history delivery, and the input audit's instruction "Do not equate midnight session keys with information availability" (`tbltickerhistory-input-audit.md:47-52`). Guard: the recipe records `"session_semantics": "session-label-not-availability"` (`panel_artifact.hpp:14`) and the receipt repeats the availability disclaimer verbatim |
| Survivorship via dropped terminals | `report-holding-period-audit.md:31`; research §C-8, §F[9] | §3.8: two variants, always both reported, with per-date drop counts |

---

## 6. Engine API

New pure unit. Header declares; `.cpp` defines. `Result`/`Status`/`ATX_TRY` exactly as
`book/` and `eval/` use them (`atx-core/include/atx/core/error.hpp`). No I/O, no logging, no
exception on any success path, no dynamic allocation inside the per-date loop.

**One spelling of every type.** This block is the sole declaration of every type and function in
the unit; §3.10 and §5.2 specify behaviour and do not re-spell members (M-3). `atx::u8` is used
uniformly for byte-sized flags and enum bases — `std::uint8_t` does not appear (M-5).

```cpp
// atx-engine/include/atx/engine/eval/cross_section_ic.hpp
#pragma once
#include <cstdint>      // std::uint16_t via atx::u16 aliases; explicit per the hygiene preset
#include <span>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::eval {

inline constexpr atx::usize kMaxIcHorizons     = 8;    // fixture-sized; see ruling R-7
inline constexpr atx::usize kMaxIcQuantiles    = 32;
inline constexpr atx::usize kMaxBootstrapDraws = 100'000;
// (kMaxIcSignals removed: the API takes ONE signal span per call and bounds no
//  signal count. The stage's three-signal loop is bounded by its own frozen list. M-4)

// --- rejectable admission enums: Unknown is never a working default ---------
enum class ForwardReturnVariant : atx::u8 {
  Unknown = 0,                 // rejects
  DropMissingForward,          // Shumway-biased upward; must be reported beside the other
  IncludeAuditedTerminalV1     // caller-supplied evidenced terminal returns; §3.8
};
enum class IcTieHandling : atx::u8 { Unknown = 0, AverageRanksV1 };
enum class SealPolicy    : atx::u8 { Unknown = 0, RejectSealedV1, MaskSealedV1 };

// Frozen integer values; the bootstrap stream key depends on them (§3.10).
enum class BootstrapStatisticId : atx::u16 {
  IcMean = 0, Icir = 1, RankIcMean = 2, RankIcir = 3, SpreadGross = 4, SpreadNet = 5
};

struct CrossSectionIcConfig {
  // BORROWED: `horizons` must outlive both plan_* and compute_*; the caller owns
  // the storage and must not modify it between the two calls (M-6).
  std::span<const atx::usize> horizons;   // strictly increasing, each >= 1, size <= kMaxIcHorizons
  atx::usize quantiles{10};               // 2 <= Q <= kMaxIcQuantiles
  atx::usize min_names_per_date{2};       // >= 2
  atx::usize bootstrap_draws{2000};       // 0 => every interval unreportable; <= kMaxBootstrapDraws
  atx::usize block_len_floor{5};          // >= 1;  L_h = max(floor, (h+1)/2)
  atx::u64   bootstrap_seed{0};
  atx::u64   stream_signal_index{0};      // stream key input (§3.10); 0 when unused
  atx::u64   stream_variant_id{0};        // 0 = DropMissingForward, 1 = IncludeAuditedTerminalV1
  atx::u64   stream_restriction_id{0};    // 0 = full, 1 = _ex34
  atx::u64   stream_sample_id{0};         // 0 = full, 1 = common  (ruling R-C)
  atx::f64   trade_bps{0.0};              // >= 0, finite
  atx::f64   annual_borrow_bps{0.0};      // >= 0, finite
  atx::f64   short_leg_gross{1.0};        // > 0, finite; the gross-2.0 book's short leg
                                          // (§3.12, ruling AR-11)
  atx::i64   day_basis{365};              // 360 or 365
  atx::usize common_sample_dates{0};      // §3.13 PREFIX LENGTH: in_common_sample(t) == (t <
                                          // common_sample_dates). 0 => no _common pass.
                                          // The frozen Stage-1 value is T - max(H) = 126.
  ForwardReturnVariant forward_variant{ForwardReturnVariant::Unknown};
  IcTieHandling        ties{IcTieHandling::Unknown};
};

// All numeric spans are date-major, length dates*instruments unless noted.
// BORROWED: the caller keeps every span alive and unchanged for the call; no
// span is modified. Spans handed out inside the result are owned by the result.
struct CrossSectionIcInput {
  atx::usize dates{};
  atx::usize instruments{};
  std::span<const atx::f64> signal;             // NaN = undefined
  std::span<const atx::f64> price;              // TRI close; NaN or <= 0 = no mark
  std::span<const atx::f64> raw_price;          // as-traded close; terminal legs only
  std::span<const atx::u8>  mask;               // 1 = admitted at (t,i)
  std::span<const atx::u8>  terminal;           // 1 = audited terminal coverage at (t,i)
  std::span<const atx::u8>  terminal_evidenced; // 1 = consideration evidenced; 0 = drop + count
  std::span<const atx::f64> terminal_value;     // consideration (+ special, already gated by
                                                // the caller on record date, §3.8 / AR-2)
  std::span<const atx::u8>  excluded_audited;   // 1 = in the 34-ID _ex34 restriction set
  std::span<const atx::i64> session_keys;       // strictly increasing, size == dates
};

struct IcDatePoint {
  atx::usize date{};  atx::i64 session_key{};
  atx::usize n_eligible{}, n_signal_finite{}, n_with_forward{};
  atx::usize n_dropped_missing_forward{}, n_terminal_applied{}, n_terminal_unevidenced{};
  atx::usize n_excluded_audited{}, n_used{};
  atx::f64 pearson_ic{}, rank_ic{};
  atx::f64 spread_gross{}, spread_net{};
  atx::i64 days_forward{};            // days_h(t), actual calendar days (§3.12)
  atx::f64 borrow_drag{}, cost_drag{}; // per-date; trade_drag is the horizon constant
  atx::u8 emitted{};          // 0 -> pearson_ic/rank_ic are 0.0 and must be ignored
  atx::u8 spread_emitted{};   // 0 -> spread_* are 0.0 and must be ignored
  atx::u8 in_common_sample{}; // 1 -> the date contributes to the _common statistics
};

struct BootstrapInterval {
  atx::f64 point{}, lo{}, hi{};
  atx::usize draws{}, block_len{}, blocks{}, series_len{};
  atx::usize modulo_fallbacks{};    // §3.10; the receipt asserts 0
  atx::u8 reportable{};             // 0 -> lo/hi are 0.0 and MUST serialize as null / ""
  atx::u8 unreportable_reason{};    // §3.10's FROZEN enum (ruling A-3):
                                    //   0 reportable
                                    //   1 series-shorter-than-twenty   (n < 20)
                                    //   2 common-prefix-gap            (§3.13)
                                    //   3 series-too-short-for-block-length (floor(n/L)<10)
                                    //   4 bootstrap-draws-zero
                                    // Several may hold: the LOWEST NONZERO code wins.
};

struct QuantileBucketStat {
  atx::usize quantile{}, n_dates{};
  atx::f64 mean_forward_return{}, mean_names{};
};

// One of these per (horizon, sample) where sample in {full, common}; the pair is
// carried side by side so a reader never has to join two files (§3.13).
struct IcSampleStats {
  atx::usize dates_emitted{};
  atx::f64 ic_mean{}, ic_sd{}, icir{}, naive_t{};
  atx::f64 rank_ic_mean{}, rank_ic_sd{}, rank_icir{}, rank_naive_t{};
  atx::f64 spread_gross_mean{}, spread_gross_sd{}, spread_net_mean{}, spread_net_sd{};
  BootstrapInterval ic_mean_ci, icir_ci, rank_ic_mean_ci, rank_icir_ci;
  BootstrapInterval spread_gross_ci, spread_net_ci;
  atx::usize common_prefix_gaps{}; // §3.13 / NEW-1: ONE counter per (horizon, sample),
                                   // shared by this block's IC and spread families. Counts
                                   // prefix dates that failed to emit at this horizon for
                                   // EITHER reason (n < min_names_per_date, or n < Q).
                                   // Meaningful only on the common block; 0 on the full block.
  atx::u8 summary_reportable{};   // 0 when dates_emitted < 2, or (common block) when
                                  // common_prefix_gaps > 0. When 0, EVERY field of this
                                  // block — means included, IC and spread alike — serializes
                                  // as null/"" (§3.9, §3.13).
  atx::u8 unreportable_reason{};  // the SAME frozen enum as BootstrapInterval (§3.10,
                                  // ruling A-3): 1 short series (incl. the n < 2 degenerate
                                  // case of §3.9), 2 common-prefix-gap, 3 block-length,
                                  // 4 draws-zero; lowest nonzero code wins.
};

struct IcHorizonSummary {
  atx::usize horizon{}, block_len{};
  atx::usize dates_below_min_names{}, dates_below_quantile_count{};
  IcSampleStats full;             // all emitted dates
  IcSampleStats common;           // §3.13 common sample; zeroed with summary_reportable = 0
                                  // when cfg.common_sample_dates == 0
  // §3.12 cost drag inputs and outputs.
  atx::f64 decile_one_way_turnover{};  // turnover_h: decile-membership one-way turnover at
                                       // lag h, expressed as a fraction of NAV for the
                                       // GROSS-2.0 book (AR-11), so its range is [0, 2] and a
                                       // COMPLETE turnover reads 2.0, not 1.0. Not the same
                                       // scale as AutocorrSummary::implied_one_way_turnover,
                                       // which lies in [0, 2] only via 1 - rho_rank and is a
                                       // different quantity entirely (§3.11, §12.9).
  atx::f64 trade_drag{};               // CONSTANT per horizon
  atx::f64 mean_borrow_drag{}, mean_cost_drag{};  // means of the per-date values on `series`
  atx::f64 mean_days_forward{};        // mean days_h(t); diagnostic for the calendar span
  std::vector<QuantileBucketStat> buckets;  // sized Q once per horizon, never per date
  std::vector<IcDatePoint> series;          // reserved to `dates` once per horizon
};

struct AutocorrSummary {
  atx::usize lag{1}, pairs_emitted{};
  atx::f64 rho_pearson{};              // REPORTED ONLY (§3.11 / AR-5)
  atx::f64 rho_rank{};                 // defines the proxy below
  atx::f64 implied_one_way_turnover{}; // == 1 - rho_rank; distinct from
                                       // IcHorizonSummary::decile_one_way_turnover
};

struct CrossSectionIcResult {
  std::vector<IcHorizonSummary> horizons;   // one per configured horizon, same order
  AutocorrSummary autocorr;
  ForwardReturnVariant variant{ForwardReturnVariant::Unknown};
};

// Scratch is caller-owned and sized ONCE by plan_cross_section_ic. compute_*
// performs no allocation after entry except the result vectors, which are sized
// before the date loop begins. The per-date inner loop touches only scratch.
struct CrossSectionIcScratch {
  std::vector<atx::f64>   x, r, rx, rr, buf;   // each sized `instruments`
  std::vector<atx::usize> order, perm;         // each sized `instruments`
  std::vector<atx::f64>   w_prev, w_curr;      // each sized `instruments`
  std::vector<atx::f64>   draw;                // sized `dates` — one bootstrap resample
  std::vector<atx::f64>   stat;                // sized `bootstrap_draws` — the B draw
                                               // statistics, sorted in place for the
                                               // percentile step (C-7 item 4)
};

[[nodiscard]] atx::core::Result<CrossSectionIcScratch>
plan_cross_section_ic(const CrossSectionIcInput& in, const CrossSectionIcConfig& cfg);

[[nodiscard]] atx::core::Result<CrossSectionIcResult>
compute_cross_section_ic(const CrossSectionIcInput& in, const CrossSectionIcConfig& cfg,
                         CrossSectionIcScratch& scratch);

// --- the calendar seal (§5.2) ----------------------------------------------
struct CalendarSeal {
  atx::i64 validation_begin_ns{};   // 2020-01-01
  atx::i64 sealed_begin_ns{};       // 2023-01-01
  SealPolicy policy{SealPolicy::Unknown};
};

struct SealReport {
  atx::usize dates_total{};
  atx::usize dates_visible{};
  atx::usize dates_at_or_after_validation{};
  atx::usize dates_at_or_after_sealed{};
  atx::usize embargo_len{};
  atx::u64 content_address{};    // eval::SealedReservation::content_address, or 0
  atx::u8 used_reserve_window{}; // 1 iff a boundary fell strictly inside the panel
};

[[nodiscard]] atx::core::Result<SealReport>
apply_calendar_seal(std::span<const atx::i64> session_keys, const CalendarSeal& seal);

} // namespace atx::engine::eval
```

### 6.1 Bounds and allocation budget

- **Per-date inner loop: zero allocation.** Every buffer is a span into `scratch`, sized once
  at `plan_cross_section_ic`. Total scratch is

  ```text
  9 * instruments * 8            # x, r, rx, rr, buf, w_prev, w_curr (f64) + order, perm (usize)
  + dates * 8                    # draw
  + bootstrap_draws * 8          # stat  — the B draw statistics for the percentile step
  ```

  At `I = 1,661`, `T = 189`, `B = 2000`: `9*1661*8 = 119,592` + `189*8 = 1,512` +
  `2000*8 = 16,000` = **137,104 bytes**. The previous draft's 121,104-byte figure omitted the
  `stat` buffer, which is why the zero-allocation claim did not hold (C-7 item 4).
- Result vectors are sized before the date loop: `series.reserve(dates)` and
  `buckets.assign(Q, {})` per horizon. Documented as the only success-path allocation.
- Every loop is bounded by `dates`, `instruments`, `horizons.size()`, `quantiles` or
  `bootstrap_draws`, each validated against a compile-time maximum at entry. No `while(true)`.
- The Lemire rejection loop carries an explicit retry cap of 64 and a counted fallback
  (§3.10) so it is statically bounded (JPL rule 2).
- `compute_cross_section_ic` is **pure** in `(in, cfg)`: same inputs → bit-identical result.
  `scratch` contents after the call are unspecified; re-use across calls is permitted and
  tested (§9.1 case 12).

### 6.2 Error codes — existing `atx::core::ErrorCode` only

| Code | Raised when |
| --- | --- |
| `InvalidArgument` | `Unknown` in any admission enum; `quantiles < 2` or `> kMaxIcQuantiles`; `min_names_per_date < 2`; `block_len_floor == 0`; horizons empty, not strictly increasing, containing 0, or larger than `kMaxIcHorizons`; non-finite or negative `trade_bps` / `annual_borrow_bps`; non-finite or non-positive `short_leg_gross`; `day_basis` not 360 or 365; `session_keys` not strictly increasing; `dates == 0` or `instruments == 0`; `common_sample_dates > dates`; **`bootstrap_draws == 0` while any configured horizon would otherwise be reportable** (C-7 item 3 — "intervals disabled" must be an explicit whole-run choice, never a silently degraded one); `SealPolicy::Unknown` |
| `OutOfRange` | any span size `!= dates * instruments` (or `!= dates` for `session_keys`); `horizons.back() >= dates`; `bootstrap_draws > kMaxBootstrapDraws`; `dates * instruments` overflowing `usize` |
| `PermissionDenied` | `apply_calendar_seal` under `RejectSealedV1` with `dates_at_or_after_sealed > 0` — the seal (§5.2) |
| `Internal` | scratch smaller than `plan_cross_section_ic` requires (a caller contract violation that would otherwise be UB); the trial-ledger chain break (atx-impl side) |

`Unknown`, `NotFound`, `AlreadyExists`, `Unavailable`, `NotImplemented`, `IoError`,
`ParseError`, `Cancelled` and `SwapFixingScheduleViolation` are not raised by this unit (M-12).
No new enumerator is added to `ErrorCode`.

### 6.3 House-style commitments

`[[nodiscard]]` on every function above. `const` on every parameter that is not mutated;
`CrossSectionIcScratch&` is the only mutable parameter. No `new`/`delete`. Rule of Zero on
every struct. `enum class` throughout, every `switch` exhaustive with no `default` so a new
enumerator is a compile error (`.agents/cpp/agent.md` §3). No narrowing: all `f64`/`usize`
conversions via `static_cast` at a single named site. 100-column limit.

---

## 7. atx-impl subcommand and output schemas

### 7.1 Subcommand

**Name: `equity-ic`.**

```text
atx-impl equity-ic --panel <identified context> --baseline-dir <baseline directory>
                   --out <fresh directory>
                   --evaluation-start YYYY-MM-DD --evaluation-end YYYY-MM-DD   (REQUIRED)
                   [--max-working-bytes <bytes>]
                   [--trial-ledger <path>]   (default atx-engine/reviews/trial-ledger.jsonl)
```

**`--evaluation-start` and `--evaluation-end` are required (ruling AR-8),** exactly as
`equity-baseline` requires them — `stage_equity_baseline.cpp:91-94` rejects an empty
`equity_evaluation_start` / `_end` outright. An omitted window would leave the evaluated date
range unpinned, and every number in this checkpoint depends on it. There is no default and no
derivation from the baseline recipe: the caller states the window and the stage then checks it
against the published baseline's own window, failing if they disagree.

**`equity-ic` accepts no `--config` (ruling AR-9).** The subcommand is **not** added to the
config-file merge predicate at `atx-impl/src/dispatch.cpp:109-111`, and `config` is **not** in
its allowed-flag set. The previous draft's merge-predicate edit is deleted: with `--config`
disallowed the branch would be unreachable dead code (`.agents/cpp/agent.md` §9), and with it
allowed a config file could inject book/optimizer fields that the copied programmatic-caller
guard would then have to reject. One decision, both sites agreeing.

Wiring, minimal and additive (line citations corrected per M-1):

- `atx-impl/src/config.hpp:17-19` — `kSubcommands` becomes `std::array<std::string_view, 13>`
  with `"equity-ic"` appended **last**, so no existing element's index moves. Its only consumer
  is a linear scan at `config.cpp:492`.
- `atx-impl/src/config.cpp` — one new flag `trial-ledger` in the same `if (flag == …)` chain as
  `baseline-dir` (`config.cpp:82-89`), setting a new `std::string equity_trial_ledger` field on
  `RunConfig` beside `equity_baseline_dir` (`config.hpp:461`). Every other flag it accepts
  already exists.
- `atx-impl/src/dispatch.cpp:123-134` — one routing line
  `if (sub == "equity-ic") return run_equity_ic(cfg);`, plus one `print_usage` block at
  `:56-66`. **No edit to the merge predicate at `:109-111`.**
- `atx-impl/CMakeLists.txt` — two source lines (§8, T4/T5).

Allowed-flag validation copies the `std::set<std::string> allowed` guard of
`stage_equity_baseline.cpp:69-73`: any flag outside the list is `InvalidArgument`, and the
programmatic-caller guard (`:75-79`) rejects book/optimizer fields set to non-defaults, so a
caller cannot silently request a different model. The allowed set is exactly
`{panel, baseline-dir, out, evaluation-start, evaluation-end, max-working-bytes, trial-ledger,
quiet, digest-only}` — note `config` is absent.

### 7.2 Reuse of `evaluate_equity_baseline`, unchanged

```text
read_panel_artifact(--panel, budget)                       [panel_artifact.hpp:71]
plan_equity_baseline(context, views_cfg)                   [equity_baseline_views.hpp:77]
  -> memory admission against --max-working-bytes, same arithmetic as
     stage_equity_baseline.cpp:314-325
evaluate_equity_baseline(context, views_cfg)               [equity_baseline_views.hpp:90]
  -> panel (close/raw_close/volume, masked), signals (2), session_keys, context_rows
read_panel_artifact(baseline-dir/evaluation.bin)
read_panel_artifact(baseline-dir/combo.bin)
require_panel_parent(combo.identity, "research", evaluation.artifact_id)
require_same_panel_axes(evaluation.identity, combo.identity)
require_same_panel_axes(evaluation.identity, <identity built from the fresh view>)
                                                           [mirrors stage_equity_book.cpp:187-194]
```

If the freshly computed view's axes or masked cell values disagree with the published
`evaluation.bin`, the run fails with `InvalidArgument` — the stage does not proceed on a panel
it cannot bind to the published baseline.

**Runtime assertion on the PCS membership inference.** §2.3's partition and §4.5's
`terminal_unevidenced_ids: [146189]` both rest on PCS being one of the 34 required-mark IDs.
The required-marks audit does not enumerate its 34 IDs in prose, so that membership is
**inferred**, not quoted. Before writing the partition into `request.json`, the stage therefore
asserts at runtime that ID `146189` is present in the required-mark ID set it loaded from
`C:/atx/data/equity_source_reconciliation_2013_20260919`, and **fails the run** with
`InvalidArgument` if it is not — rather than publishing a partition built on an unverified
inference. The same check verifies the set's cardinality is 34 and that the three evidenced IDs
`37648`, `35715`, `39970` are members. A failure here is a data-binding defect to investigate,
not a reason to soften the partition.

### 7.3 Outputs (all under `--out`, published by hard link, partials removed)

| File | Contents |
| --- | --- |
| `request.json` | recipe (§4 frozen values verbatim), input artifact IDs/digests, the audit terminal-evidence table with its source line references, producer executable SHA-256, trial-ledger `trial_id` and its pre-registration line SHA-256 |
| `seal.json` | `SealReport` (§5.2) plus the literal statement of §5.2's "non-vacuous by code, vacuous by data" for a 2013-only panel |
| `coverage.csv` | `date_index,session_key_ns,signal,horizon,variant,restriction,n_eligible,n_signal_finite,n_with_forward,n_dropped_missing_forward,n_terminal_applied,n_terminal_unevidenced,n_excluded_audited,n_used,in_common_sample` |
| `ic.csv` | `date_index,session_key_ns,signal,horizon,variant,restriction,n_used,pearson_ic,rank_ic,spread_gross,spread_net,days_forward,borrow_drag,cost_drag,emitted,spread_emitted,in_common_sample` |
| `ic_summary.json` | per `(signal,horizon,variant,restriction)`: `block_len`, `dates_below_min_names`, `dates_below_quantile_count`, and **two** `IcSampleStats` blocks keyed `"full"` and `"common"`, each carrying `dates_emitted`, `summary_reportable`, `ic_mean/sd/icir/naive_t`, `naive_t_validity`, the rank equivalents, `spread_gross_*`, `spread_net_*` and all six `BootstrapInterval`s with their `unreportable_reason` |
| `ic_decay.csv` | `signal,variant,restriction,horizon,block_len,ic_mean,ic_lo,ic_hi,rank_ic_mean,rank_lo,rank_hi,reportable,ic_mean_common,ic_lo_common,ic_hi_common,rank_ic_mean_common,rank_lo_common,rank_hi_common,reportable_common` — one row per point, full and common sample side by side (§3.13) |
| `signal_autocorr.csv` | `signal,lag,pairs_emitted,rho_pearson,rho_rank,implied_one_way_turnover,turnover_source,interpretation` — `turnover_source` is the literal `rho_rank` (ruling AR-5) |
| `quantile_spread.csv` | bucket rows `signal,horizon,variant,restriction,quantile,n_dates,mean_names,mean_forward_return`; spread rows `…,quantile=SPREAD,gross_mean,gross_sd,gross_lo,gross_hi,net_mean,net_sd,net_lo,net_hi,decile_one_way_turnover,trade_drag,mean_borrow_drag,mean_cost_drag,mean_days_forward,gross_mean_common,net_mean_common,cost_model_provenance,borrow_day_convention` |
| `manifest.json` | file list with SHA-256 and byte sizes, all parent artifact IDs, `runtime_seconds`, `peak_working_set_bytes`, `alignment`, `cost_model_provenance`, `borrow_day_convention`, the §12 qualification strings |

`restriction` is the literal `full` or `ex34` (§3.8, ruling AR-7); every statistic appears once
per restriction. `variant` is `DropMissingForward` or `IncludeAuditedTerminalV1`.

**`decile_one_way_turnover` column scale (NEW-4).** It is measured at the gross-2.0 book's
weights (`±1.0/n`, ruling AR-11), so it ranges over **`[0, 2]`** and a **complete** decile
turnover reads **`2.0`**, not `1.0`. The header comment row of `quantile_spread.csv` carries
that sentence verbatim. It is **not** comparable to `signal_autocorr.csv`'s
`implied_one_way_turnover`, which is `1 - rho_rank` — a different quantity on a different
scale (§3.11, §12.9).

### 7.4 Determinism rules for the writers

Classic `"C"` locale. Floating point via `std::to_chars` shortest round-trip, the same
`number()` idiom as `stage_equity_baseline.cpp:41-45`. LF endings, no BOM, no trailing
whitespace, fixed column order, fixed row order (`signal` outer, `horizon`, `variant`, then
ascending date). No iteration over an unordered container anywhere in the writers. Two runs of
the same binary on the same inputs must produce byte-identical files; §9.3 pins it.

Null encoding: an unreportable `BootstrapInterval` serializes as JSON `null` for `lo`/`hi` and
the literal string `""` in CSV — never `0`, never `NaN`.

### 7.5 Byte-identity obligation for every other subcommand

`load`, `panel`, `discover`, `combine`, `optimize`, `report`, `equity-baseline`, `equity-book`,
`run`, `regime`, `sweep`, `metabook` must produce byte-identical **stage output** to today. The
only edits to shared files are: one appended `kSubcommands` element, one new `if (flag == …)`
branch (reached only by a flag name that is currently rejected), one routing line, one
usage-text block, two CMake source lines. No existing function signature, struct field or
default value changes, and **no merge-predicate edit** (ruling AR-9). §9.4 pins it with a
digest comparison.

**The `--help` banner does change, by design** (one added subcommand line and one added flag
block in `print_usage`). That is outside the byte-identity claim, which covers stage outputs and
digest lines. Verified: nothing under `atx-impl/tests/` references `print_usage`, the usage
banner or `kSubcommands`, so no existing assertion breaks (M-15). §9.4 item 3 records this as
checked, not as a live risk.

---

## 8. Task breakdown

Implementers are **edit-only**. No implementer runs `cmake`, `ninja`, `ctest` or
`scripts/atx-build.ps1`; all native action is parent-only (handoff, "Native
configure/check/build/test/run actions are **parent-only**").

### T1 — engine: `cross_section_ic` types, validation, seal

**Files.** New `atx-engine/include/atx/engine/eval/cross_section_ic.hpp`; new
`atx-engine/src/eval/cross_section_ic.cpp`; edit `atx-engine/CMakeLists.txt`.

**CMake, by content anchor — never by line number** (I-7; checkpoint 13 is mid-flight in
`atx-engine/src/book/*` and may shift every line below its own insert). In
`atx-engine/CMakeLists.txt`, insert exactly one line

```cmake
    src/eval/cross_section_ic.cpp
```

**immediately after the line whose content is `    src/eval/cpcv.cpp`**, keeping the `eval`
block alphabetical (`cpcv`, `cross_section_ic`, `pbo`, `perf_metrics`). If that anchor line is
absent or appears more than once, stop and report rather than guessing. No other CMake edit;
the `eval` test group is globbed with `CONFIGURE_DEPENDS` and `eval` is already in
`ATX_ALL_TEST_GROUPS` (`atx-engine/tests/CMakeLists.txt:14-15`), so the test file needs none.

**Delivers.** All types of §6; `plan_cross_section_ic`; `apply_calendar_seal`; every §6.2
validation branch. `compute_cross_section_ic` present but returning
`Err(InvalidArgument, "not yet implemented")` is **not** acceptable — T1 ships a compiling
declaration and T2 ships the body; the two are one review unit.

**Size.** ~330 lines header, ~120 lines source. **Acceptance.** Compiles under
`/W4 /permissive- /WX`; every enum `switch` exhaustive; no allocation outside
`plan_cross_section_ic`.

### T2 — engine: IC, rank IC, coverage, autocorrelation

**Files.** `atx-engine/src/eval/cross_section_ic.cpp` (same file as T1).

**Delivers.** §3.3 forward returns including both variants, §3.4 Pearson, §3.5 rank IC with the
scratch `rank` overload, §3.7 counters, §3.9 summaries, §3.11 autocorrelation.

**Size.** ~280 lines. **Acceptance.** The `h = 0`-equivalent case reproduces
`learn::detail::oof_ic_series` semantics (`linear_alpha.cpp:119-147`) on an equivalent input —
same `>= 2` gate, same ascending single walk, same constant-series `0.0`; pinned by §9.1
case 2.

### T3 — engine: bootstrap and quantile spread

**Files.** `atx-engine/src/eval/cross_section_ic.cpp` (same file).

**Delivers.** §3.10 circular block bootstrap with the Lemire draw and the reportability rule;
§3.12 quantile assignment, gross spread, turnover and the net cost restatement.

**Size.** ~220 lines. **Acceptance.** Two runs with the same seed are bit-identical; two
different seeds differ; `reportable == 0` yields untouched `lo`/`hi`; `modulo_fallbacks == 0`.

### T4 — atx-impl: append-only hash-chained trial ledger

**Files.** New `atx-impl/src/trial_ledger.hpp`, `atx-impl/src/trial_ledger.cpp`; new
`atx-impl/tests/trial_ledger_test.cpp`; edit `atx-impl/CMakeLists.txt`.

**CMake, by content anchor** (I-7). In `atx-impl/CMakeLists.txt`, insert
`    src/trial_ledger.cpp` **immediately after the line whose content is
`    src/stage_equity_book.cpp`** (the next existing line is `    src/stage_run.cpp`). Stop and
report if the anchor is missing or duplicated. The test file needs no CMake edit
(`atx-impl/tests/CMakeLists.txt:1-3` globs `*_test.cpp` with `CONFIGURE_DEPENDS`).

**Delivers.** §4.5 schema, `append_trial`, `verify_trial_ledger`, the chain check, the sidecar
manifest. Opens with `std::ios::app` only; `std::ios::trunc` must not appear in the file.

**Size.** ~260 lines + ~200 lines of test. **Acceptance.** A hand-corrupted middle line fails
`verify_trial_ledger` with `Internal`; `append_trial` refuses to write after a break; two
appends produce a valid two-link chain.

### T5 — atx-impl: `equity-ic` stage

**Files.** New `atx-impl/src/stage_equity_ic.hpp`, `atx-impl/src/stage_equity_ic.cpp`; edit
`atx-impl/src/config.hpp` (one `kSubcommands` element + one `RunConfig` field),
`atx-impl/src/config.cpp` (one flag branch), `atx-impl/src/dispatch.cpp` (one routing line, one
usage block — **no merge-predicate edit**, ruling AR-9), `atx-impl/CMakeLists.txt`.

**CMake, by content anchor** (I-7). Insert `    src/stage_equity_ic.cpp` **immediately after the
line whose content is `    src/trial_ledger.cpp`** (added by T4), giving the order
`stage_equity_book.cpp`, `trial_ledger.cpp`, `stage_equity_ic.cpp`, `stage_run.cpp`.

**Delivers.** §7.2 reuse chain including the **runtime assertion that ID `146189` (PCS) is a
member of the loaded 34-ID required-mark set, that the set's cardinality is 34, and that
`37648` / `35715` / `39970` are members — failing the run with `InvalidArgument` otherwise,
before the partition is written to `request.json`**; §7.3 writers (including the `_ex34` and
`_common` columns, and the `decile_one_way_turnover` `[0, 2]` scale note in the CSV header
comment); §7.4 determinism; the audit evidence table with record dates; the pre-registration
and completion ledger appends to `atx-engine/reviews/trial-ledger.jsonl`; `seal.json`. The trade
constant is taken as `constexpr EquityAllocationConfig{}.trade_bps` (ruling AR-10) — a
compile-time reference that **edits no file**; `equity_allocation.hpp` is included, not
modified.

**Size.** ~700 lines. **Acceptance.** `evaluate_equity_baseline` is called with an unmodified
signature; `git diff --stat` shows zero lines changed in `equity_baseline_views.{hpp,cpp}`,
`stage_equity_baseline.cpp`, `stage_equity_book.cpp`, `equity_allocation.cpp`, `replay.cpp`,
`replay.hpp`, `claims_state.cpp`, `book_claims_replay_test.cpp`.

### T6 — tests

**Files.** New `atx-engine/tests/eval/eval_cross_section_ic_test.cpp`; new
`atx-impl/tests/config_equity_ic_test.cpp`; new `atx-impl/tests/stage_equity_ic_test.cpp`.

**Placement check, mandatory** (`.agents/cpp/agent.md` §7): the engine test goes in
`atx-engine/tests/eval/` — a mistyped directory is silent and the test simply never builds.
Prefix `eval_` matches the folder, as `eval_lockbox_test.cpp` and `eval_breadth_test.cpp` do.

**Delivers.** §9.1, §9.2 marker emission, §9.4.

**Size.** ~650 lines total. **Acceptance.** Every case in §9.1 present and named
`Subject_Condition_ExpectedResult`.

### T7a — independent exact oracle and comparator

Split from the previous single T7 (M-14): ~900 lines across three files with the least
specification in the set was the largest and loosest task; the oracle and the comparator are
also the two pieces that must stay **independent of each other's authorship discipline**.

**Files (names frozen by ruling A-9).** New
`build-equity/audits/iteration14_cross_section_oracle.py` — **not** `…_cross_section_ic_oracle.py`
— writing `build-equity/audits/iteration14-cross-section-oracle.json` (the exact
`Fraction`/`Decimal` document writer, §9.2); new
`build-equity/audits/iteration14_native_comparator.py` (decodes the marker lines, recomputes
from the native inputs, compares against the oracle, emits the verdict JSON).

**Size.** ~640 lines Python. **Acceptance.** Both import **nothing outside the standard
library**; the oracle is written **without reading** `cross_section_ic.cpp` or
`stage_equity_ic.cpp`; all text I/O passes `encoding="utf-8"` explicitly (handoff: "Use UTF-8
explicitly in Python text I/O"); the oracle covers the five scenario families of §9.2 including
the terminal leg, the coverage counters, the net cost drag and the first-8-draws bootstrap
sequence, each with its own declared bound.

### T7b — parent runner and documentation

**Files.** New `build-equity/audits/iteration14_run_ic_checks.py` (asserts output paths do not
exist, hashes sources and executables before and after, appends the pre-registration ledger
line, invokes the wrapper's serial CTest, preserves the first attempt, and **wires the
comparator to `iteration14-cross-section-oracle.json`** — the A-9 name, not the superseded
`…-cross-section-ic-oracle.json`); edit
`atx-engine/docs/PLATFORM_PROGRESS.md` (new checkpoint-14 section, existing entries retained);
edit `atx-impl/README.md` and `atx-engine/README.md` subcommand lists.

**Size.** ~260 lines Python + ~60 lines docs. **Acceptance.** The runner never overwrites an
existing artifact and versions a retry as `…-attempt2` (§10.3); the docs entry carries the
§10.5 statement verbatim and claims nothing beyond it.

### Dependency order

`T1 → T2 → T3` (same file, sequential, one owner). `T4` is independent of T1-T3 and may run in
parallel. `T5` depends on T1-T4 (which also serializes the shared `atx-impl/CMakeLists.txt`
edit). `T6` depends on T1-T5. `T7a` depends on T6 for the marker format only and may be drafted
in parallel from this document. `T7b` depends on T7a for the comparator's invocation contract.

---

## 9. Test plan

### 9.1 Engine unit tests — `atx-engine/tests/eval/eval_cross_section_ic_test.cpp`

Synthetic panels, hand-computable, no file I/O, no clock, no RNG outside the seeded bootstrap.

| # | Case | Assertion |
| --- | --- | --- |
| 1 | `PearsonIc_PerfectlyOrderedCrossSection_ReturnsOne` | 4 names, `r` a strictly increasing affine image of `x` → `pearson_ic == 1.0` to `1e-15`; `rank_ic == 1.0` exactly |
| 2a | `Pearson_FixedVector_IsBitIdenticalToLearnDetailPearson` | **the R-6 obligation.** `EXPECT_EQ` (exact `f64` equality, no tolerance) between `eval::detail::pearson(a,b)` and `learn::detail::pearson(a,b)` on a fixed hand-written pair of vectors, plus the `n < 2` and constant-series inputs. This is a pure two-function comparison and is the **only** case that includes a `learn/` header; it needs no `FeatureMatrix` |
| 2b | `PearsonIc_MatchesOofIcSeriesSemantics_OnEquivalentInput` | **optional semantic check, not the R-6 obligation.** Single horizon, all cells covered → the emitted series equals what `learn::detail::oof_ic_series` produces for the same cross-sections (`linear_alpha.cpp:119-147`): same `>= 2` gate, same ascending walk, `EXPECT_EQ` element-wise. Constructing a `FeatureMatrix` inside an `eval`-group TU is the cost; if it proves to pull too much of `learn/` in, drop this case and keep 2a — 2a alone discharges R-6 |
| 3 | `PearsonIc_ConstantSignalCrossSection_ReturnsZeroNotNan` | `va == 0` branch (`latent.hpp:175-177`) |
| 3b | `Pearson_FewerThanTwoNames_ReturnsZeroNotNan` | the `n < 2U` early return (I-1) |
| 4 | `RankIc_TiedSignalValues_AveragesRanks` | `x = {1,1,2,2}` → ranks `{0.5/3, 0.5/3, 2.5/3, 2.5/3}` exactly (`cross_section.hpp:123-136`) |
| 5 | `RankIc_OneUlpApart_AreDistinctRanks` | bitwise tie rule of §3.5 |
| 6 | `Date_WithFewerThanMinNames_IsNotEmittedAndIsCounted` | `emitted == 0`, `dates_below_min_names == 1` |
| 7 | `ForwardReturn_MissingTerminalMark_DropVariantDropsAndCounts` | `n_dropped_missing_forward == 1`, `n_used` reduced |
| 8 | `ForwardReturn_AuditedEvidencedTerminal_AppliesDeclaredLeg` | §3.8 formula, exact against a hand value; `t_last` resolved inside `(t, t+h]` only (M-8) |
| 8a | `TerminalLeg_ObservationAtOrBeforeRecordDate_IncludesSpecialDividend` | DELL shape: `session_keys[t] <= 2013-10-28` → `(13.75 + 0.13)/13.86 - 1` (ruling AR-2) |
| 8b | `TerminalLeg_ObservationAfterRecordDate_ExcludesSpecialDividend` | `session_keys[t] == 2013-10-29` → `13.75/13.86 - 1`; both sides of the boundary pinned |
| 8c | `TerminalLeg_LiveCloseAtTPlusH_UsesOrdinaryReturnAndAppliesNoLeg` | **the A-6 precedence rule.** A terminal-flagged, evidenced ID that *still has* a finite positive `P(t+h)`: the emitted return is exactly `P(t+h)/P(t) - 1`, `n_terminal_applied` is unchanged (the cell counts in `n_with_forward`), and the consideration is not added. Pins that the terminal leg is a fallback for an undefined ordinary return, never an overlay on a live price |
| 8d | `TerminalLeg_NoValidMarkInsideForwardWindow_DropsAsMissing` | `t_last` search bounded by `(t, t+h]`, not by the panel |
| 9 | `ForwardReturn_AuditedUnevidencedTerminal_DropsAndCountsSeparately` | `n_terminal_unevidenced == 1`, no return invented — the PCS shape (ruling AR-1) |
| 10 | `ForwardReturn_ZeroOrNegativePrice_IsMissingNotRepaired` | both endpoints tested |
| 11 | `Admission_NameEligibleAtTButNotAtTPlusH_IsStillUsed` | pins the no-forward-eligibility rule of §3.3 |
| 12 | `Compute_ReusedScratch_IsBitIdenticalToFreshScratch` | purity under scratch reuse |
| 13 | `Bootstrap_SameSeed_IsBitIdentical` / `DifferentSeed_Differs` | determinism |
| 13b | `Bootstrap_FirstEightDrawStarts_MatchThePinnedRecipe` | the exact §3.10 stream: the six-field `stream_key` bit layout, splitmix64 return-value convention, `(0ULL - n) % n` threshold, Lemire high-half result, one 64-bit output per attempt. **Two hard-coded index sets, for the tuples `(statistic_id=IcMean, horizon_index=0, signal_index=0, variant_id=0, restriction_id=0, sample_id=0)` and the same with `sample_id=1`** — naming them is what makes the oracle able to mirror them (§9.2 F6) and what proves the two samples draw from different streams |
| 14 | `Bootstrap_SeriesShorterThanTenBlocks_IsNotReportable` | `reportable == 0`, `unreportable_reason == 3` (`"series-too-short-for-block-length"`, §3.10's frozen enum), `lo`/`hi` untouched. Companion sub-cases pin the rest of the enum and its precedence: `n < 20` alone → `1`; `n < 20` **and** `floor(n/L) < 10` → `1` (lowest nonzero wins); a common block with a prefix gap **and** a short series → `2` |
| 14b | `Bootstrap_ZeroDrawsWithReportableHorizon_ReturnsInvalidArgument` | the C-7 item 3 UB branch is rejected at validation, never reached |
| 15 | `Bootstrap_ModuloFallbackCounter_IsZero` | the §3.10 bounded-loop counter |
| 16 | `QuantileSpread_ExactlyQNames_AssignsOnePerBucket` | `floor(p*Q/n)` boundary |
| 17 | `QuantileSpread_FewerThanQNames_EmitsNoSpreadAndCounts` | boundary |
| 18 | `QuantileSpread_TiedSignal_BreaksByAscendingInstrumentIndex` | §3.12 step 2 |
| 19 | `QuantileSpread_NetIsGrossMinusPerDateDrag` | **per-date** (ruling AR-3): for every emitted `t`, `net_h(t) == gross_h(t) - cost_drag_h(t)` exactly, with `cost_drag_h(t) == trade_drag_h + borrow_drag_h(t)`, `trade_drag_h == (trade_bps/1e4)*2*decile_one_way_turnover` (constant), `borrow_drag_h(t) == (annual_borrow_bps/1e4)*(days_h(t)/365.0)*short_leg_gross` with **`short_leg_gross == 1.0`** (ruling AR-11). Also asserts `net.series.size() == gross.series.size()` and `spread_net_ci.reportable == spread_gross_ci.reportable` |
| 19b | `QuantileSpread_CompleteDecileTurnover_CostsTwentyBps` | **the AR-11 scale check.** A 4-name, 2-date fixture with `Q = 2` and a complete turnover of both legs: weights `±1.0/n` give `Σ_i|w_i(t) - w_i(t-h)| == 4.0` exactly, `oneway == 2.0`, `decile_one_way_turnover == 2.0`, and `trade_drag == (5.0/1e4)*2*2.0 == 0.0020` (20 bps). A companion assertion on a half-turnover fixture gives 10 bps. This is the case that would have caught the gross-1.0/gross-2.0 mismatch |
| 19c | `BorrowDays_FridayToMonday_IsThreeCalendarDays` | session keys at midnight UTC for Fri 2013-04-05 and Mon 2013-04-08, `h = 1` → `days_forward == 3`, and `borrow_drag == (365.0/1e4)*(3/365.0)*1.0` exactly (`short_leg_gross == 1.0`, AR-11). A companion Tue→Wed pair asserts `days_forward == 1`, and a three-session-gap pair (a holiday weekend) asserts `4`. Pins that the day count comes from the calendar, not from `h` |
| 19d | `BorrowDays_LongHorizonSpansWeekends_ExceedsHorizonCount` | `h = 5` across a full week → `days_forward == 7 > 5`, and `borrow_drag == (365.0/1e4)*(7/365.0)*1.0`; the observation-count convention is provably not in use |
| 20 | `Autocorr_IdenticalConsecutiveCrossSections_RhoIsOneTurnoverZero` | §3.11 |
| 20b | `Autocorr_TurnoverIsDerivedFromRankNotPearson` | construct a fixture where `rho_pearson != rho_rank`; assert `implied_one_way_turnover == 1 - rho_rank` (ruling AR-5) |
| 20c | `CommonSample_EveryPrefixDateEmits_AllHorizonsShareOneSample` | §3.13 / ruling R-B: on a fixture that **guarantees every date emits at every horizon**, `in_common_sample` is 1 exactly on `[0, T - max(H))`, every horizon's `common.dates_emitted` is equal to `common_sample_dates`, and every `common_prefix_gaps == 0`. The equality to the prefix is asserted *under that fixture guarantee*, not as a general law |
| 20d | `CommonSample_OnePrefixDateFailsToEmit_VoidsTheWholeCommonBlock` | the same fixture with one prefix date's coverage dropped below `min_names_per_date`: that date's `in_common_sample` stays 1 (it is in the prefix) but it emits at no horizon, so **every** horizon's `common.dates_emitted` falls by exactly 1, `common_prefix_gaps == 1` for all five, `common.summary_reportable == 0`, `unreportable_reason == 2`, and — per NEW-1 — **every** `*_common` field serializes as null/`""`, `ic_mean_common` and `spread_*_mean_common` included, while the full-sample block is untouched. A companion sub-case drops a date to `min_names_per_date <= n < Q` (IC emits, spread does not) and asserts the **same** whole-block void: one counter, one outcome, no spread-only path |
| 20e | `Ex34Restriction_ExcludesNamedIdsAndCountsThem` | §3.8 / AR-7: `n_excluded_audited` per date, and the restricted statistics differ from the unrestricted ones on a fixture where an excluded name carries signal |
| 21 | `Seal_DateAtOrAfterSealedBoundary_ReturnsPermissionDenied` | `PermissionDenied`, count in `SealReport` |
| 22 | `Seal_UnknownPolicy_ReturnsInvalidArgument` | no default policy |
| 23 | `Seal_AllDatesBeforeBoundary_ReportsZeroCountsAndSucceeds` | the Stage-1 shape |
| 24 | `Config_UnknownVariantOrTieHandling_ReturnsInvalidArgument` | rejectable enums |
| 25 | `Config_MisSizedSpan_ReturnsOutOfRange` | each span, one case each |
| 26 | `Config_NonMonotonicSessionKeys_ReturnsInvalidArgument` | |
| 27 | `Config_HorizonAtOrPastDates_ReturnsOutOfRange` | |
| 28 | `Compute_UndersizedScratch_ReturnsInternal` | the caller-contract branch |
| 29 | `Summary_SinglePointSeries_IsUnreportableNotNan` | `n == 1` → `summary_reportable == 0`; `ic_sd`, `icir`, `naive_t` are exactly `0.0` and flagged, never NaN (I-11). A companion `n == 0` case asserts the same |

### 9.2 Oracle comparator — the checkpoint-12 pattern

Mirrors `build-equity/audits/iteration12_native_comparator.py` (handoff, "The export prefix is
`SECURITY_TRANSITION_MEASUREMENT `…").

**Native side.** A dedicated `TEST` emits, to stdout, one line per oracle scenario:

```text
CROSS_SECTION_IC_MEASUREMENT {"schema":"atx-cross-section-ic-measurement-v1","case_id":"…",
  "inputs":{…},"values":{"pearson_ic":…,"rank_ic":…,"spread":…,"ic_mean":…,"icir":…},
  "residuals":{…}}
```

**Mandatory `inputs` fields on every line (ruling A-5).** `inputs` always carries **`n`** (the
series or cross-section length the case was computed over) and **`sample_id`** (`0` full, `1`
common). Every bootstrap-bearing case additionally carries `statistic_id`, `horizon_index`,
`signal_index`, `variant_id`, `restriction_id`, `block_len` and `bootstrap_seed`, so the
emitted line reconstructs its own §3.10 stream key without the comparator having to infer one.
A line missing `inputs.n` or `inputs.sample_id` fails the comparison rather than being
best-effort matched.

Classic locale, `max_digits10`, no JSON dependency, no file I/O — identical constraints to
checkpoint 12. CTest verbose output may prepend a test number, so the comparator locates the
prefix before decoding.

**Oracle side.** `iteration14_cross_section_oracle.py` (ruling A-9), writing
`iteration14-cross-section-oracle.json`, **stdlib only** (`fractions.Fraction`
for IC and spread, `decimal.Decimal` where a square root is needed with a stated precision),
written without reading the C++ source. It computes, on a small synthetic panel (6 dates x 8
names, hand-specified rationals):

**Five scenario families, each with its own declared bound** (I-10). The previous draft covered
only the first; the three riskiest pieces of arithmetic — the terminal leg, the coverage
counters and the net cost — had no oracle at all.

| Family | What the oracle computes exactly | Declared bound |
| --- | --- | --- |
| **F1 — IC family** | Pearson IC per date (exact `Fraction` for `cov`, `va`, `vb`, then one `Decimal.sqrt` at 50 digits); rank IC per date from exact averaged ranks; `ic_mean`, `ic_sd`, `icir` over the date series | `64 * DBL_EPSILON * max(|actual|, |expected|, leg_scale)`, `leg_scale = sqrt(va*vb)` |
| **F2 — quantile spread** | spread with `Q = 4` on an 8-name cross-section (exactly divisible), exact `Fraction` bucket means and difference | `64 * DBL_EPSILON * max(|actual|, |expected|, max_bucket_mean_abs)` |
| **F3 — terminal leg (§3.8 / AR-2)** | `(P(t_last)/P(t)) * (1 + (C + s)/raw(t_last) - 1) - 1` in exact `Fraction`, for four hand-built cells: at the record date, after it, an unevidenced ID, and no valid mark in `(t, t+h]` | `64 * DBL_EPSILON * max(|actual|, |expected|, price_ratio_abs * consideration_ratio_abs)` — legs, not zero |
| **F4 — coverage counters (§3.7)** | every counter as an **exact integer** from the same synthetic panel: `n_eligible`, `n_signal_finite`, `n_with_forward`, `n_dropped_missing_forward`, `n_terminal_applied`, `n_terminal_unevidenced`, `n_excluded_audited`, `n_used` | **exact equality** — integers have no tolerance |
| **F5 — net cost drag (§3.12 / AR-3 / AR-11)** | `turnover_h` from exact decile membership at the **gross-2.0 weights `±1.0/n`**, including one complete-turnover scenario whose exact answer is `Σ|Δw| = 4`, `oneway = 2`, `trade_drag = 20 bps`; `days_h(t)` as an **exact integer** from the fixture's session keys, including a Friday→Monday span; then `borrow_drag_h(t)` with `short_leg_gross = 1.0`, `cost_drag_h(t)` and `net_h(t) = gross_h(t) - cost_drag_h(t)` for every emitted `t`, all in `Fraction` | `days_h(t)`: **exact integer equality**. The drags and net: `64 * DBL_EPSILON * max(|actual|, |expected|, trade_drag + borrow_drag)` |

**Tolerance rule.** Bounds are derived from the contributing legs, never an absolute floor —
the checkpoint-12 rule ("Near-zero bridges/residuals need bounds based on contributing legs,
not an arbitrary relative test against zero", handoff §"Recommended immediate continuation" 2).
F4 is exact-integer and takes no bound at all.

**F6 — bootstrap reproducibility (AR-6, R-C).** The bootstrap statistics are **not** compared
numerically against the oracle. Instead the oracle reimplements §3.10's stream recipe from this
document plus `random.hpp:83`'s `Xoshiro256pp` definition (N-7: reading that header is permitted
— it is not the unit under test) — the six-field `stream_key` bit layout, `splitmix64_next`
advancing state first and returning the mixed value, `Xoshiro256pp` from that return value, and
the Lemire draw consuming one 64-bit output per attempt with `(0ULL - n) % n` as the threshold
and the high half as the result — and asserts the **first 8 draw start indices bit-exactly for
both sample ids**: the tuples `(IcMean, h_idx 0, sig 0, var 0, restr 0, sample 0)` and the same
with `sample 1`, against native cases `bootstrap_f6_sample0` / `bootstrap_f6_sample1` (engine
test 13b emits both). Asserting both is what verifies R-C's `sample_id` field actually
separates the streams.

**The F6 export must carry `inputs.n` and `inputs.sample_id` (ruling A-5).** Draw values depend
on the series length `n` through the Lemire bound, so a draw sequence is meaningless without
it. Each `CROSS_SECTION_IC_MEASUREMENT` line for an F6 case therefore includes, inside
`inputs`, at minimum:

```json
"inputs": {"n": 188, "sample_id": 0, "statistic_id": 0, "horizon_index": 0,
           "signal_index": 0, "variant_id": 0, "restriction_id": 0,
           "block_len": 5, "bootstrap_seed": 20260920}
```

and `values.draw_starts` as the first 8 indices. `n` is the fixture's actual series length —
the Stage-1 lengths are `n = 189 - h = 188` for `sample_id = 0` and `n_common = 126` for
`sample_id = 1` (§4.4), but a smaller synthetic engine fixture is permitted **provided the line
declares its own `n`**, because the oracle publishes auxiliary draw-start sets for a fixed
ladder of `n` values from the same stream and the comparator selects by the declared `n`. A
line without `inputs.n` or `inputs.sample_id` is a **comparator failure**, not a warning: the
comparison cannot be made auditable without them.

The comparator separately checks that each native interval brackets its point estimate, that
`reportable` and `unreportable_reason` match §3.10's frozen enum including its lowest-nonzero
precedence, and that `modulo_fallbacks == 0`.

**Binding.** The comparator hashes both the oracle JSON and the captured log, requires the
exact expected case-ID set, and records any oracle scenario not executed natively as **not
measured natively** — never as passing (research-agent discipline; checkpoint-12 precedent
"Do not claim all 14 oracle cases were executed natively").

### 9.3 Determinism of the stage writers

`atx-impl/tests/stage_equity_ic_test.cpp`: run the stage twice into two fresh directories over
a tiny synthetic `PanelArtifact` fixture; assert every emitted file is byte-identical and every
`manifest.json` SHA-256 matches. Assert an unreportable interval serializes as `null` in JSON
and `""` in CSV.

### 9.4 Byte-identity of untouched subcommands

1. `config_equity_ic_test.cpp`, asserting **only what the codebase actually does** (N-8).
   Verified: per-subcommand allowed-flag guards exist in exactly two places —
   `atx-impl/src/stage_equity_baseline.cpp:69` and `atx-impl/src/stage_equity_book.cpp:100`.
   The other ten subcommands (`load`, `panel`, `discover`, `combine`, `optimize`, `report`,
   `run`, `regime`, `sweep`, `metabook`) have **no per-flag guard**, so an unknown-but-parseable
   flag is accepted and ignored there. The test therefore asserts:
   (a) `parse_args` accepts `equity-ic` with its documented flags and `equity-ic` **adopts the
   same rejection discipline** as those two stages — its own allowed set rejects every flag
   outside its list, including `config`;
   (b) `equity-baseline` and `equity-book` continue to reject `trial-ledger` through their
   existing guards;
   (c) the other ten subcommands are **unaffected** and are not asserted to reject anything —
   writing that assertion would produce a test that cannot pass and would silently reinterpret
   the requirement;
   (d) the regression that matters: the parsed `RunConfig` for each of the twelve existing
   subcommands is field-for-field unchanged against the frozen expectations already pinned by
   `config_baseline_test.cpp`, `config_equity_book_test.cpp`, `config_replay_test.cpp`.
2. **Parent-run digest comparison — scheduled, not merely asserted** (I-6). Re-run
   `equity-baseline` on the frozen 2013 context into a fresh directory and assert the emitted
   `[atx-impl] stage=equity-baseline digest=…` line and the published artifact IDs equal the
   recorded `57b7c21c…` / `def31e5c…` (handoff, "Baseline intermediates"). A mismatch blocks
   the checkpoint. This is parent-only work and appears as its own shell in §10.1 step 5, its
   own log in §10.3 (`iteration14-byte-identity-baseline.log`), and its own `byte_identity`
   block in the §10.4 receipt.
3. `cli_smoke_test.cpp` is left unedited. **Verified, not speculated** (M-15): nothing under
   `atx-impl/tests/` references `print_usage`, the usage banner or `kSubcommands`, so no
   existing assertion depends on the subcommand list or the help text. If an implementer finds
   otherwise, they report it rather than editing an assertion silently.

### 9.5 What is **not** tested and must not be claimed

No sanitizer, no clang-tidy, no clang-format, no include-clean `hygiene` build
(`.agents/cpp/agent.md` §8; handoff: "PCH is enabled; do not claim a hygiene/include-clean
build occurred"). No CI runs any of this (`.agents/cpp/agent.md` §8).

**Two shipped code paths are unreachable at Stage 1 and are therefore untested on real data**
(M-11): `SealPolicy::MaskSealedV1` and the `eval::reserve_window` routing of §5.2 item 6. Both
exist so the seal enum has a rejectable alternative rather than a silent one, and both are
covered only by synthetic unit cases. The receipt states this rather than implying the seal's
whole surface was exercised.

---

## 10. Measurement and receipt plan

### 10.1 Native sequence — parent only, serialized, fresh shell per group

No more than three wrapper invocations per PowerShell process (handoff: a fourth previously
made `vcvars64.bat` exit 255). Five shells, in this order. Check every exit code; do not
proceed on failure.

**Step 1 — configure. MANDATORY, FIRST, and in its own shell** (I-5). This is not conditional:

- `atx-engine` uses an **explicit** source list with no `CONFIGURE_DEPENDS`, so the new
  `src/eval/cross_section_ic.cpp` cannot enter the build without a reconfigure;
- the recorded checkpoint-12 configure used `-Groups 'risk;data;core;book'`, which **omits
  `eval`**, so the target `atx-engine-eval-tests` **does not exist** in the current build tree;
- the `check` verb resolves a source to an already-configured object target
  (`scripts/atx-build.ps1:203`: "no object target found for '$src' (is its target
  configured?)"), so configure must precede `check`, not follow it.

```powershell
Set-Location 'C:/atx/.worktrees/equity-platform'
$env:CCACHE_DISABLE='1'
& 'C:/atx/.worktrees/equity-platform/scripts/atx-build.ps1' configure `
  -Preset equity-dev -Groups 'risk;data;core;book;eval' `
  '-DVCPKG_MANIFEST_MODE=OFF' `
  '-DVCPKG_INSTALLED_DIR=C:/Users/natha/vcpkg/installed' `
  '-DFETCHCONTENT_BASE_DIR=C:/atx/.worktrees/equity-platform/deps/equity-dev'
```

`eval` is a valid group (`atx-engine/tests/CMakeLists.txt:14-15`); a bad value hits
`FATAL_ERROR` at `:48-50`.

**Step 2 — single-TU checks.** Fresh shell:

```powershell
Set-Location 'C:/atx/.worktrees/equity-platform'
$env:CCACHE_DISABLE='1'
& '…/scripts/atx-build.ps1' check `
  atx-engine/src/eval/cross_section_ic.cpp `
  atx-impl/src/trial_ledger.cpp -Preset equity-dev -Jobs 4
& '…/scripts/atx-build.ps1' check `
  atx-impl/src/stage_equity_ic.cpp -Preset equity-dev -Jobs 4
```

**Step 3 — target builds.** Fresh shell:

```powershell
& '…/scripts/atx-build.ps1' build atx-engine-eval-tests -Preset equity-dev -Jobs 4
& '…/scripts/atx-build.ps1' build atx-impl-tests atx-impl -Preset equity-dev -Jobs 4
```

**Step 4 — anchored tests only, never a full label sweep.** Fresh shell:

```powershell
& '…/scripts/atx-build.ps1' -Ctest -R '^CrossSectionIc\.' -Preset equity-dev
& '…/scripts/atx-build.ps1' -Ctest -R '^(TrialLedger|StageEquityIc|ConfigEquityIc)\.' -Preset equity-dev
```

**Step 5 — byte-identity regression on an untouched subcommand** (§9.4 item 2, I-6). Fresh
shell; output captured to `build-equity/audits/iteration14-byte-identity-baseline.log`:

```powershell
& 'build-equity/bin/atx-impl.exe' equity-baseline `
  --panel 'C:/atx/data/tickerhistory_training_native_20260919/context.bin' `
  --out   'C:/atx/data/equity_baseline_byteidentity_20260920' `
  --evaluation-start 2013-04-04 --evaluation-end 2014-01-01
```

Assert the `[atx-impl] stage=equity-baseline digest=…` line and the published artifact IDs equal
`57b7c21c…` (evaluation) and `def31e5c…` (combo). A mismatch blocks the checkpoint.

### 10.2 Real-data run

```powershell
& 'build-equity/bin/atx-impl.exe' equity-ic `
  --panel        'C:/atx/data/tickerhistory_training_native_20260919/context.bin' `
  --baseline-dir 'C:/atx/data/equity_baseline_training_2013_20260919' `
  --out          'C:/atx/data/equity_ic_training_2013_20260920' `
  --evaluation-start 2013-04-04 --evaluation-end 2014-01-01 `
  --trial-ledger 'atx-engine/reviews/trial-ledger.jsonl'
```

Both `--evaluation-start` and `--evaluation-end` are required (ruling AR-8); `--config` is not
accepted (ruling AR-9).

The pre-registration ledger line is appended **before** this command runs, by
`iteration14_run_ic_checks.py`, which also asserts the output paths do not already exist and
records source/executable hashes before and after (the checkpoint-12 runner contract).

Memory: the 189-observation baseline measured 165,318,656 bytes peak working set and 8.96 s
(research §B.2). This stage adds **137,104 bytes** of scratch (§6.1, which derives it) plus the result vectors and the
second and third `PanelArtifact` reads. Budget `--max-working-bytes 3000000000` as the baseline
does; the run records its own measured peak.

### 10.3 Audit artifacts — `build-equity/audits/iteration14-*`

```text
iteration14-ic-plan.json                  predeclared bounded plan (written before any code)
iteration14-check.log / -build.log        wrapper output
iteration14-tests.log                     ctest verbose, carries CROSS_SECTION_IC_MEASUREMENT lines
iteration14-wrapper-test-output.log
iteration14-tests.xml
iteration14-native-measurement.json       decoded marker lines
iteration14-cross-section-oracle.json     stdlib-only exact oracle document (ruling A-9)
iteration14-native-comparison.json        comparator verdict, per case, with bounds
iteration14-ic-run.stdout.log / .stderr.log
iteration14-ic-memory-samples.jsonl       100 ms working-set samples, as iteration 11/12 did
iteration14-byte-identity-baseline.log    §10.1 step 5: the equity-baseline re-run and its digest
iteration14-native-snapshot.zip           selected sources + the two executables + manifest
```

The trial ledger itself is **not** in this directory: ruling OQ-3 puts it at
`atx-engine/reviews/trial-ledger.jsonl`.

**Versioning, never overwriting** (handoff: "If a run fails, keep that evidence and version a
subsequent attempt; do not overwrite it"): a second attempt writes `…-attempt2.json` /
`…-attempt2.log` and the first attempt's files are left byte-unchanged, exactly as
`iteration12-native-measurement-attempt{2,3,4}.json` were.

### 10.4 Receipt

`atx-engine/reviews/2026-09-20-cross-section-ic-validation.json`. Required keys:

```json
{
  "checkpoint": 14, "stage": 1,
  "design_note_sha256": "<sha256 of this file>",
  "trial_ledger": {"path":"atx-engine/reviews/trial-ledger.jsonl","trial_id":"…",
                   "pre_registration_line_sha256":"…","completion_line_sha256":"…",
                   "head_sha256":"…","trial_count_declared":30},
  "inputs": {"context_artifact_id":"ec572b82…","baseline_evaluation_id":"57b7c21c…",
             "baseline_combo_id":"def31e5c…","producer_executable_sha256":"…"},
  "window": {"start":"2013-04-04","end_exclusive":"2014-01-01","observations":189},
  "seal": { … SealReport … , "statement":"non-vacuous by code; vacuous by data for a 2013-only panel",
            "untested_paths":["MaskSealedV1","reserve_window routing"]},
  "tests": {"suites":[…],"passed":n,"failed":0,"skipped":n,"log_sha256":"…"},
  "byte_identity": {"subcommand":"equity-baseline","digest_line":"…",
                    "evaluation_artifact_id":"57b7c21c…","combo_artifact_id":"def31e5c…",
                    "matched": true, "log_sha256":"…"},
  "oracle": {"families":["F1","F2","F3","F4","F5","F6"],
             "cases_total":n,"cases_compared_natively":m,
             "cases_not_measured_natively":[…],"comparison_sha256":"…"},
  "results": {"ic_summary_sha256":"…","coverage_sha256":"…","quantile_spread_sha256":"…",
              "ic_decay_sha256":"…","manifest_sha256":"…"},
  "predictions_confirmed": {"null_interval_horizons":[63],
                            "dates_below_min_names":0,"modulo_fallbacks":0},
  "terminal_evidence": {"required_mark_id_count":34,
                        "terminal_hypothesis_ids":[37648,35715,39970],
                        "evidenced_non_terminal_ids":[150340,351548],
                        "unclassified_id_count":29,
                        "terminal_unevidenced_ids":[146189],
                        "n_terminal_unevidenced_cells_measured": n,
                        "pcs_applied": false,
                        "pcs_statement":"PCS admission remains rejected; never applied"},
  "runtime": {"wall_seconds":…,"peak_working_set_bytes":…},
  "qualifications": [ … §12 verbatim … ],
  "acceptance": "sign-and-shape evidence only; not accepted alpha"
}
```

`predictions_confirmed` is written from the **observed** run, not copied from §4.4. If the
observed null-interval set differs from `[63]`, the receipt records the observed set and the
discrepancy is reported — a pre-registration whose prediction failed is evidence, not an error
to be hidden.

### 10.5 The 189-observation statement — mandatory, verbatim, in three places

In `manifest.json`, in the receipt, and in the `PLATFORM_PROGRESS.md` entry:

> A 189-observation result is **sign-and-shape evidence only**. It is not accepted alpha, not a
> Sharpe, not evidence of trading capacity, and not grounds for selecting a signal, a horizon,
> a cadence or a threshold. Research §D-1: "189 dates x roughly 200-400 admitted names is
> enough for a decay *shape* and a coverage table, and enough to expose a zero or negative IC —
> but it is a thin sample for an ICIR. Report the bootstrap interval, never a bare point
> estimate, and select nothing on it."

---

## 11. Rulings requested and open questions for the parent

Each carries the cost if the ruling is wrong.

**R-1. `equity-ic` as the subcommand name.** Requested: accept. Alternative considered:
`equity-signal-eval` (research §D-1 names the file `stage_equity_signal_eval.cpp`). `equity-ic`
matches the existing `equity-baseline` / `equity-book` prefix and is shorter.
**Cost if wrong:** a rename touching `config.hpp`, `dispatch.cpp`, one test and the docs; no
numerical effect.

**R-2. The seal is non-vacuous by code and vacuous by data at Stage 1, and `reserve_lockbox`'s
default terminal-fraction carve is deliberately NOT used.** Requested: accept §5.2. Carving the
terminal 20% of a 2013-only panel by index would reserve 2013 dates while the sealed period is
2023-2025, which would be a seal in name only. **Cost if wrong:** if the parent wants an
index-fraction carve as well, add one `reserve_lockbox(panel, frac, embargo)` call and one
receipt field; no result changes, because the visible region would then exclude late-2013 dates
and every statistic would be recomputed on a shorter series — i.e. a **new trial**.

**R-3. `N_14 = 30`, counting the forward-return variant as a trial axis.** Requested: accept.
The alternative is `N = 15` (variants as a disclosure, not a choice). **Cost if wrong:** if 30
is too conservative, every future deflated Sharpe is haircut slightly too hard — a
false-negative risk. If 15 were adopted and a reader later selected the favourable variant, the
deflation would be understated — a false-positive risk. The conservative direction is chosen
deliberately.

**R-4. The blend is read from the published `combo.bin`, so `--baseline-dir` is required.**
Requested: accept. **Cost if wrong:** if the parent prefers a self-contained run, the stage must
re-derive the blend via `extract_streams` + `WeightPolicy{}` + 0.5/0.5, duplicating
`stage_equity_baseline.cpp:341-352`; that is a second implementation of a deployed transform and
a divergence risk, and the run loses the `require_panel_parent` identity binding.

**R-5. Net decile spread restates the cost constants rather than calling replay code.**
Requested: accept, with the mandatory `cost_model_provenance` disclosure. `borrow_charge` is
file-local at `replay.cpp:251` and not declared in `replay.hpp`. **Cost if wrong:** if the
parent requires a genuine shared cost function, `replay.cpp`'s `borrow_charge` must be promoted
to the header — which edits a file checkpoint 13 is mid-flight in, and is forbidden by the
parent's own constraint. The alternative is gross-only reporting.

**R-6. The engine unit gets its own `eval::detail::pearson` rather than including
`learn/latent.hpp`.** Requested: accept. `learn/latent.hpp` pulls the whole latent/PCA surface
into an `eval` TU for one 25-line function, and `eval` must not depend on `learn`. The copy is
byte-for-byte the algorithm of `latent.hpp:151-178`, with a comment naming the source and the
reason. **Cost if wrong:** two definitions to keep in step; mitigated by §9.1 case **2a**,
which pins exact `f64` equality against `learn::detail::pearson` on a fixed vector. The same
rationale is applied to `quantile_sorted` and `splitmix64_next`, which are restated locally
rather than reached for across another module's `detail` namespace (M-2, §3.10).

**R-7. Bounded maxima `kMaxIcHorizons = 8`, `kMaxIcQuantiles = 32`,
`kMaxBootstrapDraws = 100'000`.** Requested: accept as declared placeholders sized for this
checkpoint, documented as such — the same treatment checkpoint 13's ruling 6 gave
`kMaxTransitionEvents = 64`. (`kMaxIcSignals` was proposed here and is **removed** as dead —
the API takes one signal span per call and bounds no signal count; M-4.) **Cost if wrong:** a
later study needs a constant bump and a re-measurement.

**R-8. `IncludeAuditedTerminalV1` applies only the evidenced 2013 events and drops the
remaining audited-terminal IDs into a separate counter.** Requested: accept. The audit forbids
assigning a return without evidence (`equity-required-marks-audit.md:19`).
**SUPERSEDED IN PART by AR-1 (§11.2):** this question was posed on the premise of **four**
evidenced events. The required-marks audit names **three** (HNZ, DELL, MOLX); PCS is not among
them and is never applied. **The count is also superseded:** ruling R-A (§11.3) re-issued the
partition as `3 + 2 + 29`, so the residual bias is the **29 unclassified IDs plus PCS**, not
the "31" this entry originally stated. **Cost if wrong:** the survivorship correction is
partial — which is why `n_terminal_unevidenced` is a first-class **measured** output rather
than a footnote, and why ruling AR-7 adds the `_ex34` restriction that actually bounds it. The
overstated-IC risk remains and is disclosed, not removed.

**Open question OQ-1 — is the `blend_equal` column's rank transform a confound for Pearson IC?**
Signal 2 is post-rank and post-winsorize, so its Pearson IC against a raw return is a
correlation between a bounded uniform-ish variable and a heavy-tailed one. This design reports
it anyway, beside the rank IC, and states the confound. If the parent wants a raw equal-average
signal as a fourth series, that is `+1` signal → `N_14 = 40` and a new ledger entry.
**Cost if unresolved:** the Pearson row for `blend_equal` is harder to interpret; the rank row
is not affected.

**Open question OQ-2 — should the `h = 63` horizon be dropped now that §4.4 predicts an
unreportable interval?** Keeping it preserves the decay curve's right tail with a disclosed
`null` interval; dropping it reduces `N_14` to 24. This design keeps it.
**Cost if unresolved:** one row of the decay curve carries a point estimate with no interval,
which a careless reader could over-read. Mitigated by the explicit `reportable` column.

**Open question OQ-3 — is the trial ledger's home `build-equity/audits/` durable enough?**
The handoff treats `C:\atx\build-equity\audits` as durable evidence, but the directory is a
build-adjacent path. The alternative is `atx-engine/reviews/trial-ledger.jsonl`.
**Cost if unresolved:** if `build-equity/` is ever cleaned, the chain's history is lost and the
declared `N` for every prior checkpoint becomes unverifiable — the exact failure the ledger
exists to prevent.

**Open question OQ-4 — Stage 2 sequencing.** Research §D-1 puts the 2013-2019 panel build at
roughly 4.4x the 445-date context's 1,026,482,176-byte peak, with relief available because
compaction "retains the union eligible **anywhere in the entire context, including warmup**"
and "An evaluation-only union can be smaller" (`annual-panel-memory-review.md:73-75`). Whether
Stage 2 is checkpoint 15 or follows the cadence work (§D-3) is a parent call.
**Cost if unresolved:** none now; Stage 1 is self-contained.

### 11.1 Parent rulings (2026-09-20, binding)

- **R-1** ACCEPT: subcommand name is `equity-ic`.
- **R-2** ACCEPT: seal is non-vacuous by code, vacuous by data at Stage 1; `reserve_lockbox`'s
  index-fraction carve is not used.
- **R-3** ACCEPT: `N_14 = 30`; the forward-return variant is a trial axis.
- **R-4** ACCEPT: `--baseline-dir` is required; the blend is read from the published `combo.bin`.
- **R-5** ACCEPT with one tightening: the restated trade cost must reference the constant declared
  in `atx-impl/src/equity_allocation.hpp` (it is atx-impl's own header), not a literal; the borrow
  rate may be a literal with a comment naming `replay.cpp:251` as its source. `cost_model_provenance`
  is mandatory.
- **R-6** ACCEPT: copy `pearson` into `eval::detail`; §9.1 case 2 must pin bit-equality against the
  `learn` path on a fixed vector.
- **R-7** ACCEPT: bounded maxima as declared placeholders.
- **R-8** ACCEPT: only the evidenced 2013 terminal events are applied; the rest are counted in
  `n_terminal_unevidenced`. **SUPERSEDED IN PART by AR-1 below:** the evidenced set is **three**
  (HNZ, DELL, MOLX), not four. R-8 was ruled on the false premise that PCS was evidenced.
- **OQ-1** RULED: keep the three pre-registered signals, `N_14 = 30`; do not add a raw equal-average
  signal; state the Pearson confound for `blend_equal` in §12.
- **OQ-2** RULED: keep `h = 63` with the predicted `null` interval; `N_14 = 30` stands.
- **OQ-3** RULED: the trial ledger lives at `atx-engine/reviews/trial-ledger.jsonl` (the tracked
  reviews directory that holds the receipts), not under `build-equity/audits/`. Every reference in
  this design to `build-equity/audits/trial-ledger.jsonl` is superseded by this ruling.
- **OQ-4** RULED: Stage 2 (2013-2019 panel) is deferred to a follow-on checkpoint; sequencing
  against the cadence work is decided after the Stage 1 receipt.

### 11.2 Parent rulings on the design review (2026-09-20, binding)

Issued after the independent design review (`.superpowers/sdd/equity-platform-parent-goal/
cp14-design-review.md`, verdict **Fix required**: 8 Critical, 12 Important, 15 Minor). Each is
applied throughout the body; the pointer says where.

- **AR-1 — three evidenced terminal events, PCS never applied.** The evidenced set is HNZ
  `37648`, DELL `35715`, MOLX `39970`. PCS goes to `n_terminal_unevidenced`, which becomes **31
  IDs**. R-8 is re-ruled accordingly. Applied: §2.3 (evidence table + the explicit "PCS is never
  applied" paragraph), §3.8, §4.3, §4.5 `source_exclusions`, §9.1 case 9, §10.4
  `terminal_evidence`, §12.4, §12.11. **Cost if wrong:** the survivorship correction covers
  three IDs instead of four — the residual bias is larger and is disclosed by
  `n_terminal_unevidenced`. The opposite error would have fabricated a cash-only return for a
  stock-and-cash transaction on the one security the program has formally refused to admit, and
  would have falsified §12.11 in the same run that asserts it.
- **AR-2 — DELL special dividend is record-date conditional.** The USD 0.13 applies only when
  `session_keys[t] <= 2013-10-28`; otherwise the consideration alone. Applied: §3.8 formula,
  §4.3, §9.1 cases 8a/8b, the `request.json` evidence table's `record_date_ns` field.
  **Cost if wrong:** an unentitled +0.94% on the last-date DELL cell — small in magnitude, but
  exactly the inference the audit prohibits, in the one place the design claims audit
  compliance.
- **AR-3 — net spread is per-date; block length re-pinned; borrow uses calendar days.**
  `net_h(t) = gross_h(t) - cost_drag_h(t)` on the same dates as gross, with
  `cost_drag_h(t) = (trade_bps/1e4)*2*turnover_h + (annual_borrow_bps/1e4)*(days_h(t)/365.0)*short_leg_gross`.
  The rebalance-grid construction is **deleted**. Block length is re-pinned **before any run**
  to `L_h = max(5, ceil(h/2))` → `{5,5,5,11,32}`, and §4.4 shows the reportability arithmetic per
  horizon: only `h = 63` is null. The ledger's pre-registration line carries the final values.
  Applied: §3.12 (every symbol defined), §4.3, §4.4 (two tables), §6 (`IcDatePoint` gains
  `days_forward`/`borrow_drag`/`cost_drag`; `IcHorizonSummary` carries the constant `trade_drag`
  and the means), §7.3, §9.1 cases 19/19b/19c/19d, §9.2 family F5.
  - **Follow-up ruling, part 1 — `turnover_h` ACCEPTED as implemented:** the
    **decile-membership one-way turnover at lag `h`**, measured on the actual decile portfolios.
    This is now the single frozen reading; the earlier substitution instruction is removed.
  - **Follow-up ruling, part 2 — observation-count days REJECTED.** The borrow term uses
    **actual calendar days taken from the panel's own session keys**,
    `days_h(t) = (session_keys[t+h] - session_keys[t]) / 86'400'000'000'000` by integer
    division, so `cost_drag` becomes per-date and weekends, holidays and multi-session gaps are
    charged. `borrow_day_convention` is `"calendar_days_from_session_keys"`.
    **Cost if wrong:** the prior convention charged `h/365` and understated financing by roughly
    `252/365`; this one charges the real elapsed span and leaves only the two documented
    differences from the replay (constant short leg; integer versus fractional day count), both
    in §12.8b.
- **AR-4 — ACCEPT the common-sample decay column.** An additional statistic of the existing 30
  configurations; `N_14` stays 30. Common sample = dates where all five horizons have a valid
  forward return, i.e. `t in [0, T - max(H))`. Applied: §3.13 (new), §4.3, §4.4 second table,
  §6 `IcSampleStats`, §7.3 `ic_decay.csv` / `ic_summary.json`, §9.1 case 20c, §12.
  **Cost if wrong:** one extra column set; the alternative was an uninterpretable headline
  deliverable in which a falling curve is indistinguishable from a 2013-H2-vs-H1 regime shift.
- **AR-5 — `rho_rank` defines `implied_one_way_turnover`.** `rho_pearson` is reported only.
  Applied: §3.11, §4.3, §6 `AutocorrSummary` comments, §7.3 `turnover_source` column, §9.1
  case 20b. **Cost if wrong:** one column's interpretation; both rhos are emitted, so the other
  reading is recoverable without a rerun.
- **AR-6 — the bootstrap recipe is pinned to the byte.** `BootstrapStatisticId` with explicit
  values 0..5; `horizon` enters the stream key as an **index**, not a value; `splitmix64_next`
  advances state first and the **return value** seeds `Xoshiro256pp`; Lemire nearly-divisionless
  bounded draw consuming one 64-bit output per attempt with the **high** half as the result and
  a stated rejection threshold `(-n) % n`; block starts uniform in `[0, n)`; blocks wrap
  circularly; `B = 2000` fixed; `reportable` requires `draws >= 1 && n >= 20 && floor(n/L_h) >=
  10`; `CrossSectionIcScratch` gains a `stat` buffer of `bootstrap_draws` doubles so the
  zero-allocation claim holds; the oracle reproduces the **first 8 draws** bit-exactly. Applied:
  §3.10 (rewritten), §4.3, §4.5 recipe block, §6 (enum + scratch), §6.1 (corrected arithmetic:
  137,104 bytes, not 121,104), §6.2 (`draws == 0` rejected at validation), §9.1 cases 13b/14b,
  §9.2 family F6. **Cost if wrong:** a stream-key change invalidates published intervals and
  requires a rerun — which is why it is pinned now, before any run.
- **AR-7 — ACCEPT the audited-ID restriction.** Every configuration is additionally reported
  excluding the 34 audited-terminal-or-gap IDs. A reported restriction, not a new trial; `N`
  stays 30. Columns are suffixed `_ex34`, with `n_excluded_audited` per date. Applied: §3.7,
  §3.8, §4.3, §4.4, §6 `excluded_audited` input span, §7.3, §9.1 case 20d. **Cost if wrong:**
  one extra column set; without it the checkpoint's largest stated bias is named in §12 and
  measured nowhere, since variants A and B drop the same names.
- **AR-8 — `--evaluation-start` and `--evaluation-end` are REQUIRED**, as for
  `equity-baseline` (`stage_equity_baseline.cpp:91-94`). Applied: §7.1, §10.2, §9.4 item 1.
  **Cost if wrong:** a caller must type two flags; the alternative left the evaluated window
  unpinned, which changes every number in the checkpoint.
- **AR-9 — `equity-ic` accepts NO `--config`;** the merge-predicate edit at
  `dispatch.cpp:109-111` is removed. Applied: §7.1, §7.5, §8 T5. **Cost if wrong:** a config
  file cannot drive this subcommand; the alternative was dead code or an unpinned path by which
  a file could inject book/optimizer fields.
- **AR-10 — cost-constant provenance.** `trade_bps` is `constexpr EquityAllocationConfig{}.trade_bps`
  — a compile-time reference that **edits no file** (`QpConfig` is scalar-only, so the aggregate
  is a literal type). `annual_borrow_bps = 365.0` is a literal with a comment naming the
  **deployed equity-book run configuration** recorded in the handoff — **not** `replay.hpp`,
  whose `annual_borrow_bps{0.0}` default is `0.0`. Applied: §3.12 symbol table and
  `cost_model_provenance` string, §4.3, §4.5, §8 T5. **Cost if wrong:** the prior draft
  misattributed 365 bps to a header that does not contain it, in the very sentence the mandatory
  provenance string is built from.

### 11.3 Parent rulings on the re-review (2026-09-20, binding)

Issued after the scoped re-review of Revision 2
(`.superpowers/sdd/equity-platform-parent-goal/cp14-design-rereview.md`, verdict **Fix required
narrowly**: all 20 prior findings resolved; 1 new Critical, 3 Important, 5 Minor). Revision 3
applies all nine.

- **AR-11 (N-1) — price the SAME book the spread earns.** The gross spread
  `mean_0 - mean_{Q-1}` is the return of a **gross-2.0** book (long decile at `+1.0`, short at
  `-1.0`). The gross definition is **unchanged**; the cost side is scaled to it:
  `w_i = +1.0/n_0` for decile 0, `-1.0/n_{Q-1}` for decile `Q-1`, `short_leg_gross = 1.0`, and
  `turnover_h` / `trade_drag_h` follow from those weights. Worked check, in §3.12: a complete
  decile turnover gives `Σ|Δw| = 4.0` → `oneway = 2.0` → `trade_drag = (5/1e4)*2*2.0 = 20 bps`,
  the round-trip cost of a gross-2.0 book at 5 bps. The previous `±0.5/n` gave `Σ|Δw| = 2.0`
  and 10 bps — the cost of a book half the size of the one being measured. Applied: §3.12
  (step 5 note, symbol table, turnover block, worked check, replay comparison), §4.3 (new
  "Priced book" row), §4.5 (`short_leg_gross: 1.0`, `priced_book_gross: 2.0`,
  `decile_weights`), §9.1 cases 19/19b/19c/19d, §9.2 family F5, §12.8b.
  **Cost if wrong:** every `spread_net_*` figure and its `_common`/`_ex34` variants understate
  cost by exactly 2×, in the checkpoint's only cost-aware number — and the value is
  pre-registered, so it could not be repaired after the run without opening a new trial.
- **R-A (N-2) — `n_terminal_unevidenced` is a MEASURED output, not a pre-registered constant.**
  Defined as the count of cells the caller flags terminal without evidence: an ID among the 34
  that is not one of the three evidenced events and for which a terminal event can be cited
  whose last observed session falls inside the evaluation window. The partition, stated once:
  **34 = 3 terminal-cash-event hypothesis (HNZ, DELL, MOLX — the audit's own word) + 2
  evidenced non-terminal (GNW, MA) + 29 unclassified**. The 29 are **not** flagged terminal —
  the audit declines to classify them — so their cells drop into `n_dropped_missing_forward`.
  PCS is the one unevidenced-terminal ID. The `_ex34` restriction still excludes all 34.
  **AR-1 is re-issued** with this partition in place of its "31". Applied: §2.3 (partition
  block, hypothesis labelling, measured-output paragraph), §3.8, §4.5 `source_exclusions`,
  §10.4 `terminal_evidence`, §12.4, and the AR-1 entry in §11.2.
  **Cost if wrong:** the prior text was internally contradictory (34 − 3 with GNW/MA excluded
  is 29, not 31) and attributed to the audit a terminal classification it explicitly declines
  to make — published in the ledger, the receipt and §12.4, in the section whose whole purpose
  is evidence integrity.
- **R-B (N-3) — one reading of the common sample.** It is the **prefix**
  `in_common_sample(t) = (t < common_sample_dates)`, `common_sample_dates = T - max(H) = 126`;
  §3.13's intersection clause is deleted. If any prefix date fails to emit at any horizon, the
  run reports it via `common_prefix_gaps` and the affected `_common` columns are marked
  unreportable (`unreportable_reason == 2`) rather than silently falling back to a per-horizon
  intersection that would put the five horizons on five different samples. Spread `_common`
  columns use the same prefix. Applied: §3.13, §4.3, §4.5 `common_sample_rule`, §6
  (`common_sample_dates` comment, `IcSampleStats::common_prefix_gaps` and
  `unreportable_reason`), §9.1 cases 20c (under an explicit fixture guarantee) and 20d (a
  failing prefix date shrinks the set by exactly that date for **all** horizons).
  **Cost if wrong:** three readings of one pre-registered sample; two implementers produce
  different `_common` numbers, and the per-horizon reading defeats AR-4 entirely.
- **R-C (N-4) — sixth stream-key field `sample_id`.** `0 = full, 1 = common`. The exact bit
  layout is now written out as a table and in code:
  `seed_run ^ (statistic_id<<8) ^ (horizon_index<<16) ^ (signal_index<<24) ^ (variant_id<<32)
  ^ (restriction_id<<40) ^ (sample_id<<48)` — six disjoint byte-aligned ranges, bits 0-7 and
  56-63 unused. `horizon_index` moves from bits 0-7 to 16-23 so every field owns a byte.
  Oracle F6 reproduces the first 8 draws for **both** sample ids. Applied: §3.10 (layout table
  + code block + the explanation of what sharing a stream would have meant), §4.3, §4.5
  `stream_key` / `sample_ids`, §6 `stream_sample_id` (and `stream_variant_id` /
  `stream_restriction_id` renamed to match), §9.1 case 13b (two named tuples), §9.2 F6.
  **Cost if wrong:** 12 pairs of maximally correlated intervals per configuration, and an
  implementer who spots the gap invents a sixth field — the run then stops matching this
  document, which is the failure AR-6 exists to prevent.
- **Minors N-5..N-9.** **N-5:** the Lemire draw is now spelled in C++, not pseudocode — the
  threshold is `(0ULL - n) % n` on `std::uint64_t` (no unary minus on unsigned, so no MSVC
  C4146 under `/W4 /WX`), and the 128-bit product goes through a local
  `eval::detail::umul_64_to_128` restated byte-for-byte from the existing
  `atx::core::detail::umul_64_to_128` at `atx-core/include/atx/core/decimal.hpp:120-133`
  (`unsigned __int128` under `__SIZEOF_INT128__`, `_umul128` from `<intrin.h>` otherwise), with
  a `// SAFETY:` note naming the source — restated rather than reached for, per R-6's rationale.
  **N-6:** §10.2's stale "~121 KB" replaced with **137,104 bytes**, citing §6.1's derivation.
  **N-7:** §3.10's reproducibility claim now reads "from this text **plus the `Xoshiro256pp`
  definition at `random.hpp:83`**", which T7a may read because it is not the unit under test.
  **N-8:** §9.4 item 1 rewritten to assert only what exists — per-subcommand flag guards live
  in exactly two places (`stage_equity_baseline.cpp:69`, `stage_equity_book.cpp:100`),
  `equity-ic` adopts the same rejection discipline, and the other ten subcommands have no guard
  and are explicitly **not** asserted to reject anything. **N-9:** new §12.8c states that the
  per-date net mixes a per-date financing term with a period-average trading term and is not a
  per-date realized cost; §3.12 step 1 now uses the single spelling
  `dates_below_quantile_count`.

### 11.4 Parent rulings on re-review round 2 (2026-09-20, binding)

`cp14-design-rereview2.md` confirms every N-1..N-9 finding resolved and raises four Minors plus
one informational item. All five applied in Revision 4; none changes a Stage-1 number.

- **NEW-1 — `common_prefix_gaps` is one counter per `(horizon, sample)`, shared by the IC and
  spread families, and any gap voids the ENTIRE common block for that horizon.** The previous
  draft's "counts toward `common_prefix_gaps` for the spread statistics only" clause is
  **deleted**: a single `atx::usize` cannot express a per-family split, and §6's own comment
  already voided the IC family too. On `common_prefix_gaps > 0` every `*_common` column of that
  horizon — IC and spread, **means included** — is emitted as `null`/`""` with
  `summary_reportable = 0` and `unreportable_reason == 2`; the means are **nulled, not
  emitted**, because a point estimate over an incomplete prefix is exactly the number a reader
  would mistake for a common-sample result. Applied: §3.13 (failure rule rewritten), §4.3
  (merged row), §6 (`common_prefix_gaps` and `summary_reportable` comments), §9.1 case 20d
  (renamed `…VoidsTheWholeCommonBlock`, with a sub-case at `min_names_per_date <= n < Q` that
  asserts the same whole-block void — one counter, one outcome, no spread-only path).
  **Cost if wrong:** a spread-only gap is unrepresentable either way; this ruling makes the
  behaviour explicit instead of leaving §3.13 and §6 contradicting each other.
- **NEW-2 — the last stale "31" is gone.** The R-8 entry in §11.2 now reads "the 29
  unclassified IDs plus PCS" and marks its own earlier count as superseded text.
- **NEW-3 — §4.3's two "Common sample" rows merged into one**, carrying the prefix rule, the
  scalar, both rulings (AR-4 and R-B) and NEW-1's void rule. A frozen-parameter table with two
  rows of one name is the shape that breeds divergence later.
- **NEW-4 — `decile_one_way_turnover`'s `[0, 2]` scale is stated where it is read, not only in
  the worked check.** Under AR-11 it is measured at gross-2.0 weights, so a complete turnover
  reads `2.0`. Stated in §6's field comment, in §7.3's `quantile_spread.csv` description (and
  the CSV's own header comment row), and in §12.9 beside `1 - rho_rank`, with "never compare
  the two numbers directly".
- **Informational item, adopted as a runtime check.** §2.3's partition and §4.5's
  `terminal_unevidenced_ids: [146189]` rest on PCS being one of the 34 required-mark IDs, which
  the audit does not enumerate in prose — so the membership is inferred. §7.2 and §8 T5 now
  require the stage to **assert at runtime** that `146189` is in the loaded required-mark set,
  that the set's cardinality is 34, and that the three evidenced IDs are members, **failing the
  run with `InvalidArgument`** before the partition is written. An inference that the run
  depends on is now checked by the run.

### 11.5 Parent ruling RR-1 (T1 review, 2026-09-20, binding)

Issued after the independent T1 implementation review
(`.superpowers/sdd/equity-platform-parent-goal/cp14-task-T1-review.md`, SPEC **Fix required**
1 Important, QUALITY **Fix required** 3 Important). RR-1 is the only ruling in that round that
changes this document; RR-2 (export the two calendar boundaries from the engine header) and
RR-3 (move `detail::block_len` / `detail::series_reportable` into the header) are implementation
rulings that the §6 sketch already permits and are recorded in the T1 report, not here.

- **RR-1 — `kMaxIcDates = 4096` and `kMaxIcInstruments = 4096` are added to §6's bounded
  maxima, and §6.2 rejects `dates > kMaxIcDates` or `instruments > kMaxIcInstruments` with
  `OutOfRange` before any sizing.**

  **Why.** §6.1 states that every loop is "bounded by `dates`, `instruments`,
  `horizons.size()`, `quantiles` or `bootstrap_draws`, **each validated against a compile-time
  maximum at entry**". §6 declared maxima for only three of those five. `dates` and
  `instruments` were bounded solely by the `usize` overflow guard, so the sentence was false as
  written, and the consequence was observable rather than cosmetic: on a span-consistent but
  enormous input, `plan_cross_section_ic`'s eleven `std::vector::assign` calls can throw
  `std::bad_alloc` in a unit whose stated contract is that no path throws and whose failures
  travel in `Result` (`.agents/cpp/agent.md` §4; §6's "no exception on any success path"). This
  was a **design gap, not an implementer defect** — T1 implemented exactly what §6 declared.
  The two branches are `OutOfRange` rather than `InvalidArgument` because they are extent-shape
  violations, matching the code assignment §6.2 already gives `horizons.back() >= dates` and
  `bootstrap_draws > kMaxBootstrapDraws`. They are checked before the `dates * instruments`
  overflow guard, which they subsume.

  **4,096 is chosen to clear both stages with room to spare, and neither stage is affected.**
  Stage 1 is 189 dates x 1,661 instruments (§1.2). Stage 2's planned bounded annual panel is
  ~1,950 dates x ~1,661 instruments (§1.3 non-goal 1;
  `reviews/2026-09-19-annual-panel-memory-review.md:92-97`), i.e. 48% of the date bound and 41%
  of the instrument bound. The bound therefore constrains no planned run and cannot silently
  become the thing that blocks the Stage-2 follow-on checkpoint; raising it later is a header
  constant and two error messages, and — unlike a frozen statistical parameter — changes no
  published number, so it is **not** a new trial under §4.4.

  **Scratch is unaffected.** §6.1's 137,104-byte figure is computed from the actual
  `instruments`, `dates` and `bootstrap_draws` of the call, not from the maxima; the maxima
  bound the arguments, they do not size the buffers.

### 11.6 Parent rulings on T7a ambiguities (2026-09-20, binding)

T7a (`cp14-task-T7a-report.md`) delivered the exact oracle and comparator and raised nine
ambiguities, A-1..A-9. Each is ruled below; four change the body, five ratify T7a's reading.
Nothing here changes a Stage-1 number, and the oracle document T7a already wrote remains valid
under every ruling.

- **A-6 — the terminal leg is a fallback, never an overlay. RULED.** Variant B
  (`IncludeAuditedTerminalV1`) applies the terminal cash leg **only when the ordinary §3.3
  forward return is undefined because the name has no close at `t+h`**, i.e. its last observed
  session lies inside `(t, t+h]`. If a finite positive `P_i(t+h)` exists, the ordinary return is
  used, no leg is applied and `n_terminal_applied` does not increment — even for a
  terminal-flagged, evidenced ID. T7a's reading is correct and is now the design's: read
  literally the old §3.8 would have set `t_last = t+h` and added a settlement on top of a live
  price, double-counting the consideration on a name that by hypothesis has no later marks.
  Applied: §3.8 (new precedence paragraph ahead of the formula), §9.1 **new case 8c**
  (`TerminalLeg_LiveCloseAtTPlusH_UsesOrdinaryReturnAndAppliesNoLeg`; the old 8c renumbered
  8d). T7a's F3/F4 fixtures, which never construct the ambiguous configuration, remain valid —
  the branch is now specified for T2/T3 rather than left to the implementer.
  **Cost if wrong:** an evidenced terminal name that kept printing would carry a fabricated
  extra return in every horizon series that spans its event.
- **A-3 — `unreportable_reason` is pinned once, with a precedence rule. RULED.** The frozen
  enum, now stated in §3.10 as a table and repeated in §6's two field comments: `0` reportable,
  `1` `series-shorter-than-twenty` (`n < 20`), `2` `common-prefix-gap` (§3.13), `3`
  `series-too-short-for-block-length` (`floor(n/L_h) < 10`), `4` `bootstrap-draws-zero`. **When
  several conditions hold the lowest nonzero code wins.** The `n < 2` degenerate case of §3.9
  carries code `1`. This resolves the contradiction T7a found between §3.10's natural 1/2/3
  string ordering and §3.13's pinned `== 2`. Applied: §3.10 (the table + precedence), §3.9,
  §3.13, §6 (`BootstrapInterval` and `IcSampleStats` comments), §9.1 case 14 (now pins the
  whole enum and two precedence collisions), §9.2 F6. T1's draft header comment carries an
  older two-code list; **T2/T3 align it to §3.10's table**, which is authoritative. The
  comparator may now assert the full enum instead of only two codes.
- **A-5 — the F6 export carries `inputs.n` and `inputs.sample_id`. RULED.** Draw values depend
  on `n` through the Lemire bound, so a draw sequence without a declared `n` is unauditable.
  Every `CROSS_SECTION_IC_MEASUREMENT` line carries both, and bootstrap-bearing cases also
  carry `statistic_id`, `horizon_index`, `signal_index`, `variant_id`, `restriction_id`,
  `block_len` and `bootstrap_seed`, so a line reconstructs its own stream key. A line missing
  `inputs.n` or `inputs.sample_id` is a comparator **failure**, not a warning. A smaller
  synthetic engine fixture is permitted provided it declares its own `n`, which is what T7a's
  auxiliary draw-start ladder exists to match. Applied: §9.2 (native-side mandatory-fields
  paragraph and the F6 block with a worked `inputs` example), §7 (schema note).
- **A-9 — oracle filenames. RULED in favour of T7a's brief.** The oracle is
  `build-equity/audits/iteration14_cross_section_oracle.py` and its output
  `build-equity/audits/iteration14-cross-section-oracle.json`; the design's earlier
  `…_cross_section_ic_oracle.py` / `…-cross-section-ic-oracle.json` are superseded. The
  comparator name `iteration14_native_comparator.py` was already correct. Applied: §8 T7a, §8
  T7b (the runner wires the A-9 path), §9.2, §10.3.
- **A-1 — `n_dropped_missing_forward` is gross, not net of terminal applications. ACCEPTED**
  as T7a read it: a terminal-applied cell has no §3.3 return by construction, so it is counted
  in both counters and the design never nets them; the identities and the ordering
  `n_used <= n_signal_finite <= n_eligible` hold as written.
- **A-2 — `n_excluded_audited` uses the ELIGIBLE basis. ACCEPTED**, per §3.8's "the excluded-ID
  count per date" and the counter's place in §3.7's eligible-family table; T7a's used-basis
  figure stays a non-binding diagnostic.
- **A-4 — F1's bound leg stays `sqrt(va*vb)` as declared. ACCEPTED.** It is frozen and
  dimensionally generous; T7a's `relative_only_residual_diagnostic` records the tight number
  beside it rather than silently tightening a pre-registered bound.
- **A-7 — F5's net bound leg stays `trade_drag + borrow_drag` as declared. ACCEPTED.** In this
  fixture `|gross| >> cost_drag`, so the declared leg is the *tighter* of the two candidates,
  not a loophole; T7a's `net_cancellation_leg` carries the cancellation-aware value as
  non-binding.
- **A-8 — every oracle case is required AND every missing one is recorded. ACCEPTED.** Both
  halves of §9.2 hold simultaneously: a silently unexported scenario fails the run, is also
  listed as `not_measured_natively`, and the `--optional-case` escape hatch is explicit,
  audited in the output, and may not be applied to the two design-named F6 cases.

### 11.7 Parent ruling R-4 (T4 review, 2026-09-20, binding)

Issued after the independent T4 implementation review
(`.superpowers/sdd/equity-platform-parent-goal/cp14-task-T4-review.md`, SPEC **pass with
findings**, QUALITY **pass with findings**; finding M-5).

- **R-4 — an integral-valued real in the trial ledger serializes with a forced `.0`, and
  that supersedes the §4.5 sample's bare `365`.** `trial_ledger.cpp` formats every JSON
  real with `std::to_chars` shortest round-trip (§7.4's pinned idiom) and then appends
  `.0` when the token carries no `.` and no exponent. So `annual_borrow_bps` is written
  **`365.0`**, not `365`; likewise `trade_bps` `5.0`, `short_leg_gross` `1.0`,
  `priced_book_gross` `2.0`, while `2.5` / `97.5` are untouched and an exponent form such
  as `1e+20` keeps no `.0`. **§4.5's fenced JSON block is illustrative, not a byte pin**,
  and it is internally inconsistent on exactly this point — it prints `"trade_bps":5.0`
  beside `"annual_borrow_bps":365`, so **no single formatting rule can reproduce it
  verbatim**. The forced `.0` is the resolution: one rule, applied uniformly, keeping a
  pre-registered real visibly real rather than letting it read as a count. The values are
  JSON-equivalent either way, so nothing numeric changes; the byte pin lives in
  `atx-impl/tests/trial_ledger_test.cpp`'s 3173-byte expected line, not here.
  **Cost if wrong:** none numerically — the risk this ruling removes is a later reader
  treating the §4.5 block as the authoritative byte sequence and "fixing" the emitter to
  match a sample that cannot be matched.

### 11.8 Parent ruling I-6 (T2T3 review, 2026-09-20, binding)

Issued after the independent T2/T3 implementation review
(`.superpowers/sdd/equity-platform-parent-goal/cp14-task-T2T3-review.md`, SPEC **pass with
findings**, QUALITY **pass with findings**; finding I-6, ruling requested).

- **I-6 — the spread family carries its own reportability. `IcHorizonSummary` gains
  `spread_reportable` (`atx::u8`) and `spread_unreportable_reason` (`atx::u8`).**

  **Why.** `IcSampleStats::dates_emitted`, `summary_reportable` and `unreportable_reason`
  describe the **IC family only**: they are gated on `n_used >= min_names_per_date` (2). The
  spread family emits on a strictly smaller set, `n_used >= quantiles` (10). The two are not
  the same series and nothing published the difference. On a thin-universe horizon where most
  dates clear 2 names but few clear 10, `dates_emitted = 180` and `summary_reportable = 1`
  would sit beside a `spread_gross_mean` computed from `series_stats` over an **empty** span —
  `0.0`, and indistinguishable in JSON or CSV from a genuine zero spread. §3.13's and §3.9's
  "serializes as null" rule keys off `summary_reportable`, which is 1, so nothing would have
  nulled it.

  **The rule.** Both fields are `detail::series_reportable` applied to the **spread series' own
  emitted-date count** `n_spread` at that horizon's `block_len`, with the configured
  `bootstrap_draws` — the identical `draws >= 1 && n >= 20 && floor(n / L_h) >= 10` bar the
  intervals use, not a weaker `n >= 2` test. `spread_unreportable_reason` carries the **same**
  frozen five-code enum as §3.10 (`0` reportable, `1` `n < 20`, `2` common-prefix-gap, `3`
  block-length, `4` draws-zero) under the same **lowest-nonzero-wins** precedence. When the
  gate is closed the engine emits `spread_gross_mean`, `spread_gross_sd`, `spread_net_mean` and
  `spread_net_sd` as `0.0` and the serializer publishes them as `null` / `""`, exactly as it
  already does for a closed IC gate.

  **Scope.** The pair on `IcHorizonSummary` is measured on the **full** sample. Each
  `IcSampleStats` block additionally gates its own spread means on its own spread series, so a
  common block with `common_prefix_gaps > 0` closes with code `2` there as well — which keeps
  NEW-1's whole-block void intact rather than opening a hole in it.

  **T5 publishes them beside the IC ones.** `quantile_spread.csv` and `ic_summary.json` carry
  `spread_reportable` and `spread_unreportable_reason` (plus the reason's string form, from the
  §3.10 table) next to `summary_reportable` / `unreportable_reason`, and T5's determinism test
  asserts that a closed spread gate serializes the four spread moments as `null` / `""`.

  **This changes no frozen statistical parameter and no published number on a reportable
  horizon.** `N_14 = 30` stands; §4.4's Stage-1 prediction is unaffected, because at
  `T = 189`, `Q = 10` and 200-400 admitted names every horizon's spread series is the same
  length as its IC series. The fields exist so a *thin* horizon cannot publish a point estimate
  the reader would mistake for a measured spread.

  **Cost if wrong:** two extra always-zero columns on a healthy run. The cost of omitting them
  is a spread mean of `0.0` over no dates, published as if it were a measurement.

### 11.9 Parent rulings (T5 review, 2026-09-20, binding)

Issued after the independent T5 + T6 + T7b implementation review
(`.superpowers/sdd/equity-platform-parent-goal/cp14-task-T5-review.md`, SPEC **pass with
findings**, QUALITY **pass with findings**, **SAFE TO RUN REAL DATA: no**). The review's three
blocking findings all write into the tracked append-only ledger and cannot be repaired after a
run, which is why they are ruled here rather than deferred. None of them changes a frozen
statistical parameter, a published number, or `N_14 = 30`.

- **C-1 — after the pre-registration append, EVERY exit path appends a terminal line.**
  §4.5's "A failed run keeps both lines" becomes structural rather than conventional. The stage
  is split so the pre-registration append is a **point of no return**: everything after it runs
  inside one guarded phase, and the completion line is appended on success (`"completed"`,
  `result.manifest_sha256`) and on failure (`"failed"`, `result.failure_sha256` = the SHA-256
  of the published `failure.json` bytes) alike, including on an exception. The returned
  `Result` is unchanged, so the CLI exit code is unaffected. If the **completion append itself**
  fails, nothing further can be done: that error is returned and `failure.json` names it.
  **Cost if wrong:** a dangling `"pre-registered"` line is permanent, and every future deflated
  Sharpe is fed an `N` inflated by a run whose outcome the ledger never records — the exact
  failure mode §4.5 exists to prevent.

- **I-1 / I-3 — wall time is measured by the producer; peak working set is NOT.** The
  completion line's `runtime.wall_seconds` and `manifest.json`'s `runtime.runtime_seconds`
  carry one `std::chrono::steady_clock` measurement taken across the whole stage.
  `runtime.peak_working_set_bytes` is present in both and is explicitly **`null`**, beside a
  `peak_working_set_source` string naming
  `build-equity/audits/iteration14_run_equity_ic.py` as the measuring party. **No in-process
  peak-working-set helper exists anywhere in this repository** — a read-only sweep for
  `GetProcessMemoryInfo` / `PROCESS_MEMORY_COUNTERS` / `psapi` over every `.hpp`/`.cpp` returns
  zero matches, and neither `stage_equity_baseline.cpp` nor `stage_equity_book.cpp` records
  runtime at all — so filling it in-process would mean introducing a new platform dependency
  and a link-library change at the last gate, unverifiable without a build. The external
  runner already samples `PeakWorkingSetSize` every 100 ms and the §10.4 receipt carries it.
  **The null is deliberate and is labelled as such**, so a later reader cannot mistake it for
  "not measured at all". **Cost if wrong:** the ledger line records no in-process peak; the
  measurement exists, one file away, bound by the receipt.

- **I-2 — the `design-note` parent is an EMBEDDED constant, not a runtime read.** The stage
  carries `kDesignNoteRelativePath` (this file's repository-relative path) and
  `kDesignNoteSha256`, and writes both into the `design-note` parent of **both** ledger lines
  and into `request.json`. The executable cannot reliably locate the repository (§7.1's
  allowed-flag set is frozen and has no slot for a design-note path), and a runtime read would
  make the synthetic stage tests depend on this document. The **runner** closes the loop:
  `iteration14_run_equity_ic.py` hashes the on-disk design note as a preflight precondition and
  **refuses the run** unless it equals the embedded constant. Any later edit to this document
  therefore breaks that preflight **by design** — which is the intended behaviour, not a
  defect: the pre-registration must bind to the revision that froze it, and a changed design is
  a new freeze that must be re-embedded deliberately. **Cost if wrong:** the ledger line cannot
  prove which revision froze the 30 trials it declares.

- **I-4 — the closed-spread-gate assertion §11.8 mandates is now pinned.** T5's determinism
  test asserts, at the fixture horizon whose spread series is shorter than twenty, that
  `spread_reportable` is `0`, that `spread_gross_mean` / `spread_gross_sd` /
  `spread_net_mean` / `spread_net_sd` are JSON `null`, and that the corresponding
  `quantile_spread.csv` `SPREAD` row carries `""` in those four columns and `0` in
  `spread_reportable`.

- **I-5 — `--attempt N` versions the DATA output directory too.** The runner's `--out` becomes
  `…/equity_ic_training_2013_20260920` for attempt 1 and
  `…/equity_ic_training_2013_20260920_attempt{N}` after, alongside the four versioned audit
  paths, and the preflight refuses **only the attempt's own** paths. Without this a failed
  attempt 1 — which reserves the directory before any work and leaves `.pending` plus
  `failure.json` in it — makes every retry die in preflight, while the handoff forbids deleting
  the failed evidence.

- **I-6 — the three documentation landings come AFTER the measurement.** §10.5's "three
  places" is a checkpoint **closing** condition, not a run precondition: the
  `PLATFORM_PROGRESS.md` checkpoint-14 section and the `atx-impl/README.md` /
  `atx-engine/README.md` subcommand lines land in a documentation pass that cites the §10.4
  receipt, so the progress entry states what was measured instead of what was planned. Until
  then the statement lives verbatim in `ic_summary.json`, `manifest.json` and
  `atx-impl/docs/EQUITY_IC.md`. **The three copies must be byte-identical**, which means one
  ASCII constant in the stage reproduced exactly — as an unwrapped fenced line — in the
  document, so a diff of the three finds nothing.

- **M-6 ACCEPTED as implemented — `quantile_spread.csv` is ONE 27-column table.** §7.3
  describes two row shapes (bucket rows of 8 fields, `SPREAD` rows of the rest); the
  implementation emits every row at the full width through a single writer driven by one
  27-entry column list, so bucket rows carry empty trailing fields. This is the safer
  construction — a hand-counted run of commas is how a column silently shifts — and §7.3's
  prose is amended to it rather than the code. The 27 columns are, in order: `signal`,
  `horizon`, `variant`, `restriction`, `quantile`, `n_dates`, `mean_names`,
  `mean_forward_return`, `gross_mean`, `gross_sd`, `gross_lo`, `gross_hi`, `net_mean`,
  `net_sd`, `net_lo`, `net_hi`, `decile_one_way_turnover`, `trade_drag`, `mean_borrow_drag`,
  `mean_cost_drag`, `mean_days_forward`, `gross_mean_common`, `net_mean_common`,
  `spread_reportable`, `spread_unreportable_reason`, `cost_model_provenance`,
  `borrow_day_convention`. The mandated comment row is the first line and begins `# `; a
  consumer must skip it.

- **Minors taken in the same round.** `number()` writes `""` for a non-finite real rather than
  letting `std::to_chars` publish `inf` / `nan` into a CSV (§7.4: "never `0`, never `NaN`");
  an unreportable interval never publishes `unreportable_reason == 0` — when the owning block
  is closed and the interval's own reason is `0`, the block's reason is carried instead;
  `manifest.json` is written **before** `.pending` is removed, so a manifest-write failure
  leaves a directory that still advertises itself as incomplete; and `dates_below_min_names`
  in `predictions_confirmed` is measured on ONE named block rather than summed over all sixty,
  with the scope stated in a companion string. The remaining minors (M-4, M-7, M-8, M-9, M-11,
  M-12, M-13) are deferred and recorded in the T5 report.

---

## 12. Qualifications

1. **189 observations is sign-and-shape evidence only.** Not accepted alpha, not a Sharpe, not
   capacity, not grounds for selecting anything. §10.5.
2. **An IC is a model-skill statistic, not a return.** `learning-availability-audit.md:27-30`:
   an IC-derived statistic "is a **model-skill statistic, not realized long/short portfolio
   returns after costs**. It must not be presented as investment Sharpe or evidence of trading
   capacity." A positive IC alongside the measured 9.2187 x-NAV cumulative target change
   (research §B.3) may still be uninvestable.
3. **The price series is not a verified total-return series.** `klac-source-adjustment-audit.md:37-40`:
   the KLAC 2026-06-12 contradiction "prevents describing the current adjusted-close field as a
   **verified total-return series across all securities and dates**." Every IC here inherits
   that. Disclosed in `manifest.json`, not absorbed.
4. **Dropping missing forward returns is itself a selection, and it biases upward.** Research
   §C-8, §F[9]. Variant B mitigates it for **three** events only (HNZ, DELL, MOLX — ruling
   AR-1), which the audit itself calls a terminal-cash-event **hypothesis**, and whose legs are
   `{+0.0138%, +0.144% or −0.794%, 0.000%}`. Of the 34 required-mark IDs, **29 are unclassified**
   — the audit makes no terminal finding about them — and their missing cells drop as ordinary
   missing marks; PCS is flagged terminal-without-evidence and counted. All of that residual is
   an unquantified upward bias. Variant A versus variant B is **not** a survivorship
   sensitivity: both drop the same
   names. The `_ex34` restriction (ruling AR-7) is the measure that actually speaks to it, and
   it bounds rather than removes the bias.
5. **Overlapping horizons make the naive t-statistic invalid.** §3.9. The bootstrap interval is
   the only reportable uncertainty, and at `h = 63` even that is unreportable at Stage-1 length
   (§4.4: `floor(126/32) = 3 < 10`, in both the full and common samples; every other horizon is
   reportable under the re-pinned `L_h = max(5, ceil(h/2))`). Separately, a block length shorter
   than the full overlap `h` preserves dependence *within* a block but destroys it across
   joins, so the interval is, if anything, **optimistically narrow** — read it as a floor on
   uncertainty, not a ceiling.
5b. **The decay curve confounds horizon with calendar period, and the common-sample column
    narrows the confound without removing it.** The context ends 2013-12-31 with no rows after,
    so `IC(h)` runs on `[0, 189-h)`: `h=1` reaches late December, `h=63` ends around
    2013-09-30. §3.13's `_common` column puts all five horizons on one 126-date sample — but
    that sample is 2013-H1-weighted, so the shape is a 2013-H1 shape. D-3 consumes this curve;
    it must read the `_common` column and must not treat either column as a 2013-full-year
    result.
5c. **`blend_equal` is a rank-space blend, not a monotone image of a raw average** (ruling
    OQ-1, §3.2). It is `0.5*rank_w(s0) + 0.5*rank_w(s1)` after a per-alpha rank/winsorize/gross
    pass, so neither its rank IC nor its Pearson IC is derivable from a raw equal average's,
    and its decile buckets differ too. Its **Pearson** IC in particular correlates a bounded,
    near-uniform variable with a heavy-tailed return and should not be compared directly with
    signals 0 and 1's Pearson IC. Per OQ-1 no raw-average fourth signal was added; the confound
    is disclosed rather than measured.
5d. **The return is indexed from `t`; the deployed book executes at `t+1`** (§3.3). This is a
    forecast statistic, not a book return, and it is not lookahead — signal and label windows
    are disjoint. But the curve is indexed one observation earlier than the book it informs, and
    no execution-delayed alignment variant was computed (it would be a new trial axis).
5e. **The borrow drag uses actual calendar days from the panel's session keys** (§3.12, ruling
    AR-3): `days_h(t) = (session_keys[t+h] - session_keys[t]) / 86'400'000'000'000`, so
    weekends, holidays and multi-session gaps are charged. ACT/365 on those calendar days,
    simple and non-compounding, **is** the convention `replay.cpp`'s `borrow_charge` (`:251-259`)
    and `elapsed_days` (`:70-82`) implement — see §12.8b for the two points on which this design
    still differs from it.
6. **The universe is liquidity-screened only.** `data/universe.hpp` carries an explicit
   SURVIVORSHIP CAVEAT; `stage_equity_baseline.cpp:161` emits
   `"instrument_type_eligibility": "unknown"`; the archive contains ETFs, rights and test
   symbols (`panel-identity-audit.md:109-116`). The measured IC is the IC of these expressions
   on **this** universe, not on a common-stock universe.
7. **Session keys are labels, not availability.** `tbltickerhistory-input-audit.md:47-52`:
   vendor delivery is 05:00 CT T+1 full history, and the snapshot is "neither a historical
   release-time record nor proof that this snapshot's shares and revisions were known on their
   stated trading dates". `panel_artifact.hpp:14` `"session-label-not-availability"`.
8. **The net decile spread restates two constants; it is not the replay's cost model.** §3.12,
   R-5, AR-10. `trade_bps` is a compile-time reference to `EquityAllocationConfig{}.trade_bps`;
   `annual_borrow_bps = 365` is a literal from the **deployed run configuration**, not from
   `replay.hpp` (whose default is `0.0`). `borrow_charge` (`replay.cpp:251`) is file-local and
   is never called. No agreement with the replay's accounting is claimed.
8b. **Where the borrow drag matches the replay, and where it does not.** Verified read-only:
    `replay.cpp:251-259` charges `short_dollars * ((rate / borrow_day_basis) * days)` — simple,
    non-compounding — and `replay.cpp:70-82` derives `days` as an `f64` nanosecond difference
    over `kNanosPerDay`. This design matches on the day-count axis (the same session keys), the
    `/365` basis, the simple non-compounding form, and — after ruling AR-11 — on the **book
    size**: `short_leg_gross = 1.0` is the actual short leg of the gross-2.0 portfolio whose
    return `gross_h(t)` reports, so the notional-book mismatch that made the drag price a
    gross-1.0 book is gone. Two differences remain, neither corrected: (a) the replay accrues on
    the **actual marked short dollars of each interval**, which drift with prices and trades,
    while this design holds the short leg at its rebalance weight of `1.0` across the window;
    (b) `days_h(t)` is an **integer** day count while `elapsed_days` is fractional — identical
    while session keys are midnight-aligned, as this archive's are, and divergent if one ever
    carries an intraday time. The net decile spread is therefore a cost-aware diagnostic, not a
    reproduction of the replay's ledger.
8c. **The per-date net is not a per-date realized cost.** `borrow_drag_h(t)` varies with the
    calendar span of each date's forward window, but `trade_drag_h` is a **period-average**: it
    uses the mean `turnover_h` over the whole series, not date `t`'s own turnover. So
    `net_h(t)` mixes a per-date financing term with an average trading term, and a single
    `net_h(t)` should be read as "gross at `t`, less the book's typical trading cost", not as
    the cost actually incurred at `t`. The series mean is unaffected by the averaging; the
    series dispersion is understated on the trading leg. §9.1 case 19 pins the constancy so the
    property is deliberate and checkable rather than incidental.
9. **`1 - rho_rank(1)` is a rule of thumb, not an accounting identity**, and is not comparable
   to realized turnover (`report-holding-period-audit.md:32`: the existing turnover metric
   carries previous weights unchanged between decisions, so "Price drift and financing do not
   update that previous book"). It is also a different quantity **on a different scale** from
   the `decile_one_way_turnover` that feeds the cost drag (§3.12): that one is measured at the
   gross-2.0 book's weights (ruling AR-11), so it ranges over `[0, 2]` and a complete decile
   turnover reads `2.0`, whereas `1 - rho_rank` is a correlation-derived proxy with no book
   attached. Never compare the two numbers directly.
10. **No sanitizer, no static analyser, no include-clean build, no CI covers this work.**
    `.agents/cpp/agent.md` §8. Two shipped seal paths — `MaskSealedV1` and the `reserve_window`
    routing — are unreachable at Stage 1 and are covered only synthetically (§9.5).
11. **Nothing in this checkpoint completes a replay, prices a corporate action, or changes PCS
    admission.** PCS admission remains **rejected** and **no PCS terminal return is ever
    computed or applied** (ruling AR-1): PCS carries no consideration in the required-marks
    audit, the handoff forbids applying the event, and its stock-plus-cash terms are not
    representable by §3.8's cash-only formula at all. It is counted in
    `n_terminal_unevidenced` and nowhere else. The 2013-05-01 failure, its window, panels and
    receipts are unchanged. This checkpoint is explicitly unblocked by that failure
    (research §D-1) because a cross-sectional rank correlation needs no cash, no holdings, no
    corporate action and no 2013-05-01 mark.
12. **No live trading and no broker action is performed or authorized.**
