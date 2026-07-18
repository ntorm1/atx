# atx-vol Hot-Path Review Remediation — Implementation Plan

**Source review:** `atx-vol/docs/reviews/2026-07-16-hotpath-sprint-midpoint-code-review.md`
**Sprint:** `atx-vol/sprints/2026-07-14-atx-vol-fit-price-backtest-hotpath-sota-sprint.md`
**Base commit:** `5ba7fe4`
**Branch:** local `main` (user-authorized)

## Session scope

Cleanup, optimize, and **wire** work already started. Every task below belongs to a sprint
task already in flight (W0.1, W0.2, W1.1–W1.5, W2.1–W2.6, W3.1, W3.2, W4.1, W4.4).

**Explicitly OUT of scope — do not implement, do not "helpfully" start:**
W3.3 (per-slice Legacy fallback), W3.4 (admission taxonomy), W4.2 (fit pool), W4.3 (OPRA
ingest), W4.5 (small-book cutoff), W5.1–W5.6 (kernels), and the snapshot-cache identity key
(R-19 — new public API, deferred by scope). If a task tempts you toward these, stop and report.

## Global Constraints (binding on every task)

1. **Build ONLY through `scripts/atx-build.ps1`.** It sources `vcvars64` and puts the VS-bundled
   Ninja on PATH; the presets use clang-cl + Ninja and **plain `cmake` from a normal shell will
   fail** (missing INCLUDE/LIB/PATH, and `ninja.exe` is not on PATH). Verified commands:
   **`pwsh` is NOT installed on this machine** — invoke the script through Windows PowerShell:
   `powershell -NoProfile -Command "& .\scripts\atx-build.ps1 <args>"`.
   - Configure Debug (canonical iterate loop, `build/`, preset `dev`):
     `powershell -NoProfile -Command "& .\scripts\atx-build.ps1 configure"` (add `-Bench` for
     benchmarks, `-Groups <g>` to subset)
   - Build a Debug target: `powershell -NoProfile -Command "& .\scripts\atx-build.ps1 build <target>"`
   - Run tests (always `build/`, already `-j 16 --output-on-failure`):
     `powershell -NoProfile -Command "& .\scripts\atx-build.ps1 -Ctest -R '<regex>'"`
   - Release (`build-rel`, preset `rel`) uses the raw pass-through form:
     `powershell -NoProfile -Command "& .\scripts\atx-build.ps1 --preset rel"` then
     `powershell -NoProfile -Command "& .\scripts\atx-build.ps1 --build build-rel --target <tgt>"`;
     run its tests with `ctest --test-dir build-rel -L atx_vol -j16 --output-on-failure -R "<regex>"`
     (ctest needs neither vcvars nor Ninja, so invoke it directly).
   - AVX2-specific work: preset `rel-avx2` → `build-rel-avx2`.
   - `build/` and `build-rel/` are both already configured at session start.
2. **Never run Debug and Release builds concurrently.** All FetchContent trees share
   `C:\atx-cache\deps\spdlog-build`; concurrent configs link mixed `_ITERATOR_DEBUG_LEVEL`.
   Configure the target preset immediately before its sequential build. Never leave a build
   running in the background past your turn. Use Debug (`build/`) for the correctness iterate loop;
   use Release (`build-rel`) for any timing/perf claim.
3. **STALE-BUILD HAZARD — verified in this session.** After `git checkout <ref> -- <file>` (the
   usual way to prove a test is red before your fix), ninja can report `ninja: no work to do` and
   leave the **previous** object linked — silently giving you a false red or false green. Always
   `touch` the source files you restored before rebuilding, and confirm the build actually compiled
   them (you should see the `Building CXX object .../<file>.obj` line). If you prove red/green by
   swapping file contents, force the rebuild both ways.
4. **Known-red tests on `main` at session start — NOT yours, do not chase.** These 5 fail
   identically at base `5ba7fe4` (verified by the controller by reverting and rebuilding):
   `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`,
   `PricerFitterTest.LocalRiskRefitPublishesCopyOnWriteGeneration`,
   `OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard`,
   `AllQualityModes/SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/{Latency,Balanced}`.
   Plus `PreparedPortfolio…PinnedFingerprint` (Debug-only, bit-identical hash pair at HEAD).
   If your change makes one of these **worse or different**, that IS yours — report it. If your
   change happens to FIX one, say so explicitly.
3. **Strict builds.** MSVC `/W4 /WX` — a warning is a build failure. No `#pragma warning` suppressions.
4. **Economic-correctness gate, not bit-identity.** A change is acceptable if, on the panels it
   touches, per-option price abs error ≤ `min(0.5 × tick, 0.1 × vega × 1e-4)` **and** stays
   strictly inside the quote half-spread; IV abs error ≤ `1e-4` vol points; no new butterfly/
   calendar/vertical arb; aggregate quality (in-band fraction, χ², vol-RMSE) does not regress.
   Bit-identity is a convenient telltale for pure hoists, never the goal.
5. **Every deviating change carries an in-code comment** stating: what changed numerically, why
   it is correct, and the bound it holds.
6. **TDD.** Write the test that fails against current behavior first, then implement. Tests assert
   the economic bound or the structural property claimed — not byte equality, unless the change is
   a pure hoist and byte equality is the cheapest honest check.
7. **Admission/arb safety only gets stricter.** Numeric freedom never extends to admission,
   certification, or no-arb gating. Never widen a silent `continue`.
8. **Determinism preserved** across worker counts wherever the code promises it.
9. **Commit** your work. Conventional Commits. End the message with:
   `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

## Task 1 — Wire the shared-boundary de-Am into the Configured/Hft route (R-01 part 1, R-09)

**Sprint item:** W3.1 (wiring). **This is the headline task — 84% of the SPY gate rides on it.**

**Problem.** The W3.1 shared-boundary machinery is landed, verified, and measured at 1.78×, but it
never runs on the real SPY board. SPY resolves to profile `IndexEtfUltraLiquid` → preset `Hft`
(`src/fit_policy.cpp:30-33`), and `make_session_inputs(Hft)` sets
`in.calib.max_otm_shortcut_premium_spread_frac = 0.50` (`src/session.cpp:711`). That trips the
function-level disable gate:

```cpp
// src/calib.cpp:653-656 (current)
if (!opts.use_shared_boundary_deam || opts.audit_accurate_inversions ||
    opts.anchor_kind != CalibAnchorKind::Mid ||
    opts.max_otm_shortcut_premium_spread_frac > 0.0 || ...) { return; }
