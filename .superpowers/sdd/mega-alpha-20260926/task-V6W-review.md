# Task V6-W review: ew-theme-v6, within-theme-v1 C++ redistribution, universe linked-operating-v1

Reviewer: Opus 5.5 (read-only). Tree: `C:/atx-wt/pool-4`, branch `feat/mega-alpha-v6-w-20260927`, head `34006519`
(base `04e9d5bc`; `e0dfb8c7` is an ancestor of it). Inputs: task-V6W-brief.md, task-V6W-report.md, review-V6W.diff,
v4-prereg.md "## v6 revision", v6-code-review-signal.md I1/I2/I4, `.agents/cpp/agent.md`.

**What I did.** I edited nothing, built nothing and ran no real data. I ran the two pure-Python test files on their
synthetic fixtures. I read TRAIN metadata only, as numbers:
- the v51 w-pass receipt and summary: sampled peak RSS and admitted bytes;
- the fields-v6 manifest: `link_member_cells`, the grp NaN reasons and the field list;
- the v51 admission: member counts per theme.

Nothing from 2023 or later was opened.

**Result: 0 Critical, 3 Important, 10 Minor. FIX REQUIRED.** The four required items are short, and all are listed at
the end. The rule implementation (a)-(d) is literal and correct. The C++ math is correct, deterministic and
bit-neutral on the unthemed path. The universe classifier is point in time.

---

## 1. Spec compliance

### (a) drop low_risk: PASS
`fit_composition_weights.py:1278` `ew_theme_v6_weights` gives members of `low_risk` weight 0. Rows that were in `active`
get status `fitted-theme-dropped-v6` (`:1768-1772`). T is counted after the drop.

### (b) merge options_implied into short_interest: PASS
`theme' = V6_MERGED_THEMES.get(t, t)`. iv_rv_spread is then one of 4 equal members (b = 1/4) of `short_interest'`.

### (c) fast-sleeve shrink: PASS (literal)

**Where tau comes from.** `rows[k]["tau"]` (`:1761-1764`) is the same `screen_v4` row value that is written as
`admission.json` `candidates[].tau` (`:1680`). The value is identical; nothing is recomputed. Provenance cites the key
(`fast_tau_source`), and the env example checks the result against `mega-weights-v51-ew/admission.json` (4f06bd18).

**The test.** It is `>=`, with a boundary test at exactly .08.

**Arithmetic.**
- A fast member keeps b/3.
- The freed mass, n_f x 2b/3, goes to the slow members pro rata by b. All b are equal, so this is an equal split: each
  slow member gets b + n_f x 2b/(3 n_s).
- Theme mass is exactly 1 in real arithmetic (tested to 1e-15).
- In SI' this gives 1/12, 1/12, 5/12, 5/12. That is the reading under which "gets weight x 1/3" is literally true of the
  final weight.
- The alternative reading, x 1/3 and then renormalise over all members, would give 1/8 to each fast member. It is less
  literal, because the fast members would then keep 1/2 of their weight, not 1/3.

**All-fast theme (reversal_seasonality: ind_adj_rev_5 .181, seasonality_same_month .094).**
- The freed mass can only be "reallocated within theme" to the same members, so the within-theme weights return to b.
- (d) fixes the theme's weight at 1/T.
- The prereg text therefore leaves this theme unshrunk. "x 1/3 within theme" has no cross-theme effect, and "(d) theme
  weights equal across the resulting 7 themes" is binding.
- Consequence, now on record under the controller's ruling: the fastest signal rises from 1/18 (.056) to 1/14 (.071).
  Changing this would be a post-hoc rule edit. I concur with the ruling.

### (d) equal theme weights with coverage redistribution: PASS
- T = 7 on v5.1. The v51 admission gives members per theme' of value 4, prof 8, inv 2, earn 4, mom 4, SI' 4, rev 2
  (low_risk 4 dropped).
- Per name and day, a theme adds `W_t * sum_present(w s r) / sum_present(w)`. A theme with no present member adds
  nothing and its mass is not moved to another theme. That is the literal "member missing -> theme mass stays in theme".
- The IC runner applies this only when the weights file carries the `theme_redistribution` block. The runner also
  requires `rule == within-theme-v1` and `composition == ew-theme-v6` (`strategy_ic_runner.cpp:831-870`).

