# Fixed DSL strategy runner: source qualification packet

Source frozen, runtime and recent-data results pending. This implements the user-directed $1bn strategy path; it does not assert net Sharpe >=1, economic calibration, or full DAG acceptance.

## Packet and owning closure

- `54b4ec99c02165fb3f8e89dd5767e9a582b18568`: shared execution config/CPP schedule controls.
- `7fa3c6ab31a5f4848090f0ee30616a1be5efe0cf`: new lightweight `atx-impl/src/strategy_runner.hpp`, private `strategy_runner.cpp`, standalone `atx-impl/tools/equity_strategy.cpp`.
- `97da349fe81d2405f2aec2f87be6e493f7e008ac`: report/zero-volume corrections.
- `ec6011e53b8e1f81e202f6d08df9c70ad606ae69`: four `ExecutionSchedule.*` cases in new engine factory TU `execution_schedule_test.cpp`.
- `dbd50b14ac6d173349f23fa7bd173b8d123c56bb`: three `StrategyRunner.*` cases in new impl TU `strategy_runner_test.cpp`.

Dependencies are the already reviewed execution kernel, D12 historical CS mask, B1 CostSurface, and G0 role loader original `5f3b6631` (local dependency cherry-pick `25e42f07` is NOT a new implementation to import). Root owns CMake registration and compilation; runner CPP requires the existing private JSON include. No broad Config/mine headers were changed. No local builds, test execution or market payload reads were performed. `git diff --check` passed.

## Exact research contract

The library is externally SHA-pinned. The selected artifact has 24 fixed candidates in six families, four per family. Each candidate is evaluated once through the real VM, with a full date-major as-of decision-membership mask on every internal cross-sectional operation; physical source-presence and raw trailing histories remain separate. ResearchFast corrected kernels are declared. No random search is performed; seed is recorded.

Training independently executes both signs on the predeclared `weekly_partial25` variant: 48 candidate-orientation trials. Higher finite net Sharpe wins; finite equality pins +1, either unscorable sign refuses the run. Signs and equal-family/equal-within-family weights then freeze. Centered tied candidate ranks contribute with fixed denominators; missing contributions are zero and never cause weight renormalization. The combined signal is finally rank-transformed, dollar neutral and gross one under the existing WeightPolicy. No risk/sector neutrality claim is made.

Two combined TRAIN evaluations bring the actual library's planned TRAIN count to 50. Validation performs the same two fixed combined strategies, never individual orientation selection. Every started/completed/failed execution has a JSONL trial receipt; planned counts are explicitly labeled. Partial runs cannot look complete. No registry DSR or selection-adjusted p-value is fabricated.

`weekly_partial25` is primary, `weekly_full` diagnostic. Both use every five **sessions**, phase relative to role decision-window start, not calendar weeks. Partial targets interpolate decision-known held dollars before queuing. Entry execution nets against then-marked holdings; participation caps apply to actual net fills. Multiple pending targets are not re-interpolated at entry. Off-cycle holdings still mark and accrue actual-calendar borrow; their execution turnover/cost is zero. Membership exits unwind partially at scheduled decisions, not immediately. Defaults cadence1/fraction1 preserve previous arithmetic and hash stream. The default fixture checks same-code explicit/default equality plus an independent initial cash ledger; it is not a saved prepatch hash golden.

## Costs, returns and memory

Each decision owns an immutable hash-bound CostSurface snapshot. ADV63 uses prior complete raw-close times raw-share-volume cells; observed zero volume is valid, missing source cells are not. Daily volatility is sample SD of prior 63 guarded adjusted simple returns. Raw price stays positive; mean ADV must be positive. Declared unfitted defaults are full spread10bps, commission1bps, annual modeled borrow300bps, impactY0.6 and participation1% of prior ADV. These are explicit configurable scenarios, not observed quotes, empirical estimates or locate availability. Borrow is charged independently on held shorts using actual elapsed calendar days. No free cash financing, missing held-price return, unpriceable nonzero trade or unknown borrow becomes zero.

The kernel uses decision-fixed dollars, delay1 entry, full mature endpoint returns, cash/short proceeds and marked holdings. Initial NAV is exactly $1bn. There is no automatic terminal liquidation; final NAV includes held inventory. Target rank policy is not a continuous post-drift per-name cap.

Before payload loading, compiled program slot peaks and pinned manifest geometry admit the combined Panel, independent support, ReturnGuard, owned execution copies, all decision snapshots, aggregate signal, one candidate, slot grow-before-release and execution output/scratch. One role is loaded at a time; candidate values are released sequentially; variants share immutable snapshot rows and release prior contexts. This is a conservative retained payload/workspace charge with slack, not measured RSS. A full multi-year/thousands-name role may refuse the current budget; that is not a claim of full-scale qualification. No chunked book carry is implemented.

## Publication and known limits

External SHA pins are required for library and every role manifest; the role's returned hash is compared again after loading. Score windows must be chronological/non-overlapping. Output directory must be new. Reports include gross/net Sharpe, actual/requested HAC lag and defined flag, net NAV drawdown, actual held-name breadth, max interval-entry weight, cap counts, cost sums, fixed-denominator family contribution coverage and context identities. Turnover is sum absolute actual filled dollars divided by each interval's pretrade NAV, including initial deployment; monthly21 turnover is 21 times the mean. CSV and ledger close errors refuse successful publication.

The role loader currently seals before 2025-01-01. The optional explicit holdout path remains subject to that admission; 2025+ publication is reserved and is NOT claimed runnable by this packet. No holdout file is opened by default. Archive availability clocks and common-stock/vintage qualification are explicitly modeled/unverified in the data recipe.

The postimplementation synthetic runner has only 404 dates x4 names and two supplied expressions: six TRAIN plus two validation calls. It checks fixed signs, real positive modeled execution/borrow charges, breadth and report fields, observed zero-volume admission, two-observation HAC lag clamping, external pins, combined budget and exclusive outputs. Cadence fixtures independently cover overlapping delay3/cadence2 targets, gradual exits, off-cycle mark/borrow and future-prefix invariance. Parent runtime evidence must be appended after its focused build; no synthetic result is presented as population retention, recent-data performance or executable capacity proof.
