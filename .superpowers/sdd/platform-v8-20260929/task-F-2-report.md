# Task F-2 report: marginal IC verb (contract K6)

Lane F, pool-7, branch `feat/platform-v8-f-20260929`. Not compiled here (lane rule 2); root builds.

## What was built

| file | role |
|---|---|
| `atx-engine/include/atx/engine/combine/marginal_rank_ic.hpp`, `atx-engine/src/combine/marginal_rank_ic.cpp` (new) | generic per-date kernel, reusable by mining (H-3 f2 term) and screening |
| `atx-engine/tests/combine/combine_marginal_rank_ic_test.cpp` (new) | kernel unit tests, suite `CombineMarginalRankIc` (8 tests) |
| `atx-impl/src/strategy_marginal_ic.{hpp,cpp}` (new) | thin verb: reads the cached payloads, the saved combined signal and the role; streams by date; writes K6 |
| `atx-impl/tests/strategy_marginal_ic_test.cpp` (new) | suite `MarginalIc` (4 tests, the 3 named in the brief plus binding refusals) |
| `atx-impl/tools/equity_strategy_ic.cpp` (lane B) | one dispatch branch: `argv[1] == "marginal"` goes to `dispatch_marginal_ic`, everything else is unchanged |

Engine interface (namespace `atx::engine::combine`):

- `centred_tied_ranks(values, eligible, out, sorted)`: the composition's centred tied rank, `(b + e - 1) / (2 (n - 1)) - 0.5`, term for term.
- `marginal_rank_ic_day(candidate, regressors (<= 11), label, min_names, scratch) -> {raw_ic, marginal_ic, names, spanned}`. Per date, it residualises the candidate on `[1, regressors]` with `residualize_signal` over the names where the candidate and every regressor are finite. The paired names are those where the residual and the label are finite. On them, `raw_ic` is the Spearman rank IC of the candidate (re-ranked on the pairs, as the runner does) and `marginal_ic` is the Pearson correlation of the residual with the label's pair ranks. A residual whose norm is at most 1e-10 of the candidate's dispersion counts as spanned, and the marginal IC is 0 for that date.
- `summarize_rank_ic(daily, lag, compact) -> {mean, hac_t, dates}`: `eval::hac::mean_inference(finite values in order, BartlettV1, lag, small-sample correction)`.
- `PairwiseRowCorrelation(K, min_names)`: the mean over dates of the per-date Pearson correlation of each pair of rows on jointly finite names.

Verb:

```
atx-equity-strategy-ic marginal --candidate-cache DIR --library L --pool ROLE_combined.json --role MANIFEST --output NEWDIR
    [--library-sha256 SHA] [--pool-sha256 SHA] [--themes WEIGHTS_JSON] [--fields DIR] [--min-names 50] [--max-memory-mib 600]
```

It writes `NEWDIR/marginal_ic.json` with schema `atx.marginal-ic/v1`. `candidates[]` holds one row per library candidate. Each row carries the K6 keys `{id, ic21, ic21_hac_t, marginal_ic21, marginal_hac_t, max_abs_rho, max_rho_member}` and also `dates, marginal_dates, spanned_dates, max_rho_signed, family, theme, book_member, sign, sign_source, payload_sha256`. Top-level blocks: `method` (every rule in words), `inputs` (every path and SHA256, theme membership), `window`, `working_bytes`, `stage_seconds`. Undefined values are JSON null.

Per date `t` in `[score_begin, score_end - 22)`:

- every candidate's centred rank is taken over the pool's member names (`<role>_combined_member.u8`);
- the regressors are the pool's saved combined signal and, with `--themes`, one theme composite per weighted theme, `T_j = sum s_k w_k r_k` (fixed denominator, NaN off members). That is at most 11 regressors plus the intercept;
- the label is the runner's h 21 research label, built from `--role`: `close[d+22]/close[d+1]-1`, both endpoints present, decision-eligible, the runner's return guard.

Values are in DSL orientation. `sign` / `sign_source` give the pinned sign (weights file) or the library `prior_sign`.

Memory:

- One row per stream is resident: K candidates, the composite and the themes.
- The role is read once, only to build the h 21 labels (`rows x names` f64), and is released before streaming.
- `marginal_ic_working_bytes` gives the admitted peak, which is checked before any payload load. For v7.1 (1,155 x 5,627, 734 rows, 48 candidates, 11 regressors) it is about 275 MiB [est].

Inputs are bound, not trusted:

- the role by the pool's `role_manifest_sha256`, and its sessions and ids equal the pool's axis payloads;
- `--themes` by the pool's `composition_weights_sha256`, and the weights file's `library_sha256` equals the pool's;
- every payload by its sidecar SHA256, verified whole before streaming;
- cache entries are v2 sidecars read in place, under `DIR/<this build's vm identity>/<role sha>/[fp_*/]` or `DIR/<role sha>/[fp_*/]`. Two entries for one candidate refuse unless `--fields` picks by field payload SHA256.

## How root verifies

