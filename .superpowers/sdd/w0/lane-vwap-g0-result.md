# G0 L9 result after the raw daily-close VWAP correction

Completed 2026-09-26. **The corrected run admits zero alphas and rejects the family blend.** This closes the single affected numerical rerun identified in `vwap-g0-impact-plan.md`; it does not establish a tradeable alpha or qualify later search changes.

The existing, already authorized full L9 run completed successfully after the user redirected new work toward a fast IC prescreen. No additional numerical run, build, or correctness suite was launched afterward.

| Result | Frozen G0 `bc5cc646` | Corrected V2 `e464000f` |
|---|---:|---:|
| Seeds / invalid seeds | 378 / 0 | 378 / 0 |
| Candidates | 2,504 | 2,523 |
| Scored / degenerate | 2,306 / 198 | 2,293 / 230 |
| Decorrelated validation family | 56 | 47 |
| Admitted alphas | 0 | 0 |
| Family blend admitted | false | false |
| Blend validation net Sharpe | -0.535777789 | -0.106036785 |
| Blend validation mean net bps | -0.723267971 | -0.085685166 |
| Blend validation IC, h=1 | 0.000211103 | 0.005824250 |
| Blend validation IC, h=5 | 0.002379951 | 0.014668919 |
| Blend validation IC, h=21 | 0.017456974 | 0.034431115 |
| Blend validation coverage | 0.988047809 | 0.892430279 |
| Blend BY / RW p | 1 / 1 | 1 / 1 |
| Positive family validation Sharpe | 20 / 56 | 19 / 47 |
| Best family validation net Sharpe | 0.929935 | 0.963409 |
| Minimum family raw / BY / RW p | 0.0908452 / 1 / 0.894106 | 0.0756965 / 1 / 0.865135 |
| Search trial count | 2,202 | 2,221 |
| Fresh registry records / raw N | 2,306 | 2,293 |
| Effective N | 6.750301476 | 6.094956763 |
| DSR clusters | 56 | 3 |

DSR clusters are the actual reported cluster count, **not** the 47-member decorrelation family. Registry counts are local to this fresh run and do not replace cumulative historical search accounting. The new registry was generated from scratch; no old numeric registry or candidate state was copied. Its chain head is `bcb9397d790a2b35` with 2,293 records; the search digest is `ea3d1a9c069ac075`.

This is corrected integrated evidence, not an isolated estimate of the causal effect of VWAP. The report explicitly records `vwap_rule=raw-daily-close-v2`, `vwap_basis=raw`, and `vwap_is_intraday_observation=false`. It also retains the qualifications that adjusted close/VWAP mixes price bases, adjusted OHLC carries snapshot factors, session labels are not availability timestamps, and the flat-cost simulation omits borrow, impact, and capacity. Improved IC or a less-negative blend Sharpe does not overcome failed admission or these qualifications.

The command's only changes from the frozen L9 receipt are executable path, fresh output path, and explicit `--vwap-rule raw-daily-close-v2`. Environment overrides and working directory are identical. All six context payload hashes/artifact IDs, membership hash, and exact fixture bytes were verified. Contexts remain unaugmented and were reused unchanged; all context session keys are before 2020 and holdout remains off. L10, L7, all 13 cp21 baseline/IC cells and scorecard, and the native 2013 baseline/Abort evidence were reused according to the impact plan.

Source: `e464000fc05f380740e06ce168f465b9a118201e`, whole tracked tree equal to approved candidate `fcbcc9d1a351ccd339687389b118aca65e2812b5` (tree `b008adc830408a31a7871770673bacbaa561a811`). Independent D0 approval: `b131c3f4`. Executable SHA256: `0ffc7848babbe279492eb3e3316afc72e24f22f4fc82a731c4da00d036ff9261`; Release build archive manifest: `86ee554876a59ae262c1d12481a1b0cafb0d63e69042de38a681f0a2ab8891bb`.

New artifact root: `C:/atx-wt/g0-data/w0-vwap-v2_e464000f_20260926`.

| Artifact relative to the new root | SHA256 |
|---|---|
| `g0-artifact-manifest.json` | `a1bae0951662f08298ae2483011059957ee0079dd7dab04aec0fa5dbb10651ea` |
| `comparison.json` | `13874e4084f57ac1dc43e80e1940aa833162b213d725bada620654331ba1fcd9` |
| `logs/l9.receipt.json` | `768418d97e5a3f246e383c1ec277598c3e197fff7d61f23e6fb9b348a5640da1` |
| `inputs/bindings.json` | `629569b6fe26c7d3d3bce16fc621a7531c682eb184a8e7093eda2dd655589f0f` |
| `data/equity_mine_l9_guard_v2_e464000f/manifest.json` | `ddbc0605f8eb1cbd90855ecc458fbbb70da5090599a0703947c970c9f2b08656` |
| `data/equity_mine_l9_guard_v2_e464000f/trial_registry.bin` | `38c0fd421b043c68105d55f8f35796957a175e1f98408ed113f40e9d23db6e99` |

The outer manifest was published last. All 31 file bindings were verified afterward, including the engine's five output-file bindings, executable, runtime DLLs, fixture snapshot, comparison, and run/resource receipts. The new engine manifest's input roles exactly match the frozen L9 manifest. The old evidence remains unchanged at `C:/atx-wt/g0-data/bc5cc646_20260925`; its parent manifest hash is `3e2fd328a15c6671d81aff9aa2012388aad924e995b6b8185591259f117da679` and is bound by the new manifest.

The new fixture snapshot binds working-byte SHA256 `1a04d5e7182e331e1f20c87c17528ae12156036e195fdbcf47995fa92aae1e9b`. This closes the new run's fixture provenance gap; it does not retroactively supply the missing byte binding in the old L9 manifest.

Run receipt: exit 0, 818.734 seconds, peak working set 1,034,727,424 bytes (0.964 GiB). Minimum sampled available physical memory was 1.519 GiB and commit headroom 2.010 GiB; no memory-pressure termination occurred. The atomic heavy-run lock was released after completion. These are resource observations, not a performance comparison. No remaining L9 process or queued follow-up run exists.
