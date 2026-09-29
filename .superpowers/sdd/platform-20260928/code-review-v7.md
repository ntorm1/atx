# Platform + production code review v7 (task P1)

Reviewer: Opus 5.5 (P1, read-only), 2026-09-28. Tree `C:/atx-wt/pool-2` @ `b4ebb30c` (+ brief commit `e5fb3f40`).
Nothing edited, built or run. Evidence:
- code reading;
- 212 bounded-runner receipts (`build-equity/*-run*/receipt.json`), IC-runner `summary.json` stage timers, and the build
  receipts `mega-*-receipt.json`;
- file mtimes and the CMake cache;
- one mechanical count over the v6.1 cell's TRAIN S2 daily CSV (`neutralize` outcome column).

Nothing dated 2023 or later, and nothing named validation/VAL, was opened. This review extends `v6-code-review-exec.md`
(F1-F11, C1-C5) and `v6-code-review-signal.md` (I1-I5, m1-m7) without repeating them.
- Effort: S < 1 day, M 1-3 days, L > 3 days (one implementer).
- Value: H = at least halves a cycle, or blocks production; M = one stage or one risk; L = hygiene.

**Headline**
1. **The ladder is the bottleneck, not compute.** The whole v6.1 real-data ladder (fields → u → fit → w → nav) ran in
   ≈ 4.2 min (receipts 02:27-02:31 UTC). The cycle around it took ≈ 90 min (ledger: START ~01:50, PROMOTED ~03:20).
   Most of the rest was per-version engineering: generate_fund_ic_v61.py (308 lines), its test (128) and
   v61_train.sh (169 lines, 10 hand-pinned SHAs). There are 9 such ladder scripts, 1,248 lines in total.
2. **"The IC pass needs 3-4 resumable passes" is out of date.** That was v3: 4 × 180 s time-limits on 121 candidates,
   before T15. Since v4 every u pass has completed in one bounded pass (26-122 s, ≤ 1,112 MiB).
3. **Adding one alpha costs a full cold pass.**
   - 35 of the 39 v6.1 candidates are cache-keyed by the whole fields-manifest SHA.
   - Appending `sv_ratio126` changed that SHA although 40 of 41 payloads were byte-identical, so the pass got 0/39 hits.
4. **SHA-256 takes 39% of a cold IC pass and 61% of a weighted pass.** It is scalar (~220 MB/s) and re-hashes the same
   bytes 2-3×.
5. **The research executables are Debug builds**; only 7 TUs are compiled /O2.
6. **There is no path from the frozen cell to today's portfolio.** The hard parts already exist in atx-engine: factor
   risk model, certified QP, GP aim, a bit-exact streaming VM, decay monitor, and a corporate-action dataset. What is
   missing is glue: per-name state out of the replay, a decide-at-last-row verb, a deploy manifest, and
   locate/order/reconciliation files.

## S1. Executive ranking (value / effort, best first)

