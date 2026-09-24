# `equity-ic` — pre-registered cross-sectional forecast evaluation

The first forecast-quality measurement in this program. Thirteen checkpoints made the
accounting around two frozen momentum expressions exact without ever estimating whether those
expressions predict anything. `equity-ic` estimates it, on the already-identified 2013
training slice, against a recipe frozen before the first run.

Frozen design: `atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md`
(§4 pre-registration, §5 seal, §7 subcommand and schemas, §12 qualifications). Every number
this subcommand emits is a **model-skill statistic**, never a return, a Sharpe or a capacity
claim — see [Qualifications](#qualifications), which is the part of this document that binds.

---

## Validation status

Stage 1 completed 2026-09-20, status `validated_stage1_sign_and_shape_only`. See the
[checkpoint receipt](../../atx-engine/reviews/2026-09-20-cross-section-ic-validation.json)
(SHA-256 `38dd653f7069cb4935b51d003baf9cdf87401434eccc9af5df15e907b319d723`) and
checkpoint 14 in the
[platform progress record](../../atx-engine/docs/PLATFORM_PROGRESS.md).

Attempt 1 ran the frozen 2013 training context (189 observations, 1,661 instruments,
2013-04-04 through 2014-01-01 exclusive). Headline, full sample, `DropMissingForward`
variant, rank IC by horizon `h=1/5/10/21/63`: `momentum_252` +0.029/+0.056/+0.070/
+0.092/+0.146; `momentum_126` +0.030/+0.052/+0.064/+0.093/+0.154; `blend_equal`
+0.031/+0.057/+0.071/+0.099/+0.160. The Pearson IC bootstrap 95% interval lower bound
is above zero from h=5 for all three signals (h=1 below zero for `momentum_252` only);
h=63 intervals are null by pre-registered prediction. The net decile spread is
positive at every reportable horizon. This is sign-and-shape evidence only: not
accepted alpha, not out-of-sample, no model fitted. Stage 2 (2013-2019, memory-budgeted)
is the follow-on.

---

## 1. What it computes

Per date, per signal, per horizon, per forward-return variant, over the admitted
cross-section:

| Family | Statistics |
| --- | --- |
| Information coefficient | Pearson IC and rank (Spearman) IC per date; mean, sd, ICIR, naive t |
| Decay | the IC mean at each horizon, full sample and common sample side by side |
| Uncertainty | circular-block-bootstrap 2.5 / 97.5 percentile intervals, `B = 2000`, fixed seed |
| Dispersion | decile (Q = 10) spread, gross and net of a restated cost convention |
| Persistence | lag-1 signal autocorrelation and the `1 - rho_rank` turnover proxy |
| Coverage | every inclusion and drop counter, per date, beside every statistic |

**No IC number is ever emitted without its coverage row.** Nothing is fitted, selected,
tuned or traded: the subcommand reads a published baseline and writes statistics.

### Inputs, all reused unchanged

```text
--panel        the identified 2013 context   (read through read_panel_artifact)
--baseline-dir the published equity-baseline directory: evaluation.bin + combo.bin
```

The two momentum signals come from `evaluate_equity_baseline`, called with an unmodified
signature; the third signal is the **published** `combo.bin` alpha column, so the evaluated
preference stream is byte-for-byte the one `equity-book` consumes rather than a second
implementation of the blend. The stage re-derives the view and fails the run if the fresh
values disagree with the published `evaluation.bin` on any admitted cell.

The 34 required-mark security ids come from the source-reconciliation audit's `manifest.json`,
resolved beside `--baseline-dir` in the same data root. The run **fails** unless that set has
exactly 34 ids and contains `146189` (PCS) and the three evidenced ids `37648` / `35715` /
`39970`: the frozen partition rests on that membership, and an inference the run depends on is
checked by the run.

---

## 2. The pre-registration

Frozen before the first run. Changing any of it produces a **new trial**, appended to the
ledger; nothing is ever overwritten.

| Parameter | Value |
| --- | --- |
| Signals | `momentum_252`, `momentum_126`, `blend_equal` — exactly three |
| Horizons `H` | `{1, 5, 10, 21, 63}` panel rows |
| Quantiles `Q` | `10` (deciles) |
| `min_names_per_date` | `2` |
| Bootstrap | `B = 2000` draws, seed `20260920`, `L_h = max(5, ceil(h/2))`, 2.5 / 97.5 nearest-rank |
| Reportable | `draws >= 1 && n >= 20 && floor(n / L_h) >= 10` |
| Forward variants | `DropMissingForward` **and** `IncludeAuditedTerminalV1` — both, always |
| Restrictions | full **and** `_ex34` (excluding all 34 audited ids) — both, always |
| Samples | full **and** `_common` (the date prefix `t < T - max(H)`) — both, always |
| Costs | `trade_bps = 5.0`, `annual_borrow_bps = 365.0`, `short_leg_gross = 1.0` |
| Seal | `RejectSealedV1`; window guard `[2013-04-01, 2020-01-01)` |
| `N_14` | **30** = 3 signals x 5 horizons x 2 variants |

The `_ex34` restriction and the `_common` sample are **reported for every configuration**,
never chosen between, so they are additional statistics of the same 30 configurations rather
than a search. `N_14` stays 30. If a later checkpoint ever *selects* on one of them, that
selection is a new trial and must be ledgered as such.

### The trial ledger

`atx-engine/reviews/trial-ledger.jsonl`, append-only and SHA-256 hash-chained. **Two lines per
run, never one**: a `"pre-registered"` line goes in before any statistic exists, and a
`"completed"` line with the output manifest digest goes in after. A failed run keeps both
lines and the retry is a new `trial_id`. `N_14 = 30` is the number
`eval::deflated_sharpe` must be fed when any equity Sharpe is eventually deflated.

---

## 3. Running it

```powershell
build-equity/bin/atx-impl.exe equity-ic `
  --panel        'C:/atx/data/tickerhistory_training_native_20260919/context.bin' `
  --baseline-dir 'C:/atx/data/equity_baseline_training_2013_20260919' `
  --out          'C:/atx/data/equity_ic_training_2013_20260920' `
  --evaluation-start 2013-04-04 --evaluation-end 2014-01-01 `
  --max-working-bytes 3000000000 `
  --trial-ledger 'atx-engine/reviews/trial-ledger.jsonl'
```

`--evaluation-start` and `--evaluation-end` are **required**, exactly as for
`equity-baseline`: an omitted window leaves the evaluated date range unpinned and every number
in the checkpoint depends on it. `--config` is **not accepted** for this subcommand. `--out`
must not exist. A failure **after** the pre-registration ledger line is appended keeps
`failure.json` and the `.pending` marker for investigation **and appends a `"failed"`
terminal line** whose `failure_sha256` is the digest of that `failure.json` (§11.9, ruling
C-1); the retry runs into a fresh, per-attempt directory.

The supported runner is `build-equity/audits/iteration14_run_equity_ic.py`. It refuses only
**this attempt's own** output paths, pins inputs / sources / executable before and after,
refuses to start unless the on-disk design note still hashes to the digest the stage
embedded, verifies the ledger chain independently in Python, and samples the working set.
`--attempt N` versions every audit output **and the data directory**
(`…_attempt{N}`), so a failed attempt's evidence is kept rather than overwritten. It never
builds and never runs the comparator against real-data numbers.

---

## 4. Reading the outputs

| File | What it is |
| --- | --- |
| `request.json` | the frozen recipe, input artifact ids, the terminal-evidence table with record dates, the audit binding, the ledger `trial_id` and its pre-registration line digest |
| `seal.json` | the calendar-seal report and its honest statement |
| `coverage.csv` | every counter, per date, per configuration — read this beside every IC |
| `ic.csv` | per-date Pearson IC, rank IC, gross and net decile spread, forward days, drags |
| `ic_decay.csv` | one row per point of the decay curve, full and common sample side by side |
| `quantile_spread.csv` | per-decile mean forward returns, then a `SPREAD` row per configuration |
| `signal_autocorr.csv` | lag-1 `rho_pearson`, `rho_rank` and the turnover proxy |
| `ic_summary.json` | every summary block with its six bootstrap intervals and reportability |
| `manifest.json` | file digests, parents, measured predictions, qualifications |

### Four rules for reading any of them

1. **Never read a point estimate without its interval.** `ic_decay.csv` carries
   `reportable`; `ic_summary.json` carries `unreportable_reason` with its text. An
   unreportable interval serializes as JSON `null` and as `""` in CSV — **never `0`, never
   `NaN`**. A blank `ic_lo` means "this series is too short to bound", not "zero".
2. **`naive_t` is emitted and is invalid.** Overlapping horizons make the daily IC series
   serially correlated by construction, so the naive standard error is understated. The
   schema says so in every block (`naive_t_validity`). The bootstrap interval is the only
   reportable uncertainty — and, because a block shorter than the full overlap destroys
   dependence across joins, read even that as a **floor** on uncertainty, not a ceiling.
3. **Read the `_common` column for shape.** The full-sample columns measure each horizon on a
   different, nested calendar sample: `h = 1` reaches late December, `h = 63` stops around
   September. A falling full-sample curve is not distinguishable from a 2013-H2-versus-H1
   regime difference. The `_common` block puts all five horizons on one date prefix. If any
   prefix date failed to emit, the **entire** `_common` block for that horizon is `null` with
   `unreportable_reason == 2` — the means included, deliberately.
4. **Read both variants and both restrictions.** They are emitted side by side precisely so
   that choosing the more favourable of them later is visible as the new trial it would be.

### Two scales that look alike and are not

`quantile_spread.csv`'s `decile_one_way_turnover` is measured at the gross-2.0 book's weights
(`+/-1.0/n`), so it ranges over `[0, 2]` and a **complete** decile turnover reads `2.0`, not
`1.0`. `signal_autocorr.csv`'s `implied_one_way_turnover` is `1 - rho_rank`, a
correlation-derived proxy with no book attached. **Never compare the two numbers directly.**
A "correction" of the decile weights to `+/-0.5/n` to make a complete turnover read `1.0` is
the exact pre-AR-11 defect and halves every `spread_net_*` figure.

### The cost side is restated, not shared

`trade_bps` is a compile-time reference to `EquityAllocationConfig{}.trade_bps`;
`annual_borrow_bps = 365` is a literal from the **deployed `equity-book` run configuration**,
not from `replay.hpp`, whose default is `0.0`. `replay.cpp`'s `borrow_charge` is file-local
and is never called. Every schema carries `cost_model_provenance` saying exactly that. **No
agreement with the replay's accounting is claimed.** The borrow leg uses actual calendar days
from the panel's own session keys, so weekends, holidays and multi-session gaps are charged;
two documented differences from the replay remain (a constant short leg across the window, and
an integer rather than fractional day count).

### The seal

`RejectSealedV1` refuses any observation at or after 2023-01-01. The 2013 slice contains none,
so the gate is **non-vacuous by code and vacuous by data** — a live gate this input does not
trip. `seal.json` says exactly that rather than implying a lockbox carve occurred.
`MaskSealedV1` and the `reserve_window` routing are unreachable at this stage and are covered
only synthetically.

---

## 5. Qualifications

§10.5's statement is mandatory and **byte-identical** in three places: `ic_summary.json`'s
`sign_and_shape_statement`, `manifest.json`'s, and the unwrapped line below. It is one ASCII
constant in `atx-impl/src/stage_equity_ic.cpp` (`kSignAndShapeStatement`) reproduced here
exactly, so a diff of the three finds nothing (§11.9, ruling I-6).

```text
A 189-observation result is sign-and-shape evidence only. It is not accepted alpha, not a Sharpe, not evidence of trading capacity, and not grounds for selecting a signal, a horizon, a cadence or a threshold. Research D-1: "189 dates x roughly 200-400 admitted names is enough for a decay *shape* and a coverage table, and enough to expose a zero or negative IC - but it is a thin sample for an ICIR. Report the bootstrap interval, never a bare point estimate, and select nothing on it."
```

1. **An IC is a model-skill statistic, not a return.** It must not be presented as investment
   Sharpe or evidence of trading capacity. A positive IC may still be uninvestable.
2. **The price series is not a verified total-return series.** The KLAC 2026-06-12
   contradiction prevents describing the adjusted-close field as verified across all
   securities and dates. Every IC here inherits that.
3. **Dropping missing forward returns is itself a selection, and it biases upward.** Variant B
   corrects the *evidenced* fraction only — three ids, whose legs are a few basis points — and
   both variants drop the same names, so A-versus-B is **not** a survivorship sensitivity. Of
   the 34 required-mark ids, 29 are unclassified and their missing cells drop as ordinary
   missing marks; PCS is flagged terminal-without-evidence and counted, never priced. The
   `_ex34` restriction is the measure that actually speaks to this, and it bounds rather than
   removes the bias.
4. **The decay curve confounds horizon with calendar period**, and the common-sample column
   narrows that without removing it: the shared sample is 2013-H1-weighted, so the shape is a
   2013-H1 shape.
5. **`blend_equal` is a rank-space blend**, not a monotone image of a raw average. Its Pearson
   IC correlates a bounded near-uniform variable with a heavy-tailed return and should not be
   compared directly with signals 0 and 1's Pearson IC.
6. **The return is indexed from `t`; the deployed book executes at `t+1`.** This is a forecast
   statistic, not a book return, and it is not lookahead — signal and label windows are
   disjoint — but the curve is indexed one observation earlier than the book it informs.
7. **The universe is liquidity-screened only.** The archive contains ETFs, rights and test
   symbols; instrument-type eligibility is `unknown`. This is the IC of these expressions on
   **this** universe, not on a common-stock universe.
8. **Session keys are labels, not availability.** Vendor delivery is 05:00 CT T+1 full
   history; a midnight session key is not proof the data was known on its stated trading date.
9. **`1 - rho_rank` is a rule of thumb**, not an accounting identity, and is not comparable to
   the replay's realized turnover.
10. **No sanitizer, no static analyser, no include-clean build and no CI covers this work.**
11. **Nothing here completes a replay, prices a corporate action, or changes PCS admission.**
    PCS admission remains **rejected** and no PCS terminal return is ever computed or applied.
12. **No live trading and no broker action is performed or authorized.**
