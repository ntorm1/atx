# Brief: task B-2

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task B-2: field caps and worker cap

**Files:** `atx-impl/src/strategy_ic_library.cpp` (after B-3), `atx-engine/src/factory/ic_screen.cpp:287`, tests

- [ ] **Step 1:** tests `FieldCaps.Admits200RowManifestWith40Referenced`, `FieldCaps.RefusesLibraryReferencing257Fields`,
  `FieldCaps.V71FieldPlanUnchanged`, `Workers.OutputsByteIdenticalAt4And8And16`,
  `StrategyIcRunner.AdmissionReportsRequiredBytes`.
- [ ] **Step 2:** manifest row cap 64 -> 1,024; `FieldPlan` masks from `u64` to `std::bitset<256>`; worker cap 4 -> 16 in
  the runner and in `ic_screen.cpp`; the admission envelope per worker stays as coded.
- [ ] **Step 3:** root: v7.1 u and w byte-identical at 4 and at 12 workers; record vm, ic, composition seconds.