```

Every retained row falls to scalar `american_implied_vol` (`src/calib.cpp:896-902`) — the
411.783 ms of the 492 ms single-op gate.

**Fix A (shortcut coexistence).** The shortcut and the shared boundary are not actually
exclusive — they are per-row alternatives. Compute the per-row OTM-shortcut mask *first*
(the predicate behind `use_otm_shortcut_deam`, `src/calib.cpp:856`, currently evaluated inside
the main loop), run `prepare_shared_boundary_side` over the **non-shortcut subset only**, and
remove `opts.max_otm_shortcut_premium_spread_frac > 0.0` from the function-level gate.

Requirements:
- The per-row priority order in the main loop stays exactly `shortcut → shared_proposal → scalar`
  (`src/calib.cpp:897-902`). Do not reorder.
- A row that the shortcut claims must never also be a shared lane; assert this.
- `kSharedMinSideRows = 16` now counts **non-shortcut** rows for that side, not all rows.
- The existing sentinel certification (`src/calib.cpp:617-638`) still gates acceptance unchanged.
- The shortcut mask must be computed once and consumed by both the prepare pass and the main
  loop — do not evaluate the predicate twice with a risk of divergence. Single-source it.

**Fix B (R-09, `q_eff < 0` put side).** The function-level gate also bails the whole board on
`q_eff < 0.0` (`src/calib.cpp:655`). For the **put** side the internal regime is (rate=r,
yield=q_eff); r > 0 with slightly negative q_eff is a regular single-boundary American-put
regime — common on single names, and exactly the 25-name recovery cohort's population. Only the
**call** side (internal rate = q_eff) needs q_eff > 0.

Remove `q_eff < 0.0` from the function-level gate. The per-side `internal_rate > 0.0` check
(`src/calib.cpp:600-601`) and `build()`'s xmax check (`src/boundary_interp.cpp:222-224`) already
exclude the genuinely unsupported corners. Keep the `r < 0.0` function-level bail.

**Files:** `atx-vol/src/calib.cpp`, `atx-vol/tests/calib_test.cpp`.

**Tests (TDD — these must fail first):**
1. `SharedSigmaBoundaryRunsUnderHftShortcutPreset` — build a synthetic slice under
   `max_otm_shortcut_premium_spread_frac = 0.50`; assert `n_shared_boundary_solves > 0` (currently 0)
   and that shortcut-claimed rows are not shared lanes; assert observations match the
   shortcut-disabled scalar reference within the §4 economic bound.
2. `SharedSigmaBoundaryServesPutSideOnNegativeBorrow` — r > 0, q_eff < 0; assert the put side now
   shares boundaries (`n_shared_boundary_solves > 0`) while the call side falls to scalar, and
   results match the scalar reference within the economic bound.
3. Update `SharedSigmaBoundaryKeepsNegativeRatesOnScalarPath` — it currently pins the
   over-conservative behavior Fix B removes. Retarget it to `r < 0` (still scalar) so the
   remaining guard stays pinned.

**Gate:** focused Release `calib` + `prepared_fitting` + `session` tests green; the two new tests
prove the shared path activates; economic bound asserted against the scalar reference.

---

## Task 2 — W3.1 lane-acceptance budget and σ-monotonicity certificate (R-07, R-08, R-32)

**Sprint item:** W3.1 (correctness hardening).

**Problem R-07 (budget doubling).** `src/calib.cpp:489-492` gates the interpolated-map residual
and the 9-vs-5 interpolation-error estimate against `budget` **independently**:

```cpp
std::fabs(price - lane.observation->mid) > budget ||
std::fabs(price - embedded)             > budget
```

Since the 9-node price's true error is estimated by (and can approach) the 9-vs-5 gap,
`|price_true(σ̂) − mid|` can reach ≈ `2 × budget`, where
`budget = min(0.005, 0.1·vega·1e-4, 0.125·spread)` (`src/calib.cpp:408-415`). The sprint bound is
a single budget. The gate does not prove the bound it claims.

**Fix:** gate the combined quantity so the sum is what is bounded:
`if (std::fabs(price - lane.observation->mid) + std::fabs(price - embedded) > budget)` → reject.
(Equivalent-strength alternative: halve each term. Prefer the combined form — it is the quantity
the bound is actually about.) Update the in-code comment to state the bound now proven.

**Problem R-08 (endpoint-only monotonicity).** `src/calib.cpp:429-430` checks
`lane.f_lo < 0.0 && lane.f_hi >= 0.0 && price_hi >= price_lo` — the entire shape check on a
9-node Chebyshev interpolant of the boundary. The true American price is strictly increasing in σ,
but where local vega·Δσ is comparable to interpolation error the residual can be non-monotone and
multi-rooted; bisection then converges to *a* root. `finalize_shared_lane` certifies only the
price residual, and the embedded 9-vs-5 gap cannot see a wiggle shared by both interpolants
(they derive from the same 9 solves). Sentinels sample strike ranks {first, middle, last}, not
worst-case σ regions. The per-lane vega-scaled budget mostly protects the economics, but the
sprint's "IV ≤ 1e-4 vol" is not enforced per lane.

**Fix:** add a per-side build-time monotonicity certificate. After `build()` succeeds, evaluate
`price_internal_put(Kp_ref, Kp_ref, σ)` on a ~17-point refinement grid spanning the σ-box and
require strict monotonicity across it. Quadrature-only, one pass per side, no boundary solves.
On failure: invalidate the side (existing `invalidate_shared_side` path → exact scalar fallback)
and count it. Add a counter for the rejection so the diagnostic is visible.

**Problem R-32 (counter semantics).** `n_shared_boundary_solves` (`src/calib.cpp:617`) counts only
the 9 build solves per side; the 3 sentinel `american_implied_vol` calls (`:559-561`) and 3
`american_price` reprices (`:565-567`) each perform additional internal cold boundary solves that
are not in that counter (they do hit the global `counters::BoundarySolves`). The handoff's "18
boundary solves" therefore undercounts real boundary work by roughly 2× (still O(1) per side).

**Fix:** documentation only — a comment on the counter declaration stating exactly what it counts
and what it excludes. Do not change the counter's semantics (tests pin it).

**Files:** `atx-vol/src/calib.cpp`, `atx-vol/src/boundary_interp.{hpp,cpp}` (if the certificate
helper belongs there), `atx-vol/tests/calib_test.cpp`.

**Tests (TDD):**
1. `SharedLaneAcceptanceBoundsCombinedResidual` — construct/force a lane whose two residuals are
   each just under `budget` but whose sum exceeds it; assert it is now rejected and falls to
   scalar (currently accepted).
2. `SharedSideRejectedWhenBoundaryInterpNonMonotone` — a σ-box wide enough that the certificate
   fires; assert the side invalidates, the counter increments, and results equal the scalar
   reference exactly.
3. Existing shared-boundary tests stay green (the healthy fixture must not start failing the
   certificate — if it does, the certificate is too tight; report rather than loosen the bound).

**Gate:** focused Release calib tests green; both new tests prove the tightened gates.

---

## Task 3 — W3.1 root-finder and boundary-build performance (R-11, R-31)

**Sprint item:** W3.1 (the measured 1.78× vs the 2–4× estimate).

**Problem.** Three compounding costs explain the shortfall. All are in the lane iteration and the
interp build — the boundary sharing itself is sound.

**Fix A (R-11a — Illinois step).** `solve_tol = min(max(iv_tol,1e-9), 2.5e-5)`
(`src/calib.cpp:502`); with the Robust/Accurate default `iv_tol = 1e-7` each lane bisects a
~0.1-wide bracket to 1e-7 — ~20 evals best case, up to ~48 under the 25%-shrink guard
(`src/calib.cpp:452-458`, which rejects any secant in the outer quarter and so degenerates to pure
bisection once the root hugs a bracket end). Each eval is 12 barycentric interps + a full 48-node
premium quadrature. The scalar path it replaced was a ~4–8-eval safeguarded Newton.

Implement the Illinois (or Anderson–Björck) modification of the false-position step inside
`iterate_shared_lanes`: when the same endpoint is retained twice in a row, halve that endpoint's
retained residual. This preserves the bracket invariant and the guard, and converges
superlinearly — target ~5–7 evals/lane. Keep the existing 25%-shrink safeguard as the fallback
when the modified step still lands outside the trust region.

**Fix B (R-11b — adjacent-strike warm brackets).** Lanes iterate in ascending-strike order but each
starts from the full `[interp.sigma_lo(), sigma_mkt]` bracket (`src/calib.cpp:417-424`). The scalar
path already exploits adjacency (`warm_start_deam_adjacent_strikes`). Seed lane *i*'s bracket from
lane *i−1*'s converged σ as `[σ_{i-1} − δ, σ_{i-1} + δ]`, **sign-validated** (both endpoints
evaluated; if they do not bracket, fall back to the full bracket — never assume). Only seed from a
lane that converged and was accepted. Determinism: strike order is already deterministic; keep it.

**Fix C (R-11c — warm node solves in `build()`).** `src/boundary_interp.cpp:247-262` calls
`al_solve_put_boundary` **cold** at each of the 9 σ-nodes, although adjacent nodes are small σ
bumps and `al_solve_put_boundary_warm` (`src/american_boundary.hpp:106-111`) exists for exactly
this. Chain warm seeds across adjacent nodes (solve node 0 cold, then warm-seed each subsequent
node from its neighbour's converged boundary). This also benefits the pre-existing
`slice_sigma_impl`. Warm seeding must not change the converged boundary beyond solver tolerance —
assert equality with the cold build within tolerance in a test.

**Fix D (R-31 — single-source the side mapping).** `src/calib.cpp:393-406` re-derives the
McDonald–Schroder internal-put mapping (`internal_spot = o.side == Side::Call ? o.K : S;
internal_strike = ... ? S : o.K`) that already exists in `src/boundary_interp.cpp:126-130`
(`slice_sigma_impl`). Two copies can drift. Move it into `detail::SigmaBoundaryInterp` as a
side-aware entry point (e.g. `price_side(Side, double S, double K, double sigma)`) and have both
call sites use it.

**Files:** `atx-vol/src/calib.cpp`, `atx-vol/src/boundary_interp.{hpp,cpp}`,
`atx-vol/tests/calib_test.cpp`, `atx-vol/bench/fitting_throughput_bench.cpp` (fixture, see below).

**Tests (TDD):**
1. `SharedLaneIterationCountDropsWithIlluminatedStep` (name it as you see fit) — counter or probe
   asserting mean evals/lane falls materially vs the pre-change path, with identical accepted σ
   within the economic bound.
2. `WarmBracketMatchesFullBracketResult` — same accepted σ (within solver tol) with and without the
   warm bracket, on a fixture with a steep smile so brackets genuinely differ.
3. `WarmNodeBuildMatchesColdBuild` — boundary nodes from the warm-chained build equal the cold build
   within solver tolerance.
4. **Bench fixture (R-11 evidence, and it fixes a real gap):** the existing A/B fixture in
   `fitting_throughput_bench.cpp:712-800` is a **flat 0.24-vol** synthetic chain — the σ-box is
   ~3× wide and trivially interpolable, so it measures mechanics, not interpolation stress. Add a
   **smile fixture** (σ ∈ [0.15, 0.8] across strikes) alongside it. Report both A/B numbers.
5. **Smile-stress regression test (carries Task 2's R-08 retirement evidence — do not skip).**
   Task 2 investigated a suspected σ-monotonicity/multi-root risk in the boundary interpolant and
   retired it as not-live, on this measured evidence: on a σ ∈ [0.15, 0.8] smile fixture the worst
   per-lane IV error versus the exact scalar inverter was **5e-08**, ~2000× inside the 1e-4 bound;
   genuine interpolation wiggles (≈ −3.1e-04 at K=110) exist only in low-vega wings that the
   existing `budget > 0` and bracket-sign gates already exclude. That evidence currently lives only
   in a throwaway probe. **Pin it as a test**: a steep-smile fixture asserting (a) accepted lanes'
   IV within the §4 economic bound of the exact scalar reference, and (b) that lanes in the
   low-vega wing regions fall back to scalar rather than being accepted. Your Fix A/B change the
   root-finder, so this test also guards *your* work. If the bound does not hold after your
   changes, that is a real finding — report it, do not loosen the assert.

**Gate:** focused Release calib tests green; the isolated shared-boundary A/B bench re-run
best-of-3 on both the flat and the new smile fixture, reported as
`scalar mean / retained mean / ratio` for each. Report honestly — if a fix does not move the
number, say so; do not claim a win the bench does not show.

---

## Task 4 — Route the legacy/eSSVI preparation through the shared boundary (R-01 part 2)

**Sprint item:** W3.1 (wiring). Depends on Task 1 and Task 3 (needs the lane solver factored out).

**Problem.** The eSSVI driver prepares every slice under Legacy —
`src/surface_parity.cpp:257-258` hardcodes `PreparedObservationPolicy::LegacyEssviCompatibility` —
and `prepare_legacy` de-Americanizes **per-row scalar** via `european_equiv_iv`
(`src/prepared_fitting.cpp:383-386`); it never calls the shared-boundary code. Same for facade
refit (`src/pricer_fitter.cpp:1540-1542`). On the 100-name row the eSSVI-published majority
(63/94 boards) therefore structurally bypasses W3.1.

**Fix.** Extract the shared-boundary lane pass into a reusable helper that operates on a
`std::span<FitObs>` (it already only touches `FitObs` fields), and call it from `prepare_legacy`
after its rows are populated, before/instead of the per-row `european_equiv_iv` loop.

Requirements — this is a **wiring** change, not a semantics change:
- Legacy's row set, ordering, admission, audit behavior, and provenance stamp
  (`SlicePreparationProvenance::policy == LegacyEssviCompatibility`) must be **unchanged**.
- Rows the shared pass does not certify fall to exactly today's scalar `european_equiv_iv` — the
  existing code path, in the existing order.
- Do **not** touch `PreparedSlice::create`'s Configured/Legacy policy selection, and do **not**
  add per-slice fallback between policies — that is W3.3, explicitly out of scope.
- The `LegacyEssviCompatibility` route has no audited de-Am diagnostics by design
  (`prepared_fitting.hpp:186-187`). The shared pass must not start claiming Configured-grade
  certification for legacy slices. Keep the diagnostics honest: if the shared pass cannot report
  legacy-grade evidence, keep its rows scalar rather than inventing certification.

If, on reading the code, routing legacy through the shared pass cannot preserve legacy's exact row
semantics, **stop and report** rather than bending legacy's contract — the alternative wiring
(flipping `run_surface_parity` to Configured prep behind `SurfaceParityInputs::fit_prep_policy`)
is a policy decision the controller must make, not a thing to do silently.

**Files:** `atx-vol/src/prepared_fitting.cpp`, `atx-vol/src/calib.{hpp,cpp}` (helper export),
`atx-vol/tests/prepared_fitting_test.cpp`.

**Tests (TDD):**
1. `LegacyPrepSharesBoundariesAcrossStrikes` — an eSSVI-routed board; assert
   `counters::BoundarySolves` (or the shared counter) drops from O(strikes) to O(σ-nodes), and the
   prepared rows match the pre-change legacy reference within the §4 economic bound.
2. `LegacyPrepProvenanceAndRowSetUnchanged` — row count, ordering, provenance policy, and audit
   diagnostics identical to the pre-change legacy path.

**Gate:** focused Release `prepared_fitting` + `surface_parity` + `session` tests green.

---

## Task 5 — American IV bracket-cap clamp and cold-polish audit guard (R-05, R-28)

**Sprint item:** W2.6 / W2.5 (correctness follow-up on landed work).

**Problem R-05 (silent clamp — a real quote-loss bug, made more reachable by W2.6).** When
`f(seed) > 0`, the down-bracket loop steps 7% at most 16 times (0.93¹⁶ ≈ 0.313 × seed). On
exhaustion it returns:

```cpp
// src/american_iv.cpp:285-287
if (!bracketed) {
  // Even the vol floor over-prices the quote -> IV is at/below the floor.
  return Ok(kIvMin);   // the floor was never actually evaluated
}
```

The comment claims the floor over-prices the quote; the code never evaluated the floor. Symmetrically
the up-loop returns `Err(OutOfRange, "price above max-vol price")` at `:303-304` (1.15¹⁶ ≈ 9.36×
seed) for a perfectly invertible quote.

W2.6 escalated reachability: `warm_start_deam_adjacent_strikes` now defaults **true**
(`include/atx/vol/calib.hpp:163`), so the seed is the previous accepted same-side strike's IV; after
`cap_observations_for_deam` thins a dense board (`src/calib.cpp:221-305`), adjacent *surviving* rows
can be far apart in moneyness, and a smile dropping > 3.2× across the gap silently yields
`Ok(kIvMin)`. Blast radius today: calib rows are silently dropped by the strict band check
(`src/calib.cpp:903`) — fail-safe, but valid quotes are lost and `drop_fraction` can trip
certification; carry's `deam_pcp_step` accepts `Ok(0.005)` unchecked (`src/deamer.cpp:125-132`).

**Fix:** replace both cap returns with fall-through into the existing wide-bracket block at
`src/american_iv.cpp:309-334`, which already evaluates `kSigmaLo`/`kSigmaHi` and expands to
`kSigmaHiCap`. Cost: ≤2 extra solves, on the pathological path only. `Ok(kIvMin)` then returns only
when the floor was genuinely evaluated and genuinely over-prices the quote — making the existing
comment true. Additionally: reject a `warm_start` seed further than ~2× from `euro_seed` when the
latter is available (a stale-neighbour guard), falling back to `euro_seed`.

**Problem R-28 (polish early-exit).** W2.5's trusted-accurate skip
(`src/calib.cpp:914-927`) increments `n_audited`/`n_accepted` with no reprice, justified by the
accurate inversion having been cold-polished against the exact audit map. But the polish can
`break` before achieving `|step| < tol` without re-verifying the residual:
`src/american_iv.cpp:403-405` (non-positive cold vega) and `:409-412` (Newton step past zero keeps
the *previous* iterate, whose residual is the pre-step one). Both need near-degenerate corners, so
this is a hardening item — but the skip converts a measured guarantee into an argued one.

**Fix:** signal polish early-exit to the caller (a flag on the polish result, or a distinct status),
and in the trusted-accurate branch keep the audit whenever the polish early-exited. Preserves ~100%
of the saving on healthy rows.

**Files:** `atx-vol/src/american_iv.cpp`, `atx-vol/src/calib.cpp` (audit condition),
`atx-vol/tests/american_iv_test.cpp`, `atx-vol/tests/calib_test.cpp`.

**Tests (TDD):**
1. `SeedFarAboveTrueIvWidensBracketInsteadOfClampingToFloor` — a quote whose true IV is below
   0.31 × seed but well above `kIvMin`; assert the returned IV equals the reference inversion
   (currently returns `kIvMin`).
2. `SeedFarBelowTrueIvWidensBracketInsteadOfOutOfRange` — true IV above 9.36 × seed; assert success
   (currently `OutOfRange`).
3. `StaleWarmSeedFallsBackToEuropeanSeed` — a warm seed > 2× from `euro_seed`; assert the correct
   root is still found.
4. `PolishEarlyExitStillColdAudits` — force a polish early-exit; assert the reference reprice count
   increments (currently skipped).

**Gate:** focused Release `american_iv` + `calib` + `deamer` tests green.

---

## Task 6 — Carry fixed-point tolerance and per-leg AloPricer persistence (R-06, R-10, R-27)

**Sprint item:** W2.3 / W3.1 (the promised intermediate step, not delivered).

**Problem R-06 (tolerance inversion).** W2.3 moved `kInnerIvTol` 1e-6 → 1e-4
(`src/deamer.cpp:36`) but left `kBorrowFpTol` at 1e-8 (`:43`). Converging to `|Δb| < 1e-8` requires
successive `deam_pcp_step` evaluations reproducible at the 1e-8 level, yet each leg's IV is a
1e-4-terminated safeguarded Newton whose returned value depends discontinuously on bracket-branch
decisions (`src/american_iv.cpp:257-306`). One branch flip between iterates moves σ by up to ~1e-4
→ `b_next` by ~1e-5 — a permanent limit cycle above the gate. Non-convergence is fail-safe
(`Err(Unavailable)` `:181-182` → pair dropped `:443`), so this is availability/latency: a cycling
pair burns the full 64 × (2–4 AL solves) budget before failing.

**Fix:** track `best_delta` across iterates; accept when the final `|Δb| < max(kBorrowFpTol, 1e-6)`
— still 100× inside the 1e-4 economic target and inside the loosened `rmse_pcp ≤ 1e-4` contract —
instead of returning `Unavailable`. Optionally also detect a 2-cycle (`|Δbₙ| ≈ |Δbₙ₋₁|` with
alternating sign) and exit converged at the midpoint. Document the bound held.

**Problem R-10 (promised persistence absent).** The W3.1 sprint row promised: *"Intermediate step:
persist one `AloPricer` per carry leg across FP iterations."* Commit `c485081` does not touch
`deamer.cpp`. At HEAD, `deam_pcp_step` (`src/deamer.cpp:~108-135`) calls `american_implied_vol` for
the call leg then the put leg every fixed-point iteration; both share the **single** thread-local
slot (`src/american_iv.cpp:113-158`), so each leg's `reset(S,K,T,r,q,side,opts)` clobbers the
other's boundary every iteration — zero reuse even though only `q_eff` moves by a shrinking delta
and `al_solve_put_boundary_warm` (`src/american_boundary.hpp:106-111`) exists for exactly this
warm re-seed. Carry is ~56 ms of the 492 ms SPY gate.

**Fix:** in `imply_term_borrow_from_base`, hold two `AloPricer`s (one per leg) and rebind `q_eff`
per iteration with a warm boundary seed rather than a cold reset. Sketch:
`deam_pcp_step(..., AloPricer &call_pricer, AloPricer &put_pricer)` with a `reset_carry(q_eff)`-style
rebind reusing `al_solve_put_boundary_warm(K, T, σ, r, q', sch, prev_bnd, bnd, ws)`.

Requirements: the warm re-seed must not change converged σ beyond solver tolerance (test it);
respect the existing TLS lease's re-entrancy contract — do not defeat the nested-inversion
fallback; keep the non-convergence behavior fail-safe.

**Problem R-27 (unaudited carry accuracy on high-dividend names).** W2.3's default-constructed
`DeAmOptions` moved every library caller to fast-AL/1e-4/5-pair carry — documented and disableable,
but carry pairs are selected by `|K − S|` (`src/deamer.cpp:369-373`), so for high-dividend names the
forward sits well below spot and the "ATM" pairs are materially ITM-call/OTM-put, where the fast
scheme's systematic de-Am error is largest. Carry is unaudited by design (`src/deamer.cpp:423`
`cold_caches`, no repricing audit). The existing round-trip test uses one modest cash dividend.

