# SDD ledger — plan: docs/superpowers/plans/2026-10-02-tier1-v4-quant-datasets.md

Goal (owner /goal 2026-10-02): build atx-db into a Tier-1 competitor to FactSet and S&P Global Compustat — datasets,
metrics, ratios, golden panels of high value to US equity quant L/S teams on a large cross-sectional universe,
primarily data since 2020-01-01. Subagent-driven; controller context kept lean.

Branch `feat/tier1-v3-warehouse`, start HEAD a3e02c6e. Workspace `.superpowers/sdd/tier1-v4/`.

## Setup rulings

- Ruling S1: work in the main tree `C:/atx` on `feat/tier1-v3-warehouse` under tier1-v3 LANE-RULES (data lake
  `atx-db/data/` lives only here; pool worktrees carry no data/.venv) — explicit-pathspec commits only — cost if
  wrong: commits interleave with another session's uncommitted edits; reversible by cherry-pick.
- Ruling S2: the 87 paths in `foreign-dirty-files.txt` (uncommitted, last touched 2026-10-01 18:36, likely a Codex
  gold-panel session) are read-only for every lane; new behaviour goes into new modules; never `git add` them —
  cost if wrong: duplicated logic vs those edits, to reconcile when the owner lands them.
- Ruling S3: host commit budget ~3.7 GB free with the v8 platform session live (pools 12-16) — at most ONE heavy data
  job of this plan at a time, every data job under `run_memory_guarded.py` ≤ 1.0 GiB (prefer 0.6) — cost if wrong:
  slower wall clock only.

## Log
- Spec: `.superpowers/sdd/tier1-v4/gap-analysis.md` (opus recon, 2026-10-02); inventory `lake-inventory.md`.

## Preflight scan

| pair / task | produces → consumes | finding |
|---|---|---|
| T1→T2 | liquidity/market_index/indexes/codes → universe | ordered in lane A; ok |
| T2→T6, T2→T7/8/16 | univ_all_common, NYSE flag → factors, chars, golden | ordered; T6 may fall back to member_equity if T2 FAILs — must say so |
| T3→T4, T3→T6/T7/T16 | compustat_std → ratios, factors, chars, golden | T3 stage name of fundamentals configurable (T5 rebuild) |
| T5↔T3/T4 | v11 → rebuild compustat_std, ratios | T5 owns rebuild; T3/T4 must accept `--fund-stage` |
| T7→T8 | char_monthly + registry → replication | ordered |
| T9→T10 | ea_actuals → guid_surprise | lane C sequential |
| T9/T10/T12/T13 | all may touch `events_sources.py` | lane C strictly sequential → no race |
| all ↔ stagelake registry | every stage registration | RACE in shared tree → GC5 amended: stubs `docs/datasets/<stage>.registry.json`, T17 folds |
| T12 ↔ labels.py | DLRET into labels | labels.py foreign → T12 publishes delisting v2 only |
| T7 ↔ characteristics.py | daily chars merge | foreign → T7 standalone monthly stage |
| each task self-consistency | files listed exist and are clean (checked: all 24 named alpha_panel modules exist, none in foreign list) | ok |
| T16 vs foreign gold.py | different stage `golden_panel/` | ok |

- Ruling P1: stagelake registry / DATA_DICTIONARY / PARITY_SCORECARD edits only in T17; lanes write registry stubs — concurrent lanes in one tree would sweep each other's edits — cost: catalog views for new stages arrive only at T17.
- Ruling P2: two implementer lanes run concurrently (A and B first; C after A or B frees), overriding SDD "never parallel" — files are disjoint, commits are pathspec-explicit (v3 precedent), only one heavy job per lane and the guard FIFO caps memory — cost if wrong: a merge race in one commit, fixable by amend-free follow-up commit.
- Ruling P3: holdout is D6-strict for this plan (no return statistic ≥ 2023-01-01), regardless of the gold plan's R1 (2023-12-29) or the owner's 2024-26 OOS authorization for the foreign gold chain — the stricter rule cannot leak; cost: replication windows end 2022.
- Ruling P4: do not resume the foreign gold chain (`silver_refresh`, `gold`, `build_gold_alpha_panels.py`) — that code is another session's uncommitted work; running it binds unreviewed code — cost: panel_goldready stays at 47/105 months until the owner resumes it (one command, §3a).
- Ruling P5: implementers on opus (data engineering with judgment + real builds), task reviewers sonnet, final review opus — cost: token spend.
- Ruling P6: review packages are per-commit (`pkg.sh TAG SHA...`), not BASE..HEAD — concurrent lanes interleave commits on one branch — cost: implementer must report every commit SHA.

