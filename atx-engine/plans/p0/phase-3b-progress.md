# Phase 3b — Implementation Progress

**Worktree:** none (direct on `feat/atx-core-stdlib`, per `.agents/atx-engine/agent.md` — the established
engine workflow; plan §9 permits in-place "per the established engine workflow — record in the ledger").
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `cfaf2d2` (Phase-3 close — `docs(p3-close)`; the prerequisite SHA this sprint extends).
**Started:** `2026-06-06`
**Source plan:** [`phase-3b-vm-completion-implementation-plan.md`](phase-3b-vm-completion-implementation-plan.md)
**Prior progress:** Phase 3 (alpha-expression DSL → VM) — see [`phase-3-progress.md`](phase-3-progress.md), closed at `cfaf2d2`.

> **Ledger state:** ✅ **CLOSED** 2026-06-06 — sprint 3b (vocabulary + variadic args + conformance), 5 units
> (P3b-0…P3b-4), +62 alpha-DSL tests over Phase 3's 272 (full Alpha suite 334/334). Sprint 3c (mass eval + VM→loop bridge) opens
> its own ledger [`phase-3c-progress.md`](phase-3c-progress.md) at P3c-0. Scope changes are recorded in
> *Plan adjustments* below, **not** in the frozen plan file.

---

## Plan adjustments vs. the source plan (fossil reconciliation)

The plan was frozen against *assumed* Phase-3 header/API names. Phase 3 shipped with different names; this
sprint builds on the **as-built** surface. The plan is a fossil — these deltas are recorded here, not edited
into it.

**(A) As-built header map (plan name → real name).** The plan references `engine.hpp`, `reference.hpp`,
`kernels_*.hpp`, `compile({...strings...})`, `Engine(panel, lib)`, `evaluate_batch`, `extract_streams`,
`PanelView`. As-built (Phase 3 close `cfaf2d2`):

| Plan-fossil name | As-built (Phase 3) |
|---|---|
| `engine.hpp` (Engine) | `vm.hpp` — `class Engine{ explicit Engine(const Panel&); Result<SignalSet> evaluate(const Program&); }` |
| `reference.hpp` (oracle) | `oracle.hpp` — `evaluate_reference(const Program&, const Panel&) → Result<SignalSet>` |
| `kernels_*.hpp` | `cs_ops.hpp` (cross-sectional kernels), `ts_ops.hpp` (time-series kernels) |
| `compile({strings}, lib)` one-shot | as-built pipeline: `parse_program(src, lib)` → `analyze(ast)` → `compile(ast, analysis)` |
| `PanelView` | `Panel` (self-contained, `panel.hpp`); no separate view type |
| `Engine(panel, lib)` | `Engine(const Panel&)` — the Library is consulted at parse/compile, not by the Engine |

New headers this sprint adds: none in 3b (ops live in the existing `registry.hpp`/`vm.hpp`/`oracle.hpp`/
`cs_ops.hpp`/`ts_ops.hpp`). 3c adds `streams.hpp` and a batch entry on `vm.hpp`.

**(B) No worktree; no `--no-ff` merge.** Phase 3's adjustment (2) stands: one working tree, direct on
`feat/atx-core-stdlib`; the branch IS the active line, so there is no worktree to merge. The close ceremony
runs every sprint.md step *except* the merge.

**(C) clang-tidy is disabled repo-wide** (`.agents/cpp/agent.md` §8, `.agents/atx-engine/agent.md`). The
per-unit gate is **`/W4 /permissive- /WX` + clang-format clean + tests green** (the plan §7 "clang-tidy clean"
done-criterion does not apply).