| # | id | finding | axis | value | effort | files |
|---|---|---|---|---|---|---|
| 1 | A2 | SHA-256 bound: 30 of 76 s (u), 19 of 31 s (w); the w pass verifies 1.9 GB of fields it never loads | A | H | S | atx-core/src/sha256.cpp:157-180; strategy_ic_runner.cpp:1240-1254,1718 |
| 2 | A1 | Coarse cache keys: one new field invalidates 35/39 candidates; a changed DSL under the same id is a hard refusal, so each library gets a new 1.9 GB cache | A | H | S-M | strategy_ic_runner.cpp:987-1075; fit_composition_weights.py:938-951 |
| 3 | B3 | NAV replay emits no per-name state (targets, holdings, fills) | B | H | S-M | strategy_nav_replay.cpp:845-866,1436-1511 |
| 4 | A4 | No research-cycle driver: 9 hand-copied ladder scripts, hand pins, exit codes unchecked; generator + test + library + recipe per version | A | H | M | studies/v*_train.sh; strategies/generate_fund_ic_v*.py |
| 5 | A5 | Research exes are Debug (Debug CRT, checked iterators; /O2 on only 7 TUs) | A | M | S (root) | build-equity/CMakeCache.txt:115; atx-impl/CMakeLists.txt:68-80 |
| 6 | B1 | No deploy manifest; the 2025 seal is hard-coded in C++ and Python; no owner-gated live path | B | H | S-M | strategy_data.cpp:21; prepare_recent_research.py:48,164,365,698 |
| 7 | B2 | No daily decide path: the replay never decides on its last 2 rows; positions exist only inside a replay from 2020 | B | H | L | strategy_nav_replay.cpp:908; strategy_target_replay.cpp:222-282 |
| 8 | A3 | Fields store rebuilds all 41 fields (2.0 GB) to add one; C: is 93% full, build-equity 52 GB | A | M | M | prepare_research_fields.py; v61_train.sh do_fields |
| 9 | B6 | Locates are modeled tiers; the construction already accepts a per-name no_short mask | B | M | S | strategy_nav_replay.cpp:937-942; strategy_target_replay.cpp:366-372 |
| 10 | C1 | Python fitter re-implements the C++ exposures/neutraliser (QR vs Cholesky); centered ranks exist 3× | C | M | M | fit_composition_weights.py:537-676,755; strategy_ic_composition.cpp:26-35; strategy_target_replay.cpp:183-205 |
| 11 | A7 | DSL gaps block cited families: no top-k, no Group-typed bucket/cross, slope-only ts_regression, 1-covariate cs_residualize, full-window NaN only | A | M | M | registry.cpp:107-109; cs_ops.hpp:425-433; ts_ops.hpp:25-26 |
| 12 | B9 | No live health checks vs TRAIN bands; a neutralisation skip silently skips the rebalance (never hit on TRAIN); decay_monitor unused | B | M | M | strategy_target_replay.cpp:360-402; book/decay_monitor.hpp |
| 13 | C3 | No determinism canary for build-flag changes (Release, AVX2/FMA m7) | C | M | S | (root; build tags) |
| 14 | B5 | Factor risk model + cost-aware QP + GP aim exist in atx-engine/risk; the book uses aim-partial + a 4-column OLS | B | H | L | risk/optimizer.hpp:111; risk/factor_model.hpp; equity_allocation.hpp |
| 15 | A6 | Whole-panel memory grows with dates: pre-2020 TRAIN (U2) needs ≈ 4 GiB vs the 1,536 MiB rule | A | H (if U2) | L | strategy_ic_runner.cpp:611-651; streaming_engine.hpp:26-33 |
| 16 | B7/B8 | No order file (shares, lots, min notional); no broker-position reconciliation or split handling | B | M-H | M each | (new) |
| 17 | B4 | Roles are frozen projections of a parquet in a Downloads folder; no daily append | B | H | L (atx-db) | v61_train.sh:30; role manifest |
| 18 | A8 | NAV rebuilds exposures from 253 sessions per decision; the 5 books run on one thread | A | L-M | M | strategy_price_exposures.cpp:107-146; strategy_nav_replay.cpp:896-960 |
| 19 | C2 | Scripts keep going after a refused run (`set -uo`, exit_code only grepped) | C | M | S (in A4) | v61_train.sh:8,88,103,144,157 |
| 20 | C4 | Stale configure-time provenance baked into cache sidecars | C | L | S | atx-impl/CMakeLists.txt:116-117; strategy_ic_runner.cpp:1200,1619 |
| 21 | C5-C7 | Parked cnms_to_si; recipe keys renamed without a schema bump; no full-stack gtest | C | L | S each | prepare_research_fields.py:1906-1910; strategy_nav_replay_test.cpp |

## S2. Findings in detail

### Axis A: research iteration speed

**A2 SHA-256 dominates IC-runner passes (H, S).**
- *Evidence* (`summary.json` → `roles[0].stage_seconds`):
  - v6.1 u pass (75.9 s): 29.9 s of hashing.
    - `fields_verify` 8.5 s: 36 fields, 1.87 GB, ≈ 220 MB/s.
    - `fields_load` 12.0 s: 2.44 GB, hashed again as it lands (strategy_ic_runner.cpp:1240-1242).
    - `cache_write` 9.4 s: payload SHA streamed.
  - v6.1 w pass (31.3 s, `loaded_bytes` 0): 19.2 s of hashing.
    - `fields_verify` still 9.2 s: `verify_fields` (:1243-1254, called at :1718) hashes every *referenced* field,
      loaded or not.
    - `cache_load` 10.0 s re-verifies payload SHAs.
  - All five w passes (29.8-45.5 s) show the same pattern.
  - `sha256_file` is scalar over a 64 KiB ifstream (atx-core/src/sha256.cpp:157-180).
