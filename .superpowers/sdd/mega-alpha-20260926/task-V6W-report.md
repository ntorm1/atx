# Task V6-W report: composition `ew-theme-v6` and universe `linked-operating-v1`

**Status: DONE_WITH_CONCERNS.** The Python parts are implemented and tested. The C++ change needed for rule (d) is written but not compiled or run, because this lane has a no-build rule (concern 1).

**Where:** `C:/atx-wt/pool-4`, branch `feat/mega-alpha-v6-w-20260927`, base `04e9d5bc` (the pool-2 HEAD; it replaces the `e0dfb8c7` named in the brief).

| Commit | What |
|---|---|
| `2d37de31` | fitter: `--composition ew-theme-v6`, plus tests |
| `a0566920` | IC runner and composition: `within-theme-v1` redistribution, plus a new gtest (not built) |
| `a54f20b5` | `prepare_recent_research.py role --universe linked-operating-v1`, plus tests |
| `34006519` | `studies/v6_w.env.example` |

**Tests (pure Python, synthetic fixtures only):**
- `python -m unittest test_fit_composition_weights` (from `atx-impl/tools`): 80 tests, OK.
- `python -m unittest test_prepare_recent_research` (from `atx-engine/tools`): 11 tests, OK.

Nothing was built. No real data was run through the code. I read no 2023+ data and no VAL statistic. The only real data I read was TRAIN metadata:
- `build-equity/mega-weights-v51-ew/admission.json` (`4f06bd18`), for themes and standalone tau;
- the TRAIN role v2 and fields-v6 manifests, the identity-bridge and events manifests, and the v51 run pins and summaries, to get pins and memory figures.

## 1. Changes (file:line)

### Fitter: `atx-impl/tools/fit_composition_weights.py`

The rule text is at `:92-106` (docstring) and the constants at `:158-163`:

| Constant | Value |
|---|---|
| `V6_RULE_ID` | `"ew-theme-v6"` |
| `V6_DROPPED_THEMES` | `("low_risk",)` |
| `V6_MERGED_THEMES` | `{"options_implied": "short_interest"}` |
| `V6_FAST_TAU` | `0.08` |
| `V6_FAST_FACTOR` | `1/3` |
| `V6_REDISTRIBUTION` | `"within-theme-v1"` |

`ew-theme-v6` has been added to `PRIOR_COMPOSITIONS` and `COMPOSITIONS`.

`ew_theme_v6_weights` (`:1278`) implements the rule literally:
- **(a)** Members of `low_risk` get weight 0. Their row status becomes `fitted-theme-dropped-v6`.
- **(b)** Theme' = `options_implied` → `short_interest`; members are equal within the theme.
- **(c)** Each member starts at within-theme weight b = 1/n.
  - A member with tau ≥ .08 keeps b/3.
  - The freed mass, the sum of 2b/3 over the fast members, goes pro rata by b to the theme's slow members.
  - If a theme has no slow member, the mass returns pro rata to the same members, so their weights stay b.
- **(d)** Each weight is within/T, where T is the number of resulting themes.

The tau used in (c) is the admission row's value, `rows[k]["tau"]` (`:1761-1764`). That is exactly `admission.json` key `candidates[].tau` of the `status: admitted` row; the provenance cites it under `fast_tau_source`.

**Provenance** (`:1838-1843`, `v6_provenance` at `:1863`). Only `ew-theme-v6` writes two new keys:
- A top-level `theme_redistribution` block: `{rule: within-theme-v1, composition: ew-theme-v6, themes: {id: theme'}}` for every weighted member. This is what the runner reads.
- `provenance.v6`, which records:
  - rule id and preregistration reference;
  - dropped and merged themes and members;
  - `fast_tau_threshold`, the `>=` test and the 1/3 factor;
  - `fast_tau_source`;
  - `shrunk_members` and `shrunk_tau`;
  - fast members not shrunk (all-fast theme) and fast members in the dropped theme;
  - the formula for (d);
  - input SHAs: library, TRAIN manifest, recipe, orientations and their recipe, runner summary, admission, role source, fields manifest, VM identity, script, context.

