# NAV replay design (read-only design agent, 2026-09-26; root-approved with rulings in task-T2-brief.md)

## 1. Reuse map (file:line, pool-2 at 5c9cbaed)
REUSE DIRECTLY
- Saved-blend/role loading (`atx-impl/src/strategy_target_replay.cpp`): `SavedBlend` :226-236; `pinned_json` :237-249; SHA-streamed `payload<T>` :258-284; `admit_saved` :285-336 (budget :309-315); `load_prices` :337-371 (role identity/axes/presence/member binding, close/raw/present). Extend `load_prices` to also read `volume.f64` and check `volume_basis == "raw-share-volume"` (contract mirrors `atx-engine/src/data/strategy_data.cpp:86,139-149`: present => finite and >=0, absent => NaN).
- Target rules (same file): `desired_target` :102-123 (centered tied rank, demeaned, gross 1, all-tie flat); `update_weights` :124-152 (forced exits to zero on every decision; partial fraction on cadence; v2 budget fraction cap :134-135); `calendar_month` :48-53; `validate_input` :67-99; cadence anchor `(d - decision_begin) % cadence` :201.
- Costs (`atx-engine/include/atx/engine/book/replay_cost.hpp`): formula :16-24; `LiquidityRow{adv_dollars, daily_vol, half_spread_bps}` :39-43; `FlatBpsCost` :122-140; `SqrtImpactCost::create(ReplayImpactCfg{y,delta}, max_participation, commission_bps)` :149-182; impl `atx-engine/src/book/replay_cost.cpp:84-111` (cap = max_participation*ADV; unusable ADV -> no fill, no charge; `unrationed_cost` for full-request diagnostics). Header is light; link-only (already in atx-engine sources, `atx-engine/CMakeLists.txt:159-160`). Same formula as `CostSurface` (`cost/cost_surface.hpp:106-108`, delta .5) without per-snapshot SHA hashing (CostSurface would rebuild 754x ~5.6k rows through a Debug SHA path).
- HAC t: `atx-engine/include/atx/engine/eval/hac.hpp:171` `mean_inference(x, BartlettV1, 5, true, true)` (same call as `strategy_runner.cpp:552-553`).
- Terminal stress constants: `book/replay.hpp:89-90` (Shumway -0.30/-0.55) and :124-129 `assumed_missing_price_return(Unknown, side)` -> long -0.55, short +0.30 loss. Copy as constants in the CPP (do not include replay.hpp in production: it pulls claims_state/security_transition); the test TU includes replay.hpp and asserts parity.

REUSE CONVENTIONS, NOT CODE
- DelayedSurfaceV2 timing/sizing: `factory/execution_objective.hpp:20` (entry = d+delay, endpoint = entry+1), :31-37, :111-119 (decision-NAV fixed dollars; partial move from decision-known held dollars; short proceeds stay in cash; no terminal liquidation); `src/factory/execution_objective.cpp:461-515` (entry turnover += |fill|/entry_nav :498; costs to cash :504-505), :516-572 (target NAV after entry costs :554-557; partial :564-571), :385-393 (borrow on pre-mark short $ x days / 365).
- Clocks: `strategy_data.cpp:132-133` (mark = session+22h, decision = session+23h).
- Summary stats from `strategy_runner.cpp`: Sharpe :281 (sample SD, sqrt252); month by execution session :518-522, :575; deployment = first nonzero fill :525-528; year return :529-531, :563-565; max drawdown :532-533.
- Liquidity inputs `strategy_runner.cpp:206-251`: prior-window raw $ ADV and sample SD of adjusted simple returns; guard `make_guard` :188-205 == replay guard `strategy_target_replay.cpp:170-171` (|adj log| > 1.5 or > |raw log| + 0.10).
- Final session valuation-only (`book/replay.hpp:433-434`).

NOT REUSED: `factory::ExecutionObjective` refuses missing/guarded held returns by contract (`execution_objective.hpp:108-109`; `.cpp:373-377`) — the MDCO/JAG failure. `book::replay_scheduled_intents` (`replay.hpp:423-479`) is K=1 adverse-only for causal use, needs alpha::Panel and large per-allocation vectors, and adding a stale-K policy would change a broad public header (fan-out). Its semantics are kept as stress scenario S3. `engine::data::read_strategy_role` builds a heavier Panel and does not bind to the saved blend.

