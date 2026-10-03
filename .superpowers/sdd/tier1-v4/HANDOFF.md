# tier1-v4 controller handoff

## Live resume checkpoint (2026-10-03; goal ACTIVE, not stopped)

Read progress.md after this file; it and git log supersede the historical table below. New resume-contract.md applies alongside IMPLEMENTER/REVIEWER. Actual agents are gpt-6.1-sol, per owner. The SDD skill was found at C:/Users/natha/.claude/plugins/cache/claude-plugins-official/superpowers/6.4.1/skills/subagent-driven-development/SKILL.md. No callable codebase graph tools were exposed; targeted reads/searches are fallback.

Owner added explicit 13F bronze completeness and predictive ownership gold features, plus complete profitability/growth primitives and gold families. Read owner-coverage-addendum.md and the updated Task 4/5/7/11/16/17 briefs before dispatching those tasks. Commit 55041b89 preserves the core addition.

Three current agent lanes (list live agents before dispatching duplicates):

| Agent | Current work | State/artifacts |
|---|---|---|
| /root/t2_fix_r1 | Task 2 fix round 2 | Sourcecommits02f6d78b,ee104b2e;2921attempts/79handreserve. Final704classobservations,653incrows,49statusevents/11CIKs (48positive,1termination),655unknown;observationhorizon2022-11-15. Finalfulluniversebuildrunning; handvalidation+allfileSHA/sizeverification beforeconsumerrelease. Task2x complete. |
| /root/t4_ratios | Task 4 fix round 2 | Fix1a61ef6d3 reviewclosesI1-I3localdefects/no newblocker,26tests. I4retained0/6gates,24/66membercoverage,35/66primarychecked;31primary+4scores unverified. Fix2causalformula/sourcecertification, characteristiccorrelationandliteraturerangefailure attribution; finalGC3 waitsR9. |
| /root/t14_fix_r5 | Task 14 fix round 5 | Fresh GPT6.1-sol/xhigh. r4commits74320900,45a46ceb reviewedNeedsfixes: retainedI4/denominator plusnewI5unrelated/historicalfollowingprose cancertifywrongnamedlist. Exactrepros inreview-T14-r4-findings.md. Bound declaredlists+qualifiers, revalidate14cuts/bridges, finalsource/gates; sourceallocationunused,GC3waitsR9finalT2. Atcapcontrolleradjudicates residuals, no sixthfixdispatch. |

Latest completed reviews: review-T3-r2-findings.md Approved; review-T14-r4-findings.md and review-T4-r1-findings.md Needs fixes. Task3 is complete under recorded Task5/9 dependency rulings. Task9 still orientation only; follows Task14. R9 requires completed producer files and SHA/size verification before final downstream evidence; manifest hash alone is insufficient while source builds rewrite partitions. All remaining ordering remains below; Task2x is already complete, not a new dispatch.

R5 is critical: market_index --end originally bounded only validation and a resumed run still computed product aggregate returns through 2026. Owner's strict GC2 includes portfolio-return products. Agent corrected actual source aggregation bounds and rebuilt market_index through 2022, with regression; no post-2022 evaluation values were inspected. Carry this boundary to Tasks 6/8/16. Latest membership/code/value/coverage outputs remain authorized.

Task 16 must read task-16-evaluation-addendum.md: predeclared 5/21/63-session IC/quantile-return evidence, label windows entirely before 2023, multiplicity/overlap controls, honest statistical FAILs. Do not claim significance in advance. Integration refresh queue is in progress.md: final universe invalidates older coverage reports; Task 5 v11 rebuild invalidates Task 6 if built on v10; Task 9 rdq measurement must use final compustat outputs.

Controller artifacts include f5c91b4a,4d0b0d4d,0fa53b3e,e502f601,55041b89,037a73ff; newer bookkeeping may be pending. Foreign paths remain read-only. At most three active agent lanes; every real-data job <=1 GiB guard. Temporary model-capacity failures were retried on the requested gpt-6.1-sol, with state reconciliation. Continue autonomously, no routine approval questions.