If every member sits in a dropped theme, the fitter publishes the admission table only and exits 4.

**Tests** (`atx-impl/tools/test_fit_composition_weights.py`):
- `V6WeightRules` (`:2039`) uses a hand case shaped like v5.1. It checks:
  - weights sum to 1, every theme' carries mass 1/7, and (a) drops `low_risk`;
  - the (b) merge;
  - the (c) arithmetic: 1/4 → 1/12 for the two fast members and 5/12 for the two slow ones;
  - the all-fast theme is unchanged;
  - the `>=` boundary at exactly .08, the pro-rata split 1/9 vs 4/9, refusals, and a reference port of the runner blend.
- `V6EndToEnd` (`:2129`) runs the full fitter on a synthetic world:
  - the admission bytes equal the ew-theme-v1 run's;
  - the weights equal the independent port `ref_v6_weights` (`:1975`) applied to `admission.json` taus;
  - the block is readable by a Python port of the new C++ parser, `runner_themes` (`:2014`);
  - provenance and input SHAs are right, and neither v1 nor aim output carries any v6 key.
- `V1BytesUnchangedByV6` (`:2249`): see section 2.
- `test_declared_constants`: the `PRIOR_COMPOSITIONS` tuple now includes ew-theme-v6 (an intentional constant change).

### Rule (d) needs C++

Per-name coverage redistribution cannot be expressed as fixed per-candidate weights, so it lives in the IC runner. I kept the change as small as I could, and it is gated on a composition-id check.

**`atx-impl/src/strategy_ic_composition.{hpp,cpp}`:**
- `create(..., pinned_themes = {})` (`hpp:67`) and `ic_composition_working_bytes(..., themes = 0)` (`hpp:34`, `cpp:54`: +16 B per cell per theme, +0 when there are no themes).
- Validation at `cpp:86`: themes require pinned weights; there must be one theme per candidate; each index < 32; at least one weighted theme.
- `add()` (`cpp:178`): writes to `blend`, which is the theme's plane when themed and otherwise `result.signal` itself. The expression is unchanged. The theme's present-weight plane only updates when themed.
- `finish()` (`cpp:235`) folds each cell as `W_theme * sum_present(w s r) / sum_present(w)`. A theme with no present member adds nothing. Its planes are then released.

**`atx-impl/src/strategy_ic_runner.cpp`:**
- `composition_themes` (`:831`, called at `:906`): accepts only `rule == within-theme-v1` and `composition == ew-theme-v6`. It requires known ids, theme names matching `[a-z0-9_]{1,64}`, a theme for every positive-weight candidate, and 1 to 32 themes. Indices follow first appearance in library order.
- `admit(..., themes)` (`:609`, callers `:1981`, `:1986`) budgets the planes.
- `method_recipe(..., themed)` (`:244`, `:272`): the composition string and `composition_redistribution` change only when themed.
- The combined manifest gets `composition_redistribution` (`:497`) and the summary gets `composition_weights.redistribution` (`:920`), both only when themed.
- `score_role(..., themes)` feeds `IcComposition::create` (`:1758`, `:2079`).
- The usage text is updated.

**gtest** `StrategyIcComposition.WithinThemeRedistributionKeepsMissingMassInTheme` (`atx-impl/tests/strategy_ic_composition_test.cpp:249`) checks:
- hand values: themed `[-.25, 1/6, 1/6, -.25]` against fixed `[-.125, 5/24, ...]`;
- a fully present date equals the fixed-denominator blend, and nonmembers stay NaN;
- report-only coverage is unchanged;
- the pooled blend equals the serial one bit for bit, with 2 and 3 workers;
- refusals, and the working-bytes delta.

The fitter test `ref_within_theme_blend` uses the same numbers.

### Universe: `atx-engine/tools/prepare_recent_research.py`

The rule is in the module docstring and in constants at `:51-78`.

**Why a restriction pass, not a new projection.** `--universe linked-operating-v1` restricts an existing role (`--base-role`, meaning TRAIN role v2) instead of re-running the projection. So:
- every payload except `member.u8` is copied byte for byte;
- role v2's factor-break repair is kept;
- V6-U is a pure membership change against the v5 reference cell.