**Fix:** add the missing coverage — a fixture with `F/S ≈ 0.90` (large cash dividends) pinning
`|borrow − b_true| ≤ 1e-4` under `carry_al_opts = al_fast_opts()` vs `carry_al_opts = nullopt`
(accurate). If the fast preset misses the bound on this fixture, **report it — do not loosen the
test**; that is a real accuracy finding about a landed default.

**Files:** `atx-vol/src/deamer.cpp`, `atx-vol/include/atx/vol/deamer.hpp`,
`atx-vol/tests/deamer_test.cpp`.

**Tests (TDD):** the R-27 high-dividend fixture; a limit-cycle/convergence test for R-06 (a pair
that currently exhausts 64 iterations and returns `Unavailable`, now accepted at the documented
bound); a warm-vs-cold carry equivalence test for R-10 (converged borrow equal within tolerance);
a boundary-solve counter assertion proving reuse across FP iterations.

**Gate:** focused Release `deamer` + `dividend` + `calib` tests green; report the carry-stage timing
delta on the isolated fixture (best-of-3).

---

## Task 7 — Selector served-coverage floor on the v2/risk path, and mark-path availability (R-02, R-17)

**Sprint item:** W3.2 (the landed floor is only half-wired).

**Problem R-02 (P1 — the MU hole survives on the risk path).** Both branches of `PricerFitter::fit`
(mutually exclusive, `src/pricer_fitter.cpp:475`) run CV via `select_curve`. The legacy mark branch
tightens admission when a selector decision exists:

