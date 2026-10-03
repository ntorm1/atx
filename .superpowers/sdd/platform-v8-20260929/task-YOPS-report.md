# Task YOPS report — DSL ops for the 101-formulaic-alphas constructs (platform v8)

Lane YOPS, worktree `C:/atx-wt/pool-12`, branch `feat/platform-v8-yops-20261002`, base `798d3b23`.
No C++ was built and no real data was read (lane rules). The Python checker and its tests ran on synthetic panels only.

| item | commit | content |
|---|---|---|
| 1 | `19d58b6d` | `group_sum(x, g)` (opcode `CsSumG` 105) |
| 2 | `97e6befc` | the as-of rank family (106 to 111 with `group_delay`), `group_delay`, the oracle twin, `yops_check.py` (10 frozen strings) |
| 3 | `b5596bd1` | `alpha_formulaic_ops_test.cpp` (gtest) and `test_yops_check.py` (pytest) |
| 3 | `75a73cac` | cross-lane: lists `asof_ops.hpp` in the DSL-VM sources tripwire and re-pins it (no semantics bump) |
| 4 | `7bfead7a` | this report |
| - | `6674e986` | vm.hpp comment (the ChunkAxis vocabulary); the tripwire pin was computed with it and matches at HEAD |
| - | `fe790af7` | test battery: a 6-session as-of correlation on `close` (robust finite-cell check) |

**No registration changed.** No existing registry row, opcode id, kernel result, typecheck rule for an existing op, or
factory table changed (section 7 gives the proof and how root checks it).

## 1. Ops added

All seven rows are in a new table, `detail::formulaic_ops()` (`atx-engine/src/alpha/registry.cpp`). Every `Library` registers it after the
built-in and literature rows. It is kept out of `builtin_ops()` (74 rows) and `literature_ops()` (17 rows), so the factory's
`OpCatalog`, the wrapper set and every seeded search draw are unchanged. The opcodes are appended after `TsCorrMp` (104), and
`static_assert`s freeze 105 and 111.

| name | arity (peeled) | opcode | out | kernel (VM + streaming) | oracle twin |
|---|---|---|---|---|---|
| `group_sum(x, g)` | 2 (0) | `CsSumG` 105 | F64 | `cs_ops.hpp` `cs_group_aggregate_row(..., GroupAgg::Sum)` | `oracle.cpp` `cs_group_sum` |
| `group_delay(g, d)` | 2 (0) | `GroupDelay` 106 | Group | delay's shift: VM `eval_ts_lookback(TsDelay)`, streaming `TsKind::Lookback` | `oracle_formulaic.cpp` |
| `asof_rank_ts_rank(x, w, d, j)` | 4 (2) | `AsofRankTsRank` 107 | F64 | `asof_ops.hpp` `asof_rank_row` | `oracle_formulaic.cpp` |
| `asof_rank_ts_min(x, w, d, j)` | 4 (2) | `AsofRankTsMin` 108 | F64 | same | same |
| `asof_rank_decay_linear(x, w, d, j)` | 4 (2) | `AsofRankDecayLinear` 109 | F64 | same | same |
| `asof_rank_correlation(x, w, y, d, j)` | 5 (2) | `AsofRankCorr` 110 | F64 | same | same |
| `asof_rank_covariance(x, w, y, d, j)` | 5 (2) | `AsofRankCov` 111 | F64 | same | same |

### 1.1 group_sum(x, g)