- *Change* (digests unchanged):
  - SHA-NI path with CPUID dispatch and scalar fallback, or OpenSSL EVP (libcrypto-3 already ships in build-equity/bin
    via arrow). Typically 1.5-2 GB/s where the CPU supports it.
  - Verify only the fields the FieldPlan loads (:182-188), and hash once (verify while loading).
- *Effect (est.):* u 76 → ≈ 50 s; w 31 → ≈ 12 s; every pass of every cycle.

**A1 Cache keys are too coarse (H, S-M).**
- *Evidence:*
  - Signal cache path is `ROOT/<role-sha | fields-manifest-sha>/<id>.{f64,json}` (strategy_ic_runner.cpp:987-1010).
  - Any candidate reading an extra field is keyed by the whole fields manifest (:1005-1008): 35 of 39 in v6.1 (only
    mom_12_1, high_52w, smax, seasonality_same_month are exempt).
  - fields-v7 = v6b + 1 field, and v61_train.sh do_fields asserts the other 40 payloads are byte-identical. Yet
    `mega-v61-train-u-1` shows hits 0, misses 39, vm_evaluations 39.
  - A changed DSL under an unchanged id is refused, not missed (:1069-1074); v6l u-run1..3 died on `bm`. Result: a
    1.9 GB cache per library (-v6, -v6u, -v61) plus a 20 GB legacy cache.
  - The fitter WorkStore is already payload-SHA keyed (fit_composition_weights.py:938-951), but it appends the
    fields-manifest SHA and gets a fresh dir per script (v61_train.sh:35). v6.1 fit: computed 39, reused 0.
- *Change:*
  - Key field candidates by the sorted (field, payload sha) pairs they read; the manifest `files` block already
    holds them. Record the pairs in the sidecar as `field_payload_sha256`.
  - Name payloads `<id>.<dsl_sha16>`, so a changed DSL is a clean miss.
  - One shared cache root and one fit work root per role; the old layout stays readable.
- *Effect (est.):* a new DSL alpha costs 1 VM eval (0.4-3.5 s measured) plus fixed overhead. u ≈ 30 s now, 12-15 s
  with A2. fit 21 → ≈ 5 s (context reused per role).

**A4 No research-cycle driver (H, M).**
- *Evidence:*
  - 9 ladder scripts (1,248 lines) hand-pin 7-14 SHAs each. Every version also adds a generator module + test and a
    library + recipe JSON.
  - Failure classes seen: a field-list omission (grp_ff49 → v6u u-run1..4 refused); a wrong REFN fallback (branch
    m2); exit codes only printed (`| grep -E '"exit_code"' | head -1` at v61_train.sh:88,103,144,157) under
    `set -uo pipefail`, which has no `-e` (:8).
- *Change:* `scripts/research_cycle.py` driven by a cycle-spec JSON. The spec holds the parent cell, the library as
  base pin + delta (id, dsl, theme, prior, citation), and the role pin, fields list, composition, construction flags
  and DSR-N rule. Verbs:
  - `plan`: resolve every pin; print the trial delta.
  - `run`: fields-reuse → u → fit → generic P1 gate (from v61_train.sh:110-134) → w → nav → summ paired vs the parent.
    Bounded runner; never overwrites; resumes; stops on any non-zero receipt; appends the Appendix A line
    (trial_ledger.hpp already has a hash-chained ledger).
  - `add-alpha`: write the delta plus a prereg stub.
- One-command add-alpha-and-score = this driver + A1 + A2 + A3 + delta materialisation.
- *Target:* ≤ 2 min compute and zero hand pins per cycle.

**A5 Debug research binaries (M, S, root).**
- *Evidence:*
  - build-equity/CMakeCache.txt:115 `CMAKE_BUILD_TYPE=Debug`, flags `/Ob0 /Od /RTC1` (:135).
  - /O2 only on 5 strategy TUs (atx-impl/CMakeLists.txt:68-80), ic_screen.cpp (atx-engine/CMakeLists.txt:189-193)
    and sha256.cpp. Those keep the Debug CRT and checked iterators; I/O and strategy_data.cpp stay /Od.
  - build-equity-rel/bin has no strategy exes.