```cpp
// src/pricer_fitter.cpp:640-643
publication_admission = next_selection
    ? detail::selector_served_admission_policy(cfg_.admission, cfg_.selector)
    : cfg_.admission;
```

The v2/risk branch's `admission_attempt` lambda uses raw `cfg_.admission`
(`src/pricer_fitter.cpp:1284-1292`) — no tightening — even when the curve came from the selector.
Defaults make the hole live: `FitAdmissionPolicy::min_quote_coverage{0.0}`
(`include/atx/vol/fit_policy.hpp:124`), and `risk_admission_policy()` only sets
`min_expiry_coverage = 1.0` (`:155-163`), which a 46%-of-keys-per-slice fit still satisfies. So the
exact MU failure — a CV-chosen family whose serving rebuild abstains on 54% of the board — is
rejected on the mark path and **admitted on the risk path**, contradicting the invariant stated at
`include/atx/vol/curve_selector.hpp:128-131`.

**Fix:** immediately before the `admission_attempt` lambda (`:1284`), compute the same tightened
policy from `selection_` (which is `reset()` at `:749` and set at `:1165` — no staleness):

```cpp
const FitAdmissionPolicy publication_admission =
    selection_.has_value()
        ? detail::selector_served_admission_policy(cfg_.admission, cfg_.selector)
        : cfg_.admission;
```

