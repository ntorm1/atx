# p7 Sprint 1 — Deflation Gates & Honest Selection — Progress Ledger

Base: main @ 2eaf3da
Branch: feat/p7-s1
Worktree: C:\atx-wt\p7-s1

## Unit checklist (Sequencing order)

- [ ] S1-0 — Open ledger + field plumbing (GateConfig/GateVerdict/AlphaMetrics)
- [ ] S1-1 — DSR floor in AlphaGate::admit
- [ ] S1-2 — PBO ceiling in AlphaGate::admit
- [ ] S1-3 — require_split_stable gate in AlphaGate::admit
- [ ] S1-4 — Thread cumulative trial-count into cascade_gate_passes bound
- [ ] S1-5 — Reject-histogram layout pin + AdmitKind audit + telemetry

## Determinism contract
Inert defaults: min_dsr=0.0, max_pbo=1.0, require_split_stable=false, trial_count==1.
At inert defaults all outputs byte-identical to pre-sprint.
Byte-identity gate: atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*

## DESIGN DEVIATION (binding — flagged for whole-branch review)

The plan (S1-0, dependency map line 137, risk table) instructs adding `dsr`/`pbo`/
`split_stable` fields to `combine::AlphaMetrics`, asserting "neither struct is
serialized." This is FACTUALLY WRONG: `AlphaMetrics` is embedded VERBATIM in the
on-disk library record `library/record.hpp::AlphaDirEntry` (line 110), memcpy'd to
segment files (record.hpp:272) and pinned by `static_assert(sizeof(AlphaMetrics)==56)`
+ `static_assert(sizeof(AlphaDirEntry)==96)` (record.hpp:128,130). Adding 3 fields:
  (1) breaks those frozen static_asserts in `record.hpp` (NOT in my Owns set);
  (2) changes the on-disk segment bytes -> changes segment CRC + manifest version_id
      -> breaks byte-identity of every library golden/digest — FORBIDDEN by the p7
      determinism contract (verified: a build with the fields on AlphaMetrics failed
      the two record.hpp static_asserts).
`record.hpp` is out of my Owns set and a format-version bump is far larger than a
mechanical edit, so editing it is not authorized.

RESOLUTION (Option B, byte-identity-safe, fully in Owns fence): carry the three
per-candidate deflation scalars in a NEW non-serialized POD `combine::GateDeflation`
{dsr=1.0, pbo=0.0, split_stable=false} (gate.hpp, OWNED), passed to
`AlphaGate::admit(..., const GateDeflation& defl = kInertDeflation)` as a defaulted
trailing parameter. Every pre-S1 caller (omitting `defl`) gets the inert instance ->
byte-identical verdict. The plan's GateConfig thresholds, GateVerdict enumerators,
inert sentinels, insertion point, operators, and all four determinism classes are
implemented exactly as specified; only the CARRIER of the per-candidate scalars
differs (GateDeflation vs. the serialized AlphaMetrics). AlphaMetrics is UNCHANGED
(still 56 bytes). The library `verdict_for` path (library.hpp, out of Owns) does not
yet read GateDeflation — wiring it is a later/Sprint-7 concern and is out of fence.

## Progress log

S1-0..S1-3 are co-located in gate.hpp (the plan places all of GateConfig/GateVerdict/
AlphaMetrics-carrier plumbing AND the three admit() checks in one header) + one test
file, so they ship in ONE commit. Per-unit status recorded below; all gated together
(combine 129/129, factory oracle/golden/digest 18/18, all green before+after).