- **Value.** On each date, the sum of x over the group's valid members, written to each valid member. A valid member is a non-NaN
  x with a non-NaN label (and, under the VM's Cs eligibility mask, an eligible name).
- **Order.** The sum runs in ascending instrument order, the order `group_mean` uses.
- **Missing values.**
  - A cell whose own x or label is NaN stays NaN, as for `group_mean` and `group_count`.
  - A group with no valid member has only NaN cells.
  - ±inf members are included, as in `group_mean`.
  - The brief's "finite members" is read as the house valid set (non-NaN). A deviation note is in section 6.
- **Arguments.** The group argument is any Group classifier the existing group ops accept: a `grp_*` / `IndClass.*` / `sector`
  field, `bucket(...)`, `group_cross(...)` or `group_delay(...)`. The typecheck rule is `needs_group_arg`, as for `group_mean`.
- **Implementation.** `cs_group_count_mean_row` is now a wrapper over the shared accumulator `cs_group_aggregate_row`. The same
  loop and the same final division keep it value-preserving.
- **Slot cost.** One output slot, the same as `group_mean`. No scratch slot.
  - YSIG's value-weighted leave-one-out peer return in the XSIG mirror: **41 bars / 7 slots / 19 nodes** with `group_sum`,
    against 41 / 8 / 23 with `group_mean x group_count`. YSIG had estimated 20 nodes; the mirror gives 19.
  - The checker confirms `group_sum == group_mean x group_count` on 5,354 cells (rtol 1e-12).
- **group_count(g) is not added.** `group_count` is a built-in row of arity 2 (x, g), and an arity-1 overload would change that
  registration and the factory bucket it sits in. The group size is `group_count(s, g)` for any always-valid series s.

### 1.2 The as-of rank family (the missing construct, section 2)

`asof_rank_<outer>(x, w, [y,] d, j)` takes a window d (an integer in [1, 65535]) and a lag j (an integer in [0, 65535]); both
are peeled into `Instr::imm`. At date t the output is NaN while t + 1 < d + j. Otherwise:

1. The window is the d sessions s0 .. s0 + d - 1, where s0 = t + 1 - d - j. It ends j sessions before t.
2. The as-of factor w\* is, per instrument, the latest non-NaN w in rows s0 .. t. With w = `raw_close / close` this is
   1 / F(t), so x[s] · w\* is x at s adjusted as of t (XWQ reading R1). A halted session at t borrows the last factor, which is
   constant between corporate actions.
3. R[s] is the house cross-sectional rank `cs_rank_row` of x[s] · w\*: average ties, a singleton gives 0.5, NaN outside the
   valid set. The valid set is the non-NaN products and, under the VM's Cs mask, the names eligible at s.
4. The output is the house `<outer>` over each instrument's d values R[s0 .. s0 + d - 1]:
   - `ts_rank`, `ts_min` or `decay_linear` through `ts_value_at`;
   - `correlation` or `covariance` with y over the same d sessions through `ts_pair_at`.

   The kernels' NaN rules apply: any NaN in either window gives NaN, correlation's flat guard gives NaN, covariance uses
   ddof 1, and a correlation over d = 1 is NaN. The Engine's `RankTies` and `FlatGuard` policies are honoured.

The kernel semantics follow Kakushadze (arXiv:1601.00991, appendix A, function definitions as read in task-XWQ-report.md §2):

- rank: the cross-sectional rank;
- ts_rank(x, d): the time-series rank in the past d days;
- ts_min(x, d): the time-series min over the past d days;
- decay_linear(x, d): the weighted moving average over the past d days with linearly decaying weights d, d - 1, ..., 1,
  rescaled to sum to 1;
- correlation(x, y, d) and covariance(x, y, d): the time-serial correlation and covariance over the past d days;
- delay(x, d): the value d days ago.

Each is the house kernel already used by the S/L strings: R7 rank and ts_rank, and the `stddev` / `cov` ddof 1. The lag j is
`delay(<outer>(...), j)` with the inner ranks re-evaluated as of t.

**Identities, proven bit for bit in gtest:**

- With w ≡ 1 the op is `delay(<outer>(rank(x), d), j)`, because the product x · 1 is exact.
- With d = 1 the op is the as-of rank at lag j. The minimum of one value is that value.

**Typecheck.** `analyze_formulaic_call` owns all rails:

- x, w and y must be F64 non-scalar non-record vectors;
- d and j must be integer literals in range;
- lookback = (d - 1) + j + child, and a u16 overflow is refused.

**Execution.**

- **VM.** `Engine::eval_asof` runs over date rows; `chunk_axis` is Dates. Row t reads only input rows ≤ t, so the global-DAG
  date bands are independent.
- **Streaming.** `exec_asof` keeps rings of d + j rows for x, w and y. It gathers them chronologically and calls the same
  `asof_rank_row`.
- **Modes.** The outer kernels are the batch per-cell ones in every EvalMode, so ResearchFast == AuditExact for this family.

**Slot cost.** One output slot per call. There is no scratch slot; the scratch belongs to the executor:

- VM: `AsofScratch` holds (d + 2)·N doubles plus a valid list;
- streaming: (d + j)·N doubles per input in rings.

**Time cost.** O(d · N log N) per date per call (d re-ranked rows).

### 1.3 group_delay(g, d)

`group_delay(g, d)` is the classifier d sessions ago (NaN for the first d dates), with lookback d + child. `delay` refuses a
Group, so #80's lagged point-in-time industry needs this op.

- **Kernel.** It is delay's kernel: VM `eval_ts_lookback(TsDelay)` on instrument columns, streaming `TsKind::Lookback`, oracle
  `ts_unary_at(TsDelay)`.
- **`is_shift_ts`.** It now includes `GroupDelay`, used only by the factory's window-literal classification. `analyze_call`
  routes the op to `analyze_formulaic_call` before any shift-family rule runs.

## 2. The 12 non-vwap formulas: the construct the catalog lacked

The 12 are XWQ's class N: #1, #3, #4, #13, #15, #16, #29, #31, #68, #80, #88, #92.

**The construct.** In each, a cross-sectional function of a price level sits inside a time-series operator. Under R1 (the
paper's section 2: prices adjusted "if the ex-date is today"), the value at date t needs each past cross-section s ≤ t
re-evaluated on prices adjusted as of t. That is a quantity of (s, t), not of s alone. Every house node is a function of its
date only, so `ts_rank(rank(x), d)` reads rank(x) as stored on each past date. The catalog had no node whose past cross-sections
are evaluated as of the current date. This is a missing construct, not a missing field or an operator alias.

**The fewest ops with exact semantics.**

- **(a) One as-of rank kernel,** fused with the five outer time-series ops that occur directly around an as-of rank in the 12:
  ts_rank (#4), ts_min (#29, and #88 through d = 1), decay_linear (#31), correlation (#3, #15) and covariance (#13, #16). This
  is five registry rows with one kernel.
  - The lag j covers outer windows that the paper wraps once more: #15 sums three lagged correlations; #29 takes the min of
    five lagged terms.
  - d = 1 gives the plain as-of rank at any lag. #88 needs it because its decay runs over a *composite* of four ranks (see
    below).
- **(b) group_delay** for #80. Its `delta(indneutralize(P, IndClass), 4)` neutralizes the session t-4 price (as of t) within
  the industries of session t-4 (point in time).
- **(c) Nothing for #1.** Its as-of effect is inside a mix of closes and stddevs under `ts_argmax`, and it decomposes exactly
  with existing ops:
  - split v into the stddev branch A and the close branch B;
  - the as-of close level is B · K(t), one positive number per line over the window;
  - take the first argmax of whichever branch holds the overall maximum, and the earlier of the two on a tie.

**#88 exactness note.** `decay_linear` is applied to (rank(open) + rank(low)) - (rank(high) + rank(close)). One as-of decay per
price (decay(a) + decay(b) - ...) is equal in exact arithmetic but reassociates the floating sums, which flips exact ties of
the outer rank. On the synthetic world the mismatch was max |d| 0.39. So the composite is built per lag k from
`asof_rank_ts_min(p, K, 1, k)`, and the decay is unrolled in the kernel's order:
`(((1 * C7) + (2 * C6)) + ... + (8 * C0)) / 36` (ts_ops.hpp: acc = 0 + 1·C[t-7] + ...; C is never -0). The result is bit-exact.

| # | status | ops used |
|---|---|---|
| 1 | frozen, exact | existing ops only (two-branch ts_argmax decomposition) |
| 3 | frozen, exact | `asof_rank_correlation` |
| 4 | frozen, exact | `asof_rank_ts_rank` |
| 13 | frozen, exact | `asof_rank_covariance` |
| 15 | frozen, exact | `asof_rank_correlation` at lags 2, 1, 0 (the printed `sum(., 3)` unrolled) |
| 16 | frozen, exact | `asof_rank_covariance` |
| 29 | frozen, exact (degenerate as printed) | `asof_rank_ts_min` at lags 4..0 (the printed `ts_min(., 5)` unrolled) |
| 31 | frozen, exact | `asof_rank_decay_linear` |
| 80 | frozen, exact | `group_delay`, `ts_backfill(raw_close / close, 5)` as the as-of factor |
| 88 | frozen, exact | `asof_rank_ts_min(., 1, k)` x 32, decay unrolled |
| 68 | **blocked** (byte limit) | verified unrolled; 5,315 bytes > 4,096 |
| 92 | **blocked** | four as-of stages; tens of kilobytes unrolled |

**10 of the 12 are now expressible exactly.** No vwap work was done; the 43 vwap formulas are untouched.

## 3. Notes on individual formulas

- **#29 is degenerate as printed** (as XWQ found). In its first term, rank lies in [0, 1], so `log` hits 0 and scale's L1 norm is
  inf. The term is the tie rank 0.5 on every finite cell (4,586 checked). The transcription is exact; planted errors in that term
  show only through the NaN pattern, and the mutants are chosen accordingly.
- **#1: two exact parts never bind on realistic data.** The rebase K in the cross-branch comparison and the tie branch
  `min(argA, argB)` are kept for exactness. An as-of price always exceeds a 20-day return stddev, so the comparison never turns
  on K, and the two maxima never tie. Dropping either is therefore not a detectable planted error. The as-of reading still
  matters for #1 through the within-window close order, and the point-in-time string differs from the printed formula.
- **#15 is finite on only 562 of 5,760 synthetic cells.** A 3-session correlation of ranks is NaN whenever a name's price rank
  is unchanged for three sessions (the flat guard), which is the common case for price-level ranks. The printed formula behaves
  the same; the value is equal cell for cell.
- **#80** uses `ts_backfill(raw_close / close, 5)` as the as-of factor on both the t and t-4 terms. Replacing it with the raw
  ratio is a planted error that fails.

## 4. The frozen DSL strings (SHA-256 of the exact bytes)

Templates: {R} = `((close / delay(close, 1)) - 1)`, {K} = `(raw_close / close)`, adv{d} = `ts_mean((raw_close * volume), d)`.
Figures come from lane XSIG's compiler mirror (`xsig_check.py`), extended for the new ops by `yops_check.py`:

- bars = lookback;
- slots = peak live slots;
- nodes = DAG nodes;
- extra fields = fields outside the registry's field rows.

House form is PM7-34's `R(decay_linear(x, f))`.

| # | bytes | bars | slots | nodes | extra fields | house form | form bars / slots | SHA-256 (string) |
|---|---|---|---|---|---|---|---|---|
| 1 | 763 | 24 | 9 | 28 | - | R(decay_linear(x, 21)) | 44 / 9 | `cd6d1589a3756d3326ef8fffee724af2ae9c43b00dda1f3767702054edb748ee` |
| 3 | 80 | 9 | 5 | 9 | open_adj | R(decay_linear(x, 21)) | 29 / 5 | `c8016b408c87f2c9add9b834968ef7f13e77ba040e5989402a6f89ae334f96d6` |
| 4 | 60 | 8 | 5 | 7 | low_adj | R(decay_linear(x, 5)) | 12 / 5 | `1a4f1e40c0ee80ea68ba2e9d63a598907cfea0ab367a90f0f7c2d2aae7d1a151` |
| 13 | 81 | 4 | 5 | 9 | - | R(decay_linear(x, 5)) | 8 / 5 | `036fdbcf75b39e15169897c8f40831c72f7f15ef1339d40edef247095a1bec8f` |
| 15 | 251 | 4 | 7 | 16 | high_adj | R(decay_linear(x, 5)) | 8 / 7 | `74dc73d5e6f31136798ba6b1498343f06705dddda63cd073086c392f843001b8` |
| 16 | 84 | 4 | 5 | 10 | high_adj | R(decay_linear(x, 5)) | 8 / 5 | `10e331f2a68f806c5fa0408f6793b17f61d34e9d115354ca5af7ad05c827d687` |
| 29 | 693 | 11 | 9 | 56 | - | R(decay_linear(x, 5)) | 15 / 9 | `a3b9ef0a636e1e748de2915ab2491850d4aea4c4057f1bb058ad2630d0da4b6c` |
| 31 | 227 | 30 | 7 | 27 | low_adj | R(decay_linear(x, 21)) | 50 / 7 | `d844365bb80a187e3f0eeeee86f43a9a6fe0b53316e4fc462a224b2eea46375b` |
| 80 | 366 | 17 | 10 | 33 | grp_ff49, high_adj, open_adj | R(decay_linear(x, 21)) | 37 / 10 | `307be4989b6f7e616e612b43f91971cbd3f7100d41f604054cb213fff3b402e3` |
| 88 | 1997 | 91 | 14 | 99 | high_adj, low_adj, open_adj | R(decay_linear(x, 21)) | 111 / 14 | `0e4bd47e30e5acbf882519b680cf1353c27e1ed744c5aa24eb396df04a59da55` |

The same SHA-256s are pinned in `AlphaFormulaicOps_Frozen` (C++, via `atx::core::sha256_hex`) and in `test_yops_check.py`.

#1 (house form sha256 `1ed59b990a552e9da06adbb36de83ea2a3091505ef7f499349d4d95da2a10322`):

```
(rank((((ts_max(((((close / delay(close, 1)) - 1) < 0) ? -1 : close), 5) * (raw_close / close)) > ts_max(((((close / delay(close, 1)) - 1) < 0) ? stddev(((close / delay(close, 1)) - 1), 20) : -1), 5)) ? ts_argmax(((((close / delay(close, 1)) - 1) < 0) ? -1 : close), 5) : ((ts_max(((((close / delay(close, 1)) - 1) < 0) ? stddev(((close / delay(close, 1)) - 1), 20) : -1), 5) > (ts_max(((((close / delay(close, 1)) - 1) < 0) ? -1 : close), 5) * (raw_close / close))) ? ts_argmax(((((close / delay(close, 1)) - 1) < 0) ? stddev(((close / delay(close, 1)) - 1), 20) : -1), 5) : min(ts_argmax(((((close / delay(close, 1)) - 1) < 0) ? stddev(((close / delay(close, 1)) - 1), 20) : -1), 5), ts_argmax(((((close / delay(close, 1)) - 1) < 0) ? -1 : close), 5))))) - 0.5)
```

#3 (house form sha256 `e9d700168b2b38b34d5641d1069798067bdef9d9070ae310a15b811e0d2d7229`):

```
(-1 * asof_rank_correlation(open_adj, (raw_close / close), rank(volume), 10, 0))
```

#4 (house form sha256 `b8eb9f8b970f9f436c69471d6a6bc24c54d302f986d9ad5a002d0c05b03a9129`):

```
(-1 * asof_rank_ts_rank(low_adj, (raw_close / close), 9, 0))
```

#13 (house form sha256 `c993a4c754d41c1dbb387c22c317a76763328439daba54e9adf2ca178e2111d1`):

```
(-1 * rank(asof_rank_covariance(close, (raw_close / close), rank(volume), 5, 0)))
```

#15 (house form sha256 `5a19ed71c61dbc523f7ab543fedfc3304b7c982a0ef5f909f29579c42fb45c1d`):

```
(-1 * ((rank(asof_rank_correlation(high_adj, (raw_close / close), rank(volume), 3, 2)) + rank(asof_rank_correlation(high_adj, (raw_close / close), rank(volume), 3, 1))) + rank(asof_rank_correlation(high_adj, (raw_close / close), rank(volume), 3, 0))))
```

#16 (house form sha256 `a51dfed6c3cf0c6cf8509215883af0fbbc70bb8cd9b2a07412ae94bddd052c38`):

```
(-1 * rank(asof_rank_covariance(high_adj, (raw_close / close), rank(volume), 5, 0)))
```

#29 (house form sha256 `7aef57d19f8f96b904fee4d402b9178318e65ce042bf5e6feb97e1f4bbfe19de`):

```
(min(min(min(min(product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), (raw_close / close), 2, 4), 1))))), 1), product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), (raw_close / close), 2, 3), 1))))), 1)), product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), (raw_close / close), 2, 2), 1))))), 1)), product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), (raw_close / close), 2, 1), 1))))), 1)), product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), (raw_close / close), 2, 0), 1))))), 1)) + ts_rank(delay((-1 * ((close / delay(close, 1)) - 1)), 6), 5))
```

#31 (house form sha256 `29e5d5f8577cb9ec69d49a7dd6bd3dc0ffea311f7eba632f79d0bf910a14b640`):

```
((rank(rank(rank((-1 * asof_rank_decay_linear(delta(close, 10), (raw_close / close), 10, 0))))) + rank((-1 * (delta(close, 3) * (raw_close / close))))) + sign(scale(correlation(ts_mean((raw_close * volume), 20), low_adj, 12))))
```

#80 (house form sha256 `589c76b0270976752d2713294ddf6e5236bf42b3237b546a3497b7fcbcb4dc3e`):

```
(-1 * power(rank(sign((indneutralize((((open_adj * 0.868128) + (high_adj * (1 - 0.868128))) * ts_backfill((raw_close / close), 5)), grp_ff49) - indneutralize((delay(((open_adj * 0.868128) + (high_adj * (1 - 0.868128))), 4) * ts_backfill((raw_close / close), 5)), group_delay(grp_ff49, 4))))), ts_rank(correlation(high_adj, ts_mean((raw_close * volume), 10), 5), 5)))
```

#88 (house form sha256 `2fb38ede7a5b291f0f5a883a2aec73989e0591906831090f80870dfcd1b4be81`):

```
min(rank((((((((((1 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 7) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 7)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 7) + asof_rank_ts_min(close, (raw_close / close), 1, 7)))) + (2 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 6) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 6)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 6) + asof_rank_ts_min(close, (raw_close / close), 1, 6))))) + (3 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 5) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 5)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 5) + asof_rank_ts_min(close, (raw_close / close), 1, 5))))) + (4 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 4) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 4)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 4) + asof_rank_ts_min(close, (raw_close / close), 1, 4))))) + (5 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 3) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 3)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 3) + asof_rank_ts_min(close, (raw_close / close), 1, 3))))) + (6 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 2) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 2)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 2) + asof_rank_ts_min(close, (raw_close / close), 1, 2))))) + (7 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 1) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 1)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 1) + asof_rank_ts_min(close, (raw_close / close), 1, 1))))) + (8 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 0) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 0)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 0) + asof_rank_ts_min(close, (raw_close / close), 1, 0))))) / 36)), ts_rank(decay_linear(correlation(ts_rank(close, 8), ts_rank(ts_mean((raw_close * volume), 60), 20), 8), 6), 2))
```

**Still blocked.**

- **#68** has three as-of stages: `Ts_Rank(correlation(rank(high), rank(adv15), 8.9), 13.9)`, whose inner rank(high) is
  re-evaluated as of the day.
  - It is verified exact in its unrolled form: 13 lagged `asof_rank_correlation(high_adj, K, rank(adv15), 8, j)` and 24
    comparisons that rebuild ts_rank's average-tie count, equal to the printed formula on 772 cells.
  - It is 5,315 bytes, over the IC runner's 4,096-byte DSL limit.
  - Root's options: raise the limit, or add an `asof_rank_corr_ts_rank` stage. That would be a new registration, so it is not
    done here.
- **#92** has four as-of stages: ts_rank(6) of decay_linear(6) of correlation(7) of rank(low). Unrolled it is tens of
  kilobytes, so it is blocked.

## 5. Checker (`yops_check.py`) and how the tests run

`yops_check.py` (synthetic, about 8 s) has four parts:

1. **Mirror.** It teaches XSIG's compiler mirror the new ops and checks that the 7 `formulaic_ops()` rows parse out of
   `registry.cpp`, with arity, opcode, dtype and peeled count.
2. **Interpreter.** XWQ's numpy interpreter gains the seven ops, written from section 1 independently of the C++.
3. **Oracle.** Each printed formula is implemented in numpy and evaluated at every t on prices re-adjusted as of t. The
   synthetic world has splits, dividends, a different anchor per line, 3 listings and 3 delistings in the sample, a halted
   session and 4 industry reclassifications.
4. **Checks.** Every frozen string must:
   - equal its oracle cell for cell, NaN pattern included;
   - not depend on the vendor anchor;
   - differ from the point-in-time reading, so the ops have teeth;
   - make every planted error fail. There are 40 planted errors (windows, lags, fields, signs, op swaps, factor removals) and
     each fails.

Output ends `yops_check: PASS`.

### How root verifies

- **Python:** `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .superpowers/sdd/platform-v8-20260929/test_yops_check.py`
  gives **20 passed** (here: 11.9 s). It covers:
  - the registry rows and the pinned SHA-256s;
  - rank and group_sum closed forms;
  - the as-of rebase pin;
  - the unit-factor identity;
  - a planted op error (a point-in-time factor) that is caught;
  - all 10 transcriptions with their probes;
  - #29 degenerate, #68 over the limit, and the YSIG group_sum figures.

  The checker itself can also be run directly: `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/yops_check.py`.
- **C++:** `powershell scripts\atx-build.ps1 build atx-engine-alpha-tests`, then
  `--gtest_filter=AlphaFormulaicOps*`. The suites are:
  - `AlphaFormulaicOps_Differential`:
    - every new op and its nestings: VM == oracle cell for cell;
    - a 6-root shared program: VM == oracle, and the fused, subtree-cached (cold and warm) and global-DAG (3 workers,
      16-cell chunks) paths equal plain evaluate.
  - `AlphaFormulaicOps_Streaming`: warm + step == batch bit for bit, AuditExact and ResearchFast.
  - `AlphaFormulaicOps_Modes`: ResearchFast == AuditExact for the family.
  - `AlphaFormulaicOps_GroupSum`:
    - a closed form with NaN members, a group with no valid member and NaN labels;
    - group sizes 1 and all;
    - a 300-name stress with exact integer sums;
    - group_sum == group_mean x group_count.
  - `AlphaFormulaicOps_GroupDelay`: shifted labels and a group_sum by delayed labels.
  - `AlphaFormulaicOps_AsofRank`:
    - the rebase re-ranks past sessions (the point-in-time reading differs);
    - the halted-session factor borrow;
    - lag and warm-up NaNs;
    - hand-computed covariance (-1/12, 0.25) and correlation (-1/(2√7)), a flat window and a NaN y.
  - `AlphaFormulaicOps_Identity`: w ≡ 1 equals `delay(<outer>(rank(x), d), j)` bit for bit under the default policy,
    `KernelPolicy::legacy_v1()` and a Cs eligibility mask.
  - `AlphaFormulaicOps_Typecheck`: 27 refusals and 9 lookbacks.
  - `AlphaFormulaicOps_Registry`: tables 74 / 17 / 7, names absent from the factory tables, `group_count` still arity 2,
    opcode ids 88 / 104 to 111, chunk axes.
  - `AlphaFormulaicOps_Frozen`: the 10 strings, SHA-256 pinned, ≤ 4,096 bytes, VM == oracle, finite cells present.

## 6. Deviations from the brief

- **`group_count(g)` is not added.** It would change `group_count`'s registered arity (section 1.1); use
  `group_count(s, g)` instead.
- **group_sum's "finite members" means the house valid set.** That set is non-NaN x and non-NaN label; ±inf members are
  included, as in group_mean. A member whose own x is NaN gets NaN, as group_mean does. A group with no valid member is all NaN,
  as asked.
- **"Implement in the catalog"** means the registry, typecheck, VM, streaming and oracle. The factory's op-swap catalogue
  (`OpCatalog`) deliberately does **not** get the new ops: adding them would move the search golden and the X book's search
  draws. Opting them into the factory later, behind a flag like `OpCatalogCfg::literature_ops`, is a separate decision.
- **#88 is transcribed by unrolled d = 1 as-of ranks** rather than four as-of decays (section 2), for bit-exact ties.
- **#68 is verified but not frozen** (byte limit).

## 7. Byte-identity of the X book's library: the proof and the checks

**Static proof.** The diff changes no existing behaviour. Every existing-op edit is additive: new `case` labels, or new names
added to predicates (`is_cs_op`, `is_cross_section`, `needs_group_arg`, `is_instrument_op`, `chunk_axis`, `is_ts_op`, `classify`,
`is_shift_ts`). Two edits rewrite an existing expression, and both are value-preserving:

- `cs_group_count_mean_row` delegates to the shared accumulator: same loop, same order, same division;
- streaming `ts_lookback` reads `op != TsDelta ? shifted : current - shifted`, which is identical for TsDelay and TsDelta.

On the tables and opcodes:

- The new rows sit in a third table, so `builtin_ops()` (74) and `literature_ops()` (17), and hence `OpCatalog` and the wrapper
  set, are unchanged.
- Pre-existing opcode ids are unchanged (`static_assert`s at 86, 88, 89, 104).
- A pre-existing DSL string cannot name a new op: before this change such a string failed to parse with "unknown operator".
  So every string in the X book parses, typechecks, compiles and evaluates exactly as before.

**Checks root runs on the integrated build:**

1. `atx-engine-alpha-tests`, the whole binary: the existing VM / oracle / streaming / lit / slot-reuse / conformance suites,
   plus `AlphaFormulaicOps*`.
2. `atx-engine-factory-tests` with `--gtest_filter=NsgaSearch.*:SignalFitness*`. The search golden `0x889874a3b9b29c55` must
   hold at 1 and 4 workers; it holds iff the op catalogue is unchanged.
3. `atx-impl-strategy-ic-tests` with `--gtest_filter=StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`. This is the
   catalog-hash tripwire.
   - It hashes the 34 engine sources that parse or evaluate a DSL signal; `asof_ops.hpp` is now listed because `vm.hpp`
     includes it.
   - The re-pinned digest is `fcf8021e76ee5ac3c161302367a27b8f6c49cd28a580d1339ff801d9feca1619`. It is reproduced offline by
     the same algorithm; the base pin `ad6c4ca7...` reproduces at `798d3b23`.
   - `dsl_vm_semantics_version` stays **1** (version pin unchanged) because no pre-existing string's evaluated bits move.
     The candidate-signal cache identity stays `dslvm1_*`, so cached X-book signals remain valid.
4. **Identity run (PM7-30 / PM7-39 rule).** X-5's w pass, fit and NAV re-run on the new build with the usual argv, but with
   `candidate_cache_directory` pointed at a **fresh empty directory**. Every X-book signal is then recomputed by the new VM
   instead of served from the unchanged-identity cache.
   - Expected: NAV and w byte-identical, timing fields excepted; fit identical after the usual `script_sha256`
     substitution.
   - No `registry_sha256` substitution should be needed: no library or registry row moved.

   **This step is required.** With a warm cache, steps 1-3 alone would not exercise the X-book strings under the new binary.

## 8. Files

- `atx-engine/include/atx/engine/alpha/registry.hpp`:
  - opcodes 105 to 111 with `static_assert`s;
  - `formulaic_ops()`;
  - `is_asof_op`, `asof_is_pair`, `asof_inner_op`, `is_formulaic_op`.
- `atx-engine/src/alpha/registry.cpp`: the 7 rows; the Library registers them.
- `atx-engine/include/atx/engine/alpha/asof_ops.hpp` (new): `AsofScratch`, `asof_geom`, `asof_rank_row`.
- `atx-engine/include/atx/engine/alpha/cs_ops.hpp`: `GroupAgg`, `cs_group_aggregate_row` (item 1).
- `atx-engine/include/atx/engine/alpha/typecheck.hpp` and `src/alpha/typecheck.cpp`:
  - `analyze_formulaic_call`;
  - the predicate additions.
- `atx-engine/include/atx/engine/alpha/vm.hpp`:
  - `eval_asof`, the GroupDelay dispatch, `chunk_axis` / `is_instrument_op`, `asof_scratch_`;
  - `CsSumG` (item 1).
- `atx-engine/include/atx/engine/alpha/streaming_engine.hpp`:
  - `AsofState`, `analyze_asof`, `exec_asof`;
  - GroupDelay as Lookback;
  - `CsSumG`.
- `atx-engine/include/atx/engine/alpha/oracle.hpp`, `src/alpha/oracle.cpp` (`cs_group_sum`), and
  `src/alpha/oracle_formulaic.cpp` (new): the independent twins.
- `atx-engine/CMakeLists.txt`: adds `src/alpha/oracle_formulaic.cpp`.
- `atx-engine/tests/alpha/alpha_formulaic_ops_test.cpp` (new; globbed into `atx-engine-alpha-tests`).
- `.superpowers/sdd/platform-v8-20260929/yops_check.py` and `test_yops_check.py` (new).

## 9. Cross-lane edits

- **`atx-impl/src/strategy_ic_signal_cache.cpp`.** The `dsl_vm_sources` list grows from 33 to 34 (`asof_ops.hpp`), and
  `dsl_vm_sources_sha256` is re-pinned with a comment explaining why there is no bump.
  - This is required: without it, `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` fails on the closure check and on the
    digest.
  - Item 1 (`19d58b6d`) had already moved the digest by touching the listed `cs_ops.hpp`, `vm.hpp` and `registry.*`.
  - If any other lane also edits a listed file, root must re-pin once after merging. The algorithm is in the test (SHA-256
    over, per path, `"<path>\n<len>\n<LF text>"`).
- **`atx-engine/CMakeLists.txt`.** One new source line.

## 10. Open risks

- **Never compiled** (lane rule). The code was written against the owning files' idioms and self-reviewed for `/W4 /WX`:
  - no new casts with sign changes;
  - no shadowing;
  - 100 columns;
  - exhaustive switches updated: `is_rolling_ts`, `Oracle::dispatch`, `Engine::dispatch_range`.

  A first build may still surface a warning. The likeliest spots are `formulaic_fail`'s `auto` return in `typecheck.cpp` and
  the u8 / unsigned comparisons in the test.
- **Cost.** The as-of ops cost O(d · N log N) per date per call. #88 carries 32 one-row as-of ranks: about 32 cross-sectional
  ranks per date, comparable to a d = 32 window. That is acceptable but the heaviest of the ten.
- **Streaming memory.** Streaming keeps (d + j)·N doubles per input. The rails allow d, j ≤ 65535, so a pathological string
  could allocate large rings. The IC runner's 4,096-byte strings are unaffected.
- **The as-of factor's backfill reaches back only within the window span** (rows s0 .. t). An instrument with no factor in its
  whole span is NaN. This is correct for `raw_close / close`, which is present whenever the bar is.
- **Prices without a corporate-action factor** (unadjusted fields) need w ≡ 1; the op then reduces to the house composition.