### Provenance: PASS
`provenance.v6` (`:1863`) holds:
- the rule id and the prereg reference;
- dropped and merged themes and members;
- the threshold, the test and the factor;
- `fast_tau_source`;
- `shrunk_members` and `shrunk_tau`;
- `fast_not_shrunk_all_fast_theme` and `fast_in_dropped_theme`;
- the per-member tau, the within-theme and theme rules, and the redistribution formula;
- the input SHAs: library, train, recipe, orientations and their recipe, runner summary, admission, role source, fields,
  VM identity, script and context.

The top-level `theme_redistribution` block (`:1839`) is written only for ew-theme-v6.

### ew-theme-v1 / ew-theme-aim-v1 bytes: PASS

**The test.** I ran `V1BytesUnchangedByV6` (`test_fit_composition_weights.py:2249`). It checks three cases against blob
`bb11a677`, which is the base fitter (verified with `git rev-parse 04e9d5bc:...`):
- v4-prior-v1 with ew-theme-v1;
- v4-prior-v2 with ew-theme-v1;
- v4-prior-v1 with aim.

All files and work-cache paths are byte-equal once three values are substituted: the script SHA, the context digest and
the admission SHA.

**Is the substitution acceptable?** Yes.
- The script SHA is embedded by design, and the other two are derived from it.
- The contract is the same as the already-accepted `V1BytesUnchangedByAim`.
- No C++ consumer reads these values:
  - The IC runner reads only `schema`, `library_sha256`, `weights`, `signs` and `train_manifest_sha256`, plus
    `provenance.orientations_sha256` / `fields_manifest_sha256` (`strategy_ic_runner.cpp:874-947`).
  - NAV only records the combined manifest's `composition_weights_sha256` as a binding
    (`strategy_nav_replay.cpp:1854-1866`).
- The published v1 weight files (`198375f9`, `9a9c949a`) stay canonical. A re-fit would give a new file SHA, and so new
  IC/NAV bindings, but identical values. That is expected, not a regression.

**C++ unthemed path.** It is bit-identical by construction:
- The blend pointer is `result.signal.data()`, with the same expression on the same cells.
- `finish` loops over an empty `theme_mass`.
- `themes == 0` adds 0 working bytes, so the admission is unchanged.
- Recipe, combined-manifest and summary keys are emitted only when themed.

### Old binary: guarded only by a manual check (see I1)
The env example's w-phase one-liner (`v6_w.env.example:87-90`) is correct: an old binary emits none of the three keys,
so it prints NOT APPLIED and exits 1. But it only protects a run if the controller copies it into v6_train.sh. The code
itself fails open.

### Universe linked-operating-v1: PASS on point in time

**Bridge links.** It reuses `prf.load_bridge` and `resolve_links`, so a row qualifies only if start <= d <= end_incl and
available_at <= mark(t). A late-asserted row (d310 23:00) is used only from t=311 (tested).

**class_status.** It is the qualifying row's own value. In r4 it is a "then-known common-share fingerprint and dated
vendor symbols" (`atx-db/src/atx_db/identity_links.py:547`). The only class taken from the 2026 snapshot sits on
`current_ticker_verified` rows (`identity_links.py:938-951`), and `load_bridge` excludes that basis
(`prepare_research_fields.py:1425`). A class change applies from the row's own start (tested).

**SIC.** It uses accepted_utc < mark(t-1) and age <= 550 days. This is `advance()`, the grp_ff12 clock, the same code as
`prepare_research_fields.py:1747-1753`. `--check-fields` asserts `finite(grp_ff12) == visible` on every cell and
checks `link_member_cells` against the fields manifest. Its keys match the real fields-v6 manifest: 3,220,647 /
1,269,334 / 1,946,643 / 4,670 / 0. A later filing never changes earlier sessions (tested).

**Manifest.** It records:
- `universe.id` and the rule;
- the PIT statement;
- the pins;
- `base_member_counts` and `kept_member_counts` per session;
- `dropped_member_share` per session, plus totals;
- drop reasons, overall and for the score window.

**No 2023+ data enters membership.** The role ends in 2022. Post-2022 bridge rows reach only a counter; see m5.

**Scope beyond the brief's wording.** The rule also drops secondary (J) common lines and lines with no visible SIC.
This follows the I1 fix sketch (`linked-P & finite(grp_ff12)`). On TRAIN these drops are tiny: 4,670 secondary member
cells, and 257 P-linked cells without a fresh SIC (grp_ff12 `no_visible_row` 34 + `stale` 223). The restriction is
therefore about the 39.4% unlinked, plus class-unknown, plus the non-operating SIC set.