- *Change:* build both exes with equity-rel (SSE2, no /fp:fast; branch review m7 says byte-identity holds there), then
  A/B once on the v6.1 u/w/nav by SHA (`train_combined.*`, daily CSVs).
- *Effect:* unmeasured; one A/B run decides.

**A3 Monolithic fields builds (M, M).**
- *Evidence:*
  - Each fields dir is 41 × 49.6 MiB f64 = 2.0 GB.
  - v7 rewrote 40 unchanged fields: ≈ 75 s from first file to manifest, vs 40.6 s for the 40-field part alone
    (V61 report §5).
  - build-equity is 52 GB; C: is at 93% with 33 GB free.
- *Change:* `prepare_research_fields.py --reuse <manifest> --reuse-sha256 <pin>` hardlinks fields whose recipe, code
  and source SHAs match, copies their manifest rows, builds only the rest, and records `reused_from`.
- *Effect:* a DSL-only alpha skips the fields step; a new field costs only itself (sv ≈ 30-45 s).

**A6 Whole-panel memory grows with dates (H if U2, L).**
- *Evidence:*
  - `admit()` (strategy_ic_runner.cpp:611-651) charges cells × (72 + 8·slots) + cells × (8·field_capacity + 1) + labels.
  - At v6.1 (6.50 M cells, 7 slots, capacity 5) that admits 1,449 MB: VM/role 832 MB (57%), resident fields 267 MB
    (18%), labels 196 MB, workers 59 MB, composition ≈ 59 MB, fixed 37 MB. Peak observed: 1,061-1,112 MiB.
  - A 2010-2022 role (~3,400 dates) needs ≈ 4 GiB, which the 1,536 MiB rule refuses. The shape cap refuses more than
    4,096 dates outright (:616).
  - The 5-field budget and the 314-bar lookback bound are the same limit (task-V6L-report.md:107-116; XFIN and
    EV/EBIT candidates were blocked).
- *Change:*
  - Evaluate each candidate in date blocks with a lookback halo (256 + ≤ 336 rows ≈ 0.5 GB, independent of history).
  - Stream labels and composition by date.
  - StreamingEngine is already bit-exact to the batch VM (streaming_engine.hpp:26-33) and can carry state across
    blocks.
- *Effect:* U2 becomes runnable, the field budget goes away, and candidates can run in parallel (16 logical cores,
  16 GB).

**A7 DSL gaps vs cited families (M, M).** 72 registered functions (registry.cpp); gaps:
- No top-k order statistic, so SMAX is MAX1, not MAX5 (task-V6L-report.md:116,291).
- `quantile` returns F64, while group ops need Group keys: BAC vol-quintile conditioning is inexpressible (:287-290),
  and there is no industry × size cross.
- `ts_regression` is slope-only (registry.cpp:107-109). res_mom_12_1 is hand-built from 4 correlation/stddev/ts_sum
  calls and cannot be the BHM FF3 residual (D12).
- `cs_residualize` takes 1 group + ≤ 1 covariate (cs_ops.hpp:425-433).
- NaN policy is full-window only (ts_ops.hpp:25-26), which creates 252+21-session blackouts (signal m2).
- `ear` builds its event window with a NaN hack (`+ 0*log(earn_recent*delay(earn_recent,1))`, then
  `ts_backfill(…,126)`); `trade_when` exists but is unused.

*Change:* `ts_topk_mean(x,k,d)`, `bucket(x,n)` → Group, `group_cross(g1,g2)`, `ts_resid_on(y,x1[,x2,x3],d)`,
`cs_residualize` with ≤ 4 covariates, and min-periods variants. Each needs an oracle twin, VM kernel, StreamingEngine
state and differential test. New ops leave existing bits unchanged: `dsl_vm_semantics_version` stays 1 (:66) and only
the source pin (strategy_ic_runner.cpp:106-107) is reset.

**A8 NAV throughput (L-M, M).**
- *Evidence:*
  - NAV cells take 27.7-73.0 s at 339-390 MiB.
  - Every decision re-reads 253 sessions × 5,627 names, 2 logs each (strategy_price_exposures.cpp:107-146; exec F9
    still open).
  - The 5 books run on one thread although only construction is shared (strategy_nav_replay.cpp:896-960).