## Historical handoff (2026-10-02 ~23:45 EDT)

Paste the prompt below into the next parent (controller) agent.

---

You are the controller for the atx-db tier1-v4 plan. Owner goal: build `C:\atx\atx-db` into a Tier-1 competitor to
S&P Global Compustat and FactSet — datasets, metrics, ratios and golden panels of high value to US equity quant
long/short teams on a large cross-sectional universe, primarily data since 2020-01-01. Use subagent-driven
development (superpowers:subagent-driven-development) and keep your own context lean: hand artifacts over as files,
never paste diffs or reports into your context.

## Read first (in order, nothing else until these are read)

1. Ledger `C:/atx/.superpowers/sdd/tier1-v4/progress.md` — rulings (S1-S3, P1-P6, T1-b, T2-a, T3-a/b), task states,
   deferred minors. Trust it and `git log` over any recollection.
2. Plan `C:/atx/docs/superpowers/plans/2026-10-02-tier1-v4-quant-datasets.md` (17 tasks; Global Constraints GC1-GC9).
3. Spec `C:/atx/.superpowers/sdd/tier1-v4/gap-analysis.md` §2 (items 1-13 = binding deliverables + gates) — skim; the
   briefs cite it.
4. This file's state table below; then each in-flight task's report "Stopped at" section.

## Workspace and tooling (all in `C:/atx/.superpowers/sdd/tier1-v4/`)

- `task-N-brief.md` (already extracted for N = 1..17), `task-N-report.md` (implementer writes), `global-constraints.md`.
- `IMPLEMENTER.md` — shared implementer contract; dispatch = one line of context + brief path + "read IMPLEMENTER.md"
  + interfaces from earlier tasks + rulings + report path. Implementers: opus, background, named (e.g. `t6-factors`).
- `REVIEWER.md` — shared reviewer contract; reviewers: sonnet, background; give brief, GC file, report, package path,
  and 2-4 named risks. Scoped re-review after each fix round (sonnet): findings list + fix package; verdict per finding
  ADDRESSED / NOT ADDRESSED + new breakage only.
- `pkg.sh TAG SHA...` — per-commit review package (`review-TAG.diff`). Lanes commit concurrently on one branch, so
  never use BASE..HEAD; collect the task's commit SHAs from the implementer reply.
- `foreign-dirty-files.txt` — 87 paths with another session's uncommitted edits (panel.py, characteristics.py,
  labels.py, ic_eval.py, export_impl.py, gold*.py, silver_refresh.py, top-level atx_db/fundamentals.py, …). Never
  edit or stage them (Ruling S2). Do not resume the foreign gold chain (Ruling P4).
- Fix loop per SDD: Important/Critical → resume the original implementer (rounds 1-3) with findings verbatim → scoped
  re-review → ledger line `Task N: fix round R/5 (...)`; minors → ledger `Task N: minor (deferred): ...`;
  completion → `Task N: complete (commits a..b, review clean)`.

## Environment rules

- Main tree `C:/atx`, branch `feat/tier1-v3-warehouse` (data lake `atx-db/data/alpha_panel/v1` lives only here; no
  worktrees). Commits: `git add <paths> && git commit -m "..." -- <paths>`, trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never `git add -A`.
- Host memory tight (commit free ~3.2 GB; a separate platform-v8 C++ session holds pools 12-16). Every data job under
  `../.superpowers/sdd/tier1-parity/run_memory_guarded.py` ≤ 1.0 GiB; ≤ 3 implementer lanes (Ruling P2b).
- Holdout D6 strict (Ruling P3): no return statistic on dates ≥ 2023-01-01.
- Registry rule (P1): lanes write `atx-db/docs/datasets/<stage>.md` + `<stage>.registry.json` stubs; only Task 17
  edits `atx_db/stagelake/*`, `DATA_DICTIONARY.md`, `PARITY_SCORECARD.md`.