and pass `publication_admission` into `completed_attempt_report` inside `admission_attempt` so the
primary **and every fallback rung** get it.

**Problem R-17 (availability asymmetry).** On `select_curve` failure the mark path hard-returns
`Err` (`src/pricer_fitter.cpp:606-614`) while the v2 path falls back to `decision_->curve`
(`:1166-1175`). With production's single eSSVI candidate and 1.0 coverage floors, one non-finite
wing key or one failed holdout American price zeroes the board (LKG retained). Pre-commit, four
other families could satisfy admission.

**Fix:** mirror the v2 semantics on the mark path — on `select_curve` failure with `NotFound` or
`Unavailable` (**not** `InvalidArgument`, which is a real defect and must propagate), proceed with
`next_decision->curve` + `in.calib` instead of returning, and record the selector error in the
attempt report so the fallback is visible rather than silent.

**Files:** `atx-vol/src/pricer_fitter.cpp`, `atx-vol/tests/pricer_fitter_test.cpp`.

**Tests (TDD):**
1. v2-path analogue of `ServedCoverageFloorRejectsNarrowFamilySpecificRebuild` — a selector-chosen
   family whose rebuild abstains on most of the board is **rejected** on the risk path (currently
   admitted).
2. `MarkPathFallsBackWhenSelectorUnavailable` — `select_curve` returns `NotFound`/`Unavailable`;
   assert the board still fits via the profile primary and the attempt report records the selector
   error (currently the board hard-fails).
3. `MarkPathPropagatesSelectorInvalidArgument` — `InvalidArgument` still propagates (fail-closed on
   real defects).

**Gate:** focused Release `pricer_fitter` + `curve_selector` tests green.

---

## Task 8 — Selector API hygiene and prep parallelism (R-33, R-34, R-18-lite, R-35 partial)

**Sprint item:** W3.2 (cleanup).

**Scope note:** the **full** R-18 fix (returning `PreparedExpiry` slices from `SelectorResult` and
teaching `VolaSession::build` to consume pre-prepared slices) is a cross-cutting design change to
the serving rebuild. **Do not attempt it here** — implement the bounded pieces below and report the
remaining handoff as pending.

**Fix A (R-33 — defaulted research config).** `select_curve(..., const SelectorConfig &sel = {})`
(`include/atx/vol/curve_selector.hpp:171-173`). The design intent is that a generic
`SelectorConfig{}` is *explicit* unlimited research behavior (header `:148-151`); a defaulted
parameter makes it the *implicit* behavior for any new caller that forgets the argument — 5 families,
unbounded. Today only tests rely on the default (`tests/curve_selector_test.cpp:397`); production
callers all pass `cfg_.selector` / `cfg.selector`.

Drop the default argument from `select_curve`; update the test call sites to pass `SelectorConfig{}`
explicitly. Keep the default on `score_curve_oos(..., scoring = {})`, where `candidates` is
overwritten and only floors matter.

**Fix B (R-34 — tie-break population comparability).** `src/curve_selector.cpp:549-580, 608-618`:
`oos_vw`/`oos_in_band`/coverage use fixed denominators (clean), but the chi²/RMSE vectors receive
only rows the candidate successfully priced. At default floors (1.0) all admitted candidates scored
every key, so populations coincide. If a research caller relaxes `min_holdout_coverage`, two
candidates with equal coverage but different failed keys tie on the lexicographic rank and then
compete on `chi2_distance` computed over **different sub-populations** — a residual of exactly the
non-comparability class W3.2 set out to kill.

Prefer the documentation fix: state on `CandidateScore::chi2_reduced`
(`include/atx/vol/curve_selector.hpp:69-79`) that chi²/RMSE are population-comparable only at
floors == 1.0, and that relaxed floors make them tie-break-only heuristics. (The masked-intersection
alternative costs a `std::vector<bool>` per candidate; only implement it if it falls out cleanly.)

**Fix C (R-18-lite — parallelize prep).** The per-expiry `prepare_expiry` loop
(`src/curve_selector.cpp:493-501`) is strictly serial although `sp.fit_workers` is copied in
(`src/pricer_fitter.cpp:601`) and **never consumed** by `select_curve`. Either consume it —
parallelize the prep loop over `sampled_expiry_indices` using the existing `parallel_for`, preserving
deterministic results — or, if nested-parallelism budget makes that unsafe here, delete the dead
copy and comment why. Decide from the code; report which you chose and why.

