# P9-H prerequisite closure proposal after native attempt 1

Stopped record committed `da2c68b6`; no retry or additional research computation. This document proposes
metadata closure for PM adjudication; it does not change any template, registration, manifest, receipt or source.
Ledger remains134 / `b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61`.

## Four exact requires strings and existing evidence

| Exact retained string | Prior committed proof | Applicable current evidence and proposed closure |
|---|---|---|
| P9-H registered by the PM in the P9 prereg (plan section 5.2, DEC-15), after root's zero-trial diagnostic scripts/specs/p9/diagnostics/al-comb-q5-style-variance.json has run and its q / q_lit are printed in the prereg (lit section 8 Q5) | `progress.md:512,518`, ALCOMB-10 and PRE-6; `root-wave2-gate-report.md:552-602` item5, originally reported in M2g docs `d7f762cf`, current report commit `4dc831ac8def34c9135e0daa736f33d3ca6ff664`; prereg B.2 carries the diagnostic and registration decision. | The registered Q5 gate is already satisfied, confirmed by R15-ROOT3 and R15-H-LAUNCH. Its Y-F0 choice is grandfathered under ALCOMB-10/PRE-6; no re-evaluation on accepted L is proposed. Native preflight binds this H registration commit818e0750 and accepted L parent. Propose discharge as satisfied. |
| lane AL-COMB's combined round reviewed and merged by root (Rulings ALCOMB-5 / ALCOMB-5a: D2's head merged into the lane, the theme-cov-hedge-v1 row in D2's K-P9-9 params_schema form, the fixture regenerated from the merged C++) | `task-AL-COMB-rereview-1.md` APPROVE of efb7ab2e: D2 mergeaca3570d, fixture6ec102fb; copied into root in5061579d. Root merge9d518317, `handoff-root-m2e2.md:37-43` committed3e283275 verifies merged fixture/listing and checks. Progress447/488 records review and merge. | Read-only git ancestry confirms9d518317 and6ec102fb are ancestors of current HEAD. Reviewer commit098e6ccd itself is not an ancestor (review document was copied into root at5061579d); this is disclosed, not misrepresented. Native preflight successfully validates the rule against frozen IC/NAV tables and derives its flag. Propose discharge as satisfied. |
| wave_kinds.COMPOSITION_FLAGS maps theme-cov-hedge-v1 to {fit: {--theme-hedge: theme-cov-hedge-v1}} (lane E2's table; AL-COMB merge request) | Current committed `scripts/wave_kinds.py:121`, last file commit0c9f75d94c451903182f12047b1483ddf8b7d4dd, is exactly this mapping. Progress562 records E2 merged/verified and MR-4 present. | Attempt1 `01-preflight.json /outputs/kind/flags` is exactly `{"fit":{"--theme-hedge":"theme-cov-hedge-v1"}}`; `/outputs/kind/tables/ic`179f2e8d4f6f6994bf314250f0b69b8bda1c9b5ef3c1b24a3c491685e0136aec, NAV74919056d7f3b2f04341c3b661fecd8c091a3dc716e7349271e2feec4e1b5af4. Generated committed21c78bfd spec contains the derived flag and frozen exe pins. Propose discharge as satisfied. |
| root sets parent to the last accepted cell whose fit is --composition ew-theme-std-v1 or ic-shrink-v1 (with or without --theme-erc, --theme-tsmom or --mom-volman) and carries neither theme-resid-v1 nor two-speed-v1, and runs the cell as an atx.research-wave/v2 kind rule wave whose rules block is [{name: theme-cov-hedge-v1, params: {}}] (Ruling ALCOMB-1) | PM accepted L and released H in R15-ROOT3; plain L specac903247 SHAde44a668999ab8997c7118afb52efd93551ac2e7df93efe9118edbf719aa9a4d. Registration818e0750 sets exact parent and embeds the registered v2 kind/rules fragment. | `cells/logs/p9h-resolved-plan.txt` shows L fit ew-theme-std-v1 + theme-erc-v1, no resid/two-speed, flag-absent. Attempt1 preflight binds L spec digestab85fa22561b1c38965f88491097ca92e9594d0cd610b255e3ca821beb445ddd, its published NAV and v15. Rules params remain{}. Propose discharge as satisfied. |

These are textual preconditions already evidenced independently of H returns. The guard is functioning as
written: `cycle/cli.py:397-399` -> `research_spec.py:248-259` refuses nonempty requires; `wave_steps.py:253-267`
copies those strings. No generic suppression of requires, hidden bypass or weakening of the driver is proposed.

## Supported restart and minimal JSON delta proposed

Reusing the original wave state after changing the template/manifest would be stale: `wave_stage_preflight.py:26-34`
pins manifest/template hashes; `atx-engine/tools/stage_chain.py:257-268` rejects changed input bindings and explicitly
offers a new state directory. The spec stage also refuses an existing generated file whose unpinned document
differs (`wave_stage_util.py:93-106`). Preserve all originals exactly: original registered template/rule/manifest,
generated21c78bfd spec, the eleven first-attempt files and their hashes. Do not delete, move, rewrite or re-date them.

Proposed two new code files, no existing code/spec mutation:

1. `scripts/specs/p9/templates/theme-cov-hedge-ready.json`: byte-formatted clone of the original registered
   `theme-cov-hedge.json` at818e0750, with exactly `/requires` changed from the four strings above to `[]`.
   All scientific keys, parent, description, output paths, constants and predictions stay identical. Name stays
   `p9-theme-cov-hedge` in this reusable template; the wave owns the derived cell name.
2. `scripts/specs/p9/waves/p9h-cov-hedge-attempt2.json`: clone of the original manifest with exactly four JSON leaves:

| JSON pointer | Before | Proposed after |
|---|---|---|
| /out_dir | build-equity/waves/p9h-cov-hedge | build-equity/waves/p9h-cov-hedge-attempt2 |
| /rule_cell/template | scripts/specs/p9/templates/theme-cov-hedge.json | scripts/specs/p9/templates/theme-cov-hedge-ready.json |
| /rule_cell/template_sha256 | 1f56c19428b7b62de9788e37ec21bf20a78f122e411fb8fb948a0cc0347ec082 | SHA-256 of the new requires-closed clone, computed only if PM authorizes it |
| /rule_cell/name | theme-cov-hedge-v1 | theme-cov-hedge-v1-attempt2 |

The wave id remains `p9h-cov-hedge`: same registered scientific cell, no candidate variation or second admission.
Expected N_c63, construction cap71, fields/parent/rules/acceptance/match/caps/driver options stay unchanged.
The original registration `rules/theme-cov-hedge-v1.json` and its historical manifest_fragment stay untouched;
PM would authorize the new ready-template binding as a metadata registration amendment, with this exact delta
shown in its commit body and adjacent PRE-40 brackets. New generated spec becomes
`scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2.json`, preserving the old generated file. Its downstream
physical output names remain original registered H outputs (all absent after attempt1); inherited L u output is
unchanged. Fresh native preflight/spec/lock validate the new route from scratch; all actual scientific phases
would run for the first time. No manual stage/receipt construction, no driver guard alteration, no new executable.

Native command after authorization would be the same CLI with the new manifest path. The read filter's WAVE
directory and read plan must point to attempt2, preserving attempt1 console/metadata logs. This is postprocessing
configuration, not research orchestration. The explicit `scoreboard --results` path must likewise name attempt2.
G-P1 zero-spec-edits-on-retry is **unmet** by this metadata re-registration; report the interrupted first invocation
and any authorized second invocation separately. No end-to-end pass is inferred from a successful later attempt.

## Narrow seal provenance proposal, not yet implemented

Actual hits are only compact20261003 at first-attempt consoles004/005 commit.log line1. Propose source class C
only when the hit lies within the exact standalone argv token
`:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache` on the first `$ git add ...` or
`$ git commit ...` command header of a numbered `*-commit.log` file. Match exact token boundaries and the
token's span; any other occurrence of20261003 remains OTHER, including another filename, data row, free text,
non-Git command or other line. A/B behavior remains unchanged. The checker must classify every actual hit before
any token allowance; any OTHER still stops. Proposed allowance text, only after that successful actual scan and
PM ruling: `--seal-allow 20261003=<PM ruling id>`; no prospective exemption for any other date/source.

Requested PM disposition: adjudicate all four closures, authorize (or revise) precisely the two-file metadata
clone delta and narrow source class C. Until then no mutation of these prerequisites, no retry and no next cell.