The default `research-prior63-usd-adv-topn-v1` (the `--cache` path) behaves exactly as before.

**The rule, applied at session t.** A base member stays only if all of the following hold, using only information visible by t 22:00 UTC:
1. **Primary-line issuer link, point in time.** The line has a primary (P) identity-bridge link. The code calls `prepare_research_fields.load_bridge` and `resolve_links` directly, so the link cells are the same ones the fundamentals and `grp_*` fields use: `start <= date(t) <= end_incl` and `available_at <= t` 22:00.
2. **Common class.** Every qualifying bridge row has `class_status == "common"`.
3. **Visible SIC.** The linked CIK has a SIC row with `accepted_utc < (t-1)` 22:00 and age ≤ 550 days. This is `load_events` / `advance`, the `grp_ff12` clock, so `finite(grp_ff12) == linked-P & visible SIC`.
4. **Operating company.** That SIC is not in `NON_OPERATING_SIC = (6189, 6221, 6722, 6726, 6770)` (`:58`).

**Code:**
- Pure classifier `classify_linked_operating` (`:480`). Drop reasons are counted in the order unlinked, ambiguous, secondary_line, class_not_common, no_visible_sic, non_operating_sic.
- `_bridge_class` (`:503`) reads `class_status`, aligned with `load_bridge`'s rows.
- `_check_fields` (`:522`), enabled by `--check-fields`, refuses unless the u pass's fields manifest satisfies all of these:
  - it is bound to the same role;
  - its lag is 1;
  - its `source_checks.issuer.link_member_cells` equals this run's counts. This is the key the review cites at `manifest.json:4582-4584`: `unlinked 1269334` of `member_cells 3220647`;
  - `finite(grp_ff12)` equals linked-P & visible SIC on every cell.
- `restrict_role` (`:557`) writes the new role.
- CLI at `:700`.

**Manifest.** It keeps every base key, including `membership_recipe`, because the C++ role reader requires the ADV rule string. `score_member_counts` is recomputed. A `universe` block (`:661`) holds:
- `id` and the rule text;
- `point_in_time` and `limits` statements;
- the base role pin;
- inputs: the bridge and SIC pins, with `class_status_rows` and the checks;
- `link_member_cells` and `fields_crosscheck`;
- `dropped_by_reason`, overall and for the score window;
- `base_member_counts` and `kept_member_counts` per session;
- `dropped_member_share` per session (1 - kept/base; null where there is no base member), plus totals.

**Tests** (`atx-engine/tools/test_prepare_recent_research.py:180`, `LinkedOperatingUniverse`):
- the classifier's reasons and precedence;
- the full restriction;
- point in time:
  - SIC accepted d100 12:00 is used only from t=101;
  - a link asserted late (d310 23:00) is used only from t=311;
  - a class change applies from the row's own start;
  - SIC older than 550 days drops;
- a later filing never changes earlier sessions;
- payloads byte-identical, manifest keys and per-day shares correct;
- the fields cross-check passes, and refuses on a tampered `grp_ff12` or on different counts, with nothing published;
- pin, overwrite and double-restriction refusals;
- CLI refusals;
- the default universe is unchanged.

### `studies/v6_w.env.example`

Path: `.superpowers/sdd/mega-alpha-20260926/studies/v6_w.env.example`. It is force-added because `.superpowers/` is gitignored. It holds:
- the knobs `COMPOSITION=ew-theme-v6`, `UNIVERSE=...`, `W_MAX_MEMORY_MIB=2304` and `W_MAX_RSS_MIB=2048`;
- the TRAIN pins;
- the exact u, fit and w command lines, with checks.

## 2. How ew-theme-v1 bytes are shown unchanged

**Python.** `V1BytesUnchangedByV6` loads the fitter blob at the base commit (`git cat-file -p bb11a677`, which is `fit_composition_weights.py` at `04e9d5bc`). It runs both the old and new fitter on the same synthetic inputs for:
- `v4-prior-v1` + `ew-theme-v1`;
- `v4-prior-v2` + `ew-theme-v1`;
- `v4-prior-v1` + `ew-theme-aim-v1`.

