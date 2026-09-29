# Brief: task W0-3

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task W0-3: pre-registration and ledger event

**Files:**
- Create: `.superpowers/sdd/platform-v8-20260929/v8-prereg.md` (text in Appendix A of this plan)
- Modify: `.superpowers/sdd/platform-v8-20260929/progress.md`

- [ ] **Step 1:** copy Appendix A into `v8-prereg.md`; fill the role and fields manifest SHAs from W0-2.
- [ ] **Step 2:** append one line of kind `protocol` to `build-equity/trials.jsonl`: window id, owner ruling text, date, the
  SHA of `research_window.json`. It does not raise N.
- [ ] **Step 3:** commit with `git add -f` before any B0 cell runs.

