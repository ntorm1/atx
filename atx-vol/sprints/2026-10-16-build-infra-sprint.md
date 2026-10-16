# 2026-10-16 build-infrastructure sprint (atx-vol lanes)

Branch `feat/vol-build-infra-20261016` (pool-4), frozen base `c718c3bd`. Scope: CMake,
presets, wrapper/lease scripts, ccache, docs only — no library source touched.

Host: i7-1260P (16 threads), 15.7 GB, 4–6 other lanes building throughout. **Every wall
time below is unqualified**; the comparable metrics are step counts, per-step seconds
from `.ninja_log` ("CPU-s" = summed step durations) and isolated ccache counters
(`CCACHE_STATSLOG` + `ccache --show-log-stats`, so the shared stats were never zeroed).
Receipts: `C:\atx-cache\quote-benchmark\build-infra-20261016-*`.

## Findings

1. **Why vol lanes compiled atx-engine.** `atx-vol-tests` linked `atx-engine` (for
   `vrp_model_test.cpp` only) and `atx-vol-vrp-train` linked it too, while every vol
   preset configured the whole engine/impl graph. `ninja -t inputs` in pool-34
   `build-vol-rel-avx2`: atx-vol-tests 1222 inputs, 294 under atx-engine; the graph had
   2394 targets, 734 of them atx-engine/atx-impl outputs. Focused targets were already
   engine-free (quote-slice 488 inputs, american-markov 494, quote-bench 490, umbrella
   484, all 0 engine). Three of five vol lanes paid it:

   | tree (vol-rel-avx2) | steps | CPU-s | atx-engine | atx-vol-tests TUs | link s (atx-vol-tests) |
   |---|---|---|---|---|---|
   | pool-34 tick-path | 639 | 2173 | 146 steps / 1063.8 | 243 / 982.1 | 214.6 (138.0) |
   | pool-35 implied-fwd | 612 | 1976 | 145 / 974.1 | 203 / 828.0 | 94.6 (—) |
   | pool-36 slice-cov | 628 | 2272 | 146 / 933.9 | 231 / 1151.3 | 231.9 (180.6) |

   Slowest steps were engine TUs: learn/ensemble.cpp 49.6 s, eval/breadth.cpp 48.6 s,
   risk/specific_risk.cpp 48.0 s, learn/gbt.cpp 47.4 s.
2. **ThinLTO on every gate.** Lanes used `vol-rel-avx2` (IPO ON) for check/build/ctest:
   `atx-vol-quote-bench` linked in 11.7–23.6 s, `atx-vol-quote-slice-tests` 10.9–13.0 s,
   `atx-vol-tests` 138–181 s.
3. **ccache.** Config is sound (`base_dir` from the wrapper, `hash_dir=false`,
   prefix-map `ignore_options`, `pch_defines,time_macros,include_file_*` sloppiness,
   direct mode, `compiler_check=mtime` — one compiler install, so `content` would only
   add a ~100 MB hash per call). Controlled experiments, same sources, second worktree:

   | experiment | TUs | hits | mode | note |
   |---|---|---|---|---|
   | exp1 vol-rel-avx2 quote-slice closure (pool-4 vs pool-33) | 231 | 229 (99.1 %) | 228 direct | 2 misses = test files that had changed |
   | exp2 atx-vol-tests black76/greeks + PCH | 3 | 0 | — | PCH consumer |
   | exp3 atx-engine data/adjust.cpp + PCH | 2 | 0 | — | PCH consumer |
   | exp4 vol-rel 3 focused targets (pool-1 vs pool-4) | 242 | 242 (100 %) | 242 direct | 14.0 s wall vs 512 s cold |
   | exp5 vol-rel core+fitting slices (pool-1 vs pool-4) | 59 | 55 (93.2 %) | 54 preprocessed | 4 misses = test_paths consumers |

   Why PCH consumers never transfer (CCACHE_DEBUG input dumps, exp2): the direct hash
   carries eight `-D…="C:/atx-wt/pool-4/…"` values on every atx-vol-tests TU; the
   preprocessed hash carries `C:/atx-wt/pool-4/atx-vol/tests/pch.hpp` (CMake's
   `cmake_pch.hxx` includes it by absolute path) and `pch_hash` of a `.pch` that embeds
   absolute paths. Making the `/Yc` TU path-independent would let ccache restore a `.pch`
   that references another tree's headers, so there is no safe fix; the answer is to keep
   lanes off PCH targets. Global counters (47.2 % hits; 32 % "uncacheable" =
   16,126 `preprocessor_error`) are dominated by PCH classes and old equity builds:
   `preprocessor_error` stayed at 16,126 from 11:31 to 12:40 while vol lanes built, so it
   is historical.