All files (`composition_weights.json`, `admission.json`, `admission.csv`) are byte-equal. The only substitutions are the embedded script SHA and the two SHAs derived from it (context digest and admission SHA), which cannot match by construction. The work-cache paths are identical too. The existing `V1BytesUnchangedByAim`, compared against the pre-T31 blob, still passes.

**C++ (not built).** The argument is by construction:
- **Weights file:** without the `theme_redistribution` key, `composition_themes` returns before touching anything. The v1 weights files `9a9c949a` and `198375f9` have no such key.
- **Blend:** the unthemed blend pointer is `result.signal.data()`, with the same expression on the same cells. The extra branch does no floating-point work.
- **`finish()`:** loops over an empty `theme_mass`.
- **Working bytes:** `themes == 0` adds 0.
- **Recipe, combined manifest, summary:** new keys and strings are written only when themed.

The existing exact-value and bit-equality gtests (`FixedFamilyBudgets...`, `PooledAddMatchesSerialBits...`, and the recipe-string asserts in `strategy_ic_runner_test.cpp:655`, `:1380`) will confirm this once the gate builds.

## 3. Root command lines

Merge these into `v6_train.sh`. Full versions with pins are in the env example.

**V6-W fit.** Uses library v5.1 and the v5.1 u pass (`$U51-1`: orientations `49fcfdbc…`, summary `857cebc0…`):
```
run $BR --output $W_V6-run$j --bind $L --bind $LR --bind $R2 --bind $O --bind $S -- "$PY" \
  atx-impl/tools/fit_composition_weights.py --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
  --orientations $O --orientations-sha256 $OS --runner-summary $S --runner-summary-sha256 $SS \
  --orientation prior --screen v4-prior-v1 --composition ew-theme-v6 --recipe $LR --recipe-sha256 $LRS \
  --work-dir build-equity/mega-fit-work-v6w --max-seconds 150 --output build-equity/mega-weights-v6w-v51
```

**V6-W w.** Needs a rebuilt IC binary and a higher memory bound:
```
$IC --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS \
  --output build-equity/mega-v6w-train-ew51-$i --max-memory-mib 2304 --min-names 1000 --workers 4 --save-combined \
  --candidate-cache $CC --composition-weights $W_V6/composition_weights.json --composition-weights-sha256 $WS
```
Then refuse the pass unless `train_combined.json`, `summary.json` and `recipe.json` all say `within-theme-v1`; the env example has the one-liner. The nav line is unchanged; only the combined differs.

**V6-U u.** Three steps:
1. Restrict the role:
   ```
   "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v1 \
     --base-role build-equity/recent-fast-train-2020-2022-v2 --base-role-sha256 210fff96…d1de \
     --identity-bridge build-equity/identity-bridge-r4-v1 --identity-bridge-sha256 ddf97164…baea \
     --sic-events build-equity/fundamental-events-v2 --sic-events-sha256 74ed9a50…dd71 \
     --check-fields build-equity/recent-fast-train-2020-2022-v2-fields-v6 --check-fields-sha256 32565c32…a7a8 \
     --out build-equity/recent-fast-train-2020-2022-v2-lo1 --memory-mib 1024 --max-seconds 170
   ```
2. Re-run the fields-v6 recipe on the new role. Fields bind the role SHA, so this is required. The exact field list and flags are in the env example.
3. Run the v51-style unweighted IC u pass on (role-lo1, fields-lo1).

Fit, w and nav then use the new role and fields pins, and nav needs `--role` / `--fields` of lo1.

Before step 2, check `min(kept_member_counts[399:])` against `--min-names 1000`; the env example has the one-liner.

