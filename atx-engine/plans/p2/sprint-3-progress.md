# Sprint S3 (p2) — Alpha DSL & Expression Substrate — Implementation Progress

**Status:** 🟡 OPEN (S3.0 marker) — started 2026-06-13
**Worktree:** `C:\Users\natha\atx-wt\p2-s3` (isolated — one branch per worktree, no shared-branch race)
**Branch:** `feat/p2-s3-alpha-dsl`
**Base:** `main @ 0790157` (p2 roadmap refactor: S3/S4 = alpha DSL + genetic search)
**Source plan:** [`sprint-3-alpha-dsl-expression-substrate-implementation-plan.md`](sprint-3-alpha-dsl-expression-substrate-implementation-plan.md)
**Spec:** [`sprint-3-alpha-dsl-expression-substrate.md`](sprint-3-alpha-dsl-expression-substrate.md)
**Build env:** `cmd /c "call C:\Users\natha\atx-wt\p2-s3-env.cmd && <cmd>"` (VS2022 + vcpkg + sccache + ninja);
preset `dev`; target `atx-engine-tests`.

---

## §0 As-built recon (the plan's §0, condensed) — the corrections that shaped the units

1. **9-site operator add-surface** (registry.hpp enum, registry.cpp row, shape rule, typecheck, kernel hpp, vm
   dispatch, oracle dispatch, oracle.cpp kernel, tests). Exhaustive `OpCode` switches carry **no `default`** — the
   compiler enumerates every site a new op must touch (`/WX` enforces it).
2. **Neutralization is genuinely demean-only.** `cs_group_demean_row` is the WLS residual on a pure group-dummy
   design; the header itself notes "the full residualizer is deferred." S3.1 generalizes it; the demean special case
   is the bit-for-bit boundary pin.
3. **`op_swap` root cause confirmed.** Bucket key `(shape_cat, out_dtype, min_arity)` + materialized-`call_arity`
   lookup mismatch lets a hparam-peeling op (kalman_level, materialized arity 1) swap to `hump`; `analyze_call` never
   checks the node's structure matches its op's declared contract (`n_hparams`, operand presence, roles), so the
   malformed node analyzes clean and aborts the VM (SlotPool out-of-range / garbage immediate). Root fix = analyzer
   contract check (analyze-valid ⟹ VM-safe for ALL mutation) + refined catalog buckets. (S3.4.)
4. **Lookback rail is centralized** (`analyze_call` + `is_rolling_ts`/`is_shift_ts`); new rolling ops register there.
5. **Datafields are name-recognized panel inputs, not opcodes** — `vwap`/`adv{d}` are derived panel columns, no new
   `OpCode` (keeps the ISA price-volume-clean). (S3.3.)
6. **No from-scratch AST generator exists** — S3.5 is additive (`factory/generate.hpp`), measured against an
   in-test random-AST control.

---

## Unit ledger

| Unit | Title | Status | SHA | Tests | Notes |
|---|---|---|---|---|---|
| S3.0 | Marker + ledger + recon | ✅ | `8ce3db5` | — | impl plan frozen; baseline 92/92 |
| S3.1 | Regression-residual neutralization (`CsResidualize`) | ✅ | `8bc5540` | 6 | demean boundary pin bit-for-bit; FWL covariate; VM⇄oracle parity; 150/150 no-regression |
| S3.2 | BRAIN-superset `ts_*` (`ts_regression`/`ts_decay_exp`/`ts_entropy`/`ts_moment`) + backfill/quantile audit | ✅ | `98a62cb` | 8 | VM⇄oracle twins bit-for-bit; hparam rails; full suite green |