**Fix D (R-35 — dead policy copy).** `src/curve_selector.cpp:245` hardcodes
`inputs.policy = PreparedObservationPolicy::Configured`; the `sp.fit_prep_policy` copied at
`src/pricer_fitter.cpp:602` is dead in the selector. This invites the false belief the selector honors
it. Either honor it or stop copying it and comment why the selector always scores on Configured prep.

**Files:** `atx-vol/src/curve_selector.cpp`, `atx-vol/include/atx/vol/curve_selector.hpp`,
`atx-vol/src/pricer_fitter.cpp`, `atx-vol/tests/curve_selector_test.cpp`.

**Tests:** determinism test for any parallelized prep (serial vs N workers → identical selection);
existing selector tests green after the default-argument removal.

**Gate:** focused Release `curve_selector` + `pricer_fitter` tests green.

---

## Task 9 — Populate: streaming writes, durability, LPT ordering, worker budget (R-03, R-12, R-13, R-14, R-35)

**Sprint item:** W4.1 (the landed queue is correct; these are its costs).

**Problem R-03 (P1 — peak memory O(all boards)).** `std::vector<FitSlot> slots(n)`
(`src/surface_db_populate.cpp:168`) is sized over **every** board across **all** dates; the scheduler
joins globally (`:190-203`) before the first `write_partition` (`:208`); each Ok `FitSlot` owns a full
`PricedSurface` (`src/corpus_board_fit.hpp:44`). The pre-commit code scoped `slots(range_n)` inside
the per-date loop, so at most one date's surfaces were live. `with_uid` additionally deep-clones each
surface into `stamped` during the write pass. On the sprint's target workloads (multi-year backfill,
the 519-name cohort) that is thousands-to-10⁵ live dense surfaces — plausibly GBs — and the pending
throughput gate runs exactly there.

**Fix:** pipeline aggregation with fitting. Keep the single global queue. Add a per-`DateRange`
`std::atomic<std::size_t> remaining` initialized to that range's eligible-board count; each task
decrements its date's counter after storing its slot and notifies a condvar. The caller drains dates
in **ascending order**, aggregating + writing + releasing (`slots[pos] = FitSlot{}`) each date as soon
as its counter hits zero **and** all earlier dates are written. Write order stays date-asc/symbol-asc
(deterministic), live memory bounds to the in-flight window, and date-granular durability returns.

This requires splitting `run_bounded_fit_tasks` into launch/join (or draining from the caller thread
while workers run). Preserve every guarantee the current scheduler holds: exactly-once claims,
exception containment inside the worker loop, transactional launch-failure abort, lowest-task-index
error selection.

**Fix R-12 (durability).** Falls out of R-03: today an exception in any worker makes
`run_bounded_fit_tasks` return `Internal` and populate returns at `:204-206` having written **zero**
partitions — hours of completed fits discarded (the old code wrote each date immediately, so a crash
left earlier dates durable under `skip_existing` resume). With streaming writes, completed dates are
durable. Add a test.

**Fix R-13 (LPT ordering).** `fit_positions` is filled in (date asc, symbol asc) order and the queue
claims in that order (`src/fit_scheduler.cpp:77`). No LPT balance. Worst case: an SPY-sized board
(~4× a median name, now pinned to `fit_workers=1` at `:197`) is claimed last and extends the makespan
by its **full serial** cost while the other workers idle — directly threatening the ≥6-effective-cores
acceptance. Output ordering is fully decoupled from claim ordering (slots indexed by `pos`; writes
iterate `date_ranges`), so sorting the *claim* order is determinism-free.

`std::stable_sort` `fit_positions` by descending cost proxy — `boards[order[pos]].frame` row count
(quote count is the dominant cost driver) — tie-broken by `pos`.

**Fix R-14 (core starvation).** `parallel_outer` is true whenever `fit_positions.size() > 1 &&
worker_budget > 1`, and then **every** fit is pinned to `fit_workers = 1` (`:188-198`). With 2–4
eligible boards on a 12-thread budget, 8–10 cores idle for the whole run. The `parallel_for` contract
(`include/atx/vol/parallel_for.hpp:14-19, 43-45`) guarantees bit-identical results for any worker
count, so a shared budget is determinism-safe:
`pc.fit_workers = max(1, worker_budget / min(worker_budget, fit_positions.size()))`.

**Fix R-35 (three small ones).**
- `src/fit_scheduler.cpp:84-88, 124-127`: a task exception surfaces as a bare
  `"run_bounded_fit_tasks: task threw an exception"` — no task index, no board identity, no
  `what()`. The no-allocation constraint applies only inside the worker catch; the **final scan runs
  on the caller thread** where allocation is safe. Include the task index; let populate map index →
  board.
- `src/surface_db_populate.cpp:178-179`: `found.has_value() ? *found : cfg.fallback` treats an
  IO/corruption error identically to `NotFound`. This commit moved resolution onto the serial caller
  thread (`:176-184`) where propagating is trivial:
  `if (!found && found.error().code() != ErrorCode::NotFound) return Err(found.error());`
- `src/surface_db_populate.cpp:188` / `src/fit_scheduler.cpp:69-72`: `worker_budget` is clamped only
  to `task_count`, so `cfg.n_threads = 64` on a 12-core box really creates 64 jthreads. Clamp to
  `max(1u, std::thread::hardware_concurrency())` or document the footgun on
  `SurfaceDbPopulateConfig::n_threads`.

**Files:** `atx-vol/src/surface_db_populate.cpp`, `atx-vol/include/atx/vol/surface_db_populate.hpp`,
`atx-vol/src/fit_scheduler.cpp`, `atx-vol/include/atx/vol/detail/fit_scheduler.hpp`,
`atx-vol/tests/surface_db_populate_test.cpp`, `atx-vol/tests/corpus_test.cpp`.

**Tests (TDD):**
1. `StreamingWritesReleaseCompletedDates` — completed dates are written (and their surfaces released)
   before all fitting finishes.
2. `WorkerExceptionPreservesCompletedPartitions` — a throwing board leaves earlier dates durable
   (currently zero partitions written).
3. `HeavyBoardsAreClaimedFirst` — LPT ordering by row count, deterministic tie-break.
4. **Coverage gap the current suite has:** the existing
   `GlobalParallelQueuePreservesDeterministicPartitions` is 4 boards / 2 dates / 4 threads — threads ≥
   boards, so the dynamic-claim path degenerates. Add ~12 boards / 3 dates (middle date pre-written to
   exercise the skip-range/`fit_positions` gap logic), one failing board, `n_threads=3`, asserting
   bit-equality vs serial.
5. Scheduler error message carries the task index.

**Gate:** focused Release/Debug `surface_db_populate` + `corpus` tests green; determinism (serial vs
N workers) bit-equal.

---

## Task 10 — Sampling-counter bias (R-20, R-21)