4. **Git SHA.** `engine_git_sha` is configure-time only and already confined to
   generated `.cpp`s (atx-impl `build_provenance.cpp`, atx-engine
   `research_fields_build_identity.cpp` in an EXCLUDE_FROM_ALL library), never a
   compile definition or shared header. Docs commit `2ce7dfa6` + reconfigure, then
   rebuilding `atx-vol-quote-slice-tests`: **0 recompiled TUs, 0 relinks** (no-op 4.7 s
   wrapper overhead) — before the fixes as well as after; under `ATX_VOL_ONLY` neither
   file is even configured. No change needed.
5. **Lease.** Every cold slot ran `dev`, which cannot configure in this checkout
   (atx-kb, atx-agent-db, atx-options-engine missing → CMakeLists.txt:417/418/422):
   the build-infra lease took 54.6 s and ended in "configure failed; lease remains
   held". Vol trees also always showed `[cold]` (only `build\build.ninja` was checked).
6. **FetchContent over the network.** Each vol preset has its own
   `FETCHCONTENT_BASE_DIR`, so a first configure per tree per preset git-clones eight
   deps: vol-rel-avx2 66.7 s, vol-rel 82.6 s configure; one probe failed outright on a
   gitlab.com HTTP 503 for eigen.
7. **PCH review (optional item).** The atx-vol library has no PCH; only atx-vol-tests
   does. Its 168 TUs hit 99–100 % across trees *because* they have none (finding 3), so
   adding Eigen/xsimd/gtest to a library PCH would trade that for once-per-tree
   recompiles. Not done, deliberately; recorded in `.agents/cpp/agent.md`.
8. **Trap (environment, not fixed).** CMake's FindPython3 follows the caller's
   `VIRTUAL_ENV`; this session had `c:\atx\atx-db\.venv` active, which lacks `polars`,
   so `atx-vol-quote-benchmark-native` failed 1/107 until reconfigured with
   `-DPython3_EXECUTABLE="C:/Program Files/Python312/python.exe"` (then pass). pool-33
   had resolved the system Python.

## Fixes

| commit | change |
|---|---|
| `b522b4d8` | `ATX_VOL_ONLY` root option, ON in every vol-* preset: only atx-core + atx-tsdb + atx-vol are configured. `vrp_model_test.cpp` moved to `atx-vol-vrp-model-tests`; it and `atx-vol-vrp-train` exist only with `-DATX_VOL_ONLY=OFF` |
| `552def37` | `vol-rel` preset (Release + AVX2 + PCH, no ThinLTO, own deps dir) for lane gates; `vol-rel-avx2` = perf pins only; docs + wrapper help |
| `4523bd0a` | lease `-ConfigurePreset auto\|<name>\|none`, per-preset warmth, `-Status` lists warm build dirs; 4 Pester cases (13/13 pass) |
| `77a03bf7` | `atx-vol/tests/slices.cmake`: ten PCH-less module slices partitioned from atx-vol-tests' SOURCES; docs table |
| `314ccf91` | wrapper exports the preset ccache keys for `build`/`check`; `configure` clones FetchContent git deps from the local `$ATX_DEPS_DIR` mirrors |
| (this note, docs) | Jobs rule, CLAUDE.md lease timings, ledger lines |

### Before / after

Dependency closure (`ninja -t inputs`, vol-rel-avx2): atx-vol-tests 1222 → 926 inputs,
atx-engine 294 → 0; graph 2394 → 1209 targets, atx-engine/atx-impl outputs 734 → 0.
With `-DATX_VOL_ONLY=OFF` the engine path still configures: vrp-model-tests and
vrp-train 294 engine inputs, atx-vol-tests 0.

| lane build | steps | CPU-s | engine | links (s) | ccache |
|---|---|---|---|---|---|
| before: pool-34 tick-path, vol-rel-avx2 (whole session) | 639 | 2173 | 1063.8 | 214.6 (atx-vol-tests 138.0) | PCH TUs 0 % cross-tree |
| after, first tree cold: pool-4 vol-rel, quote-slice + american-markov + quote-bench | 253 | 2013 | 0 | 9.0 (3.1 / 2.0 / 2.9) | 0/242 (new preset flags) |
| after, second tree: pool-1 vol-rel, same 3 + core/fitting slices | 316 | 107 | 0 | 12.0 (each 2.1–2.4) | 242/242 + 55/59 |

