# Task W2 report: DSL ops for the literature families (A7)
Branch feat/platform-v7-w2-dslops-20260928 (pool-10), base 5b958dd2, HEAD c692d93a (0a01c19c golden test = capture commit; d3221a30 ops + checker; c692d93a tests). NOT built or run (rules). Semantics version 1; every pre-W2 opcode id frozen by static_assert. New ops live in `detail::literature_ops()`, not `builtin_ops()` (still 74 rows), so factory op-swap / wrapper tables are unchanged.

| id | op (signature) | NaN rule (pinned spec: lit_ops.hpp) |
|---|---|---|
| 89 | pack2(a,b) / pack3(a,b,c) -> record | f64 vectors only; usable as a regressor argument or via `.p0..p2`; a record root / record operand elsewhere is refused |
| 90 | ts_topk_mean(x,w,k), k in [1,w] | full window: NaN if t+1<w or any NaN in it; mean of the k largest |
| 91 | bucket(x,n) -> Group, n in [2,65535] | floor(avg-rank pct * n) clamped; ties share a bucket; NaN / out-of-universe -> no group (NaN); == quantile(x,n)*(n-1) |
| 92 | group_cross(g1,g2) -> Group | g1*2^26+g2; NaN unless both are integers in [0,2^26) |
| 93 | ts_resid_on(y,x1[,x2[,x3]],w), w>=k+2 | OLS+intercept, full window; any non-finite or short -> NaN; flat regressor / collinear (pivot <= 1e-12*diag) -> NaN; flat y -> 0; residual at t |
| 94 | ts_beta_on(y,x1[,x2[,x3]],w) | as 93; returns the slope on x1 |
| 95 | cs_resid_on(x,c1[..c4]) | per date, rows = universe & x & all c finite; n<k+2 / flat / collinear -> whole date NaN; excluded rows NaN |
| 96 | ts_count_increases(x,w) | NaN if t+1<w or x[t] NaN; walk back from t: x>0 counts, x==0 skipped, x<0 or NaN stops |
| 97-103 | ts_sum_mp, ts_mean_mp, ts_std_mp, ts_zscore_mp, ts_min_mp, ts_max_mp, decay_linear_mp (x,w,m), m in [1,w] | pandas min_periods over finite cells of the partial window; NaN if count < m (std/zscore: max(m,2)); std flat -> 0; zscore needs x[t], flat -> NaN; decay weight w-age |
| 104 | ts_corr_mp(x,y,w,m) | Pearson over pairwise-finite cells; NaN if pairs < max(m,2) or a side is flat |

**Files:** registry.hpp:211-226 (enum), :297 (kMaxDefaults 2->3), :464-547 (lit helpers); registry.cpp:177 (literature_ops), :216 (Library); typecheck.cpp:420/458/498 (rails, analyze_lit_call); parser.cpp:55 (pack_regressors); dag.cpp:77 (param); bytecode.cpp:91; lit_ops.hpp (lit_ols:110 topk:199 nincr:218 mp:296 corr:338 ts_regress:374 bucket:443 cs_resid:465 group_cross:509); vm.hpp:1329/1351/1402 (eval_lit_map/cs/ts); oracle_lit.cpp:378 (independent twins); streaming_engine.hpp:410/551/580/620; check_fund_ic_v6.py:35 (W2_OPS), :119 (Group-builder rule).

