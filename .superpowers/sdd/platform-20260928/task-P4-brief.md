# Task P4: wave-2 pre-registration text (read-only research; no code, no runs)

**Scope:** read-only everywhere. You write exactly one file:
C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/library-v7-wave2-prereg.md. Never build, never run IC / NAV / fit,
never spawn subagents, never read any return-conditioned statistic of any v7 candidate, never read validation / VAL /
2023+ statistics. Field coverage counts and field distributions on TRAIN 2020-2022 are allowed (they are not returns).

**Inputs (all under C:/atx-wt/pool-2 unless noted)**
- .superpowers/sdd/platform-20260928/library-v7-draft.md section 0 (conventions) and section 2 (wave 2).
- v7-prereg.md ("Library v7.0" paragraph: wave 2 = section 2 minus eap_8k = 4 trials: ins_opp, inst_best_ideas, ftd_fail,
  ea_overdue; "W5a field rules").
- task-W5a-report.md, task-W5b-report.md; atx-engine/tools/research_fields_sec.py, research_fields_holdings.py (the real
  field semantics: units, scaling, clocks, NaN rules, forward fill, sign conventions).
- build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9/manifest.json (63 fields; coverage).
- atx-impl/strategies/check_fund_ic_v6.py + the v6.1 generator (budget rules: fields per candidate, slots, lookback; the
  runner has a 64-field limit and lookback limits), atx-engine/src/alpha/registry.cpp (exact op spellings), task-W2-report.md.
- atx-db docs (read-only): C:/atx/atx-db/docs/ALPHA_PANEL_SEC.md, ALPHA_PANEL_SHORTFLOW.md, ALPHA_PANEL_STATUS.md.

**Deliverable: library-v7-wave2-prereg.md** (<= 120 lines), ready to be appended to v7-prereg.md by root:
1. For each of the four candidates: final DSL (exact op spellings; every field name exists in fields-v9), theme, tier,
   prior sign, citation, and each mechanical respelling versus the draft with its reason (e.g. if `ins_opportunistic_net`
   is already per share outstanding the division is dropped; whether `ea_days_to_expected` is signed; the ftd field's
   denominator). A respelling must be mechanical: decided by field semantics, never by a return.
2. Static budgets per candidate (fields read, estimated peak slots, lookback) against the checker's limits; flag any
   exception needed.
3. Coverage on member cells 2020-2022 per field used, and any data hazard (FTD stale window 2020-11-30..12-21, Reg SHO
   NaN, Form 4 classification start) with the rule that handles it.
4. Runner limits: fields-v9 has 63 of 64 slots; state whether library v7.1 (= v7.0 + these four) runs on fields-v9 as-is.
5. Acceptance text mirroring wave 1: wave whole, P2 paired S2 net dSR > 0 vs the reference AND mechanics; admitted but
   losing members disclosed, never dropped one by one; theme `ownership_flow` changes ew-theme-v1 from 9 to 10 themes
   when a member is admitted (state the weight change). Leave the reference cell as `<REFERENCE: v7.0 cell if wave 1 is
   accepted, else the v6.1 cell -- root rules before the run>` and the N as `<N>`.
6. Open questions for root (each with your recommendation).

Reply to the parent in < 15 lines.