- *Change:* a session-log ring buffer; books on a DetPool after the shared decision; later, construction variants in
  lockstep so a θ/dust grid is one run.
- Not a finding: build time. Incremental mega-build takes 17.6-47.8 s (1-8 TUs, 3-4 jobs by free RAM,
  mega-build.ps1:12-13); wide rebuilds take 171 s (52 TUs) and 269 s (122).

### Axis B: production readiness of the daily path (inputs → broker)

**B1 Deploy manifest + seal policy (H, S-M).**
- *Evidence:* pins are spread over handoff §3, recipe.json, summary.json and a shell script. The research seal is
  hard-coded in strategy_data.cpp:21 (`kSeal`, exclusive 2025-01-01), prepare_recent_research.py:48,164,365,698 and
  `prf.Role`. Every live session is ≥ 2025.
- *Change:*
  - `atx.book-deploy/v1` JSON binding library, recipe, orientations, weights, fields list/recipe, universe rule, NAV
    flags, L, exe SHAs and source SHAs.
  - A `live` role class admitted past the seal only with that manifest and an owner-gate record.
  - The live path emits positions and orders only, never returns-conditioned statistics, so the 2025+ holdout policy
    stands until the owner rules on live P&L.

**B2 Decide verb (H, L).**
- *Evidence:* the replay's last decision is end−3 (strategy_nav_replay.cpp:908, `decision = t + 2 < end`).
  aim-partial is path-dependent through `current[i]` (strategy_target_replay.cpp:222-282), so today's target needs
  today's actual positions, not a replay from 2020.
- *Change:* `atx-equity-strategy-targets decide --deploy <m> --asof <s> --positions <csv> --locates <csv>`:
  1. signals for the as-of row only: batch VM on the trailing max-lookback window (≤ 336 rows × 5,627 ≈ 0.3 GB), or
     StreamingEngine;
  2. IcComposition with the pinned weights and signs;
  3. `detail::form_desired` + `update_weights`, the same functions the NAV calls;
  4. write `targets.csv` (name, current, desired, aim, next, reason) and `orders.csv`.
- *Acceptance:* bit-parity with the replay's planned weights at pinned TRAIN decisions (needs B3).

**B3 Per-name output (H, S-M).**
- *Evidence:* daily CSVs carry about 70 aggregate columns; events CSVs carry only stale/write-off events
  (strategy_nav_replay.cpp:1436-1511). The exec review (§6) could not measure forced-exit dollars because of this.
- *Change:* `--emit-holdings` on the primary book: one row per decision × instrument with non-zero state (desired,
  aim, planned, held $, order $, filled $, capped/blocked, tier, cost $), ≈ 1.4 M rows. Flag off leaves bytes unchanged.
- *Enables:* B2 parity, B8 reconciliation, attribution, ex-ante vs realised risk, and D9/F2 measurement.

**B4 Live data (H, L; atx-db).**
- *Evidence:* roles are frozen projections: 1,155 dates, warm-up from 2018-06-01, score to 2022-12-31. The source is
  `C:/Users/natha/Downloads/TickerHistory3.parquet` (v61_train.sh:30); FINRA raw sits in another session's tree
  (C:/atx/atx-db/data/raw).
- *Change:* a daily append landing under the data-request §0 contract, with per-day manifests and incremental fields
  (A3 + StreamingEngine).

**B5 Risk model / optimiser (H, L; a pre-registered construction trial).**
- *What exists:* atx-engine/risk has:
  - FactorModel: styles + sector dummies, WLS, EWMA F, specific risk, eigen adjust (risk/README.md).
  - PortfolioOptimizer: α'w − λw'Vw − κ‖w−w_prev‖₁ with gross/net/name/beta/sector/turnover/participation
    constraints, ADMM with certificates (optimizer.hpp:111).
  - garleanu_pedersen.hpp, multi-horizon MPC, and the certified QP book in equity_allocation.hpp (used by
    stage_equity_book).
  - None of it is used by the mega path, which runs aim-partial-v5 + price-risk-v1 (a 4-column OLS).
- *Change:* `--rule qp-factor-v1` in `update_weights`:
  - V from the existing exposures (beta252, vol63, ladv63, momentum, FF12);
  - κ from the S2 cost model;
  - participation caps via CapacityRef, plus a name cap;
  - daily ex-ante vol reported.