**(D) Shared-branch discipline.** `ROADMAP.md`, `.agents/cpp/agent.md`, `.clang-tidy`, `.clangd`, `.vscode/`
carry concurrent **uncommitted** edits (a parallel effort on this shared branch). Stage **explicit pathspecs
only** (never `git add -A`); do not touch those files. The ROADMAP Phase-3b status flip is left for the
concurrent owner (same posture as Phase 3's close note).

Realistic scope for this sprint (P3b-0…P3b-4):

1. **P3b-0** — Open this ledger; freeze scope; record the Phase-3 base SHA. Marker commit.
2. **P3b-1** — Variadic/default/optional operator arguments (`OpSig` `arity` → `[min_arity,max_arity]` +
   trailing `defaults`; parser default-fill + range check). The foundation the new ops sit on.
3. **P3b-2** — BRAIN-superset ops: element-wise (`sigmoid`/`tanh`/`normalize`/`winsorize`), rolling
   (`ts_zscore`/`ts_backfill`/`ts_av_diff`/`ts_quantile`/`ts_scale`/`ts_count_nans`), group
   (`group_count`/`group_mean`/`group_scale`). Each = 1 OpCode + 1 row + 1 VM kernel + 1 oracle entry + diff test.
4. **P3b-3** — `trade_when` + `hump` stateful causal recurrences (reuse the ema/wma state-slot mechanism).
5. **P3b-4** — Alpha101 conformance battery + pinned semantic resolutions + sprint-3b close.

Defer to Phase 3c (or future-work):

- **Batch / cross-alpha eval API, per-alpha PnL/position streams, `VmSignalSource` bridge, delay knob** →
  Phase 3c (P3c-0…P3c-4).
- **True O(1)/cell incremental rolling** for the new rolling ops — carry Phase-3's deferral (bit-exactness
  vs the oracle dominates; an online accumulator needs a documented ULP tolerance).
- **Full BRAIN op zoo** (100+ ops, region universes, vector/matrix ops) — ship the consultant-used subset;
  the registry makes any later op a one-row add.

---

## Per-unit ledger

