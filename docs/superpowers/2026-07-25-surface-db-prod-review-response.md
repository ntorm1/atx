# Response to the external code review of `feat/surface-db-prod`

**Date:** 2026-07-25
**Reviews:** `2026-07-25-surface-db-prod-code-review.md` (external, commit `342557b`)
**Supersedes the merge posture of:** `2026-07-25-surface-db-prod-status-3.md`

---

## The headline disagreement, resolved

Status-3 said the sprint was finished and the final review said **SHIP**. The external review
says **request changes** and that the branch should not be labelled production-complete.

Both are right about different questions, and the external one asked the better question.

- The internal final review was scoped to *this branch's diff against its own known-issues
  list*. Against that question, SHIP was correct and remains correct.
- The external review asked whether the thing earns the word **production** at the scale the
  design claims — roughly 4,000 symbols × 250 sessions, a million surfaces. It does not.
  Findings P-01 and P-02 are architectural and I verified both.

**I concede the label.** What this branch built is a correct, deterministic, resumable surface
database that has produced and served a real 51-symbol × 17-date artifact. That is a pilot,
not a million-surface production system. The status documents overclaimed by using the design
spec's word instead of measuring against it.

What does *not* change: the artifact is real, the carry mechanism is sound at the scale it has
been run, and `C:/atx-data/surface-db/prod-2026-07` is untouched and healthy.

---

## Verification method

Every finding below was checked against the source by the controller before any fix was
dispatched. **The severity a reviewer assigns is a claim like any other.** Two findings moved:

- **C-06 is worse than filed.** Graded High; it is a silent, unkillable hang, and its trigger
  is the `std::bad_alloc` path — which this project's own production run hit twice. Raised to
  the front of the fix queue.
- **C-01 had a second half the review missed.** Beyond the partial-overlap case, an
  *unreadable* destination also fell through to the destructive rewrite. Fixed with it.

No finding was dismissed without reading the code it names. Where a finding is deferred, the
reason is stated and is not "we disagree".

---

## Disposition of all 23 findings

### Fixed on this branch

| ID | Sev | Finding | Disposition |
|---|---|---|---|
| C-01 | Blocker | Partial-overlap migration deletes destination-only symbols | **Fixed** — validated union, destination-wins precedence, plus refusal to rewrite an unreadable destination (the half the review missed) |
| C-06 | High | Scheduler setup failure deadlocks the date drain | **Fixed** — scheduler termination published to the drain; failure propagates instead of hanging |
| C-04 | High | A wholly corrupt hive window exits 0 with an empty database | **Fixed** — an all-corrupt requested window now exits nonzero; partial corruption warns |
| C-07 | Medium | `2026-02-31` is accepted and silently normalised | **Fixed** — round-trip calendar validation |
| F-06 | Medium | "ACTUAL SPEND" is a sampled estimate | **Fixed** — relabelled `REALIZED ESTIMATE` with interpretable counts |
| P-04 | Medium | The frozen design spec contradicts itself on row-group pruning | **Fixed as an erratum** — §2 said "future optimization, not built now", §3 promised it was used. §2 is what shipped. Corrected the spec to match the code, marked as an erratum rather than silently rewritten |

### Deferred with the reason stated

