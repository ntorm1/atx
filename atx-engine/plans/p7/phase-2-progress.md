# p7 Sprint 2 — Information Breadth — Progress Ledger

Base: main @ 2eaf3da (docs(p7): module roadmap + 7 disjoint sprint plans)
Branch: feat/p7-s2
Worktree: C:\atx-wt\p7-s2

## Scope (binding overrides)

Per user decision D1, the CLI surface for the `augment` subcommand is DEFERRED to
Sprint 7. S2 lands the engine + the PURE augment core + tests + fixtures ONLY —
NO CLI:

- LAND (from track-b ref `worktree-track-b-information-structure` @ 8df5010,
  adapted): `data/finra_short.{hpp,cpp}`, `data/finra_short_test.cpp`
  (FinraShort.* x4), `atx-impl/src/stage_augment.{hpp,cpp}` keeping ONLY the pure
  core `augment_panel_with_finra` (run_augment CLI stage STRIPPED),
  `augment_test.cpp` (Augment.* x4), `iv_earnings_templates.txt`,
  `neutralized_templates.txt`, `seed_parse_test.cpp` (SeedParse.* x3).
- DO NOT touch: `atx-impl/src/config.{hpp,cpp}`, `dispatch.cpp`, `stages.hpp`,
  the run_augment CLI stage, `stage_regime_oos.cpp` / `regime_oos_test.cpp` (B4).
- CMake: add `finra_short.cpp` to `atx-engine/CMakeLists.txt`; `stage_augment.cpp`
  to `atx-impl/CMakeLists.txt`. No other CMake edits.

## Unit checklist

- [ ] S2-0  marker + ledger (this commit)
- [ ] S2-1  land track-b FINRA short-interest (CLI-stripped) — FinraShort.* (4),
            Augment.* (4), SeedParse.* (3) green; default panel byte-identical.
- [ ] S2-2  IV-surface derived fields (with_iv_fields) — IvFields.* (5)
- [ ] S2-3  liquidity Amihud field (with_liquidity_fields) — LiquidityFields.* (5)
- [ ] S2-4  multi-family seed catalog (2 fixtures + 2 parse tests) — SeedParse.* (5)
- [ ] S2-5  multi-family smoke — MultiFamilySmoke.* (5)

## Byte-identity gate (run green before AND after every unit)

`atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*`

## Progress

S2-0: complete (commit ae73fde) — ledger opened; base main @ 2eaf3da; no source.
S2-1: complete (commit b0f9c1f, 11 files) — landed track-b FINRA short-interest
      CLI-FREE per D1. FinraShort.* (4), Augment.* (4), SeedParse.* (3) green.
      Byte-identity gate (factory Oracle/Golden/Digest) 18/18 green. Full data
      suite 118p/3 env-skip, full impl suite 169p/4 env-skip — zero regressions.
      CLI deferral: run_augment + config/dispatch/stages NOT landed (S7). Drift
      notes: (1) on current main augment_test.cpp is the p6-S5
      WithAlpha101Fields/DelegationIdentity file, NOT the FINRA file the plan
      assumed — appended the 4 Augment.* tests in a separate namespace, kept the
      6 existing tests green. (2) seed_parse_test dropped `#include "config.hpp"`
      (would hit S7-owned src/config.hpp) for the ATX_IMPL_TESTS_DIR compile-def +
      #ifndef fallback. (3) dropped track-b's unused kNaN const (/WX).
S2-2: complete (commit 16be122) — with_iv_fields (iv_term/iv_vrp/iv_lo).
      IvFields.* (5) green. Helpers cs_zscore_row_aug + rolling_sample_std added
      (match cs_zscore_row ddof=1 / rolling_mean full-window policy). Byte-id gate
      18/18, full alpha suite 580/580 green.
S2-3: complete (commit 59d2f56) — with_liquidity_fields (illiq = group_neutralize
      (zscore(-adv20), sector)). LiquidityFields.* (5) green. Helper
      group_demean_row added (CsNeutG semantics). adv20 absent -> Err(NotFound);
      sector absent -> global demean. Byte-id gate 18/18, alpha suite green.
      NOTE on unit split: S2-2 and S2-3 both live in augment.hpp; committed as two
      clean compiling commits by removing/restoring the liquidity code+test around
      the S2-2 commit (each commit builds + its suite is green in isolation).
S2-4: complete (commit 6314536) — short_interest_seeds.txt (10) + liquidity_seeds
      .txt (8) fixtures; 2 new SeedParse tests (ShortInterest/Liquidity) over a
      multi-family panel. All 5 SeedParse.* green. alpha101.txt untouched (frozen).
S2-5: complete (commit cc9270c) — MultiFamilySmoke.* (5): price/iv/liquidity
      seeds evaluate finite end-to-end, exact augmented field count, off-path
      digest unchanged (local FNV-1a NaN-safe). Drift: panel uses 25 dates (not
      the plan's nominal 15) because adv20=ts_mean(dollar_volume,20) needs a full
      20-date window — a 15-date panel leaves adv20/illiq all-NaN.

## Final gate results

- FinraShort.* 4/4, Augment.* 4/4, SeedParse.* 5/5, IvFields.* 5/5,
  LiquidityFields.* 5/5, MultiFamilySmoke.* 5/5  (28 new + existing).
- Byte-identity gate (factory *Oracle*:*Golden*:*Digest*): 18/18 green
  (verified before S2-1, after S2-1/2/3, and after S2-5).
- Full alpha suite: 585/585. Full data suite: 118p/3 env-skip. Full impl suite:
  169p/4 env-skip. Zero regressions.
- Branch diff (2eaf3da..HEAD): 18 files, all owned; no config/dispatch/stages/
  oracle/alpha101.txt/ts_ops/vm touched.