| Unit | Status | Commit | Notes |
|------|--------|--------|-------|
| P3b-0 | ✅ done | `875d1e7` | Open ledger; freeze scope; fossil reconciliation (A–D) recorded; Base `cfaf2d2`. Marker commit. |
| P3b-1 | ✅ done | `b331728` | Generalized `OpSig` arity `u8 arity` → `[min_arity, max_arity]` + trailing `std::array<f64, kMaxDefaults> defaults` (kMaxDefaults=2). All 42 built-in rows migrated to the 8-field positional form (fixed-arity ⇒ min==max, empty defaults — no behavior change). Parser `parse_call` now range-checks arity (`min ≤ k ≤ max`, else ParseError) and default-fills omitted trailing args: finite default ⇒ append `Literal(default)` node, NaN sentinel ⇒ skip (kernel handles absence). **`scale` → (1,2,{1.0})**: the end-to-end proof — `scale(x)` materializes a 2nd `Literal 1.0` arg and is structurally equal to `scale(x,1)` (same OpSig row, same CsScale opcode, same 2 materialized args, arg1 Literal 1.0). **`group_neutralize` stays fixed-arity (2,2,{})** — optional cap deferred to P3b-4. Forced mechanical follow-on: `OpSig::arity` was read in `typecheck.hpp` (window selection) and `dag.hpp` (Call child_count); both now call a new `call_arity(const Expr&)` helper in `parser.hpp` that counts populated child slots (the materialized count — correct for variadic/default-filled calls). Zero semantic change; the full Phase-3 Alpha suite (DAG/typecheck/VM/oracle/cs/ts/proof) stays green. **12 new TEST blocks** (6 registry: min/max range, scale 1..2 + default 1.0, group_neutralize fixed 2, variadic register_op, + the two `register_op` arity-range guards added in the follow-up; 6 parser: scale default-fill + structural ≡ scale(x,1), explicit 2-arg unchanged, too-few/too-many ParseError, NaN-sentinel skip via synthetic register_op). Full Alpha suite: 272/272 green (277/277 after the fix + concurrent tsdb tests). **Follow-up `fix(p3b-1)`**: `register_op` now rejects `max_arity < min_arity` and `max_arity - min_arity > kMaxDefaults` (the parser's default-fill indexes `defaults[k-min_arity]` within that bound — previously latent OOB/UB for a malformed user op); trailing-optional invariant documented on `OpSig::defaults` + `// SAFETY:` precondition on `fill_default_args`. ⚠️ **Shared-index race:** the fix's staged content was swept into concurrent tsdb commit `1406815` before its own commit landed; content is correct + in HEAD, attribution carried by empty marker commit `a5dc4f5`. (Lesson adopted: path-limited `git commit -- <paths>` from here on.) |
| P3b-2 | ✅ done | `fcb119f` (kernels) + `2d84e56` (tests) | BRAIN-superset 13 ops, each = 1 OpCode + 1 registry row + 1 VM kernel + 1 INDEPENDENT oracle kernel + differential tests. **Element-wise (P→P):** `sigmoid` (1/(1+e^-x)), `tanh` — inline unary lambdas (vm.hpp `eval_unary` + oracle `op_sigmoid`/`std::tanh`), NaN→NaN naturally. **Cross-sectional (P→V, `cs_ops.hpp` + oracle `cs_*`):** `normalize` (cross-sectional demean x−mean_cs), `winsorize(x, std=4)` (clamp valid cells to mean±std·σ, σ=SAMPLE std ddof=1; sub-2-valid σ=NaN ⇒ pass-through; reads the `std` multiplier from its 2nd operand EXACTLY as CsScale reads its `a`), `group_count`/`group_mean` (shared `cs_group_count_mean_row`, broadcast member-count/mean), `group_scale` (per-group L1=1, zero-L1 group→0). **Rolling (P→P, `ts_ops.hpp` + oracle `ts_unary_at`, trailing `[t−d+1,t]`):** `ts_zscore` ((x−tsv_sum/d)/√tsv_var, full-window), `ts_av_diff` (x−tsv_sum/d), `ts_quantile` (rolling median — mirrors TsMed kernel exactly), `ts_scale` ((x−min)/(max−min), flat→0), all full-window any-NaN→NaN; `ts_backfill` (last valid in window, looks PAST NaNs — own policy, causal/underflow-safe scan), `ts_count_nans` (**full-window-only** convention, matching the simplest existing partial op delay/delta: incomplete window→NaN, else finite count; documented in-header). **Differential bit-for-bit proof:** every op evaluated by BOTH `Engine::evaluate` (vm.hpp) and `evaluate_reference` (oracle.hpp) on a fixed synthetic panel (delisted+NaN cells) and asserted cell-identical (NaN==NaN); VM & oracle kernels written independently but share the IDENTICAL reduction order (tsv_sum/tsv_var mirror sum_of/sample_var; quantile/backfill/count order-independent). **winsorize uses the P3b-1 default-arg machinery** — registered `(1,2,{4.0})`; `winsorize(x)` auto-fills 4.0, proven ≡ `winsorize(x,4)`. typecheck wired: new Cs ops added to `is_cross_section`, group ops to `needs_group_arg` (arg2=Group rail), new Ts ops to the exhaustive `is_rolling_ts` switch; Sigmoid/Tanh to its non-rolling arm. Registry array 42→55. **20 new TEST blocks** (2 differential incl. nested cross-family composites; 11 known-value: sigmoid 0/±large, tanh 0/large, normalize mean≈0, winsorize bound + default≡4, group count/mean/scale broadcast, ts_zscore ramp, ts_backfill gap+truncation-invariant, ts_quantile median, ts_scale ∈[0,1]+flat, ts_av_diff, ts_count_nans gap; 5 boundary: all-NaN, 1-instrument, single-member group, d=1, d>history). /W4 /permissive- /WX clean; clang-format clean; full Alpha 297/297 green (orchestrator-verified). The test file landed as a separate `test(p3b-2)` commit `2d84e56` — the path-limited `git commit -- <paths>` feat commit cannot stage an untracked file. **Deferred residual:** O(1)/cell incremental rolling (carries Phase-3 deferral — bit-exactness vs oracle dominates). |
| P3b-3 | ✅ done | `3a385c3` | **2 stateful causal-recurrence ops**: `trade_when(trigger, alpha, exit)` (arity 3) and `hump(x, threshold=0.01)` (arity 1–2). Each = 1 OpCode + 1 registry row + 1 VM kernel + 1 INDEPENDENT oracle recurrence + typecheck wiring + differential/causality tests. **NEW eval path `eval_recurrence`** (on both `vm.hpp::Engine` and the oracle): unlike the windowed Ts* ops (ema/wma seed on a trailing-d window and reset per window, dispatched via `eval_time_series`), these carry TRUE cross-date state from the panel's FIRST date forward (no window). Per instrument j we forward-scan dates t=0…D-1 holding the prior output out[t−1,j] — in the VM a **pooled `state_[n_instruments]` slot grown once and reused across calls (ZERO hot-path alloc)**; in the oracle a scalar `prior` per column — compute out[t,j], then advance the state. **`trade_when` policy** (exit branch FIRST): `out = NaN if exit_true; elif trigger_true → alpha[t]; else → prior`; first date holds NaN (flat) unless triggered-and-not-exited. trigger/exit are **Mask** dtype (`mask_true` = finite non-zero; a **NaN trigger/exit is false** → neither enters nor exits → holds prior), alpha is F64. **`hump` policy**: `out = x[t] if |x[t]−prior| STRICTLY > threshold else prior`; first date = x[0]; a NaN diff is never > thr so a NaN x[t] holds prior (no state poison). threshold read from the scalar 2nd operand EXACTLY as winsorize/CsScale read theirs (`src[1]` cell [0]); **`hump(x)` uses the P3b-1 default-fill** (registered `(1,2,{0.01})`, proven ≡ `hump(x, 0.01)`). **Causal BY CONSTRUCTION** (the #1 sprint risk): the scan reads only `state[t−1]` + operand cells at index t*I+j (date t); there is NO index into state[>t] or any input at a date > t — a forward reference is unrepresentable; `// SAFETY:` on the state-slot reuse + the no-state[>t] invariant in both `state_ops.hpp` (shared scalar policy) and the oracle recurrence. **Differential bit-for-bit**: VM (`eval_recurrence` + `state_ops.hpp` kernels) and the oracle's independent recurrence written separately, asserted cell-identical (NaN==NaN) on a synthetic panel with delisted+NaN cells, incl. nested/stateful-over-stateful composites (`hump(trade_when(...))`). **Truncation-invariance proven** per op (full vs date-truncated panel byte-identical at dates ≤ t — the causality proof). typecheck: per-arg dtype pins in `analyze_call` (trade_when arg0/arg2 Mask like Select's cond, arg1 F64; hump arg0 F64, optional arg1 F64 scalar); lookback takes the **max-child + 0** path (NOT in `is_rolling_ts`/`is_shift_ts` — a recurrence seeded at the first date adds no window term; nested child lookback still flows through). New header `state_ops.hpp`. Registry array 55→57; both OpCodes in BOTH exhaustive vm/oracle dispatch switches + the typecheck `is_rolling_ts` false-arm (the /W4 exhaustiveness rail). **20 new TEST blocks** (trade_when: hand-trace enter/hold/close/re-enter, first-date trigger/no-trigger/exit-dominates, all-trigger≡alpha, all-exit≡NaN, NaN-trigger/exit-holds; hump: suppress/pass, threshold boundary strict-`>`, first-date, default≡0.01, NaN-input; + differential ×2, truncation-invariance ×2, determinism, boundaries 1-date/1-instrument/all-NaN). /W4 /permissive- /WX clean; clang-format clean; full Alpha 317/317 green (+20). Deferred residual: none new (the recurrence is inherently O(1)/cell — no rolling-recompute deferral applies). |
| P3b-4 | ✅ done | `a375ca9` (+ fix `1efed1f`) | **Alpha101 conformance battery + floor(d) + 5 semantic locks.** New `alpha_conformance_test.cpp`: a deterministic, RNG-free synthetic panel (16 dates × 6 instruments; fields open/high/low/close/volume/vwap/returns/adv20/IndClass.sector, closed-form values so hand-computation is exact). **Battery = 14 expressible Alpha101 fixtures** (Alpha#4/#6/#101/#12/#23/#33/#41/#53/#54 + signedpower/scale/decay_linear/indneutralize/covariance probes) compiled as ONE program and asserted **VM == oracle bit-identical** (the established `same_cell` differential); 3 hand-value checks (Alpha#101 == 1/4.001 exact, Alpha#41 == √(base²−4)−(base+0.5) to 1e-12, rank == ordinal percentile i/(N−1) exact). **floor(d) CODE CHANGE** in `typecheck.hpp::window_value`: a non-integer **positive** literal window is now **floored** (was: rejected) — the paper mines fractional constants (`ts_mean(close, 8.7)` → window 8 ≡ `ts_mean(close, 8)`); the other rails are PRESERVED (non-constant → reject; `d ≤ 0` → reject; `floor(d) == 0`, e.g. `0.5` → reject; negatives → reject). `// SAFETY:` on the relaxed rail. **5 semantic locks** (each pinned + documented below): (1) `indneutralize` == per-group demean == `group_neutralize` (verified identical + vs hand demean); (2) `signedpower(x,a)` == `sign(x)·|x|^a` NOT `x^a` (kernel + end-to-end vs `power`); (3) `floor(d)` (8.7→8 compiles/evaluates, 0.5/neg/non-const reject, arity-3 corr window floors); (4) full-window `min_periods` (any-NaN window → NaN, uniform across ts_mean/ts_sum/stddev/correlation — the as-built reality; the plan's "partial allowed for ts_mean/ts_sum" is a fossil, documented honestly); (5) `min/max` element-wise (MinP/MaxP, 2 panels) vs `ts_min/ts_max` (TsMin/TsMax, panel+window) disambiguated by registry name + shape. **Out-of-battery list EXHAUSTIVE** via a 1..101 classification table + meta-test (every Alpha# in-battery or out-with-a-concrete-field-reason; no gap/duplicate; out-rows must carry a reason, in-rows must not). Out-of-battery count = **36** (driven purely by unshipped FIELDS — every operator is shipped): `cap` (×1: #56), `adv{d}` for d≠20 (×22), `IndClass.industry` (×9), `IndClass.subindustry` (×4); the **65** in-battery alphas use only shipped fields incl. adv20 + IndClass.sector. Existing `alpha_typecheck_test.cpp` `FractionalWindow_IsError` → rewritten to `FractionalWindow_IsFloored` (+ `SubOneWindow_FloorsToZero_IsError`) — the only existing test the rail change touched. **15 conformance TEST blocks**; /W4 /permissive- /WX clean; clang-format clean; full Alpha **333/333** green (was 317; +15 conformance, +1 from the typecheck split), whole binary 1388/1388. **No as-built mismatch found** — all 5 locks already matched the paper convention in the as-built VM+oracle; floor(d) was the only behavior change. **Follow-up `fix(p3b-4)`** (`1efed1f`): code-quality review found a reachable UB — `window_value` floored a positive literal then `static_cast<u16>` it, so a literal > 65535 (e.g. `ts_mean(close, 70000)`) floored in-range of `>= 1` but **overflowed the u16 destination = UB** ([conv.fpint]/1, not a clamp). Added an upper-bound rail (reject `floored > u16::max` → `"window literal too large (max 65535)"`) BEFORE the cast so the conversion is provably in `[1, 65535]`; corrected the `// SAFETY:` comment (integrality alone did not make the cast safe — the rails do); added `+limits` include + `OverMaxWindow_IsError` typecheck test (failing-first). Optional minors landed: meta-test now pins `in_count==65`/`out_count==36` (catches an In↔Out flip that still totals 101); `Membership::InBattery` documented ("expressible; a representative 14-alpha subset is evaluated as fixtures"). Full Alpha **334/334** green (+1 OverMaxWindow test). |

### P3b-4 semantic resolutions (locked)

The conformance battery (`alpha_conformance_test.cpp`) PINS the five disputed conventions with targeted
tests. Each lock states the engine's chosen convention and that it matches the *101 Formulaic Alphas* paper
intent. **All five already matched the as-built VM+oracle** (verified against the kernels) — only floor(d)
required a code change; the rest are now guarded against regression.

1. **`indneutralize(x, g)` = per-group DEMEAN.** As-built: `indneutralize` → `OpCode::CsDemeanG`,
   `group_neutralize` → `OpCode::CsNeutG`, and both kernels route to `cs_group_demean_row` (subtract the
   per-group mean within the valid set; a NaN group label stays NaN). The lock asserts
   `indneutralize(x, g) == group_neutralize(x, g)` cell-identical AND both == the hand-computed per-group
   demean. Matches the paper: residualizing on a pure group-dummy design equals demeaning within group; a
   full WLS residualizer with extra regressors is out of scope (documented in `oracle.hpp`).

2. **`signedpower(x, a)` = `sign(x)·|x|^a`** (NOT plain `x^a`). As-built: `signedpower` → `OpCode::Spow`;
   both `vm_spow` and `op_spow` compute `sign(x)·pow(|x|, a)` (sign(0)=0 ⇒ signedpower(0,a)=0). The lock
   asserts `signedpower(−8, 1/3) == −2`, `signedpower(−4, 0.5) == −2`, contrasted end-to-end with
   `power(−4, 2) == +16` while `signedpower(−4, 2) == −16`. `power(x,a)` stays plain `x^a`. Matches the
   paper's practical convention (WQ §5.2).

3. **`ts_{O}(x, d)` non-integer window → `floor(d)`.** CODE CHANGE in `typecheck.hpp::window_value`: a
   non-integer **positive** literal is floored (was: rejected). `ts_mean(close, 8.7)` → window 8, compiles
   and evaluates bit-identically to `ts_mean(close, 8)`; the arity-3 corr/cov window arg floors too. The
   other rails are PRESERVED: a non-constant window still rejects, `d ≤ 0` rejects, and `floor(d) == 0`
   (e.g. `0.5` → 0, `−0.5` → −1) rejects. Matches the paper: mined window constants are routinely fractional
   and the canonical convention is `floor(d)`.

4. **Per-op NaN / `min_periods` = UNIFORM full window.** As-built (confirmed, NOT re-engineered): a rolling
   window containing ANY NaN (or fewer than `d` observations) yields NaN — for stddev/correlation AND
   ts_mean/ts_sum alike; delay/delta need only the single shifted observation. This is the **as-built
   reality**: the plan's "partial allowed for ts_mean/ts_sum" is a fossil; the engine uses full-window for
   ts_mean/ts_sum too, stated honestly here. The lock injects a NaN into a `close` cell and asserts every
   window touching it is NaN while a clear window is finite (VM == oracle).

5. **`min/max` element-wise vs time-series.** `min(x, y)` / `max(x, y)` over TWO panel args is per-cell
   element-wise (`OpCode::MinP` / `MaxP`); `ts_min(x, d)` / `ts_max(x, d)` over a panel + window is the
   trailing-window reduction (`OpCode::TsMin` / `TsMax`). Disambiguated by the registry (distinct names) +
   operand shape. The lock exercises both forms against hand values (VM == oracle).

### P3b deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_

- **(P3b-2)** True O(1)/cell incremental rolling for the new rolling ops (`ts_zscore`/`ts_av_diff`/`ts_quantile`/`ts_scale`) — kept O(d)/cell per-window recompute to stay bit-exact vs the oracle; an online accumulator needs a documented ULP tolerance.
- **(P3b-2)** `ts_count_nans` partial-window convention pinned to **full-window-only** (incomplete window → NaN) to match the simplest existing partial op (delay/delta); an alternative "count over the available [0..t] slice" is a future knob if a use case appears.

---

## Phase 3b sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| `875d1e7` | marker (P3b-0) | — (no logic; build stays green) |
| `b331728` | P3b-1 (variadic/default args) | AlphaRegistry+AlphaParser 64/64; full Alpha 272/272; 0 fail / 0 skip (+10 new TEST blocks) |
| `1406815` (+ marker `a5dc4f5`) | P3b-1 fix (register_op arity-range guard) | AlphaRegistry+AlphaParser 66/66; 0 fail / 0 skip (+2 new TEST blocks) |
| `fcb119f` | P3b-2 (brain-superset element-wise/rolling/group kernels) | (kernels; tests in 2d84e56) |
| `2d84e56` | P3b-2 tests (brain-superset differential battery) | AlphaBrain 20/20; full Alpha 297/297; 0 fail / 0 skip (+20 new TEST blocks) |
| `3a385c3` | P3b-3 (trade_when + hump stateful recurrence ops) | AlphaTradeWhen+AlphaHump 20/20; full Alpha 317/317; 0 fail / 0 skip (+20 new TEST blocks) |
| `a375ca9` | P3b-4 (alpha101 conformance battery + floor(d) + 5 semantic locks) | AlphaConformance 15/15; full Alpha 333/333; whole binary 1388/1388; 0 fail / 0 skip (+15 new TEST blocks, +1 typecheck split) |
| `1efed1f` | P3b-4 fix (reject out-of-range window literal before u16 cast) | AlphaTypecheck_Window 6/6; full Alpha 334/334; 0 fail / 0 skip (+1 OverMaxWindow test; meta-count pins) |

---

## What Phase 3b proves / Next sprint priorities

**Phase 3b proves the signal VM speaks the operator vocabulary real WorldQuant consultants use, and that its
disputed semantics are pinned against the paper — not accidental.** On top of the Phase-3 compute core, 3b added:
**variadic/default/optional arguments** (`OpSig` `[min,max]`+`defaults`; `scale(x)`, `winsorize(x)`, `hump(x)`
all default-fill); the **BRAIN-superset ops** — `sigmoid`/`tanh`/`normalize`/`winsorize` (element-wise/CS),
`ts_zscore`/`ts_backfill`/`ts_av_diff`/`ts_quantile`/`ts_scale`/`ts_count_nans` (rolling),
`group_count`/`group_mean`/`group_scale` (group); and the **stateful conditional recurrences** `trade_when`
(event-gated hold-prior) + `hump` (turnover damper) via a new `eval_recurrence` forward-scan path with a pooled
per-instrument state slot. **Every one of the 15 new opcodes matches the tree-walking oracle bit-for-bit**
(independent VM + oracle kernels, a differential battery on a delisted/NaN panel proves cell-identity); the
stateful ops are **causal by construction** (forward scan reads only `state[t−1]` + inputs `≤t` — a forward
reference is unrepresentable; truncation-invariance proven per op). The **Alpha101 conformance battery**
(14 expressible alphas + an exhaustive 1..101 in/out classification, 65 expressible / 36 field-blocked) and the
**five locked semantic resolutions** (`indneutralize`=per-group demean; `signedpower`=`sign(x)·|x|^a`;
non-integer window→`floor(d)`; uniform full-window `min_periods`; element-wise `min/max` vs `ts_min/ts_max`)
make the engine's behavior *defined and regression-guarded*, not incidental. The only behavior change a lock
required was `floor(d)` (the rest already matched as-built). Full Alpha suite **334/334**, every unit
`/W4 /permissive- /WX` + clang-format clean.

**Deferred (lifted to the ROADMAP future-work backlog):** true O(1)/cell incremental rolling for the new
rolling ops (kept O(d)/cell recompute for bit-exactness vs the oracle; needs a ULP-tolerant differential);
`ts_count_nans` partial-window "count over `[0..t]`" knob (currently full-window-only). Neither blocks 3c/4.

**Baton → Phase 3c (mass eval + VM→loop bridge):** 3c adds `Engine::evaluate_batch` (N alphas over the one
shared hash-consed DAG + the measured cross-alpha CSE ratio), per-alpha PnL/position `extract_streams` (the
typed Phase-4 feed), and the `VmSignalSource` green-gate + delay-0/delay-1 knob that finally drives the real
Phase-2 `BacktestLoop`. The `phase-3.md` user reference is extended at the 3c close (per plan §P3c-4).

> **Close note — ROADMAP & merge.** Per adjustments (B)+(D): no worktree/merge (in-place on `feat/atx-core-stdlib`),
> and `ROADMAP.md` carries concurrent **uncommitted** edits (a parallel tsdb-v2/SQLite effort on this shared
> branch), so the ROADMAP Phase-3b status flip + `Last reviewed` bump are left for that owner's commit — same
> posture as the Phase-3 close. Phase 3b's close is fully captured by this ledger + the per-unit commits
> (`875d1e7` → `1efed1f`).
