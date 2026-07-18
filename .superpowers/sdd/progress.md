# atx-vol Hot-Path Review Remediation — SDD Progress

Controller: Claude Fable 5
Repo root: C:\atx (atx-vol sources under atx-vol/)
Branch: main (user-authorized in-place)
Plan: docs/superpowers/plans/2026-07-16-atx-vol-hotpath-review-remediation.md
Review: atx-vol/docs/reviews/2026-07-16-hotpath-sprint-midpoint-code-review.md
Base commit at start: 5ba7fe4

OUT OF SCOPE this session: W3.3, W3.4, W4.2, W4.3, W4.5, W5.x, R-19 snapshot-cache identity.

BUILD TRAP: never run Debug and Release builds concurrently (shared C:\atx-cache\deps).
Implementers run strictly serially.

## Task ledger
(none complete yet)

## Minor findings roll-up (for final review triage)
(none yet)

Task 1: complete (commits 5ba7fe4..0a5b7dc, review clean — Spec ✅, Approved)
  Wired W3.1 shared-boundary de-Am into the Configured/Hft route (R-01 part 1) + enabled
  put side for negative borrow (R-09). Shortcut mask hoisted and single-sourced; rows are
  partitioned shortcut|shared|scalar with an Err on overlap. 10/10 focused tests green.
  Verified by controller: T1's 3 new tests red at base, green after; area gate 78/78.
  Minor (roll-up): report §3 overclaimed a regression guard — corrected in 0a5b7dc.
  Concern (carried): call side never shares under Hft (shortcut claims it wholesale) —
    realised win is put-side only, ~half the naive ceiling. Not a defect; don't over-attribute.
  Concern (carried): NO real-OPRA measurement — 411.783ms/1.78x SPY headline unverified,
    because OpraBreadthCorpus is red at base (see known-red list).

KNOWN-RED AT BASE 5ba7fe4 (verified by controller, NOT ours):
  SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily
  PricerFitterTest.LocalRiskRefitPublishesCopyOnWriteGeneration
  OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard
  AllQualityModes/SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/{Latency,Balanced}
  (+ PreparedPortfolio...PinnedFingerprint, Debug-only, bit-identical hash pair)

PROCESS HAZARDS LEARNED:
  - ninja can miss `git checkout -- <file>` restores -> stale object linked -> false red/green.
    touch restored files; confirm the `Building CXX object` line appears.
  - .superpowers/sdd/*.md is git-TRACKED (pre-dates the .gitignore rule). This session's
    scratch lives in .superpowers/sdd/hotpath/ to avoid clobbering other sprints' files.
  - Agents running `git add -A` swept an unrelated staged revert into a docs commit
    (b16ec45), which 7fca341 had to restore. Agents MUST commit only their own files.

Task 2: complete (commit 1426591, review clean — Spec ✅, Approved, zero findings)
  R-07 landed: lane acceptance now bounds the SUM |price-mid| + |price-embedded| <= budget
  (was each term independently -> admitted up to 2x the stated bound). Provably strictly
  stronger (new-accept subset of old-accept). Bonus: new predicate closes a NaN-evasion hole
  (old code compared fabs(price-mid) > budget; NaN mid made every IEEE compare false).
  Exposed detail::shared_lane_residual_within_budget (precedented: detail::gauss_legendre,
  detail::cheb_node) because finalize_shared_lane has internal linkage. 99/99 Debug + Release.

  R-08 NOT IMPLEMENTED - controller adjudicated the review finding INVALID as specified:
    The certificate probe point in the review (price_internal_put(Kp_ref, Kp_ref, sigma), ATM)
    is the MAXIMUM-vega point, where 17-pt grid step dP ~= vega*dSigma ~= 2 vs ~1e-5 interp
    error - 5 orders from ever tripping. The review's own concern was about LOW-vega regions,
    so the specified probe structurally cannot show the defect. Controller verified the
    dimensional argument. Implementing as specified = permanently-dead code + a counter
    reading 0 forever, which reads as evidence of health. Correctly refused.
    Measured reality: real wiggles exist (-3.1e-04 at K=110) but only in low-vega wings;
    worst per-lane IV error vs exact scalar on a sigma in [0.15,0.8] smile fixture = 5e-08,
    ~2000x inside the 1e-4 bound; wiggle regions already excluded by budget>0 + bracket-sign
    gates. R-08 retired as "investigated, not a live risk". Evidence to be PINNED as a
    regression test by Task 3's smile fixture (added to that brief).
  R-32: documented the counter mechanism rather than enshrining the review's unverifiable
    "~2x" undercount figure (likely itself an undercount: each sentinel inversion runs ~10-30
    cold solves, not one). Accepted.
