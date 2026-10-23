# Sprint note — r5 R1-B: the SPY1s owner work rebased onto the r5 root, phase-B gates (2026-10-22)

Lane R1-B of the vol-sota master plan (package R1). Branch `feat/vol-owner-rebase-20261022`, base = the r5
integration root `feat/vol-resume5-20261022 @ 9cfd3dd0`, leased tree `C:/atx-wt/pool-3`, run
`vol-sota-r5-20261022`, Opus 5.5. Gate contract: `lane-review-R1-owner-r5-report.md`, section "Phase-B gate
list". Report: `.superpowers/sdd/2026-10-18-vol-sota/lane-R1B-owner-rebase-r5-report.md`. Evidence:
`C:/atx-cache/quote-benchmark/R1B-20261022-owner-rebase/` (manifest `sha256.txt`).

No promotion. Every key below stays opt-in. `graded.family_native` is not promotable until review-R1-owner-r5
MJ1 and MJ2 are fixed:
- MJ1: the native confirm divides by a non-local secant.
- MJ2: native refusals are untyped and drop their bracket.

K2 / K3 own both. Nothing here fixes a review finding.

## 1. What was picked

`git cherry-pick -x`, in original order, onto `9cfd3dd0`. The owner's pre-rebase tip was `1d74d36c`.

| original | rebased | change | patch-id |
| --- | --- | --- | --- |
| `cb7cead0` | `42e9bc7a` | Screen routing gate kappa (`graded.seed_gate_kappa`) | differs: union lines in the key list and strict cases |
| `c9d99835` | `86e44826` | sigma-family Premium iterates (`graded.family`) | differs: a key-list union line |
| `80376a8e` | `bfa8c014` | Fast-grid headroom and reason-split tests | differs: context only |
| `69e238be` | `7f9f86ec` | (sigma, q) families in iterates, band gate and grid (`family_q`, `grid_kernel family`) | differs: a key-list union line |
| `2e824b48` | `b88f59d0` | Phase 2 r4 sprint section | equal |
| `9baa6d76` | `ecf1e6c3` | M1: probe the family bound between q nodes | **equal** (`daca906b`) |
| `e648da0c` | `ce22bdbf` | opt-in family-native iterates (`graded.family_native`) | differs in the key-list line only; every other file is equal |

All conflicts were at union points:
- the graded key list in `tools/quote_bench_policy.hpp`;
- the strict-case tuple in `python/tests/test_quote_bench_cli.py`;
- the test tail of `tests/quote_surface_test.cpp`.

Two checks show the rebase changed nothing else:
- One-shot merge: compare the rebased tip with a 3-way merge of `e648da0c` into the root. Every file outside those three is
  byte-identical. In those three, the only differences are where the conflict markers were.
- Unions: the key list is root + owner keys, with no duplicates, and the strict cases are root + owner cases.

## 2. What was dropped, and why

- **`1f96fca6` (WIP certificate code), dropped because of review-R1-owner-r5 MJ4: the WIP is NOT unreachable.**
  - How it is reached: the public C++ entry `fit_quote_slice` (PointCurve, VolChannel, de-Am cache) builds `VolSpaceCache`
    without `validate_channel`. So `QuoteSliceDeamConfirm::Certify` reached `certify_root`.
  - Why that matters: `certify_root` serves roots without a confirm, on a sampled estimate (deep review F02).
  - What happens to it: it is removed from the lineage. The SHA stays as preserved WIP, and K3 redesigns the certificate
    contract.
  - Proof it is absent:
    - the tree has no match for `Certify`, `certify_root` or `qfamily_cert_bound`;
    - its patch still applies forward and does not reverse-apply, so none of its hunks are present.
- **`1d74d36c` (the Phase 3 checkpoint note), dropped by the lane brief.** It documents the checkpoint, including the WIP.
- **The owner's merges `baba32ab`, `5ec6a451` and `4c0bbb2f`, not picked.** The root already carries L2h-b, W2c-a and
  L2h-c. The one resolution in `5ec6a451` (the policy unions) is reproduced by the picks.

## 3. Phase-B gates

These ran on code tip `ce22bdbf`. Quote-bench binary `7bf84c23`, `ATX_VOL_FIT_WORKERS=2`, every build `-Jobs 1`.