All ten slices cold in pool-4: 204 compiles, 2814.5 CPU-s, 10 links 37.4 s. Without a
PCH that is ~1.65× the monolith's 1703.8 CPU-s for the same sources under the same
preset, but it is paid once per changed file across all trees (93–100 % cross-tree
hits) instead of once per tree, and a lane builds one slice (9–40 files), not all ten.

| lease | before | after |
|---|---|---|
| fresh slot (grow) | 54.6 s, **failed** (dev configure) | — |
| released slot, cold for vol-rel (network clones) | — | 64.0 s, OK (configure 55.5 s) |
| cold vol preset configure with local mirrors | 55.5–82.6 s configure | 42.4 s configure (vol-hygiene, pool-1) |
| slot warm for vol-rel | reconfigured / failed as "cold" | **6.6 s**, configure skipped |

`-Jobs` sweep, 16 cold atx-vol library TUs (src/fitting), `CCACHE_DISABLE=1`, other
lanes active: ninja span 70.6 / 53.9 / 35.6 s at -Jobs 2 / 4 / 8; summed step time
136.8 / 177.9 / 192.1 s; min free RAM 3.06 / 1.76 / 1.46 GB.

## Lane rules (verbatim in `atx-vol/CLAUDE.md` "Build lanes")

1. Preset `vol-rel` for every lane configure / check / build / -Ctest; `vol-rel-avx2`
   only for -Bench perf pins (bit-identical results per the `rel` preset's ThinLTO
   parity evidence, commit 63f6f29; timings only from vol-rel-avx2).
2. Every vol preset is vol-only; engine consumers need `-DATX_VOL_ONLY=OFF`.
3. Never build `atx-vol-tests` in a lane; build the owning focused target / slice and
   run it anchored.
4. Lease with the default `-ConfigurePreset auto`.
5. `-Jobs = clamp(floor(12 / N), 1, 8)` for N concurrently building lanes; one step
   lower under ~2 GB free RAM.

## Verification

- Configure: vol-rel (pool-4, pool-1), vol-rel-avx2 (pool-4), vol-dev
  (pool-4, with and without `ATX_VOL_ONLY`), vol-hygiene (pool-1) — all OK.
- `check`: 16 atx-vol library TUs under vol-rel (the Jobs sweep).
- vol-rel builds: atx-vol-quote-slice-tests, atx-vol-american-markov-tests,
  atx-vol-quote-bench, all ten slices.
- Anchored ctest (pool-1, vol-rel): `^(atx-vol-quote-slice|atx-vol-american-markov|atx-vol-quote-benchmark-native)$`
  3/3 pass — gtest 49/49 and 60/60, native CLI pytest pass; slices
  `^(core/|pricing/Black76)` 98/98 pass; `ctest -N -L atx_vol_slice` lists 4146 cases.
- vol-rel-avx2 still links: atx-vol-quote-slice-tests rebuilt and 49/49 pass.
- Monolith without atx-engine: `atx-vol-tests` built under vol-rel in pool-4 (219
  compiles incl. PCH, 1703.8 CPU-s, 0/219 ccache hits, **link 4.9 s** vs 138–181 s under
  ThinLTO, 440 s wall at -Jobs 4); `--gtest_list_tests` 4328 cases (= 4146 in slices +
  the suites owned by pre-existing focused exes); smoke filter 62 pass / 1 designed skip.
- Pester lease suite 13/13; atx-build dry-run argv test passes.

## Not done / follow-ups

- atx-vol-tests' PCH stays (it is the qualification exe; its cross-tree misses are now
  confined to integration trees).
- Slices hit ccache cross-tree in preprocessed mode (abs-path `-D` copied from
  atx-vol-tests); restricting those defines to the ~18 `test_paths.hpp` consumers would
  turn them into direct hits — skipped, ~1 s/TU gain.
- `scripts/oracle-targeted-gate.ps1` still names `-Preset dev` / `rel-avx2`, which do
  not configure in this checkout; owned by the oracle workflow, untouched here.
- The FindPython3/`VIRTUAL_ENV` trap (finding 8) is documented, not automated.
