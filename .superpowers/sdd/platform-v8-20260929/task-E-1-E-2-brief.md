# Brief: task E-1-E-2

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task E-1: tracked research build script. Task E-2: Release adoption

**Files:** `scripts/research-build.ps1` (from the untracked `build-equity/mega-build.ps1`, root resolved from the script
path), `atx-impl/CMakeLists.txt:86-95`

- [ ] **E-1 step 1:** the script takes `-Tag`, `-Targets`, `-Preset equity|equity-rel`; refuses an existing tag receipt.
- [ ] **E-2 step 1:** root, on a quiet host: rebuild the three research executables under `equity-rel`; run u, w and NAV of
  the v7.1 cell with both builds. **Step 2:** adopt `build: "equity-rel"` as the spec default only if the daily CSVs,
  orientations and `train_combined` are byte-identical and CPU stages are at least 25% lower. gtests stay in the Debug tree.
- [ ] **E-2 step 3:** the e2e fixture runs after every build tag under both builds; the SHAs must agree (the canary).