## 2. Files
ADD
- `atx-impl/src/strategy_target_replay_detail.hpp` (private; included only by the two CPPs and the new test; no nlohmann):
  ```
  namespace atx::impl::strategy::detail {
  struct LoadedSavedBlend { usize dates, names, begin, end;
    std::vector<f64> signal, close, raw, volume; std::vector<u8> member, present;
    std::vector<i64> sessions; std::vector<u64> ids; std::string manifest_json;
    TargetReplayInput view() const; };
  Result<LoadedSavedBlend> load_saved_blend(const TargetReplayRunConfig&, bool with_volume);
  Status validate_replay_input(const TargetReplayInput&, const TargetReplayConfig&);
  void desired_target(span<const f64>, span<const u8>, std::vector<std::pair<f64,usize>>&, std::vector<f64>&);
  void update_weights(const TargetReplayInput&, const TargetReplayConfig&, usize d, bool rebalance,
                      f64 spent, const std::vector<f64>& desired, std::vector<f64>& current, TargetReplayDay&);
  u32 calendar_month(i64 session_ns); }
  ```
- `atx-impl/src/strategy_nav_replay.hpp` (includes only `strategy_target_replay.hpp` + std): `enum class NavCostRule : u8 { FlatBpsV1=1, SqrtImpactV1=2 }`; `struct NavScenario { std::string id; NavCostRule cost; f64 flat_bps, half_spread_bps, commission_bps, impact_y, impact_delta, max_participation, annual_borrow_bps, fallback_daily_vol; usize stale_exit_sessions; bool adverse_terminal; }`; `struct NavReplayConfig { TargetReplayConfig target; NavScenario scenario; f64 initial_nav{1e9}; usize liquidity_window{63}, min_vol_pairs{20}; u64 max_events{262144}; }`; `struct NavReplayInput { TargetReplayInput target; std::span<const f64> volume; }`; `NavReplayDay`, `NavEvent`, `NavReplayResult`; pure `Result<NavReplayResult> replay_nav(const NavReplayInput&, const NavReplayConfig&)`; `run_nav_replay(const TargetReplayRunConfig&, std::ostream&)`; `dispatch_nav_replay(argc, argv, out, err)`.
- `atx-impl/src/strategy_nav_replay.cpp` (~500-600 lines): `validate_nav_input`, `liquidity_row`, `mark_session`, `execute_orders`, `plan_decision`, `replay_nav`, `summarize_scenario`, `write_daily`, `write_events`, `nav_recipe`, `run_nav_replay`, `dispatch_nav_replay`. `book/replay_cost.hpp`, `eval/hac.hpp`, nlohmann, sha256 are CPP-only includes.
- `atx-impl/tests/strategy_nav_replay_test.cpp` (postimplementation fixtures, section 10).
CHANGE
- `strategy_target_replay.cpp`: append `detail::` wrapper definitions forwarding to the existing anonymous-namespace functions (no moves, no arithmetic change). Optional `volume` vector in private `SavedBlend`; `bool with_volume=false` on `load_prices` (reads `volume.f64` + basis/contract checks); `admit_saved` per-cell budget 28 -> 36 only when volume requested (:312). `run_target_replay` unchanged (volume=false) so its outputs and the 7 existing fixtures stay identical.
- `atx-impl/tools/equity_strategy_targets.cpp`: if `argv[1] == "nav"` -> `dispatch_nav_replay(argc-1, argv+1, ...)`, else existing `dispatch_target_replay`.
- CMake (ROOT owns): add CPP to `atx-impl-core` list (:5-49) and to the Debug `/O2 /Ob2 /clang:-finline` + `SKIP_PRECOMPILE_HEADERS` list (:72-75); add test to `atx-impl-strategy-target-tests` (tests/CMakeLists.txt:102-104).

## 3. Data flow
CLI nav -> TargetReplayRunConfig (+rule params) -> detail::load_saved_blend(cfg, with_volume=true) [pins: combined SHA, role SHA == manifest.role_manifest_sha256, source SHA, axes, member==member&present&close>0] -> NavReplayInput -> for scenario in {S1,S2,S3} (fixed, all run): replay_nav -> exclusive output dir: recipe.json -> daily_<S>.csv, events_<S>.csv -> summary.json last.