| gate | result |
| --- | --- |
| B0 preconditions | root = `9cfd3dd0` (R0 G0); lineage as above; patch-ids: `9baa6d76` equal, `e648da0c` differs in the key-list hunk only (needs the fresh review B0.4 names); certificate grep exit 1 |
| B1 build | 4 targets exit 0, 0 warnings. Bisect `check` at `ecf1e6c3`: not run by R1-B (the detached checkout was refused by the session's permissions); RUN by the R1-B reviewer in a leased tree: `check -Preset vol-rel` exit 0, 0 warnings, for `deam_iv_cache.cpp`, `quote_slice.cpp` and the M1 test TU `quote_slice_vol_space_test.cpp` (`lane-review-R1B-owner-rebase-r5-report.md`, Evidence 3) |
| B2 ctest | quote-slice 183 / 183 (subset 31 = p2 29 + 2 M1); pricing 54 / 54 (`deep ITM put: native fallback 0`); chain-batch 31 / 31; native CLI 172 passed (incl. both `family_native` refusals) |
| B3 defaults | 9 / 9 MATCH: dev200 / dev200b auto `c89d0739` / `4b273b08`; graded dev200 `2bc7777d`, dev200b `ed34eef7`, cohort5 `45e491ea`, SPY `feac8d2c`; divsched `abc75955` / `747d3e6c`; SPY kill switch `087e6a54` |
| B4 fingerprint | `53c4df68` MATCH (vol-rel-avx2) |
| B5 PCH-off | 7 TU checks exit 0; hygiene build of the 3 owning targets exit 0 (529 s, cold tree); 0 first-party diagnostics |
| B6 keys-ON | native SPY `a3c0b01e` MATCH; famq-grid SPY `86ca3e10` MATCH (`e67dd513` not back); famq Fast grid SPY `6368997d`; native + Fast grid SPY `6c898a94`, cohort5 `f897be99`, dev200 `393e1cd8`, dev200b `7b3a58f4`; B-family SPY `a134fc48` (vs `815fee36`: same 1,410 rows, max \|dP\| 7.1e-12) |
| B7(a) served sets | lost 0 and gained 0 in all 12 comparisons (4 cohorts x vs default / vs kill switch / native ON vs OFF). Native vs default max \|dP\|: SPY 4.566e-8, cohort5 1.118e-6, dev200 1.380e-6, dev200b 6.18e-8 |
| B7(b) audit | status mismatch 0 on all 4 cohorts; max Newton 7.7e-11 / 7.7e-11 / 2.0e-10 / 2.3e-10 (SPY / cohort5 / dev200 / dev200b) |
| B7(c) grid isolation | rows over allowance: 0 on all 4; max ratio SPY 0.055, dev200b 0.105 (= owner) |
| B7(d) accuracy | cohort5 / dev200 / dev200b PASS under ruling a; every metric equal to 8 digits |
| B8 | SPY @1 native 5.24 s at 15-24 % load (unqualified) |

## 4. The finding

With `graded.family_native` ON vs OFF (every other key equal), the served set is identical on all four cohorts. Served
prices still move, by up to:

| cohort | max \|dP\| ON vs OFF (USD) |
| --- | ---: |
| cohort5 | 1.118e-6 |
| dev200 | 1.380e-6 |
| SPY | 4.0e-11 |
| dev200b | 2.7e-8 |

So the cohort5 move is above the 4.6e-8 that the owner reported on SPY alone. Where the moves are:
- cohort5: MSTR long-dated far-OTM calls. The 2.8e-8 is the FITTED-CURVE VOL delta ON vs OFF (|dsigma_fit| <=
  2.8e-8 in `predicted_vol`), not a root error: the served move is exactly vega x dsigma_fit (MSTR K 470: 40.2948 x
  2.7742e-8 = 1.1179e-6 USD; `lane-review-R1B-owner-rebase-r5-report.md`, F-R1B-1 ruling). Root-level 1e-7
  compliance of the native roots rests only on Newton distances (B7(b), <= 2.3e-10). A bracketed root-error record
  for the native roots is still owed by K3, and it is a precondition for any `family_native` promotion, beside MJ1
  and MJ2.
- dev200: META and ASML chains.

The same move appears with the Fast grid, so the native iterates cause it, not the family grid. It is recorded as a named
delta, not waived: K2's typed native outcomes should report it.