- This is exec lever 11. It needs B3 to validate ex-ante against realised risk.

**B6 Borrow/locate (M, S once data exists).**
- *Evidence:* swap-fin-v1 tiers are modeled; the special tier is a flat 500 bps guess.
- *Change:* the construction already takes a `no_short` mask (strategy_nav_replay.cpp:937-942 → form_desired
  :366-372). Ingest the prime broker's ETB/HTB file into that mask plus a per-name fee. A missing file means refuse
  to decide.

**B7 Orders (M-H, M).**
- *Evidence:* dollars only: no shares, lots, minimum notional (exec F10) or order-type tag.
- *Change:* `orders.csv` with instrument, side, shares = round(Δ$ / close), notional, MOC/VWAP style and reason.
  Refuse names without a valid close. Hand-off is a file drop; there is no broker API.

**B8 Reconciliation + corporate actions (M-H, M).**
- *Evidence:* neither exists.
- *Change:* compare expected positions (B3 state + fills + drift) with broker positions and write a breaks report.
  Handle splits and mergers through data/corporate_actions.hpp and book/security_transition.hpp, which already exist.

**B9 Monitoring (M, M).**
- *TRAIN bands* (v6.1 daily CSV, measured here): neutralize applied on 754/754 rebalances; amplification max 1.31;
  excluded share max .0082; held names mean 1,848; fills 1,659/day; missing-predictor shorts max 70.
- *Silent failure:* a skip or refusal makes form_desired return false, and the rebalance is skipped
  (strategy_target_replay.cpp:385-402). This never happened on TRAIN, so it is untested on real data.
- *Change:*
  - `daily_health.json` with TRAIN p1/p99 bands and per-candidate coverage vs TRAIN;
  - alert on any skip;
  - wire book/decay_monitor.hpp (Page-Hinkley + PSR) once the owner rules on reading live returns.

### Axis C: code stress points