## 4. Accounting
Decisions d in [d0, T-2); executions t = d+1 <= T-2; return rows t in [d0+2, T) (TRAIN 754). Last two scored decisions dropped (as `strategy_runner.cpp:217`); disclose vs replay's 756.
State: cash C (1e9), marked dollars h_i, last observed adjusted close p^_i, absent run a_i, working order (T_i $, active_i).
Per session t: MARK -> EXECUTE -> DECIDE.
MARK (t > d0): Ddays = calendar days between sessions. B_t = sum_{h_i<0}(-h_i)*b*Ddays/365 (b = 300e-4) on pre-mark short $. Held & present: g_i = close/p^ - 1 (covers gap); P_t += h_i g_i; h_i *= (1+g_i); p^ = close; if a_i>0 record gap-resolved, a_i=0; guard test (adj vs raw) -> guarded event. Held & absent: a_i += 1 (stale at p^); if a_i >= K: write off W_t += h_i*eta_side; C += h_i(1+eta_side); h_i=0; active_i=0; event. C -= B_t. NAVpre_t = C + sum h. r_t = NAVpre_t/NAVpre_{t-1} - 1 (fill costs at t-1 land in r_t, per `execution_objective.hpp:137-141`). Components / NAVpre_{t-1}: gross = P+W, cost = TC_{t-1}, borrow = B_t. Identity r = gross - cost - borrow checked in code.
EXECUTE (t <= T-2): for active i: absent -> blocked, order stays; else q = T_i - h_i; (f,c) = model.cost(i,t,q,L_i,t); h_i += f; C -= f; TC_t += c; traded += |f|; if h_i was 0 set p^ = close; if f==q inactive else capped/unfilled counted, order persists. C -= TC_t. one_way_turnover_t = traded/NAVpre_t. NAVpost = NAVpre - TC_t.
DECIDE (d = t in [d0, T-2)): cur_i = h_i/NAVpost_d (stale names at stale marks). Rebalance day: desired_target(signal_d, member_d). next = cur; update_weights(...) (planned forced/discretionary, applied_fraction; spent by decision month, v2 semantics). For every i with next != cur: T_i = next*NAVpost_d, active (incl. zero exits). On rebalance days members with next == cur get working orders cancelled. Cash 0%, no rebate, short proceeds stay in cash; report min C/NAV.
Stats per scenario: net Sharpe mean/sd(n-1)*sqrt252; gross Sharpe; HAC t (Bartlett lag 5); ann mean; CAGR; ann vol; max DD; calendar-year returns. Monthly one-way turnover by execution month; deployment separate; mean excluding deployment month; planned (decision-month) turnover for reconciliation; count of months <= 0.30.

## 5. Missing/ended price policy (causal)
Detection only from `present[t,i]` (known at mark t). Primary "stale-carry, K-session write-off" K=5, eta=0: (a) gap < K: hold stale, block orders; at reprint realize true cumulative return, then execute pending orders at that close; stale valuation disclosed per day. (b) K consecutive absences: write off at last mark x (1+eta) — trigger uses only past counts. (c) reprint after write-off: diagnostic `reappeared-after-write-off` event with forgone return; never touches NAV. (d) still stale at final session: included at stale mark, reported unresolved. (e) entry on absent name: unfilled, counted.
Stress S3: K=1, eta = long -0.55 / short +0.30 loss (engine causal default `DelistingPolicy::TerminalReturn`, `replay.hpp:30-37,124-129`). Never scan ahead; `TerminalReturnExPostV1` is the hindsight rule (not used).
Guarded intervals: realize adjusted return; event with exposure, r_adj, r_raw, P&L difference; summary sum h*(r_raw - r_adj) sensitivity.
Counts by run length: 1, 2-4, written off, reappeared, unresolved — each with $ exposure and P&L.

## 6. Cost scenarios (fixed in code, recorded in recipe; no cost flags)
Repo params `atx-impl/src/strategy_runner.hpp:21-27`: liquidity_window 63, full spread 10 (half 5), commission 1, borrow 300, impact_y 0.6, max_participation 0.01, NAV 1e9. `borrow_tiers.hpp:20-22` GC 27.5 bps / warm 300 / special 27.5% (needs cap + short interest; not available now).
- S1 `linear-6bps-stale5-v1`: FlatBpsCost(6), borrow 300 D365, K=5, eta=0 (registered hypothetical).
- S2 `modeled-1bn-stale5-v1` (PRIMARY): SqrtImpactCost({0.6, 0.5}, max_participation 0.01, commission 1), half_spread 5; cost = |f|*6e-4 + |f|*0.6*sigma*sqrt(|f|/ADV), |f| <= 1% ADV/session; borrow 300; K=5; eta=0.
- S3 `modeled-1bn-terminal-adverse-v1`: S2 with K=1 and adverse eta.
Liquidity row for execution t from window [t-63, t): ADV = sum_{present} raw*volume / 63 (absent = 0); sigma = sample SD of adjusted simple returns over adjacent-present unguarded pairs if >= 20 pairs else fallback 0.05/day (counted); ADV=0 -> no fill (liquidity-blocked). Only for names with active orders.
Capacity diagnostics: capped fills, unfilled $, participation p95/max, impact vs linear split, full-request cost via `unrationed_cost`.

## 7. CLI
`atx-equity-strategy-targets nav --combined PATH --combined-sha256 SHA --role PATH --role-sha256 SHA --output NEWDIR [--rule baseline-v1|monthly-budget-v2] [--cadence 5] [--trade-fraction .25] [--monthly-budget .30] [--max-bytes 536870912]`. `--role` required; `--one-way-bps`/`--annual-borrow-bps` rejected in nav mode. All three scenarios every invocation.

