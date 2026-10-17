# P9-M native launch and scalar read plan

Committed before M execution under R15-M-READY / R15-M-LAUNCH. Registration a11bfb9d;
exact inputs/delta and prior blobs are in p9m-registration-commit.txt and p9m-prerequisite-closure.md.
R15-M-PREPSCOPE adjudicates the inherited H covariance/shares exposure; it affects no fixed choice.

From C:/atx-wt/pool-2, exact environment:
PYTHONDONTWRITEBYTECODE=1
ATX_WAVE_COMMIT_TRAILER=Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
ATX_WAVE_COMMIT_EXCLUDE=:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache

Exact native command:
```text
"C:/Program Files/Python312/python.exe" -u scripts/research_cycle.py wave run scripts/specs/p9/waves/p9m-mom-volman.json --seal-allow 20261003=R15-SEAL-C --seal-allow 2026-10-02=SEAL-ALLOW --seal-allow 2026-10-03=R15-SEAL-D --seal-allow 2026-10-06=R15-SEAL-D --seal-allow 2026-10-17=SEAL-ALLOW
```

State free RAM/C: first, C:>=15GB; launch immediately, registered inherited caps, w300s/3584MiB,
floor512MiB; no admission-wait/host-budget flag, RAM polling, parallel real run or automatic retry.
Preserve raw stdout/stderr unread at cells/logs/p9m-native-console.txt and outer elapsed/exit at
cells/logs/p9m-native-wall.json. Metadata progress uses p9m_read_filter.py metadata only.
A driver/resource/permission stop is preserved and handed back, never silently retried.

1. On completion run unchanged tools/sealsrc.py --root C:/atx-wt/pool-2 --manifest
   scripts/specs/p9/waves/p9m-mom-volman.json; save all per-hit paths/tokens/classes.
   A/B/C/D predicates and exact allowed source/token pairs are those in the preparation record.
   OTHER stops even if token is allowed; different UTC date stops. No checker test/rerun of H.
   Verify frozen exes/CRT/reference listings, fields and parent pins, cache/index/store, generated
   commit one-file scopes/trailers, inputs and ledger135-prefix preservation. Verify generated spec
   inherits both print flags once, flag-absent fit, no cache-index/timing/data-window changes.
2. Only p9m_read_filter.py mechanics exposes six verify-receipt mechanics rows; any failure stops.
   Save cells/logs/p9m-mechanics-first.json before paired fields.
3. Only p9m_read_filter.py rho reads existing bundle.json after hash binding to08-judge,
   frozen eval e2b48c46...d962c, and exact pair:
   base build-equity/mega-nav-p9-theme-cov-hedge-gm;
   final build-equity/mega-nav-p9-mom-volman-gm.
   Save cells/logs/p9m-rho-first.txt and record before any later scalar.
   It prints sessions/rho/one_minus_rho/degenerate only. R15-V2-RHO missing-both encoding means
   inferred FALSE threshold flag and numeric stable difference not supplied. Inconsistency or
   boundary concern stops. C.11 fallback unchanged; no extra engine for rho.
4. p9m_read_filter.py prints selects pm7-34 accepted/dSR and its two deciding booleans;
   paired dsr/memmel_se/sr/sr_reference; lw.p_value and lw.p_one_sided individually;
   book cell/parent tau_per_gross and x4_net_sharpe only. No confidence interval or whole
   nested stats block. Save cells/logs/p9m-registered-prints.json.
   p9m_read_filter.py traces selects only role, rule/theme, each block's from_session/sigma/target/scale
   from build-equity/mega-p9w-train-mom-volman-1/summary.json /roles/*/composition_schedule_trace.
   Save cells/logs/p9m-schedule-traces.json. Expected effect +.01 to +.03 [est], nondeciding.
   No H-only gross-alpha/vol-ratio/cost print or criterion.
5. Registered sleeve print (R15-M-LAUNCH) uses exactly the following command form for each
   native-produced role pair, role=train or validation, under the same interpreter:
```text
scripts/run_bounded_research.py --root C:/atx-wt/pool-2 --output build-equity/p9m-sleeve-<role>-run1 --seconds 300 --max-rss-mib 2048 --min-free-mib 512 --build-type Release --bind build-equity/mega-p9w-train-mom-volman-1/<role>_mom_volman_sleeve_raw.csv --bind build-equity/mega-p9w-train-mom-volman-1/<role>_mom_volman_sleeve_scaled.csv -- build-equity-rel/bin/atx-research-eval.exe --vol --daily raw=build-equity/mega-p9w-train-mom-volman-1/<role>_mom_volman_sleeve_raw.csv --daily scaled=build-equity/mega-p9w-train-mom-volman-1/<role>_mom_volman_sleeve_scaled.csv --json --out build-equity/p9m-sleeve-<role>.json
```
   Thus exact permitted inputs are train_mom_volman_sleeve_{raw,scaled}.csv and
   validation_mom_volman_sleeve_{raw,scaled}.csv in that w directory. Outputs:
   build-equity/p9m-sleeve-train.json / p9m-sleeve-validation.json, respective -run1 receipts.
   Only the train role is configured by the inherited TRAIN-only IC argv; disclose any absent
   role pair. Both files absent means no command for that role; one absent is a stop.
   Save raw runner console unread at cells/logs/p9m-sleeve-<role>-console.txt.
   Check receipt producer/pins/outcome/source first, then p9m_read_filter.py sleeve OUTPUT selects
   /cells/*/{label,path,net_vol.vol_annual,net_vol.sharpe_annual} only into
   cells/logs/p9m-sleeve-<role>-prints.json. No CSV returns opened by agent/Python, no imputation.
   Full-TRAIN fit sleeve, zero outside TRAIN, only recorded block overlap; in-sample, not OOS.
6. Native judge alone writes one construction trial. Record prefix135 unchanged, expected ledger136,
   N_c65, no new admissions, Appendix A and identity-bridge flag. No manual ledger write/DERIV.
   Timing-only scoreboard uses explicit --results build-equity/waves/p9m-mom-volman/wave-result.json
   --ledger build-equity/trials.jsonl --timings, raw output unread. Stage_seconds, receipt phase sum,
   reused receipts, overlap and invocation wall stay distinct; G-P1 remains unawarded.
   Hash outputs/receipts and complete record/handoff; stop for PM disposition before any next cell.

The filter is postprocessing only. The registration helper's first inline invocation had a Python
quote syntax error before execution/mutation; the file-based exact helper then succeeded.
No scientific refusal or permission refusal occurred in preparation.