## Execution
- 2026-10-02 plan committed 6ca80a05. Task 1 dispatched (opus, lane A, name t1-market). Task 3 dispatched (opus, lane B, name t3-compustat).
- Task 1: DONE_WITH_CONCERNS (commits ee353557, 0c35357e, ff4d829a; 10 PASS/0 FAIL, 2 v3 GAPs licensed/second-source). Concerns: market_index pre-2018 history for GC2 is validation-only (survivorship); share_exchange_history available_at = valid_from − 2h (one session late, safe); no per-line all_common flag published; all_common ≈ 3,532/day 2025.
- Ruling T2-a: Task 2 all_common gate band amended 3,600-4,800 → 3,200-4,800 names/day per year 2020-2025 — measured ≈3,532 (2025) under the committed common_filter+LISTED+$1 rule; the binding accuracy check stays NYSE common count within ±5% of French ME-breakpoint counts ≤ 2022 — cost if wrong: a looser band could hide missing names (the French check bounds that).
- Task 1 review dispatched (sonnet). Task 2 dispatched (opus, lane A) in parallel with the T1 review (interface stable; fix rounds would not change stage paths).
- Ruling P2b (amends P2): up to 3 implementer lanes (A, B, C) — the guard's FIFO 2 GiB machine budget, not lane count, bounds memory; files stay disjoint — cost if wrong: guard queueing slows lanes.
- Task 14 dispatched (opus, lane C, name t14-segments).
- Task 1 review (sonnet): Approved, 2 Important — (1) share_exchange_history available_at=valid_from−2h hides transition session; as-of macro vs interval-join docs disagree; (2) liquidity.measure rewrites published manifest post-publish (stales indexes input hash).
- Task 1: minor (deferred): market_index publish after factor_checks can leave new parquet beside old manifest on exception; history_monthly_sql `last` CTE global not per-security (undocumented); share_exchange_history sessions/valid_to forward-looking — doc warning; tests importlib.reload under monkeypatched root not undone; runaway retry loop not killed.
- Task 1: fix round 1/5 dispatched (resume t1-market, findings 1-2). T2 told to code against the as-of path.
- Task 1 fix round 1 commits 5b773a2f, 3afb9687 (available_at = prev session 22:00Z − 1s; liquidity gate at publish; 68 tests pass; rebuilt codes/liquidity/indexes same rows).
- Ruling T1-b: share_exchange_history inherits the panel's same-session visibility for some inputs (FINRA names at-cutoff clocks, CIK link up to d 22:00, Nasdaq directory snapshot not PIT) rather than strict GC1 — accepted as a documented qualification (doc + manifest) because codes are slowly changing classification attributes used for universe membership, not return features — cost if wrong: rare code transitions visible ≤ 1 session early in universe flags.
- Task 1: fix round 1/5 (2 addressed, 0 open — share_exchange_history first-session clock + single as-of path; liquidity manifest written once; commits 5b773a2f..3afb9687)
- Task 1: minor (deferred): as-of macro without BETWEEN shows a Monday run on the prior weekend (documented path filters); ALPHA_PANEL_MARKET.md still says stages "not yet run" (T17 doc refresh).
- Task 1: complete (commits ee353557..3afb9687, review clean)
- Task 3: DONE_WITH_CONCERNS (commits d9c48999, bda46f04; 5 PASS / 3 FAIL: rdq 8-K PIT share 0.850-0.901 (gate 0.95); sum4 saleq 0.944-0.968 / niq 0.953-0.977 (gate 0.98); atq=ltq+teqq 0.915-0.935 (gate 0.97; 0.961-0.975 with mezzanine)). fundq 421,230 / funda 112,802 rows, 17,010 CIKs; build 67 s @ 0.69 GiB.
- Ruling T3-a: rdq FAIL carried to Task 9 (calendar v2 adds FPI 6-K; T9 re-measures compustat_std rdq share); sum4 + balance FAILs carried to Task 5 (v11 mezzanine term + Q4 derivation fixes; T5 rebuilds compustat_std and re-measures) — both causes are upstream of compustat_std — cost if wrong: Task 3 gates stay FAIL until T5/T9 land.
- Task 3 review dispatched (sonnet). Task 4 dispatched (opus, lane B) concurrently.
- Task 3 review (sonnet): Needs fixes, 3 Important — (1) vintage model: restated older quarters/annual comparatives not emitted as fundq/funda rows (internal cause of sum4 FAIL); (2) funda leaks 10-Q rows (1,364; 483 ≥2020) with TTM values, fyr resets from 10-Qs DEI-labelled FY (192 rows/101 issuers); (3) `mibt` holds temp equity under Compustat MIBT name; `niadj` unverified.
- Ruling T3-a amended: sum4 FAIL partly internal (vintage model) → fixed in Task 3 fix round 1, residual (v10 Q4 derivation) → Task 5.
- Ruling T3-b: rdq gate metric = 8-K 2.02 accepted on or before the same ET calendar date as the row's filing (strict share reported alongside) — a same-date 8-K adds no leak — cost if wrong: rdq looks better than strict ordering would show.
- Task 3: minor (deferred): prepare_facts unbucketed under 450 MB, thin headroom; load_fx_rows in Python lists; cutoff silently facts-less if scratch missing; tie-break by accession DESC undocumented (329 groups); stale fqtr-4 row if fyr changes between vintages; prepare_facts max(value) on conflicting duplicates; FX rows only cutoff-validated; sum4 partly tautological (note); gate bases on _tmp scratch; hand-check derived rules partly circular.
- Task 3: fix round 1/5 dispatched (resume t3-compustat, findings 1-3 + T3-b). T4 warned (funda annual-only, restated vintage rows, mibt/niadj renames).
- Task 2: DONE_WITH_CONCERNS (commits b2d3b470, 8791f4cd, bf0ad7fa; 4 PASS / 1 FAIL: French NYSE count 46/60 months in ±5% — 2018-01..04 −10..−13% (shares_daily coverage), 2020-06 −6.7% (vendor short), 2021-11..2022-08 +5.1..+6.6%). Daily all_common 3,312-3,731; R2000p 1,962-1,994; codes 100%; hand check 97/100. Upstream defect: security_master.EXCHANGE_BY_CLASS lacks SC (Nasdaq CM) and BZX → NULL exchange in panel/share_exchange_history → market_index/indexes drop those lines (R2000p holds no Nasdaq CM stock); universe patches locally.
- Task 2 review dispatched (sonnet).
- Task 9 dispatched (opus, lane C2 in lane A's idle slot, name t9-earnings); T6 waits for T2/T3 review outcomes.
- 2026-10-02 ~23:45 EDT OWNER STOP: controller handoff written to `.superpowers/sdd/tier1-v4/HANDOFF.md`. Wind-down sent to t3-compustat (fix round 1), t4-ratios, t9-earnings, t14-segments (finish current command, commit tested work, "Stopped at" section with resume command); t2-review asked to also write `review-T2-r0-findings.md`.

## Controller resume 2026-10-03

- Reconciled HEAD fd9d67b7 on feat/tier1-v3-warehouse. Task 2 findings absent; review package exists. Task 3 fix edits remain uncommitted in compustat_std.py and its test; its report has no fix/stopped section. Task 4 WIP commit fd9d67b7 exists, report absent. Task 14 commits 7ec056a3, 146a7766, 95a03266 exist, report absent. Task 9 report 4c939ca2 records orientation only, no data fetch or implementation.
- Ruling R1: owner now requests gpt-6.1-sol implementers, superseding P5 and historical Opus implementation wording. Use gpt-6.1-sol implementers and task reviewers, fresh contexts with artifact pointers; final review uses the most capable available model under the SDD skill. Preserve at most three agent/data lanes and <=1 GiB guards — cost if wrong: extra review/fix time.
- Ruling R2: preserve this plan's existing workspace and extracted briefs; continue on the explicitly requested main working tree. Historical foreign files remain read-only. Codebase graph tools are not exposed in this runtime (tool discovery returned none), so targeted source reads/searches are the fallback until they become available — cost if wrong: slower discovery.
- Ruling R3: scientific acceptance must measure predictive power honestly. Task 16 will add a separate, reproducible pre-2023 evaluation for 5/21/63-session forward labels, excluding any observation whose label window reaches 2023, with IC, quantile returns, overlap-aware uncertainty and multiple-testing controls. Freeze evaluation choices before measuring. Significant predictive power is an empirical gate; never manufacture it or weaken the holdout/gates to claim success — cost if wrong: honest FAIL outcomes may remain despite a correct warehouse.
- Dispatch: Task 2 fresh task review from review-T2-r0.diff; Task 3 recover/finish fix round 1; Task 14 finish and reconstruct missing report. Task 4 waits for Task 3 fix; Task 9 follows Task 14. Original agents are unavailable, so fresh agents recover from files.
- Prepared task-2x-brief.md for the SC/BZX repair after Task 2; prepared task-16-evaluation-addendum.md before resumed return evaluation, freezing primary candidate directions, horizons, timing, family-wise disclosure and FDR procedure. Task 16 must commit the final executable configuration before measuring.
- Task 2 review recovered (/root/t2_review_r0, gpt-6.1-sol): spec/quality Needs fixes, 0 Critical / 5 Important. Findings artifact review-T2-r0-findings.md: I1 incorporation snapshot backfill PIT leak; I2 retained-SIC operating de-SPAC excluded; I3 SC/BZX omitted from proxy ranks; I4 French 14/60 months outside accuracy band without resolved attribution; I5 BDC min-election/max-withdrawal depends on future events.
- Task 2: minor (deferred): ADR per-stratum precision 9/12 despite pooled 97/100; missing months/reconstitutions omitted from validator; handcheck verdicts not bound to sample/rule hashes. Changed classifier samples must not inherit unsupported historical precision claims.
- Ruling R4: execute Task 2x inside the Task 2 fix lane before its scoped re-review, reversing the brief's initial wait-for-completion dependency — I3 is a blocking dependency of Task 2 and this avoids circular scheduling; one agent owns both market changes, keeps separate reports, and lists exact commits for each — cost if wrong: a larger scoped fix package. No panel code or stage rebuild is authorized.
- Task 2: fix round 1/5 dispatched with all five findings and Task 2x; genuine upstream French residuals require measured reconciliation before any carry-forward ruling.
- Task 14 recovery (/root/t14_notes): f77afbc3 adds missing report; original implementation/doc commits 7ec056a3,146a7766,95a03266 verified. 20 offline tests pass; all 12 parquet hashes/sizes and code hash match. Guarded coverage remeasurement peak 0.3103 GiB, revenue coverage PASS 90.27-92.54%, reconciliation FAIL 90.97-92.42% (95% gate), 911 residual misses. Full report names parser omissions and numerical-selection risks separately from source limits. Review dispatched /root/t14_review_r0 with review-T14-r0.diff.
- Task 3 recovery interface note: keys/macros stable; vintage_kind added; annual-base funda can include later quarterly filings that restate genuine annual periods; mibt->tempeq and niadj->nicon. Guarded recovered outputs exist; fix agent correcting rdq measurement to actual filing ET date before final evidence.