**Preview of V6-W on v5.1** (my arithmetic on the TRAIN admission metadata `4f06bd18`; no statistic of returns):
- There are 7 themes, each with mass 1/7.
- **Shrunk:** `si_change` (.0915) and `iv_rv_spread` (.0898) go to .0119 each. `si_ratio` and `dtc` rise to .0595 each.
- **Not shrunk:** `ind_adj_rev_5` (.181) and `seasonality_same_month` (.094) are an all-fast theme, so they keep 1/14 = .0714 each.
- **Dropped:** `low_beta`, `low_ivol`, `low_max` and `lowvol_ind` (`low_max`, tau .0887, is fast and dropped with its theme).
- The fast-sleeve mass goes from .287 to .167.
- The weighted standalone tau goes from .0518 to .0439.

## 4. Concerns

1. **The C++ is not compiled and not tested** (no-build rule). The gate must build `atx-equity-strategy-ic` and the atx-impl tests and run `StrategyIcComposition.*` and the `strategy_ic_runner` suites. There is no runner-level gtest for the parsing refusals in `theme_redistribution`; only the Python port `runner_themes` covers the format. Worth adding at the gate.
2. **An old binary silently ignores the block**, because unknown keys are allowed. The weights would then be applied without redistribution. The env example's w-phase check refuses that case.
3. **Memory.** A themed w pass admits about 2076 MiB, against the 1536 used so far, so it needs `--max-memory-mib` of at least ~2100 (2304 suggested). Actual RSS should be about 505 + 694 MiB, so raise the bounded runner's `--max-rss-mib` for that phase.
4. **The combined manifest keeps the old `signal_semantics` string.** It still ends in `missing-or-unoriented-neutral-fixed-denominator`, because NAV C++ admits only that string and I may not edit NAV C++. The new `composition_redistribution` key states the real rule; this follows the `composition_signs` precedent. For ew-theme-v6 the old string is stale.
5. **Interpretations** (literal, but please confirm):
   - **(c)** Shrunk members keep exactly 1/3. The pro-rata reallocation goes to the theme's slow members only. `reversal_seasonality` is all fast on TRAIN, so for it (c) is a no-op and its theme weight rises from 1/9 to 1/7.
   - **(d) "present"** means the name has a finite signal on a day when the candidate ranks at least 2 names.
   - **(d) empty theme:** when no member of a theme is present, the theme adds nothing; its mass is not moved to other themes.
   - **Diagnostics:** `contribution_fraction` and the fitter's `blend_in_sample_TRAIN_diagnostic` stay raw and ignore the redistribution.
6. **Universe rule choices** to confirm before any TRAIN read:
   - The `class_status == common` requirement: I did not look at how common "unknown" is on TRAIN, because that would mean reading bridge rows that run into 2024. So the kept count per day is unknown and could fall below `--min-names` 1000; the gate is in the env example.
   - The `NON_OPERATING_SIC` set is my reading of "operating". It drops BDCs filed under 6726; REITs (6798) stay.
   - An ADR linked to a filing issuer with common-class evidence stays. V6-U drops ADRs without a link, and no point-in-time ADR flag exists in the u-pass inputs.
7. **V6-U side effects.** It needs a fields rebuild on the new role. `mkt_ret` then becomes the equal-weight market of the restricted members, and it feeds `ear`, `low_beta` and `low_ivol`.
8. **Work directory.** The fitter's script SHA changed, so the V6-W fit needs a new `--work-dir`. Records in `mega-fit-work-v51` never match now and would all be recomputed.

## Fix round 1

Commits on `feat/mega-alpha-v6-w-20260927` (pool-4), on top of 34006519:

| Commit | Item | Change |
|---|---|---|
| c126ef26 | I1, I3 | Fitter: ew-theme-v6 emits `atx.dsl-composition-weights/v2` (`WEIGHTS_SCHEMA_V2`); ew-theme-v1 / ew-theme-aim-v1 stay v1 with bytes unchanged. Runner: `composition_weights()` accepts v1 or v2; after `composition_themes()` it refuses "schema ...v2 requires a theme_redistribution block" and "theme_redistribution requires composition weights schema ...v2". An old binary refuses v2 through "composition weights schema/library identity". The usage text is updated. Two new runner gtests (see below). |
| c9bbfbbe | I2, ruling | `NON_OPERATING_SIC` gains 6792 / 6795 (royalty trusts); REITs 6798 stay. The docstring, the rule text and the classifier test are updated. The uint8 builtin `sum` is replaced by int64 counts throughout the restriction test. |
| 3481cd95 | m1 | `W_MAX_RSS_MIB` and the "raise --max-rss-mib" line are removed. `W_MAX_MEMORY_MIB=2304` is documented as the admit estimate only; the RSS cap stays 1536 MiB. The header says an old binary refuses v2. |