## 8. Outputs
- `recipe.json` schema `atx.dsl-nav-replay/v1`: SHAs, rule params, every scenario number; declarations `timing`, `accounting`, `turnover`, `missing`, `guard`, `cost_input_status: declared-unfitted-scenario-no-locate`.
- `daily_<S>.csv` per session in [d0+1, T): session_ns, exec_month, decision, rebalance, pretrade_nav, posttrade_nav, net_return, gross_return, writeoff_return, trade_cost_return, borrow_return, traded_dollars, one_way_turnover, linear_cost$, impact_cost$, capped_fills, unfilled$, blocked_absent, blocked_liquidity, fallback_vol_fills, planned_turnover, planned_forced, planned_discretionary, applied_fraction, month_planned, budget_excess, long$, short$, gross/NAV, net/NAV, held_names, stale_names, stale_long$, stale_short$, guarded_intervals, min_cash_ratio.
- `events_<S>.csv`: kind {gap-resolved, write-off, guarded, reappeared-after-write-off, unresolved-at-end}, session_ns, instrument_id, side, run_length, exposure$, r_adj, r_raw, haircut, pnl$.
- `summary.json` schema `atx.dsl-nav-replay-summary/v1`, status complete (written last): scenarios[], primary_scenario; per scenario: observations, net_sharpe, gross_sharpe, hac_t, ann_mean, cagr, ann_vol, max_drawdown, calendar_year_returns[], months[] {month, one_way_turnover, traded_dollars, execution_sessions, is_deployment_month}, mean/max monthly turnover, mean excl. deployment month, months_le_0.30, deployment {session, turnover, dollars}, planned-vs-actual reconciliation, cost totals by component, borrow, write-off P&L, guard sensitivity, missing-event histogram with $, exposure stats, capacity (capped, unfilled$, participation p95/max), meets_sharpe_target, meets_turnover_target_mean, meets_turnover_target_all_months, CSV SHAs; source_bindings; capacity_qualified false; limitations.

## 9. Memory/runtime
~36 B/cell -> TRAIN ~234 MB, validation ~164 MB; per-name state ~0.7 MB; per-day output ~1 MB; events capped 262,144 (explicit refusal beyond). Admission ~320 MiB TRAIN under 512 MiB default. Per scenario < 0.5 s at /O2; total ~3-6 s dominated by SHA-verified load.

## 10. Postimplementation fixtures (`strategy_nav_replay_test.cpp`)
1 ConstantPricesNoCost_ReproducesTargetReplayPlanned (initial_nav 1.0, 0 bps, 0 borrow: planned turnover/gross/net bit-equal to replay_targets for baseline and v2; traded$/NAV at d+1 == planned at d; NAV constant). 2 TwoNameHandComputedDriftCostBorrow (weekend 3-day borrow, 6 bps; exact NAV path; r = gross - cost - borrow). 3 InteriorGapCarriesStaleThenRealizesCumulativeReturn. 4 TerminalWriteOffAfterKIsCausal (S3 -55%/+30%; rewriting all post-write-off data leaves earlier rows bit-identical; model on `strategy_target_replay_test.cpp:226-246`). 5 FutureSignalAndPricesDoNotChangePast. 6 ParticipationCapWorkingOrderAndSqrtCost (1% ADV fills, exact SqrtImpactCost formula, ADV=0 no fill, fallback-sigma counter). 7 MonthlyTurnoverDefinitionDeploymentReconciles. 8 PinnedRunWithVolumePublishesLastAndRefusesTampering (extend `artifact()`/role helpers :73-100, :248-285 with volume.f64; exclusive output, tamper, volume_basis mismatch, budget refusal before output). 9 TerminalStressConstantsMatchBookReplay. Existing 7 target-replay cases must pass unchanged.

## 11. Turnover levers (one value each; TRAIN only; freeze before validation)
(a) soft-exit-v3: present nonmembers unwind by fraction on cadence days (repo's earlier registered `strategy_runner.cpp:958` policy). (b) band-v4: members with |desired-cur| <= 0.5/N_t keep weight; others move by f (cheapest). (c) ewma-v5: desired_s = .5 prev + .5 new, renormalized. Plumbing: enum `strategy_target_replay.hpp:11`, `validate_config` :55-56, `rule_name` :372-374, CLI :529-532.

## 12. Risks
Vendor factor unverified (guard sensitivity); common-stock status unverified; K=5 misclassification measured by histogram; declared unfitted costs (constant half-spread, Y=.6, flat 300 bps borrow, no locate); no cash interest/rebate (returns ~excess returns); v2 budget planned by decision month vs actual by execution month; working orders keep decision-NAV $ targets; NAV sim differs from target proxy (drift, drops last 2 decisions, execution-month turnover) — report both.
