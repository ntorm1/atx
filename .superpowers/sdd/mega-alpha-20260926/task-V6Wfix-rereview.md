# Task V6-W fix round 1: re-review

Reviewer: read-only. Tree `C:/atx-wt/pool-4`, branch `feat/mega-alpha-v6-w-20260927`, head `3481cd95` (confirmed via
`git rev-parse HEAD`). Ran both pure-Python test files under numpy 1.26.4 (`C:/Program Files/Python312/python.exe`)
and numpy 2.5.2 (`python` on PATH): `test_fit_composition_weights` 81/81 OK both, `test_prepare_recent_research`
11/11 OK both. No build, no real data, no edits.

**I1 — ADDRESSED.** `fit_composition_weights.py:1837` sets `document["schema"] = WEIGHTS_SCHEMA_V2` only inside
`if v6:`; v1/aim untouched (`test_schema_v2_only_for_v6` passes). Runner `strategy_ic_runner.cpp:885-886` accepts
v1 or v2; after `composition_themes()` populates `pinned.themes`, `:912-918` refuses `v2 && themes.empty()` ("...v2
requires a theme_redistribution block") and `!v2 && !themes.empty()` ("theme_redistribution requires ...v2") — this
is exactly "v2 iff block present, v1 iff absent." **Old-binary check, as asked**: the pre-existing, unmodified
`:889` `return co::Err(...,"IC runner: composition weights schema/library identity")`, fired from the unmodified
condition `j.at("schema")!=weights_schema` that an old binary still has (it has no `weights_schema_v2` symbol), so
any v2 file fails that equality and hits this exact message. Confirmed at `:717-719` (existing case, wrong schema
refuses the same way) and the new `:803` case (`/v3` schema refuses "schema/library identity").

**I2 — ADDRESSED.** `test_prepare_recent_research.py:276-289` now sums via `wide_base = base_member.astype(np.int64)`
/ `wide = member.astype(np.int64)` everywhere a count crosses a uint8 array boundary. Ran green under both
interpreters (see above); this reproduces the exact repro case the finding cited.

**I3 — ADDRESSED.** `strategy_ic_runner_test.cpp:769-853`
`ThemeRedistributionRefusalsAndSchemaGatePrecedeAnyPayloadOrOutput` covers: schema/block mismatch both directions,
v3 schema, block shape (non-object, wrong rule, wrong composition, missing `themes`, array `themes`), unknown id,
name regex (uppercase/hyphen/empty/non-string/65-char), missing theme for a weighted id, 0 themes, and 33-vs-32
themes — each under both `--plan-only` and a full run, asserting no payload/output touched (mirrors the established
`InvalidCompositionWeightsRefuseBeforeAnyPayloadOrOutput` pattern at `:700-754`, which already builds/passes).
`ThemeRedistributionIsRecordedInRecipeSummaryAndCombinedOnlyWhenPinned` (`:854-917`) asserts all three
`within-theme-v1` keys: recipe `composition_redistribution`, `summary.json` `composition_weights.redistribution`,
and `<role>_combined.json` `composition_redistribution` (plus `signal_semantics` unchanged, and the plain-v1 recipe
equal to the themed one once the new keys/SHA are stripped) — this is m6's disclosure now also machine-checked.
Verified helper signatures used by the new code (`text_file`, `json_file`, `read_json`, `run_named`, D/N constants)
against their existing declarations — all match call sites exactly.

**Compile hazard check (/W4 /WX), not built per no-build rule.** No narrowing, no unused vars, no ODR clashes
(`weights_v1`/`weights_v2` names are new). The one candidate warning, `for (usize k=1;k<=31;++k)` comparing
`std::size_t` to a positive int literal, matches the pre-existing, already-building pattern `for (usize r=0;r<2;++r)`
used 8x elsewhere in the same file — not a new hazard. All new code structurally mirrors already-compiling
neighboring tests (`bound_text`/`weights_text` vs. new `themed_text`/`theme_block`).

**m1 — ADDRESSED.** `v6_w.env.example`: `W_MAX_RSS_MIB` line and "raise BR's --max-rss-mib" removed;
`W_MAX_MEMORY_MIB=2304` now commented as "admit ESTIMATE only," RSS cap stated as staying 1536 MiB; header updated
to say an old binary refuses v2 outright (matches I1's real mechanism, not just a manual grep check).

**Controller ruling (SIC 6792/6795 excluded, REITs kept) — ADDRESSED.**
`prepare_recent_research.py:58,113-117` (`NON_OPERATING_SIC` gains 6792/6795), docstring and `LINKED_OPERATING_RULE`
text updated; `test_prepare_recent_research.py:239-244` asserts the exact tuple and each SIC classifies as
`non_operating_sic`, with 6798 (REIT) still passing.

**No new Critical/Important found.** Checked: no other consumer hardcodes the v1 schema string (only the fitter and
runner reference `dsl-composition-weights`, confirmed by repo-wide grep), so NAV/cache paths are unaffected; the
schema OR-check doesn't loosen anything else (still exactly two literal strings); existing `pin_weights` test helper
still emits bare v1 with no block, unaffected by the new exclusivity check.

## Verdict: ALL ADDRESSED (I1, I2, I3, m1, and the controller ruling). No regressions found; both Python suites
pass under numpy 1.26 and numpy 2. C++ remains unbuilt per the lane's no-build rule — the gate must still build and
run `StrategyIcRunner.ThemeRedistribution*` and `StrategyIcComposition.*` before merge.
