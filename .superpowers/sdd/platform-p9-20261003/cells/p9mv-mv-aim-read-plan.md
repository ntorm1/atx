# P9-MV proposed native launch and scalar allowlist

Premeasurement proposal; no run authorized by this document. After PM rules the
exact proposal, registration precedes every measurement. Implement the reader from
this explicit allowlist and commit it before launch; no broad nested result printing.
Existing source checker stays unchanged; no test/build/probe or new rho computation.

Working directory C:/atx-wt/pool-2. Exact environment:
```text
PYTHONDONTWRITEBYTECODE=1
ATX_WAVE_COMMIT_TRAILER=Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
ATX_WAVE_COMMIT_EXCLUDE=:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache
```
Every manual/generated commit has explicit positive paths, that negative pathspec,
and the literal trailer. Actual agent GPT-6 Astra. PM final-report.md and owner PNGs
remain untouched/unstaged. No new source/tool allowance is inferred from M.

Proposed command, only after explicit release and required prelaunch source check:
```text
"C:/Program Files/Python312/python.exe" -u scripts/research_cycle.py wave run scripts/specs/p9/waves/p9mv-mv-aim.json --seal-allow 20261003=R15-SEAL-C --seal-allow 2026-10-02=SEAL-ALLOW --seal-allow 2026-10-03=R15-SEAL-D --seal-allow 2026-10-06=R15-SEAL-D --seal-allow 2026-10-17=SEAL-ALLOW
```

State then-current free RAM/disk, launch immediately with supported bounded runner;
omit --admission-wait and --host-budget-mib entirely, including zero. Inherited
runner180s/1536MiB, u300s/2560MiB, card300s/2560MiB, w300s/3584MiB, floor512MiB,
C:>=15GB. Do not raise stage caps. Preserve one actual resource/driver/permission
stop and exact command; no automatic retry or route-around. No manual phase run.

Raw console goes unread to cells/logs/p9mv-native-console.txt; outer wall/exit to
p9mv-native-wall.json. A metadata filter emits only known stage labels, receipt
path/hash/time and failed-receipt path, never raw console or verdict payload.
The native judge may compute/write its registered verdict before human reads.

## Human read order and exact selection

1. **Source/pins/void/mechanics first.** Run unchanged tools/sealsrc.py as a CLI
   with --root C:/atx-wt/pool-2 and explicit --manifest
   scripts/specs/p9/waves/p9mv-mv-aim.json (NEVER import its module). SHA remains
   fd1aa3e60bf63c0988f74fa3fc0994d4dd9523904bd715079dbf5c7ef3eea551.
   Before launch and on completion, classify every actual hit under existing exact
   predicates: A exact X-5 owner filename02; B own started_utc17; C exact standalone
   mandated exclusion token on numbered commit.log line1 `$ git add/commit`;
   D exact four03/06 owner PNG filename spans only in native numbered dirty headers
   or dirty_outside_pathspec JSON entries. OTHER/different UTC day stops even if
   its token has an allowance. M's sleeve exact-file ruling grants no exception.
   Save completion source evidence p9mv-seal-complete.txt; verify input pins,
   frozen54/CRT/six refs, cache/index/store absence, ledger prefix, generated commit
   paths/trailers, no altered date windows. Actual NAV argv must have aim-partial-v5,
   cadence1/trade-fraction.05, both print flags, target/risk-path/risk-SHA each once,
   no --rate, no forbidden construction/fit flag. Preserve any native preflight stop
   for PM; COMPOSE's combined clock+target condition is absent with H.
   From 07-verify.json select outputs.mechanics.pass and each of six rows'
   check/value/limit/pass only; save p9mv-mechanics-first.json. Failure stops reads.

2. **Existing rho only.** Bind bundle.json hash to 08-judge outputs.bundle.sha256;
   base exactly build-equity/mega-nav-p9-theme-cov-hedge-gm, final exactly
   build-equity/mega-nav-p9-mv-aim-gm. Bind bundle.nav_summ_run.engine_exe_sha256 and
   actual frozen eval to e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c.
   Select only paired.sessions, rho, one_minus_rho, degenerate, with provenance
   paths/hashes. Require integer sessions>=2, finite rho in[-1,1]; threshold boundary
   or inconsistent structure stops. R15-V2-RHO: absent both optional leaves means
   inferred FALSE under documented encoding, stable difference "not supplied by
   bundle"; supplied degenerate=true requires finite stable difference in[0,1e-6).
   Save p9mv-rho-first.txt before deciding fields. C.11 unchanged: point dSR and
   mechanics decide in degeneracy; SE/t/p na and G-B7 skipped. Never recompute rho.

3. **Deciding and common scalar prints.** Require successful first two records and
   unchanged bundle hash. From 08-judge.outputs.verdict select rule, accepted, dsr,
   checks.dsr_positive, checks.mechanics; from bundle.paired select dsr, memmel_se,
   sr, sr_reference, lw.p_value, lw.p_one_sided individually. No whole paired/lw,
   t, confidence interval or unregistered statistic. For each judge book cell/parent
   select tau_per_gross, cost_bps_traded, x4_net_sharpe individually.

4. **Registered MV prints only.** For H and final MV summary.json, select primary_scenario
   metadata and its matching scenarios[] row's ann_vol and ann_mean_gross only.
   Reader divides cell/parent printed values: ann_vol ratio<1; gross-alpha ratio
   in[.90,1.10], nondeciding. No ex-ante statistic invented. From each capacity_curve.csv
   select swap-fin primary book rows only: multiple, book, net_sharpe (capacity curve,
   including4x) and multiple1 cost_bps_per_traded_dollar. Require unique rows of the
   capacity-x<M>-v1+swap-fin-v1 book; report unavailable rather than change selection.
   Registered cost/turnover comparisons decide nothing.
   From final MV summary.json root mv_aim select EXACT leaves:
   - rule; risk_model.directory, risk_model.manifest_sha256;
   - decisions.formed, applied, pass_through.no_forecast, fewer_than_two_priced, flat;
     warm_up_decisions.formed, applied;
   - priced_names.n/mean/min/max and unpriced_members.n/mean/min/max;
   - specific_vol_ratio.pooled; specific_vol_ratio.per_decision.n/mean/min/max;
   - eigen_00_02_share.mv.n/mean/min/max and .desired.n/mean/min/max when object;
     if null, print literal null and eigen_00_02_share_unavailable.decisions/first_error;
   - special_tier_negative.locate_in_aim/name_decisions/decisions.
   Source shape verified from strategy_mv_aim.cpp:294-335; print failure may leave
   eigen share null without failing the cell. Never impute, substitute or print
   another covariance block. Provenance source paths and streaming hashes accompany
   the selected leaves. Save p9mv-registered-prints.json. Prior H CI and M covariance
   overreads remain disclosed in their records; this allowlist does not repeat them.

5. **Record then PM disposition.** Native judge alone appends one construction trial;
   verify unchanged initial137 prefix, expected138/N_c67/N_hand128/N_tot238, M110,
   P9 admissions+0 and canonical eight-rerun count0. Preserve Appendix A and exact
   identity-bridge/survivorship wording from prereg. Timing-only scoreboard uses
   wave-result.json and --timings, raw output unread; keep fresh/reused receipt sums,
   phase/stage-override/outer-wall distinctions. G-P1 unawarded. No DERIV, W/X/V or
   adoption work until PM rules MV. Record any absent registered producer; no rerun.

Pending: PM exact closure/delta/manifest/source/read release, registration and the
implementation of this scalar filter before measurement. There is no new source
checker change and its existing 37 controls are not rerun.
