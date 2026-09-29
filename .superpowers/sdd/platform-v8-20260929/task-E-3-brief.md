# Brief: task E-3

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task E-3: tiny_world fixture and end-to-end test (lands first)

**Files:**
- Create: `scripts/tests/fixtures/tiny_world.py`, `scripts/tests/test_cycle_e2e.py`, `scripts/specs/tiny.json`
- Modify: `atx-impl/tests/CMakeLists.txt:54,84-111` (register the three strategy test executables with
  `gtest_discover_tests`, label `atx_equity_strategy`)

**Interfaces:**
- Produces `tiny_world.build(root: Path, seed: int = 7) -> dict`: writes a role of 448 dates x 64 names (score_begin 384),
  3 extra fields with a manifest, a 4-member library with 2 planted signals (true IC .05 at h 21), 1 noise member and 1 copy
  of a planted member, a registry and `tiny.json`. Returns the manifest SHAs.
- Consumes K3 (`--no-git`).

- [ ] **Step 1:** write `test_cycle_e2e.py`: skip unless `ATX_EQUITY_BIN` is set; run `research_cycle.py run tiny.json
  --root <tmp> --no-git`; assert exit 0, golden SHAs of `orientations.json`, `admission.json` and the primary daily CSV;
  assert the copy is rejected as redundant; assert a second run reports every phase as up to date.
- [ ] **Step 2:** run, see it fail (no fixture).
- [ ] **Step 3:** implement `tiny_world.py` with `numpy.random.Generator(PCG64(seed))`; returns are
  `r = 0.05 * z_planted / sqrt(21) + noise`, noise sd .02.
- [ ] **Step 4:** root runs the test with the Debug executables, records the golden SHAs, commits them.
- [ ] **Step 5:** commit `test(platform): tiny_world end-to-end fixture and ctest registration`.

**Acceptance:** under 15 s; same SHAs on a second machine-clean run; `ctest -L atx_equity_strategy` lists the 243 tests.