| ID | Sev | Why it is not fixed here |
|---|---|---|
| C-02 | Blocker | A failed refit dropping its stored surface needs an **archive format change**. Already disclosed in the manual, and pinned by a test that fails by design if someone "fixes" it without confronting the argument. The review's own suggested mitigation — a coverage-regression gate — is smaller than the format change and is the right next step; see *Accepted, scheduled* below |
| C-03 | High | The carry fingerprint covers fit configs only, not `--r`/market inputs. Folding `r` in would silently re-fit every rewritten date of the existing production database on its next run. Disclosed at the fingerprint's declaration and in the manual, with a recovery procedure that a reviewer ran end to end |
| C-05 | High | Carried cells exempt a run from `is_total_fit_failure`. The exemption is deliberate — without it a healthy converged database exits 3 and the diagnostic advises a change that would destroy every surface in it. The tool names the ambiguity instead of judging it. A strict mode is the right answer; scheduled |
| C-08 | Medium | Auto-config failures lose their cause. Real; a structured outcome type is a contained but non-trivial change to a public struct |
| C-09 | Medium | No fsync-before-rename, no generation CAS. **Pre-existing and inherited from `main`** — `surface_archive.cpp` is untouched by this branch |
| P-01 | Blocker | The loader retains the whole requested window before population starts. Architectural: the batch return boundary has to become a bounded per-date pipeline |
| P-02 | Blocker | The per-date split calls the table seam once per symbol, each call rescanning every row. Needs sorted-span slicing at the seam. **Correction (whole-branch review):** this document originally filed P-02 under "architectural" alongside P-01. That is wrong and the word was doing rhetorical work. P-02 is a contained per-date change — one lambda plus one new seam overload. It is real work and correctly deferred, but it is not architectural, and filing it with P-01 let it inherit P-01's excuse |
| P-03 | High | Long failures commit no progress — the same boundary as P-01 |
| F-01 | High | Python cannot set `r`. Small and important; scheduled |
| F-02 | High | No force-refit / input-version invalidation. Depends on C-03's fingerprint |
| F-03 | Medium | `--max-absent` has no automated CLI test. Correct, and already the named first task of the next sprint. **Correction (whole-branch review):** the original reason given here — "no CLI test harness, so this needs a new test target with a process-spawning fixture" — is stale, and stale *because of a commit on this same branch*. REV-R3 fix-1 established that the way to test a CLI contract here is to lift the decision out of `main()` into a testable function, and it worked. Most of F-03 is now a small refactor, not a new test target. `verify`'s verdict is still an inline decision in `main()`, which is the actual remaining work |
| F-04 | Medium | Failed-cell state is not persisted, so permanent failures retry forever. Depends on the same format change as C-02 |
| F-05 | Medium | Both CLIs are behind `ATX_BUILD_EXAMPLES` with no install rules — confirmed at `atx-vol/CMakeLists.txt:213`, targets at `:433` and `:442`. Packaging, not correctness; and changing CMake gates while another session builds from the same presets is a cross-session hazard this sprint has deliberately avoided |
| F-07 | Medium | `atxvol` and `pyarrow` collide on Arrow DLLs on Windows. Documented in the Python README and the design spec. The real fix is `delvewheel`-style repair or static linking |

### Accepted and scheduled, not yet written

These three are the review's most valuable *tractable* suggestions and are the top of the next
wave:

1. **A coverage-regression guard (mitigates C-02).** Before replacing a partition, compare the
   candidate's coverage against what is stored; if coverage regresses, refuse the write unless
   an explicit override is supplied. This would have prevented the measured 95-surface loss
   without any format change, and it does not fire on a healthy converged run — the
   permanently-failing cells are *absent*, so nothing regresses.
2. **`r` and the production build spec in the Python binding (F-01).** Confirmed:
   `py_build_surface_db` never sets `spec.hive.r`, so Python builds at `r = 0` while
   production required `0.043`. Python genuinely cannot reproduce the database.
3. **A strict mode (C-05)** in which `scheduled > 0 && fitted == 0` exits nonzero regardless
   of carried cells, opt-in so the converged-database exemption stays the default.

---

## Findings I judged and did not simply accept

- **F-03's framing is right but its "required change" is not free.** Letting verification
  consume an expected-coverage manifest is a new file format, a new operator artifact to keep
  in sync, and a new way to be wrong. The cheaper 90% is the CLI test that proves the existing
  `--max-absent` contract works.
- **C-05's "ideally the default" is wrong for this database.** Making strict mode the default
  reintroduces exactly the permanently-red signal that the carry exemption was added to remove
  — the production database has three permanently-failing cells and would exit nonzero on
  every run forever. Strict mode should exist; it should be opt-in.
- **The review's guardrail 2** ("build into a fresh database root whenever anything may have
  changed") is sound advice and is strictly stronger than what the manual currently says.

---

## What an operator should take from this

The operational guardrails in the review's §"Operational guardrails" are correct and should be
followed. The two that matter most, unchanged by anything fixed here:

1. Use an explicit, independently checked `--r`. The default is `0.0` and a wrong rate
   destroys stored surfaces — measured, 95 in one run.
2. Compare the absent-cell list against the previous run's. A changed absent set is the signal;
   a constant one is the converged steady state.
