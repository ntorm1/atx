# Task T3 — causal price-risk exposures and target neutralization (new private module)

Owner worktree: `C:/atx-wt/pool-3`, branch `feat/mega-alpha-exposures-20260926` (base `5c9cbaed`).
New files only: `atx-impl/src/strategy_price_exposures.hpp`, `atx-impl/src/strategy_price_exposures.cpp`,
`atx-impl/tests/strategy_price_exposures_test.cpp`. Do not edit any existing file (root owns CMake and
a later task wires this into the target/NAV replay). List the CMake lines root must add in your report
(CPP into `atx-impl-core` and the Debug `/O2 /Ob2 /clang:-finline` + `SKIP_PRECOMPILE_HEADERS` list in
`atx-impl/CMakeLists.txt` like `strategy_target_replay.cpp`; test TU into `atx-impl-strategy-target-tests`
in `atx-impl/tests/CMakeLists.txt`).

## Why
The combined 48-alpha blend has material negative market beta (about -0.12 TRAIN, -0.20 validation)
and ~10%/yr volatility at gross 0.9; a TRAIN-only prototype showed ex-ante neutralization against
trailing beta, volatility and log dollar-ADV cuts volatility ~30% and beta to ~0. The portfolio layer
needs this as a deterministic, causal, O(N)-state step applied to the desired target at a decision.

## Inputs (role payload layout)
Role payloads are date-major little-endian arrays of `dates x instruments`: `close.f64` (adjusted),
`raw_close.f64`, `volume.f64` (raw shares), `present.u8` (physical vendor row; absent => NaN prices),
`member.u8` (decision membership). Decision d is known after the close of session d.
See `atx-impl/src/strategy_target_replay.hpp/.cpp` (`TargetReplayInput`, `desired_target`, guard at
~:170: an adjacent interval is guarded if |log adj ratio| > 1.5 or > |log raw ratio| + 0.10).

## Interface (adapt names to house style; keep it this small)
```
struct PriceExposureConfig { usize beta_window{252}, vol_window{63}, adv_window{63}, min_return_pairs{126};
                             usize min_names{50}; f64 clip_z{5.0}; };
struct PriceExposureInput { usize dates, instruments; std::span<const f64> close, raw_close, volume;
                            std::span<const u8> present; };
inline constexpr usize kPriceExposureCount = 3; // beta, vol, log_adv
// Exposures known at decision d: windows end at session d inclusive, never read d+1.
//  - market return per session = equal-weight mean of valid adjusted simple returns (both endpoints
//    present, finite positive closes, interval not guarded) across ALL instruments on that session
//  - beta = cov(r_i, m)/var(m) over valid pairs in the last beta_window intervals; ok requires
//    >= min_return_pairs valid pairs
//  - vol = sample SD of r_i over the last vol_window intervals (>= vol_window/2 pairs)
//  - log_adv = log(mean over last adv_window sessions of raw_close*volume, absent days count 0); ok if > 0
// out is row-major instruments x 3; ok[i] = 1 iff all three finite.
Status compute_price_exposures(const PriceExposureInput&, const PriceExposureConfig&, usize d,
                               PriceExposureScratch&, std::span<f64> out, std::span<u8> ok);
// Rows used: member[i] && ok[i]. Z-score each exposure over those rows (clip at +-clip_z), OLS of
// target on [1, z1, z2, z3], replace target by the residual, then scale so sum|target| over the rows
// equals the input gross (sum|target| before neutralization). Rows member && !ok -> 0. Nonmembers
// untouched (must already be 0). Report counts (used, excluded) via an out struct. Fewer than
// min_names usable rows or a singular/ill-conditioned normal matrix -> explicit error Status.
Status neutralize_target(std::span<f64> target, std::span<const u8> member,
                         std::span<const f64> exposures, std::span<const u8> ok,
                         const PriceExposureConfig&, NeutralizeScratch&, NeutralizeStats&);
```
Performance: called about 150 times per role on ~5,600 instruments; O(window x N) per call is fine, but
reuse scratch (no per-call allocations after the first) and avoid O(N^2). Use a 4x4 solve with a
condition check (e.g. Cholesky with pivot threshold); keep FP deterministic (fixed loop order).

## Constraints
- House style: read `C:/atx-wt/pool-3/.agents/cpp/agent.md` first. Light header (std + atx core types/error).
- Do NOT compile or run anything: root alone builds. Write careful code; root returns compile errors.
- Not TDD: implement first, then postimplementation fixtures: (a) synthetic panel where name returns
  = k*market exactly -> beta == k (tight tolerance), vol and log_adv hand-checked; (b) causality:
  rewriting all data after d leaves exposures at d bit-identical; (c) guarded/absent intervals excluded;
  (d) neutralized target has |X' w| <= 1e-12 per exposure and intercept, preserved gross, nonmembers 0,
  member&!ok -> 0; (e) too few names / collinear exposures -> error.
- No subagents. Commit in pool-3 (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
Full report to `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T3-report.md`. Return only:
status, commit SHAs, one-line summary, concerns.