## State at handoff (verify against ledger + git log + reports)

| task | state | commits | next action |
|---|---|---|---|
| 1 market infra runs | COMPLETE (review clean after fix round 1) | ee353557..3afb9687 | — |
| 2 universe | implemented, DONE_WITH_CONCERNS (4 PASS / 1 FAIL French NYSE 46/60 mo) | b2d3b470, 8791f4cd, bf0ad7fa | review was running (t2-review); findings should be in `review-T2-r0-findings.md` — if absent, re-dispatch the review with `review-T2-r0.diff`. Then fix loop. |
| 3 compustat_std | fix round 1 in progress when stopped (3 Important: vintage rows for restated quarters/annual comparatives; funda leaks 10-Q rows + fyr resets; `mibt`/`niadj` naming) + Ruling T3-b rdq metric | d9c48999, bda46f04 (+ any fix commits) | read report "Fix round 1"/"Stopped at"; finish fix → scoped re-review of the fix commits |
| 4 ratios | implementing when stopped | see report | finish after Task 3 fix lands (it was warned of schema changes: funda annual-only, restated vintage rows, mibt/niadj renames); then review |
| 9 EA actuals + calendar v2 + SUE | implementing when stopped | see report | resume per report; also re-measures Task 3 rdq share (same-ET-date rule) |
| 14 segments + notes | implementing when stopped (commits 7ec056a3, 146a7766 seen) | see report | finish gates/report; then review |
| 5, 6, 7, 8, 10, 11, 12, 13, 15, 16, 17 | not started | — | order below |

Stopped agents were told: finish the current command, commit tested work, write a "Stopped at" section with the
exact resume command. Check `git status` for any uncommitted non-foreign files they left and decide (finish via a
resumed/fresh implementer, never commit them yourself).

## Open issues to rule on / carry

- **Exchange-map defect (from Task 2):** `alpha_panel/security_master.py` `EXCHANGE_BY_CLASS` lacks FINRA class SC
  (Nasdaq Capital Market) and BZX → NULL exchange in the panel and share_exchange_history → Task 1's `market_index`
  all_common and `indexes` proxies drop those lines (the R2000 proxy holds no Nasdaq CM stock). `universe/` patches
  it locally. Recommended: a small follow-up task (security_master.py mapping + identity_v3 codes/market_index/indexes
  rebuild, then universe rebuild) — panel itself is foreign, so apply the mapping in the codes builder, not panel.
- Task 3 FAIL routing (T3-a): rdq residual → Task 9; sum4 residual (v10 Q4 derivation) + balance identity
  (mezzanine) → Task 5, which rebuilds compustat_std and ratios on fundamentals_v11 and re-measures.
- Task 2 French NYSE FAIL: 2018 months (shares_daily coverage), 2020-06 (vendor short), 2021-11..2022-08 +5-7%
  (SPAC boom suspected — reviewer was asked to check SPAC treatment).
- Owner decisions outstanding from tier1-v3 (`atx-db/docs/TIER1_V3_STATUS.md` "Owner actions"): merge branch to main,
  panel ME from market_shares, S2.1 denominator, consumer research lake restore. Foreign gold chain resume belongs to
  the owner.

## Remaining order

Lane A: Task 2 fix loop → exchange-map follow-up → Task 6 (factors/risk; needs 2 + 3).
Lane B: Task 3 fix loop → Task 4 → Task 5 (v11 + rebuild 3/4).
Lane C: Task 14 → Task 9 → Task 10 (needs 9) → 11 → 12 → 13 → 15 (sequential; they share events_sources.py).
Then Task 7 → Task 8 (needs 2, 3, 6), Task 16 golden panels (needs most), Task 17 catalog/scorecard (last).
Final: whole-branch review on opus with the ledger's deferred minors, one fix wave, then
superpowers:finishing-a-development-branch (merge is the owner's call).
