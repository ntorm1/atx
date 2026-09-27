# Frozen-sign validation completed; TRAIN export stopped at the RAM floor

Evidence: `fast-ic-validation-20260926/`, 46 indexed files totalling 1,218,507
bytes. The index file `sha256-index.json` has SHA256
`352ca7b7d98f877a53d6fcc578f39ed7b5f2b3a4f56f90bc0144b2364bca2417`. The 46 files
are 44 copied artifacts plus `review-checks.json` and `.gitattributes`
(`* -text`). The index also records hash-only entries for 8 omitted dense
payloads and daily-IC CSVs. Every indexed size and SHA256 matched the bytes on
disk when this report was written. Origin root: `C:\atx-wt\pool-2`.

## Build and native qualification

Source and configured provenance are both
**`a9b8814c5797599dd73fd448f5c5d63d88149dc6`**. This is the last source that was
built and tested. It is not the current root HEAD and does not include the
memory fix below.

- **Build:** focused `atx-equity-strategy-ic` + `atx-impl-strategy-ic-tests`,
  exit 0 in **82.169s at Jobs2**. It was admitted at 1986 MiB free and 3238 MiB
  commit headroom. CMake re-ran (configure 25.7s). The build did seven C++
  compiles and four links, with 0/7 ccache hits.
- **Native tests:** **34 tests from 4 suites passed in 10.484s**. The guard
  measured 10.828s wall and a 35,061,760-byte peak. Test executable SHA256:
  `26e5c25938c4ae0d1ff48610fe5295dca362b9784fe17fb37820ae0a48325447`.
- **Research executable:** SHA256
  `8833a852edec05d7ecaca0d92a5bbe48095c8c76340fa7f0fa169542a3b726d0`. The
  review re-hashed both binaries and both matched their receipts.
- Each `start.json` reads `launch-failed`/`null` because it is the pre-launch
  placeholder. The actual outcome is in `receipt.json`.

## v3 frozen-sign validation

The run pinned the library (`1ec75242...`), the TRAIN manifest (`3f53ee9a...`),
the validation manifest (`0c757c41...`) and the original v2 orientations
(`5106fdc1...`, no refit). It used workers 4, min-names 2000, 1536 MiB and
`--save-combined`.

- **Evaluations:** the summary reads `complete`, **48 candidates + 1 blend,
  `train_candidates_planned: 0`**. Frozen signs are 36 at +1 and 12 at -1.
- **Guard:** exit 0 in **110.5s**, peak **660,586,496 bytes**, minimum free
  1,011,068,928 bytes.
- **Stage seconds:** VM 38.704, IC 33.847, composition 19.546, load 9.177,
  labels 1.247, save 3.115.
- **Validation role:** 903 dates x 5048 IDs, scored [401, 903), 502 sessions,
  2994-3000 eligible names per day.

| Horizon | Combined mean rank IC | HAC SE (lag 2h) | Valid days |
| --- | --- | --- | --- |
| 5 | 0.018033565459307746 | 0.01566902744171605 | 496 |
| 21 | 0.035690280179471075 | 0.027300949553404476 | 480 |
| 63 | 0.05966692932690373 | 0.017772073033932835 | 438 |

Only the 63-session mean exceeds 2 SE. The 5- and 21-session means are within
about 1.3 SE of zero.

**Target proxy** (archived audit JSON): mean monthly turnover is **34.4356%**.
The maximum is **70.5991%** in 2023-01, which includes the 25% initial
deployment. Total turnover is 8.2645448796, mean gross 0.90637, maximum absolute
net 0.01908 and mean contribution fraction 0.9323. This is planned
sum(|weight change|), with no drift, fills, costs or capacity.

**Saved blend.** The manifest is `validation_combined.json`
(`7407d7e7b72548e5577fdf94e2f52d238f9ff751a8ebc5640987130c1390d8e1`). The
orientation candidate array is
`4a3e8004ec15b104e045d06ee255447e5db7df8f1b1f6430668ebab0cbea9081`. The payloads
below total 45,631,048 bytes and are not archived:

| File | Bytes | SHA256 |
| --- | --- | --- |
| `validation_combined.f64` | 36,466,752 | `614d59f3e7bd15a2c5372bf23f7ab498c45bbf3f15856744259b9947e6ce645d` |
| `validation_combined_finite.u8` | 4,558,344 | `8b12b93d752016f14618704e9439f4c6f3b3de3e4f0987de5d8ebeadb5885d7d` |
| `validation_combined_member.u8` | 4,558,344 | `8b12b93d752016f14618704e9439f4c6f3b3de3e4f0987de5d8ebeadb5885d7d` |
| `validation_combined_ids.u64` | 40,384 | `9b1ba628d1bb71ac969d9fbd6735e986f3617fcd3d84259d8e2a5b51f8a13eb1` |
| `validation_combined_sessions.i64` | 7,224 | `92013075f09e90f2ac982b5579980b1ef0d66f5015a1d737c9c59a0a1e3523d4` |

The finite and member masks are identical: both have 2,519,482 set cells.

## Audit

The final audit (run2) passed with exit 0 in 1.422s. Script SHA256:
`5c4e80b0a881b6256ae66eb9feef6fce0c8411f9cce20143fcbbc8aa9dc7d2c3`.

- Result `verified`: 0 TRAIN evaluations and **48 signs unchanged**.
- It paired the 13 v2 validation candidates that had completed (v2 stopped with
  a 14th started): **55,098 daily values, 0 differences**. **One unterminated old
  CSV row** was excluded.
- All payload lengths and hashes were verified.
- Paired 13-candidate IC stage: 24.394s -> 8.620s. VM and composition times
  varied with host load.

The first audit failed: exit 1 in 1.109s on `assert compared and differences ==
0`. Its receipt binds the earlier script (`07ed7696...`), not the archived one.
Per handoff, that script parsed the stopped run's unterminated last float
(`0.027`) as a complete value. This was an audit input defect, not an engine
arithmetic mismatch. The failed receipt is preserved. The review relied on the
root final audit and did not rerun it.

## v4 TRAIN export: stopped by the RAM guard

v4 used the same source and executable as v3. It was a TRAIN-only
`--save-combined` export repeat, not 48 new ideas.

- **Outcome:** **`system-memory-limit`, exit 15, 41.578s**, peak
  **1,101,864,960 bytes**. Minimum free memory was **794,648,576 bytes**, below
  the 768 MiB (805,306,368-byte) floor.
- **Progress:** **10 candidates completed.** Candidate 11
  (`risk_scaled_6_1_s21`, slots=7) started but never reached VM-complete.
- **Artifacts:** there are no orientations, TRAIN blend or target CSV.
  `summary.json` still reads `running` with `roles: []`, which is stale. Do not
  use it as a TRAIN result.

**Root cause** (per handoff): `Engine::ensure_pool` constructs the new arena
before it frees the old one. Earlier candidates peaked at 5 slots (archived log)
on a 1155 x 5627 = 6,499,185-cell TRAIN panel. Growing to 7 slots therefore adds
a 5 x 6,499,185 x 8 = **259,967,400-byte** (~248 MiB) transient. v3 completed on
the smaller 4,558,344-cell validation panel.

**Pending fix** (per handoff): `8527a839` makes the runner own the Engine
through `unique_ptr` and destroy it before a larger-slot candidate or combined
scoring. It also changes admission from `16*max_slots` to `8*max_slots`. The
paired fixture `23f1541b` checks ascending-slot versus maximum-first parity. Both
were imported to root as `050c0efc`/`bfb6b859` and are source-approved only.
**Neither has been built, tested or runtime-qualified**, and the RAM savings have
not been measured.

## Limits

- IC is not Sharpe: no net, costed or portfolio-return result is claimed.
- Turnover exceeds the 30% monthly target in both the mean and the maximum
  month. It is a planned proxy, not fills.
- No capacity, impact or borrow claim is made.
- The universe is a liquidity research cohort, not a certified point-in-time
  common-stock universe.
- 2025+ data has not been used and stays reserved.
