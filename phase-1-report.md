# p7 Sprint 1 — Deflation Gates & Honest Selection — Final Report

**Status:** DONE_WITH_CONCERNS (one significant, byte-identity-driven design deviation
from the plan's field placement — see §2; functional intent fully delivered).

**Branch:** `feat/p7-s1`  **Base:** main @ 2eaf3da  **Worktree:** `C:\atx-wt\p7-s1`

**Commit range:** `2eaf3da..b564c39`
- `d0c17a7` feat(p7-s1): deflation gates in AlphaGate::admit (S1-0..S1-3)
- `8b132dc` feat(p7-s1): thread realized trial-count N into cascade pre-gate (S1-4)
- `b564c39` docs(p7-s1): S1-5 reject-histogram layout pin + AdmitKind audit

---

## 1. What shipped (per unit)

**S1-0 — plumbing + ledger (d0c17a7).** `GateConfig` gains `min_dsr=0.0`,
`max_pbo=1.0`, `require_split_stable=false` (all inert). `GateVerdict` gains
`RejectDsr`, `RejectPbo`, `RejectSplitUnstable`, **appended at the END** (indices
5/6/7 — the frozen reject-histogram order). Per-candidate deflation scalars are
carried in a NEW non-serialized POD `combine::GateDeflation{dsr=1.0, pbo=0.0,
split_stable=false}` (NOT on `AlphaMetrics` — see §2). Layout-pin static_asserts +
sentinel tests shipped.

**S1-1 — DSR floor (d0c17a7).** In `AlphaGate::admit`, after the holding-days floor,
before the lazy corr sweep: `if (cfg.min_dsr > 0.0 && defl.dsr < cfg.min_dsr) return
RejectDsr;`. Inert at `min_dsr=0.0`.

**S1-2 — PBO ceiling (d0c17a7).** Next in order: `if (cfg.max_pbo < 1.0 && defl.pbo >
cfg.max_pbo) return RejectPbo;`. Inert at `max_pbo=1.0`.

**S1-3 — split-half guard (d0c17a7).** Next: `if (cfg.require_split_stable &&
!defl.split_stable) return RejectSplitUnstable;`. Inert at the false flag.

Fixed order: fitness → sharpe → turnover → holding → **DSR → PBO → split** →
correlation. `admit()` gained a defaulted trailing parameter
`const GateDeflation& defl = kInertDeflation`, so every pre-S1 caller (which omits it)
is byte-identical.

**S1-4 — trial-count in cascade pre-gate (8b132dc).** `cascade_gate_passes` removed
`static_cast<void>(trial_count)` and folds the expected-maximum-Sharpe benchmark into
the keep side:
```
SR*_N = (trial_count > 1) ? eval::expected_max_sharpe(trial_count, 1.0/kAnnualizationDays) : 0.0
keep iff  sr_tr * cascade_gate_factor + SR*_N >= cfg.min_dsr
```
At `N<=1` / `min_dsr<=0` / `factor<=0` the term is 0 (or an early return fires), so the
bound is byte-identical to the pre-S1-4 expression. `expected_max_sharpe` was already
`#include`d in factory.cpp (line 20).

**S1-5 — layout pin + AdmitKind audit (b564c39).** Layout pin shipped in S1-0. Audit:
no code change needed (see §3).

---

## 2. Significant deviation — `GateDeflation` instead of fields on `AlphaMetrics`

**The plan (S1-0, dependency map line 137, risk table) instructs adding
`dsr`/`pbo`/`split_stable` to `combine::AlphaMetrics`, stating "neither struct is
serialized." That statement is factually wrong.**

`AlphaMetrics` is embedded **verbatim** in the on-disk library record
`library/record.hpp::AlphaDirEntry` (line 110), memcpy'd into segment files
(record.hpp:272), and pinned by `static_assert(sizeof(AlphaMetrics)==56)` +
`static_assert(sizeof(AlphaDirEntry)==96)` (record.hpp:128,130). I verified this
empirically: a build with the three fields on `AlphaMetrics` **failed both record.hpp
static_asserts.** Adding the fields would:
1. break those frozen static_asserts in `record.hpp` — a file NOT in my Owns set;
2. change the on-disk segment bytes → change the segment CRC and the manifest
   `version_id` → **break byte-identity of every library golden/digest**, which the p7
   determinism contract forbids and which would require a `kLibFormatVersion` bump
   (far larger than a mechanical edit, and out of my Owns fence).

**Resolution (byte-identity-safe, fully in the Owns fence):** carry the three
per-candidate scalars in a new non-serialized POD `combine::GateDeflation`, passed to
`AlphaGate::admit(..., const GateDeflation& defl = kInertDeflation)` as a defaulted
trailing parameter. The plan's `GateConfig` thresholds, `GateVerdict` enumerators,
inert sentinels, insertion point, operators, and all four determinism classes are
implemented **exactly** as specified — only the *carrier* of the per-candidate scalars
differs. `AlphaMetrics` is **unchanged (still 56 bytes)**; `record.hpp` is untouched.

**Consequence to be aware of:** the library admit path (`library.hpp::verdict_for`,
out of my Owns set) does not yet read `GateDeflation`, so the new screens are active
only through the direct `AlphaGate::admit` caller today. Wiring `GateDeflation` into the
library/CLI is a later concern (Sprint 7 owns CLI/library wiring). The S1 screens are
proven directly against `AlphaGate::admit` by the `gate_dsr_pbo_test` suite.

This is the single item warranting reviewer attention. It is the only design choice
that achieves the plan's intent without breaking byte-identity or the Owns fence; the
alternative (editing the frozen serialized record + a format bump) is explicitly
disallowed.

---

## 3. AdmitKind audit (S1-5, the plan's mandated audit)

- `FactoryReport::reject_histogram` is `std::array<usize, 8>` indexed by
  **`library::AdmitKind`**, NOT `GateVerdict` (factory.hpp:265; every write is
  `reject_histogram[static_cast<usize>(kind)]`, `kind:AdmitKind`). It was **already**
  size 8 pre-S1 — the plan's "5 → 8 for GateVerdict-driven buckets" is a misread. **No
  resize.**
- `AdmitKind` **diverges** from `GateVerdict`:
  - `AdmitKind` = {Accept, Duplicate, RejectSharpe, RejectFitness, RejectTurnover,
    RejectCorrelated, RejectPriceScale, RejectDsrSubwindow}
  - `GateVerdict` = {Accept, RejectSharpe, RejectFitness, RejectTurnover,
    RejectCorrelated, RejectDsr, RejectPbo, RejectSplitUnstable}
  The library path returns `AdmitKind` directly and never produces a
  `GateVerdict::RejectDsr/Pbo/SplitUnstable`, so **no new AdmitKind enumerator is
  required** and there is no compile break.
- **There is no `switch` on a `GateVerdict` value anywhere** in the codebase (grep-
  verified). The only enum-mapping switch is `map_kind(AdmitKind)→GateVerdict` in
  `library_integration_test.cpp` (switches on `AdmitKind`, which I did not change →
  still exhaustive). So appending the three `GateVerdict` enumerators forced **ZERO**
  switch-arm additions and **ZERO** out-of-Owns edits — the authorized Owns-fence
  exception (for mechanical switch-arm additions) was **not needed**.
- `atx-impl/src/stage_discover.cpp` prints the histogram by iterating
  `rep.reject_histogram.size()` (robust to size); its comment "per library::AdmitKind,
  0..5" is now stale (should read 0..7). The code is correct; the file is Sprint-7 /
  out of my Owns set, so it is left untouched (flagged for Sprint 7).

---

## 4. Risk-table check — does the factory populate the gate's deflation scalars?

The factory's gate-call sites (`factory.cpp:250` for `mine`, and `admit_on_holdout` →
`library::verdict_for` for the OOS path) pass `AlphaMetrics` from `compute_metrics`,
which does NOT set dsr/pbo/split_stable — and now *cannot* (those are not on
`AlphaMetrics`). The factory does **not** pass a `GateDeflation`, so the new gate
screens are **dormant on the factory path by design**: the factory tier already
enforces DSR / PBO / split via its own machinery (`dsr >= cfg.min_dsr` accept-
expression at factory.cpp:265, `finalize_run_pbo`, `split_floor_ok`). Wiring
`GateDeflation` into the factory would double-gate and risk byte-identity, and is
explicitly out of scope ("Out of scope: populating AlphaMetrics::dsr/pbo/split_stable
from the DSL search evaluation loop"). The two-tier architecture in the plan is
preserved: factory tier (existing) + gate tier (S1, for the standalone/library
`AlphaGate` caller). No propagation was added.

---

## 5. Determinism contract — four classes per opt-in field

Each of the three gate fields ships (a) off-path byte-identity, (b) on-path RED→GREEN,
(c) twice-run, (d) seq==parallel (pure-const verified via a worker-thread `std::async`
admit() compared to the serial verdict), plus inert-sentinel and fixed-order tests.
S1-4 ships inert-N1 / inert-min_dsr-0 / factor-off / hopeless-stays-skipped /
monotone / keeper-never-skipped / twice-run on the bound math, plus three end-to-end
classes (gate ON==OFF byte-identity, seq==parallel via ProcessExecutor, twice-run) at
a realized `trial_count > 1` through `mine_into_oos` + `mine_into_oos_parallel`.

---

## 6. S1-4 spec reconciliation (documented in factory.cpp)

The plan's S1-4 prose says the bound becomes "stricter" with N, but its own concrete
formula adds `SR*_N` to the keep side (looser) and its concrete monotone Accept test
asserts `skip@N=100 ⇒ skip@N=10` (skip set shrinks with N = looser). The arithmetic and
the concrete test agree (looser/safer); the "stricter" prose is the plan's internal
inconsistency. I implemented the **safe** arithmetic — the only direction that keeps the
binding `AdmittedSetUnchanged_AfterCascadeGate` + byte-identity invariants green. Adding
the non-negative `SR*_N` to the keep side can only shrink the skip set vs. the
N-ignoring bound, so it can never start skipping a candidate the gate-off run would have
evaluated; the conservatism proof holds by construction. Both the existing cascade tests
and the new end-to-end tests confirm ON==OFF at the real N.

---

## 7. Test results (final, zero regressions)

| Group | Result |
|---|---|
| combine | 129/129 green |
| factory (full) | 208/208 green |
| library | 43/43 green (2 pre-existing DISABLED, unrelated) |
| byte-identity slice `*Oracle*:*Golden*:*Digest*` | 18/18 green BEFORE and AFTER every unit |

New tests: 21 (combine `gate_dsr_pbo_test`) + 10 (factory `cascade_trial_count_test`)
= 31. Existing cascade proofs (`AdmittedSetUnchanged_AfterCascadeGate`,
`CascadeGate_SeqEqualsParallel`) pass unchanged with the S1-4 bound.

Note on the `AtxImplDiscover` slice the plan references
(`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`):
the latter is in the factory group and is green (full 208/208); the `atx-impl` slice
itself is out of my touched groups (Sprint 7) and was not built/run. The engine
byte-identity slice is the binding gate and is green.

---

## 8. Drift / deviations summary (for the whole-branch review)

1. **GateDeflation carrier (significant).** dsr/pbo/split_stable carried in a new
   non-serialized `GateDeflation`, NOT on the serialized `AlphaMetrics`. Forced by the
   on-disk record format + byte-identity contract + Owns fence (§2).
2. **Test file names.** Plan said `gate_dsr_pbo_tests.cpp` / `cascade_trial_count_tests.cpp`;
   the engine glob is `*_test.cpp` (singular), so the files are `gate_dsr_pbo_test.cpp`
   and `cascade_trial_count_test.cpp`.
3. **S1-4 direction.** Implemented the safe (looser-with-N) arithmetic the plan's
   concrete formula + concrete test specify; the plan's "stricter" prose is its own
   inconsistency (§6).
4. **Histogram audit.** The histogram is AdmitKind-driven (size 8 already), not
   GateVerdict-driven; no resize and no AdmitKind change needed (§3).
5. **Commit granularity.** S1-0..S1-3 are co-located in one header + one test file (the
   plan's own layout), so they ship in one commit (d0c17a7) with per-unit ledger rows.
6. **Stale atx-impl comment** ("0..5" histogram comment) left untouched (out of Owns).

No `oracle.hpp` edits, no golden re-baseline, no push/merge/PR. No out-of-Owns source
edits were required or made.