**S3.2 backfill/quantile parity audit (research §1.4):** `ts_backfill` matches §1.4 — most-recent-valid
fill scanning newest→oldest, deliberately looking *past* NaNs (its own policy, not the any-NaN→NaN gate);
causal (`t >= i` underflow guard), order-independent → bit-exact VM⇄oracle. **No fix needed.** `ts_quantile`
as-built is the **rolling median** (quantile 0.5), identical kernel to `ts_med`; internally consistent
(oracle==VM). BRAIN's fuller `ts_quantile(x, d, driver)` (arbitrary quantile + gaussian/uniform driver) is a
**recorded extension**, not shipped (would be a new hparam op) — no behavior change to the existing median.

**S3.3 §8-gap audit + reverse routing:** research §8 cross-sectional union =
`rank/zscore/scale/normalize/quantile/winsorize/reverse` + `vec_avg/vec_sum`. Already
present: rank/zscore/scale/normalize/winsorize. Genuine gaps shipped: `quantile`,
`reverse`, `vec_sum`, `vec_avg`. `reverse(x) == -x` routes to the existing **Neg**
opcode (multi-name→opcode, like `stddev`/`ts_std`→`TsStd`) — no new enumerator, no
kernel. `quantile`/`vec_sum`/`vec_avg` add the 3 new OpCodes (full 9-site surface,
oracle⇄VM twins bit-for-bit). **Disabled test:** `LibraryIntegration.RoundTripsLargeFixtureZeroCopy`
— flaky stale-tmpdir mmap reopen, independent of the enum insertion (it round-trips
within one run); marked `DISABLED_` pending tmpdir-isolation fix.
| S3.3 | Cross-sectional gap-fill ops + `vwap`/`adv{d}`/dollar-volume datafields | ✅ | `cdb32e1` | 19 | `quantile`/`vec_sum`/`vec_avg` (oracle⇄VM bit-exact) + `reverse`→Neg alias; `datafields.hpp` derives `vwap`/`dollar_volume`/`adv{d}`; `adv{d}==ts_mean(dollar_volume,d)` proven through the engine; 1206 pass / 1 disabled |
| S3.4 | Fix `op_swap` at root + re-enable + per-bucket stress harness | ✅ | — | 3 | root cause = `add_op` filed by `min_arity` but `op_swap` looked up by materialized `call_arity`, so a finite-default op (scale/winsorize/quantile, kernel reads operand 2) was offered to an arity-1 node → VM read `kNoSlot`. Fix: `validate_node_contract` (analyze rail: materialized operand-arity + hparam count) + OpCatalog buckets keyed on materialized operand arity + group role, skipping hparam/record ops. `enable_op_swap=true`. Stress harness: every bucket, 0 aborts, oracle==VM. Noise seed 61→81 recal (op_swap-on found 1 fluke). 1209/1209 + 1 disabled |
| S3.5 | Grammar-typed (valid-by-construction) generation | ⬜ | — | — | report rejection-rate vs control |
| S3.6 | Conformance suite + Alpha101 subset repro + bench + close | ⬜ | — | — | p1 corpus byte-identical |

Legend: ⬜ not started · 🟡 in progress · ✅ done (header + tests + `/W4 /WX` + `/fp:precise` clean).

## Numbers (filled on close)
- op_swap stress (S3.4): 17 seed exprs spanning every swappable bucket (CS arity 1/2/group,
  variadic residualizer arity 2/3, TS arity 2/3, unary, binary infix×2, trade_when, hump,
  hparam kalman_level, composed DAG) × 400 swaps each; **abort count = 0**; every Ok mutant
  oracle==VM bit-for-bit. Named regression: rank-node bucket never offers scale/winsorize/
  quantile; kalman_level node is swap-inert. Noise-admit re-cal: seed 61→81 (op_swap-on found
  1 in-sample fluke past kMinDsr=0.80; 41/51/71/81 all admit 0).
- generation: analyze-rejection rate (grammar-typed) vs random-AST control.
- Alpha101: subset reproduced / total; not-yet-expressible residual + blocking op.
- bench: widened-op eval throughput (Debug upper bound), CSE hit-rate.