**NON_OPERATING_SIC = {6189, 6221, 6722, 6726, 6770}.** Sensible and conservative:

| SIC | What it drops |
|---|---|
| 6189 | Asset-backed trusts |
| 6221 | Commodity ETP trusts; GLD/SLV/USO-type grantor trusts file 10-Ks under 6221 |
| 6722 | Open-end funds; rarely in FSDS, so they would also fail `no_visible_sic` |
| 6726 | UITs such as SPY/QQQ/DIA, closed-end funds, and BDCs coded 6726 |
| 6770 | Blank checks, i.e. SPACs |

What the set keeps:
- **REITs (6798).** CRSP shrcd 10/11 would drop them, but the prereg list (ETF/ETN/SPAC/ADR) does not name them, so
  keeping them is literal.
- **Royalty trusts (6792, 6795).** These are pass-through trusts, not operating companies. They are few, and could be
  added only before the u pass (m10).
- **6799.**

---

## 2. Code quality

### C++ redistribution (`strategy_ic_composition.cpp`)

**Correctness.** `add()` accumulates `w*s*r` into the theme blend plane and `w` into the present plane, for each ranked
cell (member, finite signal, at least 2 names). `finish()` adds `W_t * blend/present` wherever `present > 0`.

**Mass conservation.** The effective per-candidate weight at a cell is `W_t * w_k / sum_present(w)`. Summed over the
present members, this is W_t exactly in real arithmetic. In floating point the error is about 1e-16, because
`present >= min w`, about 0.012, so there is no amplification.

**NaN handling.**
- Only finite signals are ranked.
- Nonmember cells are never present, so they stay NaN (tested).
- A member with no present theme stays 0, as on the fixed path.
- No NaN or Inf can arise.

**Determinism.**
- Candidates arrive in library order.
- Each plane cell has one band writer.
- Themes fold in index order.
- Pooled equals serial bit for bit (gtest, 2 and 3 workers).

**Bounds.** Zero-weight candidates may carry any theme index, including 99, but they return before
`theme_blend[theme[index]]` is indexed. Weighted indices are < 32, and `themes = max + 1`. The planes are allocated
inside the existing bad_alloc try block and released before the target pass.

**/W4 /WX.** I see no narrowing, no sign-compare and no unused parameter in the new code:
- `std::max(usize, usize)` is fine.
- `static_cast<usize>(at - names.begin())` is fine.
- The braced literals in the test are constant, representable values.

The only nit is several new lines over 100 columns (`strategy_ic_composition.cpp:88, 96, 240`; m9).

### Memory: a real RSS increase, which fits the fixed 1536 MiB cap

**The planes.** 7 themes x 2 planes x 1155 x 5627 cells x 8 B = 727.9 MB, or 694 MiB. These pages are really committed,
because `assign(cells, 0.0)` touches all of them. The RSS increase is real.

**Why `--max-memory-mib` must exceed 1536.** Only because the admit envelope is conservative. The v51 w pass admitted
1,449,071,914 B (1382 MiB) against a sampled peak tree RSS of 529,510,400 B (505 MiB), per
`mega-v51w-train-ew-run1/receipt.json`. Themed admission is about 2076 MiB, so `--max-memory-mib 2304` is needed.

**Expected real peak.** About 505 + 694 = 1199 MiB, below the fixed runner cap of 1536 MiB with about 340 MiB of margin.

**Env example.** `W_MAX_RSS_MIB=2048` and "raise BR's --max-rss-mib" (`v6_w.env.example:16, 86`) are unnecessary and
contradict the standing ruling (m1). If the RSS ever does exceed 1536, the runner kills the run, and the fix is an
efficiency change. Two options: store the planes only for the decision rows, or accumulate into f32 present planes.

### Fitter provenance: complete (see §1)
One consequence to note: `weighted_standalone_turnover` and the in-sample blend diagnostic use nominal weights, not
redistributed ones. Both are declared.

### min-names 1000 gate
- `--min-names` only makes IC dates undefined (`objective_ic.cpp:328`). It never refuses, and the fitter's own floor is
  `MIN_NAMES = 50`.
- The env gate on `min(kept_member_counts[399:])` is a sound pre-read, but:
  - it overstates the consequence ("needs");
  - it hard-codes `score_begin = 399`, which is correct for role v2 but brittle.