**Sprint item:** W0.2 (the counters' own honesty).

**Problem R-20.** `SamplerState{position=0, target=0, random=<fixed seed>}`
(`include/atx/vol/counters.hpp:328-341`); `take_sample` rerolls `target` only at the end of each
64-block (`:343-357`). So block 0's target is the initializer `0` — the **first** eligible operation
on every thread is sampled with probability 1, not 1/64. And the xorshift seed is a compile-time
constant shared by all threads and processes, so every thread follows the **identical** target
sequence: "randomized phase" holds only from block 2 onward, and phases are perfectly correlated
cross-thread. For low-volume threads (or the one-operation-per-process corpus harness), `estimate()`
multiplying by 64 over-reports by up to 64×; a fan-out pool where each worker sees < 64 events
over-reports **systematically**, not randomly.

**Fix:** seed `random` per thread (mix e.g. `reinterpret_cast<uintptr_t>(&t_state)` or a hash of
`std::this_thread::get_id()` into the constant), then roll `target` once from that seed **before**
first use (e.g. lazily on a sentinel), so block 0's sample position is uniform and thread phases
decorrelate. Keep the hot path allocation-free and keep `ThreadState` constant-initialized (no TLS
dynamic-init guard on the hot path — verify this still holds after the change).

**Problem R-21.** `t_state.active_inversion` is set only when the outer scope is *sampled*
(`include/atx/vol/counters.hpp:408-441`). A nested inversion under an **unsampled** outer sees
`previous_ == nullptr` and is offered to the sampler as a root operation; the same nesting under a
**sampled** outer is folded in. "Root op" is therefore defined inconsistently: `american_iv_samples`
is inflated by ~(nested fraction × 63/64), and per-inversion work ratios blend root and nested costs.

**Fix:** track lexical depth separately (a TLS `int depth`) and gate sampling on `depth == 0`
regardless of the outer's sample outcome.

**Files:** `atx-vol/include/atx/vol/counters.hpp`, `atx-vol/tests/counters_test.cpp`.

**Tests (TDD):** first-op-not-always-sampled (statistical over many simulated threads, deterministic
seed injection preferred over randomness in the test); two threads decorrelate; nested-under-unsampled
does not count as root.

**Gate:** counters-on Debug focused tests green; the W0.2 overhead claim must not regress —
re-verify the hot path stays allocation-free and constant-initialized.

---

## Task 11 — SIMD batch edges (R-22, R-23, R-24)

**Sprint item:** W1.1 (edges the landing missed).

**Problem R-22 (NaN escapes the patch predicate).** `src/simd/black76_batch_avx2.cpp:68-75` and
`src/simd/greeks_batch_avx2.cpp:83-90` build `patch_bits` from ordered compares
(`_CMP_GT_OQ`/`_CMP_LT_OQ`), which are **false on NaN**. With all inputs finite and positive (so
`input_patch_mask` is clear), `d1 = (ln(F/K) + v²/2)/v` is NaN when `F/K` underflows to 0
(`ln → -inf`) **and** `v² = (σ√T)²` overflows to `+inf` (σ ≳ 1.3e154). Both wing compares are then
ordered-false, the lane is not patched, and the clamped Chebyshev Φ
(`include/atx/vol/detail/vector_math.hpp:128-131, 153-159` maps NaN → `-HalfRange`) turns NaN into a
**finite garbage price** where the scalar source of truth yields NaN. Violates the
"patched lanes identical to pure-scalar" contract. Adversarial magnitudes only — unreachable from
market data — but it is a contract hole.

**Fix:** OR an unordered self-compare into `patch_bits`:
`_mm256_cmp_pd(d1, d1, _CMP_UNORD_Q) | _mm256_cmp_pd(d2, d2, _CMP_UNORD_Q)` — one extra cmp per
block, forcing NaN lanes through scalar.

**Problem R-23 (identity aliasing rejected).** `src/batch.cpp:46-62` (`spans_overlap`/`overlaps_any`)
rejects **any** byte overlap including exact identity (`price_out == F`). `f1dd590^`'s batch had no
overlap checks — all six entries were elementwise scalar loops, so identity aliasing was well-defined
and worked. Identity aliasing is safe even for the 4-lane kernels (within a block all loads complete
before the block's stores, and stores only touch already-consumed indices), and the rejection is
applied even to the two routes that remain purely scalar (`implied_vol_batch`,
`black76_price_from_lnfk_batch`).

**Fix:** permit exact identity aliasing (`lhs.data() == rhs.data() && lhs.size() == rhs.size()`) while
continuing to reject partial overlap. Document the allowance in `include/atx/vol/batch.hpp:28-31`
alongside the existing contract text, and test both (identity accepted, partial overlap still
rejected).

**Problem R-24 (contradictory public IV routing).** `atx::vol::implied_vol_batch`
(`src/batch.cpp:145-170`) is scalar — correct, per the measured-slower AVX2 IV rationale recorded at
`include/atx/vol/batch.hpp:17-19`. But `atx::vol::simd::implied_vol_batch` remains a second **public**
batch-IV entry point (`include/atx/vol/simd/iv_batch.hpp:38-41`) that **always** dispatches to AVX2
(`src/simd/iv_batch.cpp:41-44`). Correctness is unaffected; the inconsistency is that `simd::` callers
get the measured-slower route while span-API callers get the faster scalar one.

**Fix (documentation).** Document the divergence at `include/atx/vol/simd/iv_batch.hpp` so both entry
points' routing rationale is discoverable and consistent: `simd::` is the explicit
"give me the AVX2 kernel" API; the span API picks the measured-faster route. Do **not** change
`simd::implied_vol_batch`'s routing — that is W5.4's call, and W5.4 is out of scope.

**Files:** `atx-vol/src/simd/black76_batch_avx2.cpp`, `atx-vol/src/simd/greeks_batch_avx2.cpp`,
`atx-vol/src/batch.cpp`, `atx-vol/include/atx/vol/batch.hpp`,
`atx-vol/include/atx/vol/simd/iv_batch.hpp`, `atx-vol/tests/batch_test.cpp`.

**Tests (TDD):**
1. `NanIntermediateLanePatchesToScalar` — construct the underflow/overflow lane; assert the AVX2 route
   returns exactly what the scalar route returns (NaN), not a finite value.
2. `IdentityAliasingIsAccepted` / `PartialOverlapStillRejected`.

**Gate:** focused Release `batch` + `simd` tests green on an AVX2 host (note: `build-rel` is
sse2-configured per the baseline name — if the AVX2 kernels are not exercised by the default preset,
use the `rel-avx2` preset for this task's gate and say which you ran).

---

## Task 12 — Small contract fixes (R-25, R-26, R-30, R-35 backtest)

**Sprint item:** W1.5 / W2.4 / W2.1 / W4.4 (cleanup).

**Fix A (R-25 — explicit `score_parity=false` silently overridden).**
`src/corpus_board_fit.cpp:199-214` assigns `cfg.score_parity = true;` **unconditionally** when the
profile's corpus rule consumes fit-parity metrics. But `include/atx/vol/pricer_fitter.hpp:158-176`
documents that "An explicit false skips the second de-Am diagnostic pass and therefore **fails
closed** when admission requires that evidence" — and `evaluate_surface_admission` /
`src/corpus.cpp:163-167` do fail closed on missing metrics. So the corpus path flips an explicit
caller `false` back to `true`, and a caller measuring the elided path gets the scored (slower) path
instead of the documented rejection.

Fix: only default when unset — `if (!cfg.score_parity.has_value() && ...) cfg.score_parity = true;`
Explicit `false` then fails closed per the documented contract.

**Fix B (R-26 — vestigial `tol`).** `src/dividend.cpp:66-68, 88-89`: the closed-form borrow ignores
`tol` entirely (the endpoint snap uses a hardcoded 16·ε slack), yet a caller passing `tol <= 0` still
errors. The header doc calls it an "endpoint-roundoff allowance", which the code does not implement.
Either honor it (`max(endpoint_slack, tol)`) or deprecate it in the header doc. Pick one and make code
and doc agree.

**Fix C (R-30 — trust-based geometry revalidation).** `src/american.cpp:698-700`:
`al_bind_geometry_static` early-returns on `geo_static_bound == true` **without** verifying
`(T, r, q, n, nq)` identity; `al_bind_geometry_sigma`'s assert (`:739`) checks only that *some* static
bind happened. All current entry points force-invalidate first (reset `:1421`, wrapper `:769`), so
there is no live bug — but the invariant is one new call site away from silent stale geometry, which
is exactly the shape of the prior accurate-scheme regression (memory obs 23864).

Fix: under `!defined(NDEBUG)`, store a bind key `{bnd.T, r, q, bnd.n, ws.n_quad_fp}` in `AlWorkspace`
at static-bind time and `assert` it matches in `al_bind_geometry_sigma`. Also add a counter for the
Release `ws.specialize = false` fallback (`:741`), which permanently degrades a retained pricer to the
generic kernel until the next reset — safe direction, currently invisible.

**Fix D (R-35 — backtest scratch).** `src/backtest.cpp:94-100`: `reserve(capacity)` to the exact size
reallocates once per step for a book growing by one lot per step; use
`alive_.reserve(std::max(capacity, alive_.capacity() * 2))` (or let `push_back` grow it). And at
`:518/:556/:81`: `compute_step` passes `retained.alive_` back into `retained.prepare(alive, ...)`,
where the rebuild path does `key_ = lots;` — currently safe (copy-assign between distinct members,
`lots` only read), but a future edit that mutates `key_`/`alive_` before reading `lots` self-aliases
silently. Add `assert(&lots != &key_)` or a comment stating the constraint.

**Files:** `atx-vol/src/corpus_board_fit.cpp`, `atx-vol/src/dividend.cpp`,
`atx-vol/include/atx/vol/dividend.hpp`, `atx-vol/src/american.cpp`, `atx-vol/src/backtest.cpp`, and
the corresponding tests.

**Tests (TDD):** explicit-`false` fails closed on a consuming corpus rule; dividend `tol` behavior
matches whichever contract you chose; a Debug test that the geometry bind key catches a mismatched
rebind (if expressible without contriving a bad call site, an assert-only change plus a comment is
acceptable — say so).

**Gate:** focused Release + Debug tests for the touched suites green.

---

## Task 13 — Benchmark regression gate sees the fit rows (R-16)

**Sprint item:** W0.1 (the gate has a hole).

**Problem.** `bench/compare_baseline.py:48-60` (`aggregates()`) keeps only `run_type == "aggregate"`
rows. `bench/e2e_hotpath_bench.cpp:488-501` (`register_corpus_scale`) sets `Iterations(1)` and never
applies `Repetitions`, deliberately (corpus rows must execute one operation per process). The
checked-in baseline confirms: aggregate rows exist **only** for the four `price/backtest/spy_real/*`
names; `fit/e2e/spy_real` and `fit/e2e/100name` appear only as `run_type: "iteration"`.

Since `aggregates()` drops iteration rows, the fit rows are absent from **both** sides of any
comparison — so they are neither ratio-gated **nor** caught by the "present in BASELINE but ABSENT
from new run" fail-loud path. A crash or a 10× regression in `fit/e2e/*` passes
`compare_baseline.py` silently. (The `atx-vol-e2e-benchmark-name-coverage` CTest does catch *name*
drift for all six rows via `--benchmark_list_tests`, but it never reads the baseline JSON; its
required-name list is duplicated as CMake literals.)

**Fix:** in `aggregates()`, fall back to the `iteration` row's `real_time` (median-of-1) when a
`run_name` has no aggregate rows, so single-iteration rows enter both the ratio gate and — critically
— the missing-benchmark check. The existing noisy-CV guard should treat a median-of-1 row as
unguarded-by-CV rather than pretending to a confidence it does not have; make the report say so.

Optionally (say whether you did it): derive the CTest's `--required` list from the baseline JSON's
names so the three sources of truth cannot diverge.

**Files:** `atx-vol/bench/compare_baseline.py`, its test if one exists,
`atx-vol/bench/CMakeLists.txt:153-174` (only if you do the optional part).

**Tests (TDD):** a synthetic baseline+run pair where an iteration-only row regresses 10× → the
comparison **fails** (currently passes); a synthetic pair where an iteration-only row is missing from
the new run → **fails**; existing aggregate-row behavior unchanged.

**Gate:** the comparison script's own tests green; run it against the checked-in baseline and report
what it now says about the two fit rows.

---

## Task 14 — W4.4 allocation/latency acceptance benchmark (the owed gate)

**Sprint item:** W4.4 (status `partial` — "Allocation/latency acceptance benchmark remains").

**Problem.** W4.4's landed half (grow-only `PortfolioWorkspace`, retained `alive` scratch) has passing
correctness tests but **no allocation/latency acceptance evidence**, which the ledger itself lists as
the outstanding requirement.

**Fix — build the benchmark the handoff owes:**
- A test/bench binary with a counting `operator new` hook.
- Fixed-book `run_backtest`: ~250 synthetic snapshots preloaded into a caller-supplied
  `cfg.snapshot_cache` (so archive loads do not pollute the count), a stable 64-lot book, no mid-run
  expiries, hedging off, `record_every_n = 1`.
- **Assert allocations/step == 0 after a 10-step warmup**, measured around the step loop.
- A second variant **with** a trading strategy + Daily hedge, to baseline the allocations that remain
  by design (`execute()`'s `std::vector<std::uint32_t> uids` at `src/backtest.cpp:1325`; the
  single-slot `RetainedBookPricer` thrashing between the pruned book (`:556`) and the post-trade book
  (`:1226`), each miss paying `positions_at` + `Portfolio::create` + `pricer_.emplace` + `key_` copy).
  This gives the sprint's remaining retained-pricer/2-slot-cache work a **before** number.
- Latency: per-step p50/p99 vs `52324e5^`.

**Do not** implement the 2-slot retained-pricer cache itself — that is remaining W4.4 scope beyond
this session's cleanup mandate. Measure and report; leave the fix to the sprint.

**Note the constraint:** if the stable-book variant does **not** hit 0 allocations/step, that is a
finding about the landed W4.4 work — report it with the allocation source, do not weaken the assert to
make it pass.

**Files:** new bench or test under `atx-vol/bench/` or `atx-vol/tests/`, following the existing
`bench_gate.hpp` env-gating pattern (benches gated behind `ATX_VOL_BENCH`, skipped by default) so the
default suite stays fast.

**Gate:** the benchmark runs Release, reports allocations/step for both variants and p50/p99 latency,
and is registered so it is discoverable but not run by default.

---

## Final steps (controller)

1. Whole-branch review over `5ba7fe4..HEAD`.
2. Update `atx-vol/sprints/2026-07-14-atx-vol-fit-price-backtest-hotpath-sota-sprint.md`:
   ledger rows for touched tasks, the §8 corrections from the review (handoff misdiagnosis, W3.2
   over-credit, W3.2 gate status, counter semantics, W2.6 score aliasing, time_budget divergence),
   and a truthful statement of what remains.
