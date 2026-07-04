# Sprint 3 progress ledger — Gârleanu-Pedersen Aim-Portfolio Trading

**Goal:** behind an inert `--gp-trading` flag, replace the position-mode deploy's crude linear
partial-trade (`w := prev + trade_rate·(target − prev)`) with the FROZEN GP aim-portfolio trade
(`risk::gp_aim_and_value` + `risk::gp_turnover_native_step`) so the live book trades toward the
risk-curvature-aware GP AIM. Zero new estimator math; the GP functions are called, never edited.

- **Worktree:** `C:\atx-wt\p9`
- **Branch:** `feat/p9`
- **Base:** `main @ c7c7b44` (S1+S2 already landed on this branch: tip `52ac04f`)
- **Build gate:** `powershell -File <scratch>\p9-build.ps1 -Target atx-impl-tests` then
  `powershell -File <scratch>\p9-ctest.ps1 -R <Suite>` (self-contained MSVC-env wrappers).

One line per unit (ROADMAP §141). Newest last.

| Unit | Commit  | Deliverable                                                                                  | Review |
|------|---------|----------------------------------------------------------------------------------------------|--------|
| S3-0 | a59ee77 | ledger opened; `RunConfig::{gp_trading,gp_risk_aversion,gp_trade_cost_scale}` + CLI parse arms | —      |
| S3-1 | 52551a5 | wire `gp_aim_and_value`/`gp_turnover_native_step` into the position-mode partial-trade step   | —      |
| S3-2 | (this)  | determinism battery (off-path byte-identity, twice-run) + mean-reverting turnover proof; close | —      |

## Determinism contract — all four classes satisfied

- **(a) off-path byte-identity** (`OffPathByteIdentical`): implicit-default run == run with
  `gp_trading=false`, `gp_risk_aversion=0.0`, `gp_trade_cost_scale=0.0` set EXPLICITLY — identical
  `StageResult::digest` AND byte-identical `books.bin`. The new `if (cfg.gp_trading …) … else if
  (trade_rate<1.0) …` structure takes the untouched `else if` arm and never builds the Diagonal `V`
  when the flag is off.
- **(b) on-path mean-reverting turnover proof** (`GpLowersTurnoverAtMatchedOrBetterSharpe`):
  GP realizes **strictly lower** cumulative turnover at **strictly higher** capital on the
  true-edge names (see the measured numbers below). RED→GREEN: pre-S3-1 the two paths are
  byte-identical (gp_trading unread) so `EXPECT_LT` is `159.67 < 159.67` → fails; the S3-1 wire
  makes it `29.48 < 159.67` → passes.
- **(c) twice-run byte-identity** (`TwiceRunByteIdentical`): same panel/config on the GP path
  (with `gp_trade_cost_scale=0.25` active) → identical digest and identical `books.bin` bytes.
- **(d) seq==parallel:** N/A, justified — the position-mode trade loop is an inherently sequential
  per-period state machine (`w[s]` depends on `prev=w[s-1]`); no `parallel_for`/executor touches
  this branch before or after S3. Recorded as a one-line comment at the top of the test file.

## GP-trading win, measured (the concrete S3 claim — the honesty gate)

On the S3-2 mean-reverting fixture (`M=4`, `D=120`, `trade_rate=1.0`, `gp_risk_aversion=0.5`):
names 0-1 are stable/low-noise with a genuine constant-sign edge (return variance hits diag_risk's
`1e-4` floor); names 2-3 are high-variance (`0.05·U(-1,1)` per-period return), zero-drift, with a
mean-reverting alpha that flips sign every period.

| Metric                                   | Legacy linear blend | GP aim-portfolio |
|------------------------------------------|---------------------|------------------|
| Cumulative book turnover (Σ over periods)| 159.67              | **29.48**        |
| Capital on the true-edge names (avg \|w0\|+\|w1\|) | 0.6667      | **0.9402**       |

GP cuts turnover ~82% **and** keeps MORE capital on the genuinely profitable names — a strict
double win, not a knife-edge tuned constant. **Mechanism (genuine, not relabeled):** the GP aim
`(2λV)⁻¹ᾱ` divides each name by its own Diagonal return variance `D_i`; the noisy flippers 2-3
(large `D_i`) are damped, so GP stops chasing their every-period sign flip at full rate, while the
legacy `shape_book` target re-derives and fully redeploys them each period. The variance-damping is
the load-bearing structure; the raw amplitude/λ are the only tunables.

**Fixture tuning note (honest):** the plan's draft fixture used a `0.02` per-period noise amplitude
on the mean-reverting names, which sits only marginally above diag_risk's `1e-4` variance floor
(damping ratio ≈ 0.75 — a weak, possibly non-strict win). Raised to `0.05` (return std ≈ 2.9%,
variance ≈ 8× the floor) so the two-tier variance structure is unambiguous. This is fixture
construction, not assertion softening — the `EXPECT_LT` is a genuine strict inequality with a large
margin, exactly the regime the plan's risk table sanctions tuning toward. A separate scope finding:
a 2-name dollar-neutral book can NEVER diverge (the demean forces ±0.5 symmetry regardless of the
aim), so `GpTradingChangesBookWhenSet` was built at `M=4` with two-tier variances, not the plan's
draft `M=2`.

## Regression

- Full `atx-impl-tests`: **167 passed / 0 failed / 1 pre-existing env-skip**
  (`AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`), the 4-test `AtxImplOptimizeGpTrading` suite
  included.
- Frozen-body engine GP tests (`GpTurnover*`, `*GarleanuPedersen*`) still green, unmodified —
  confirms `garleanu_pedersen.{hpp,cpp}` was never edited.
- Files touched (whole sprint): `config.{hpp,cpp}` (additive fields + CLI arms), `stage_optimize.cpp`
  (position-mode branch partial-trade site only — the MVO-branch `build_risk_model` sites are
  textually untouched), and the two new test files + this ledger. No `CMakeLists.txt` edits.