- See m2.

### Tests: meaningful, with gaps
- **Fitter:** 80 tests, OK.
  - `ref_v6_weights` is an independent port, compared end to end on admission taus.
  - The hand case matches v5.1's structure.
  - There are boundary, all-fast, all-dropped and refusal cases.
  - The v1 bytes test runs against the real base blob.
- **Universe:** 11 tests, OK under the study interpreter (numpy 1.26.4). They FAIL under numpy 2.5.2, which is the
  `python` on PATH (I2).
- **C++:** the composition gtest is meaningful: hand values, fixed versus themed, pooled bits, refusals and the bytes
  delta. It is not built.
- **Runner parse and emit path:** no native test (I3).
- **Python `ref_within_theme_blend`:** it only checks itself (m3).

---

## 3. Findings

| # | Sev | file:line | Defect | Failure scenario | Fix sketch |
|---|---|---|---|---|---|
| I1 | Important | `strategy_ic_runner.cpp:874-884` (unknown keys allowed; schema v1 only) + `fit_composition_weights.py:1839` (v6 document still `atx.dsl-composition-weights/v1`) | **An old IC binary fails open.** Any binary without a0566920 (e.g. build tag v6-0 from C1 alone, or the deployed `647c71a7`) accepts the ew-theme-v6 file. It applies the (a)-(c) weights with the fixed denominator and without redistribution. NAV admits the combined, because `signal_semantics` is unchanged. | A V6-W TRAIN cell is published under rule (d) that was never applied. The pre-registered composition budget and the DSR N are spent. Only a manual one-liner in an example file (`v6_w.env.example:87-90`) catches it, and only if the controller merges it. | Fail closed at the source. The fitter emits `"schema": "atx.dsl-composition-weights/v2"` only for ew-theme-v6, so v1/aim bytes are unchanged. The runner accepts v2 iff `theme_redistribution` is present and v1 iff it is absent. Old binaries then refuse loudly with "schema/library identity". Keep the env check as a second guard. |
| I2 | Important | `atx-engine/tools/test_prepare_recent_research.py:283` | **The test is numpy-version dependent.** `int(sum(base_member[t, col[4]] ...))` sums uint8 scalars, which wrap under NEP 50 (numpy ≥ 2): got 324, expected 68. Reproduced with the `python` on PATH (numpy 2.5.2): `FAILED (failures=1)`. With `C:/Program Files/Python312` (numpy 1.26.4) it is OK. The production code is unaffected, because every count goes through `count_nonzero` / `int()`. | The gate runs the default interpreter, and the V6-U test file goes red. | `int(base_member[[t for t, s in enumerate(stale) if not s], col[4]].astype(np.int64).sum())`, or cast each term with `int(...)`. |
| I3 | Important | `strategy_ic_runner.cpp:831-870, 244-279, 497, 920` | **No native test covers the new runner parse and emit path.** `composition_themes` refusals and the three `within-theme-v1` keys have no C++ test. The w-phase check depends on exactly those keys. Only the Python port `runner_themes` exists, and it tests itself. agent.md §7 says no production code goes in without a test. | A key-name or plumbing slip (e.g. `themed` not passed to `save_combined_artifact`) makes a correct binary print NOT APPLIED, or makes a wrong one pass. It is found only on real data. | Extend `strategy_ic_runner_test.cpp:699` (`InvalidCompositionWeightsRefuse...`) with these refusals: wrong rule or composition, unknown id, missing theme for a weighted id, bad name, and 33 themes. Extend `:672` (`UnequalPinnedWeights...`) with a themed file: assert the recipe `composition` string, `composition_redistribution` in recipe, summary and combined, and the unthemed recipe bytes unchanged. The code can be written now and built at the gate. |
| m1 | Minor | `v6_w.env.example:16, 86` | `W_MAX_RSS_MIB=2048` and "raise BR's --max-rss-mib" contradict the ruling that the RSS cap is 1536 for every phase. The expected peak is about 1.2 GiB. | Merged as written, a phase runs under a longer cap than the RAM rule allows. | Delete both lines. Keep `W_MAX_MEMORY_MIB=2304` and document it as the admit estimate only. |
| m2 | Minor | `v6_w.env.example:45-47` | `[399:]` is hard-coded, and "the IC pass needs >= 1000" overstates the consequence: min-names only makes IC dates undefined. There is no pre-declared action if the gate fails. | A misread gate. Someone might tune min-names after seeing the counts. | Use `m['score_member_counts']`. State: "below 1000 on any scored day -> stop and report; min-names is not changed". |
| m3 | Minor | `test_fit_composition_weights.py:2108-2109, 2001` | The docstring says the numbers are the same as the C++ gtest, but b1 differs. Python has b1 on all 4 names, giving [0, 0, 1/12, -1/4]. C++ has b1 missing on name 0, giving [-.25, 1/6, 1/6, -.25]. `ref_within_theme_blend` only tests itself. | Misleading cross-reference. | Use the C++ inputs (b1 = [None, .5, 0, -.5]) and the C++ expected values, or fix the comment. |
| m4 | Minor | `test_fit_composition_weights.py:2175` | The (c) end-to-end assertion is inside `if rows[slow]["tau"] < threshold:`. | If the fixture's two taus tie, (c) is silently not checked. | Assert the precondition, then assert unconditionally. |
| m5 | Minor | `prepare_recent_research.py:577, 669` | `class_status_rows` (and, as in fields-v6, the `bridge_st` counters) counts every on-axis bridge row, including rows whose intervals start in 2023-2024. | A small count derived from post-TRAIN rows lands in a TRAIN artifact. It is not a statistic and does not affect membership. | Tally only rows that qualify on some role session (`t_lo < t_hi`), or document the precedent. |
| m6 | Minor | `strategy_ic_runner.cpp:489-497` | For a themed blend the combined `signal_semantics` still says fixed-denominator (report concern 4). NAV admits only that string. `composition_redistribution` does travel into the NAV summary through `source_bindings` (`strategy_nav_replay.cpp:1854-1866`). | Readers of `signal_semantics` alone are misled. | No code action. Disclose in the scorecard row for V6-W. |
| m7 | Minor | `prepare_recent_research.py:660` + `strategy_data.cpp:108-117` | The restricted role keeps `membership_recipe` = the ADV top-N rule, because the C++ reader requires it. Downstream artifacts therefore record a membership rule that does not describe V6-U. The restriction is traceable only via role SHA -> `universe` block. | A reader who trusts `membership_recipe` alone misreads the universe. | Disclose. A reader-accepted `universe` key is out of scope. |
| m8 | Minor | rule interplay (fitter (c) × runner (d)) | Two disclosures. First, per-name redistribution undoes the (c) shrink wherever a theme's slow members are missing: an SI-missing name gives iv_rv_spread the full 1/7, more than its v5.1 weight of 1/9. Second, whole-theme absence is not moved across themes, so I4's cross-theme coverage imbalance (fundamentals missing on unlinked names) is untouched by V6-W on the v5.1 universe. Only V6-U addresses it. | V6-W's result could be read as "coverage balance fixed". | Literal per the prereg. Disclose when interpreting V6-W alone versus V6-W × V6-U. |
| m9 | Minor | `strategy_ic_composition.cpp:88, 96, 240` (+ runner) | Several new lines are over 100 columns. | Style only. | Wrap to 100 columns where the file does. |
| m10 | Minor | `prepare_recent_research.py:557-559, 707` | `--check-fields` is optional. It is the only guard that the classification equals the fields' link/SIC semantics on real data. Royalty trusts (6792/6795) are not in NON_OPERATING_SIC. | A run without `--check-fields` still publishes. | Make `--check-fields` required for linked-operating-v1. The controller decides on 6792/6795 before the u pass; after it, adding them would be a post-hoc edit. |

## 4. Verdict

**FIX REQUIRED.** The implementer must address:
1. **I1:** a fail-closed guard against an old IC binary. Schema v2 for ew-theme-v6 documents only, and the runner
   accepts v2 iff `theme_redistribution` is present and v1 iff it is absent. Add a fitter test that v1/aim still emit
   schema v1 (the bytes test already covers this) and v6 emits v2.
2. **I2:** fix the uint8 sum at `test_prepare_recent_research.py:283`, and rerun under numpy ≥ 2 (the `python` on PATH)
   and numpy 1.26.
3. **I3:** add runner gtests for the `composition_themes` refusals and for the three emitted keys, plus unthemed recipe
   bytes unchanged. Unbuilt is acceptable; they build at the gate.
4. **m1:** delete `W_MAX_RSS_MIB` and the "raise --max-rss-mib" line from the env example, per the standing ruling.

m2-m10 are optional. m6, m7 and m8 are disclosures for the scorecard.