1. Build: `powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests atx-engine-combine-tests atx-equity-strategy-ic`.
2. Tests: `atx-impl-strategy-ic-tests --gtest_filter=MarginalIc.*:CombineMarginalRankIc.*:StrategyIcRunner.*`, then `atx-engine-combine-tests --gtest_filter=CombineMarginalRankIc.*:CombineOrthogonalize.*`. The runner suite shows that the untouched verb still passes. The orthogonalize suite covers the new Debug `/O2` flag on `orthogonalize.cpp`.
3. Identity: no runner, composition or engine IC source changed, so the pinned `dsl_vm_sources` and `ic_result_sources` are unchanged. The tool dispatches every argv not starting with `marginal` exactly as before. The v7.1 u pass needs no re-run for identity. If root wants a spot check: `--plan-only` output and the u-pass SHAs are unchanged.
4. Step 3 of the brief (the 48 v7.1 candidates) runs through the bounded runner, 180 s / 1,536 MiB:
   ```
   build-equity/bin/atx-equity-strategy-ic.exe marginal
     --candidate-cache build-equity/mega-candidate-cache-v71
     --library atx-impl/strategies/fund_industry_ic_v71.json --library-sha256 787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259
     --pool build-equity/mega-v71w-train-ew-1/train_combined.json
     --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json
     --themes build-equity/mega-weights-v71-ew/composition_weights.json
     --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9
     --min-names 1000 --output build-equity/mega-v71-marginal-1
   ```
   `--themes` must be the file whose SHA256 the pool manifest names (`7b0a59c9...` for `mega-v71w-train-ew-1`); the verb refuses any other. The run is TRAIN 2020-2022, so it is inside the window. Estimated 30-60 s: payload verification about 2.5 GB, then 48 x 734 residualisations at `/O2`.

## Deviations from the brief, with reasons

1. **The marginal IC is linear in the residual.** It is the Pearson correlation of the residual with the label's ranks, not the Spearman correlation of a re-ranked residual. Ranking the residual lets a tiny ordered remainder of the projection set the order of every name the book explains. I checked this with a numpy replica of the verb test world: candidate = book + 1% rank noise gave a re-ranked marginal IC of 0.28 with t 62 (a false "additive" reading), against t -1.9 to -1.5 in linear form. The raw `ic21` stays a true Spearman rank IC. The residual is already in centred-rank units, so this is still a rank-based read-out.
2. **Known property, not a defect: rank-transform curvature.** Linear residualisation of rank-transformed signals leaves nonlinear dependence on the book. The replica with an 80-name book of IC 0.68 plus Gaussian noise showed a marginal bias of -0.007 to -0.016 (t -1.5 to -4). The bias shrinks with the book's own IC: at a book IC of 0.30 it was not visible (t between -1.2 and 1.9 over 6 seeds). Real member ICs are about 0.02 to 0.05. It is documented in the header.
3. **Interface additions.**
   - `--role` is required, because labels need the role's closes; the pool manifest does not carry its path.
   - `--themes` takes the weights path; a bare flag cannot locate it.
   - `--library-sha256` and `--pool-sha256` are optional pins (always recorded).
   - `--fields`, `--min-names` (default 50) and `--max-memory-mib` (default 600) are also new.
4. **HAC.** Bartlett lag 21 as briefed, on the defined dates compacted in order, which follows the `combine::marginal_ic` precedent. It is not the runner's calendar-preserving `max(2h, NW)` screen estimate, so `ic21_hac_t` is not the card's t. `ic21` and `marginal_ic21` share one support and one HAC rule, so they compare like for like.
5. **`max_abs_rho`** is taken over every other library candidate, not only weighted members, so two new candidates that duplicate each other also show. The rule is: mean daily Pearson of centred ranks, at least `min_names` joint names.
6. **The label and guard are re-implemented in the verb.** `guard_for` and the research label are private to `strategy_ic_runner.cpp`, and `ResearchIcCache` has no label accessor. Adding one would change `ic_research.hpp`, which is pinned in `ic_result_sources`. After B-3's `strategy_ic_detail.hpp` lands, the verb should call the runner's `guard_for` (dedupe item). The copy follows `ic_screen.cpp` `prepare_cache` with `require_endpoint_presence` line for line.

## Cross-lane edits

- `atx-impl/tools/equity_strategy_ic.cpp` (lane B): `#include <string_view>`, `#include "strategy_marginal_ic.hpp"`, one `if` branch.
- `atx-impl/CMakeLists.txt` (lane E / B-3): `src/strategy_marginal_ic.cpp` added after `strategy_ic_composition.cpp`, with a comment line. Not added to the `/O2` list; its own loops are light.
- `atx-impl/tests/CMakeLists.txt` (lane E): `strategy_marginal_ic_test.cpp` and `atx-engine/tests/combine/combine_marginal_rank_ic_test.cpp` added to `atx-impl-strategy-ic-tests`. The first is also globbed into `atx-impl-tests`.
- `atx-engine/CMakeLists.txt` (no lane owner): `src/combine/marginal_rank_ic.cpp` source added. In the Debug-only `/O2 /Ob2 /clang:-finline` + skip-PCH block, `src/combine/orthogonalize.cpp` and `src/combine/marginal_rank_ic.cpp` are added. Without that, `/Od` Eigen COD makes a full-role run take minutes. No accepted output uses `orthogonalize.cpp`.

## Open risks

- The code is uncompiled. I read every call site against the headers (`residualize_signal`, `hac::mean_inference`, `read_strategy_role`, `Panel`, `Result`/`ATX_TRY`, nlohmann), but a `/W4 /WX` slip is possible. The verb file keeps the wide impl line style (lines up to about 130 columns, like `strategy_ic_runner.cpp`). The engine files hold 100 columns.
- The statistical assertions depend on fixed seeds: engine world 20260929 and verb world 7106. Both were validated against an independent numpy replica: engine null t 0.42, planted t 235; verb null t -0.23, planted marginal 0.93. A logic mismatch between the C++ and the replica would show as a threshold miss, not as a crash.
- Runtime on the real role is an estimate. If the 180 s cap binds, the pairwise-rho pass (O(K^2 N) per date) is the second cost after the COD; it can be restricted to book members.
