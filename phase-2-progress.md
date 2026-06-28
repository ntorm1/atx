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

(unit rows appended below as each completes)