**New runner gtests** (in `strategy_ic_runner_test.cpp`; written but not built):

- `StrategyIcRunner.ThemeRedistributionRefusalsAndSchemaGatePrecedeAnyPayloadOrOutput`:
  - Admitted under `--plan-only`: v2 with one theme, v2 with two themes, v2 where a zero-weight candidate has no theme, and v1 without a block.
  - Refusals, each run both under `--plan-only` and as a full run, with payloads deleted, no output and no progress: v2 without a block, v1 with a block, a v3 schema, a non-object block, a wrong rule, a wrong composition, a missing `themes`, an array `themes`, an unknown id, an uppercase name, a hyphenated name, an empty name, a non-string name, a 65-character name, a missing theme for a weighted id, and zero weighted themes.
  - A 33-candidate library: 33 themes are refused and 32 are admitted.
- `StrategyIcRunner.ThemeRedistributionIsRecordedInRecipeSummaryAndCombinedOnlyWhenPinned`, with the weights plain v1, themed v2 and signed themed v2:
  - The recipe `composition` string is correct in both signs variants, and `composition_redistribution` is `within-theme-v1`.
  - The summary has `composition_weights.redistribution`.
  - Both `<role>_combined.json` files have `composition_redistribution`, and `signal_semantics` is unchanged.
  - The plain v1 recipe has no new key; apart from those keys it equals the themed recipe.
  - The themed blend is `2 * centered rank` for d >= 63.

**Tests run** (both Python files, under numpy 1.26.4 at `C:/Program Files/Python312/python.exe` and numpy 2.5.2 as `python` on PATH):

- `test_fit_composition_weights`: 81 OK under each. This includes the new `test_schema_v2_only_for_v6`, and the `runner_accepts` port now enforces v2 iff the block is present.
- `test_prepare_recent_research`: 11 OK under each.

**Not done:** no C++ build or run, per the no-build rule. The root must build `atx-impl` tests and run `StrategyIcRunner.ThemeRedistribution*` and `StrategyIcComposition.*`.

Concern 2 above is resolved by I1. Concern 3 is superseded by m1: the RSS cap is not raised.

## Fix round 2

**Verdict: a fixture bug, not a code bug.** Commit cdeb4709 is on top of 3481cd95.

**Cause.**
- `ThemeRedistributionRefusalsAndSchemaGatePrecedeAnyPayloadOrOutput` wrote every case to `weights.json` and pinned `cfg.composition_weights_sha256`, but it never set `cfg.composition_weights_path`.
- `run_ic`'s first check is `composition_weights_path.empty() != composition_weights_sha256.empty()`. That is the existing path/SHA pairing check, already covered by the "unpaired option" case of `InvalidCompositionWeightsRefuse...`.
- So the check refused every run in that test as "IC runner: bounded config" before the weights parser ran.

**Why this is not a code bug.**
- The sibling test `ThemeRedistributionIsRecordedInRecipeSummaryAndCombinedOnlyWhenPinned` sets both fields and passed at v6-1, so the v2 accept path works.
- The real w pass passes both `--composition-weights` and `--composition-weights-sha256` (`v6_w.env.example`), so it is not affected.

**Fix.** The test now sets `cfg.composition_weights_path` to `weights.json` once, next to where the SHA is pinned. The test's intent is unchanged: the schema gate and the refusals all come before any payload read or output.

**Note on the round-2 failure report.** The same missing path affected every `run_ic` call in this test. It should also have failed:
- the refusal `EXPECT_NE(...find(reason))` at the old line 833;
- the 33/32-theme checks.

At v6-2, check the whole test, not only lines 788-792.

**Not done:** no build or run, per the rules.