**C1 Python/C++ duplication (M, M).**
- *Evidence:*
  - `PricePanel` (fit_composition_weights.py:537-612) and `neutralization_basis` (:615-647) mirror
    strategy_price_exposures.cpp. The basis uses QR, which the code comment calls "the same OLS residual to rounding".
  - Forward returns are set NaN→0 (:755-756, signal m1).
  - Centered ranks are implemented 3× (fit :649; strategy_ic_composition.cpp:26-35; strategy_target_replay.cpp:183-205).
  - Every construction change (ind-v1, beta vs mkt_ret, V6-U's mkt_ret) must be mirrored by hand, or admission
    factors silently diverge from the book.
- *Change:* a C++ `exposures` verb writes the per-decision basis and forward arrays once per role, in the fitter's
  Context layout; the fitter reads it (parity test). Use one shared rank helper.

**C2 Silent continuation (M, S).** Folded into A4: stop on any non-zero receipt; `set -euo pipefail`.

**C3 Determinism (M, S).**
- Strong today: DetPool is bit-identical, reduction order is fixed, and byte-identity re-runs passed (D2 12/12,
  I1 10/10, m1 6/6).
- Risk: build-flag changes (A5, AVX2/FMA m7). `vm_fp_flavor` keys the signal cache but not the NAV.
- *Change:* an identity canary (a small pinned role slice) run after every build tag, with outputs compared by SHA.

**C4 Stale provenance (L, S).**
- *Evidence:* `engine_git_sha` is set at configure time (atx-impl/CMakeLists.txt:116-117). The v6-0/1/2 receipts all
  show ConfiguredProvenance `97e6b392`, although their sources were `ad31e817`, `a48677cb` and `e5e8eb26`. That
  stale value is written into every cache sidecar (strategy_ic_runner.cpp:1200,1619).
- *Change:* regenerate it at build time.

**C5 Parked cnms_to_si (L, S).**
- *Evidence:* prepare_research_fields.py:1906-1910 `str.replace`s p/r/w anywhere in the symbol (used :2126).
- *Change:* an anchored suffix regex. With A3 only sv_ratio126 rebuilds; it is still a new trial for the v6.1 cell.

**C6 Schema drift (L, S).**
- *Evidence:* `exit_rule` became `exit_rate_rule` without a version bump (D2); the limitations text is stale under delta
  orders (D7/m3).
- *Change:* golden key-set test per schema id, with a bump on any rename.

**C7 Tests (L-M, S-M).**
- 130 strategy gtests: NAV 42, target 24, IC 42, exposures 15, composition 7.
- Accounting identities are runtime-enforced every session: return decomposition (:614-619), cash + holdings = NAV
  (:861-863), and monthly reconciliation.
- *Missing:* a full-stack gtest (m8), and a randomized property test (random 50-name paths × flag grid) asserting the
  identities, θ=1 target≡delta, and replay≡decide parity.

## S3. Measured stage timings and memory (TRAIN, existing artefacts)

| stage | run (dir under build-equity/) | wall s | peak MiB | breakdown / note |
|---|---|---|---|---|
| fields (builder, unbounded) | fields-v6 on role v2 (40 fields) | 42 | 637 | ledger; the 700-1536 MiB / 1800 s caps are limits, not use |
| fields | lo1-fields-v6b (40) | 40.6 | 525 wset | V61 report §5 |
| fields | lo1-fields-v7 (41 = v6b + sv) | ≈ 75 | n/a | file mtimes; 40 unchanged fields rewritten |
| u cold | mega-v6l-train-u2-1 (38) | 121.9 | 1,060 | vm 33.3, fields_load 32.6, verify 16.8, cache_write 14.9, ic 9.6, comp 4.7 |
| u cold | mega-v6u-train-u-5 (38) | 78.1 | 1,061 | vm 22.0, load 12.8, verify 8.4, write 9.8, ic 11.0, comp 7.6 |
| u cold | mega-v61-train-u-1 (39) | 76.1 | 1,061 | vm 22.4, load 12.0, verify 8.5, write 9.4, ic 10.4, comp 7.0; 0/39 hits |
| fit | weights v6l-ew / v6l-ew6 / v6u-ew / v61-ew | 55.3 / 46.7 / 20.9 / 21.4 | 489 / 508 / 319 / 309 | v61: computed 39, reused 0 |
| w (all hits) | v6lw-ew-1 / v6uw-ew-1 / v61w-ew-1 | 45.5 / 30.0 / 31.5 | 505 | cache_load 15.9/10.7/10.0, verify 13.0/8.7/9.2, comp 9.1/6.2/7.4 |
| w (theme planes) | v6lw-train-ew6-4 | 35.0 | 1,199 | admit estimate 2,177 MB; runs 1-3 refused at admit |
| nav | v6l cells (14 runs) | 34.1-73.0 | 339-390 | 5 books lockstep, one thread |
| nav | v6u / v61u cells | 27.7-30.1 | 339 | liquidity cache; restricted role |
| nav_summ | all | n/a | n/a | not bounded, not receipted |
| build | mega-v6-0 / v6-1 / v6-2 | 47.8 / 47.4 / 27.7 | 3.3-4.2 GiB free | 5 / 6 / 8 TUs, 4 jobs; wide rebuilds 171 s (52 TUs), 269 s (122) |
| v3-era IC | mega-v3f2-train-r2-run1..4 | 4 × 180 (time-limit) | 969 | 104-116 of 121 candidates; fixed by T15 |
| wasted runs | v6l u-run1..3; v6u u-run1..4; ew6 w-run1..3 | 20-25 / 0.3 / 0.3 | - | cache-id refusal / field list / admit |
| **v6.1 ladder** | fields + u + fit + w + nav | **≈ 234 s compute** | ≤ 1,061 | 02:27-02:31 UTC ≈ 4.2 min, in a ≈ 90 min cycle |

- **u-pass memory** (admitted 1,449,072,426 B): VM/role 128 B/cell × 6.50 M cells = 832 MB; resident fields 41 B/cell
  = 267 MB; IC labels 196 MB; workers 59 MB; composition ≈ 59 MB; fixed 37 MB. Everything except labels, workers and
  fixed scales with dates × instruments.
- **Host:** 16 GB RAM, 12 physical / 16 logical cores; build receipts show 3.2-5.9 GiB free. RAM, not CPU, is binding.
- **After L1 + L2 (est.):** a DSL-only alpha costs u ≈ 15 s + fit ≈ 5 s + w ≈ 12 s + nav 30 s ≈ 1 min of compute from
  one spec file. Today it costs ≈ 4.2 min plus ≈ 600 lines of new generator, test and script.

## S4. What a 4-lane sprint should build first

The lanes own disjoint files. The only cross-lane touch is L4's source-pin reset at strategy_ic_runner.cpp:106-107,
which root makes at merge (merge order L1 → L4). The L1↔L2 contract is declared in the plan before dispatch: the
sidecar key `field_payload_sha256` (map of field → payload sha), and payloads named `<id>.<dsl_sha16>.{f64,json}`.

**Lane 1: incremental, fast IC runner (A1 + A2).**
- *Owns:* atx-impl/src/strategy_ic_runner.cpp and its test; atx-core/src/sha256.cpp, its test, and
  atx-core/CMakeLists.txt (per-source flags only).
- *Builds:* content keys (old layout readable); verify only loaded fields, hashed once; SHA-NI with scalar fallback;
  `--cache-report` listing unreferenced entries.
- *Root acceptance:*
  - the v6.1 u pass on a v6u-seeded cache gets ≥ 38 hits;
  - orientations.json, train_daily_ic.csv and train_candidates.jsonl (minus timing) are byte-identical to
    mega-v61-train-u-1;
  - incremental u ≤ 20 s, w ≤ 15 s;
  - digests equal the scalar ones on every existing manifest.
- *Trials:* none.

**Lane 2: research-cycle driver + field reuse + shared fit store (A4 + A3 + C2).**
- *Owns:* scripts/research_cycle.py (new) with its tests and specs/; atx-engine/tools/prepare_research_fields.py
  (`--reuse`); atx-impl/tools/fit_composition_weights.py (WorkStore/CacheLayout on the L1 contract).
- *Root acceptance:*
  - `research_cycle.py run specs/v61.json` reproduces the v6.1 cell's S2 daily CSV SHA with zero hand pins;
  - `--reuse` from v6b builds only sv_ratio126, with payload SHAs equal to fields-v7;
  - the fit reports computed 1;
  - a refused receipt stops the cycle.
- *Trials:* none (identical book).

**Lane 3: daily decide path, TRAIN-only (B3 + B2 + B1 manifest, the B6 mask input).**
- *Owns:* atx-impl/src/strategy_nav_replay.{cpp,hpp} (`--emit-holdings`); new atx-impl/src/strategy_live.{cpp,hpp};
  atx-impl/tools/equity_strategy_targets.cpp (`decide` verb); new atx-impl/tests/strategy_live_test.cpp;
  atx-impl/CMakeLists.txt (source list). Calls `detail::` functions read-only.
- *Root acceptance:*
  - flag off → every v6.1 NAV output is byte-identical;
  - at 3 pinned TRAIN decisions, decide (positions from the emitted holdings) equals the replay's planned weights
    bit-for-bit;
  - `atx.book-deploy/v1` refusal tests pass;
  - no seal change (that is an owner gate).

**Lane 4: DSL ops for literature families (A7).**
- *Owns:* atx-engine/include/atx/engine/alpha/{cs_ops,ts_ops,ts_order_stat,registry,typecheck,streaming_engine}.hpp;
  atx-engine/src/alpha/{registry,typecheck,oracle}.cpp; atx-engine/tests/alpha/*.
- *Ops:* ts_topk_mean, bucket → Group, group_cross, ts_resid_on (≤ 3 regressors), cs_residualize with ≤ 4 covariates,
  and min-periods variants.
- *Root acceptance:*
  - oracle ↔ VM ↔ streaming differential tests pass;
  - existing opcodes are bit-unchanged (semantics version stays 1);
  - the static library check can express MAX5-SMAX, BAC quintiles and an FF3-style residual momentum.
- *Trials:* no data runs; new candidates are disclosed admission trials after a prereg.

**Root, in parallel:**
- A5: build the exes with equity-rel, A/B for identity on v6.1, adopt Release if identical.
- C3: an identity canary per build tag.
- Disk GC (C: 93%). The legacy 20 GB cache still backs the v5.1 cells (v51_train.sh:47, v6_w.env.example:27), so GC
  only what the owner does not need reproducible.

**Deferred to next sprint:**
- B5 qp-factor-v1: a construction trial, and it needs L3's holdings for ex-ante vs realised risk.
- A6 date-blocked runner: only if U2 is granted.
- B4 and B7-B9: need the atx-db daily landing and prime-broker files.
- C5: rides the next fields rebuild, which becomes cheap after L2.