**Tests (atx-engine-alpha-tests):** alpha_lit_golden_test.cpp `AlphaLitGolden_ExistingOps.{EveryPreW2OpcodeDigestIsPinned, BatteryCoversEveryPreW2NamedOp}`; alpha_lit_ops_test.cpp `AlphaLitOps_Differential.{EveryNewOpVmMatchesOracleCellForCell, SharedPackProgramMatchesOracleAndEveryVmPath}` (fused, SubtreeCache, global-DAG == plain), `AlphaLitOps_Streaming.WarmThenStepMatchesBatchBits{AuditExact,ResearchFast}`, analytic `AlphaLitOps_{TopkMean,CountIncreases,MinPeriods(x2),TsRegression(x2),CsRegression,Bucket,GroupCross}`, `AlphaLitOps_Typecheck.{GroupWhereVectorRequiredIsRefused, VectorWhereGroupRequiredIsRefused, RecordsAndCountRailsAreRefused, WellTypedNestingsAreAcceptedWithLookback}`, `AlphaLitOps_{Parser,Linearizer,Registry,Families}`. Pytest atx-impl/strategies/test_check_fund_ic_v6_ops.py: a synthetic library using all 17 names passes; unknown ops, denied ops and a group builder without a group op are refused.

**Golden (PINNED 41894ea4, rebased on pool-2 96f3ed96):** root captured on the merged W2 HEAD (build v7-w2, AlphaLitOps 22/22): vm_audit_exact=0x971c3da60ca89aa3, vm_research_fast=0xe9e7128fd5359900, oracle=0x971c3da60ca89aa3, now pinned at alpha_lit_golden_test.cpp:53-56. Root is also checking the v6.1 IC pass for byte identity.

**Design:** (1) The IR has 3 operand slots, so the parser folds surplus regressors into pack2/pack3 records (ArgPack, contiguous block); the consumer's Instr.param = width(b) | width(c)<<8; VM, oracle, streaming, global-DAG remap and subtree hash all read blocks via the base slot; unparse prints the pack and re-parses to the identical Ast. (2) Min-periods: separate `*_mp` ops, the smaller change (no existing kernel, streaming class, fusion rule or arity/default touched). (3) Named `cs_resid_on` because `cs_residualize(x,g[,z])` already means group-dummy FWL with a Group 2nd arg. (4) ts_count_increases counts an event/change series; nincr builds its events in DSL. (5) Regressions cost O(w*k^2) per cell (no rolling update); FF3 at w=252 is the costliest op, so time it in root's IC pass.

**Family DSLs** (NOT in any library; all five compile and match VM==oracle in `AlphaLitOps_Families`; r := `close / delay(close, 1) - 1`, written inline in the real strings; placeholder fields needing producers: ff_mkt/ff_smb/ff_hml daily FF factors, ni_q quarterly NI ffill, tobin_q, cop_at, droe, ia = I/A):
- MAX5-SMAX (lb 21): `rank(-1 * ts_topk_mean(r, 21, 5) / stddev(r, 21))`
- BAC quintile (lb 252): `group_rank(-1 * correlation(r, ff_mkt, 252), bucket(stddev(r, 252), 5))`
- FF3 residual momentum (lb 503): `rank(ts_sum(delay(ts_resid_on(r, ff_mkt, ff_smb, ff_hml, 252), 21), 231) / stddev(delay(ts_resid_on(r, ff_mkt, ff_smb, ff_hml, 252), 21), 231))`
- nincr (lb 797): `rank((X > 8) ? 8 : X)`, X = `ts_count_increases((ni_q != delay(ni_q, 1)) ? ((ni_q > delay(ni_q, 252)) ? 1 : -1) : 0, 546)`
- q5 Eg (lb 503): `rank(ts_mean_mp(b_logq, 252, 63) * log(tobin_q) + ts_mean_mp(b_cop, 252, 63) * cop_at + ts_mean_mp(b_droe, 252, 63) * droe)`, FWL slope b_A = `vec_sum(cs_resid_on(delay(A, 252) + 0 * g, delay(B, 252), delay(C, 252)) * g) / vec_sum(power(<same resid>, 2))`, g = `ia - delay(ia, 252)`; full 1.3 kB string in the test.

**Open for root:**
- q5 needs `vec_sum`. DONE (59831d9e): root policy admits it as an explicit `POLICY_OPS` entry citing q5, with a pytest.
- The IC runner refuses lookback > score_begin-63; check this for nincr (797) and FF3 / q5 (503).
