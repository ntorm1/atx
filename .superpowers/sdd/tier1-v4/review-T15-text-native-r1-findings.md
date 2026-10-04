# Task15 native text/TNIC scoped repair1 review

Scope: only commit `dab239f553dcc1171b07637a3f1bcb00e58d2317`, isolated
`review-T15-text-native-r1.diff` (1157lines), prior findings1â€“7 and new breakage
in that repair. Whole Task15 repair1/5; no reset or completion claim.

**Spec verdict: PASS for this scoped repair.** All seven implementation findings
are addressed. Original source qualification, full yearly coverage gates and
delivery acceptance remain pending; this does not establish Task15 completion.

**Quality verdict: PASS for this scoped repair.** No new Critical or Important
finding identified in the changed paths. This review does not approve our own
Task17 helper75efb231 or any Task17/golden integration.

## Prior findings disposition

1. **Important, ADDRESSED â€” tnic_native.py:78,87,114â€“117,143â€“145.** Raw corpus
   qualification/clock and encoded histogram qualification/clock are separate.
   Year-y encoded state includes raw y-1 DF clock and provenance; subsequent
   calibration stores that encoded clock/digest. Missing2018 cannot qualify2020;
   late2018 remains in2020availability. Raw DF completeness is not incorrectly
   made dependent on historical SIC used only for histogram calibration. The
   missing/late prior-cohort focused fixtures directly cover the initial repro.

2. **Important, ADDRESSED â€” tnic_native.py:54â€“72.** Complete original inventory
   is retained separately. Native raw vocabulary must meet original MIN_WORDS50
   before entering fitting tokens, and eligible-before-latest selection matches
   original tokenize_pass/_load_year. A latest short filing cannot displace an
   earlier eligible filing; forty short records do not change the DF denominator.
   Short/excluded records remain in complete corpus provenance and source checks,
   and published linked/member denominators are not shortened. Named outside-
   diff checks were original tnic.py:210â€“267 and filing_text.py:58 only.

3. **Important, ADDRESSED â€” text_features_native.py:298â€“347.** Publication stores
   target manifest/plan and previous active-pointer byte identity/value before
   rename. Existing-generation recovery requires independently expected manifest
   and transition SHA, full original replay and exact transition target. It
   rejects intervening pointers, preserves previous-pointer rollback history and
   is idempotent for the exact already-active target. It no longer returns
   successful recovery while leaving the intended generation inactive. Retained
   process-kill locks still require controller reconciliation; no unsafe cleanup.

4. **Important, ADDRESSED â€” text_features_native.py:377â€“414.** Public admission
   includes source_root and runtime. Bounded iteration checks original manifest,
   immutable source-plan identity, complete admitted source/code/runtime inventory
   and product before/after each batch, and in finally. Explicit early close
   executes the full checks. Focused fixture changes original source bytes,
   actual pinned helper code and runtime; each is refused. Caller must explicitly
   close when stopping early, as documented. Original full replay is retained.

5. **Important, ADDRESSED â€” filing_text_native.py:63â€“68,152â€“180,247â€“255,358.**
   Original Accepted index body must name exactly the accession and both issuer
   and archive-owner CIKs. Original issuer/owner submissions must contain that
   accession; absent owner correspondence clears the public clock and retains
   unknown_acceptance_owner_chain. URL/receipt/hash matching alone cannot qualify
   wrong-page evidence. EX13 index semantics also use the fixed identity check.
   DST ambiguity and raw trueUTC/ET-wall-clock cross-check remain unchanged.

6. **Important, ADDRESSED â€” filing_text_native.py:197â€“204;
   text_features_native.py:257â€“266.** Identity retains actual available_at and
   supports only original strict/dated/name tiers effective on the monthly cell,
   known strictly before same-session22UTC. A single distinct CIK must agree
   with any supplied member CIK; supported identity resolves NULL members.
   Ambiguous/conflicting/unresolved cells remain full population and are excluded
   from qualified text coverage. Unresolved identity also blocks TNIC gate PASS,
   avoiding a falsely improved ratio from lost linked filers. Other universe
   cells receive the same resolution without shrinking their denominators.

7. **Important, ADDRESSED â€” filing_text_native.py:52â€“60,358;
   text_features_native.py:47â€“83,112â€“115; tnic_native.py:38â€“39,59â€“60.** Current
   primary/EX13 byte and word limits apply before profile/token allocation;
   current plus retained issuer profiles are checked before output. Original SIC
  16MiB/receipt1MB and accession-index5MB bounds precede reads. TNIC vocabulary
   has word/character limits. The native500000word/current and EX13/SIC focused
   fixtures cover the previously unbounded paths. Large-corpus actual memory
   and runtime remain unmeasured; these checks do not assert an actual gate.

## Checked invariants and evidence limits

Read isolated diff in documentation, source and focused-test passes; read original
Task15 brief/continuation brief, report's repair1 section, global constraints,
foreign-protected list, R21/R24 and binding gap-analysis item13/section3h. Named
outside-diff checks were only current changed-function line references and the
original vocabulary/token/selection and code/source-plan guards needed to assess
this repair. Graph tools unavailable; targeted reads/rg used. No broad source
crawl, actual source/hash/metadata job, HTTP, return measurement, implementation
mutation, git/index action or subagent occurred in this review.

Full immutable original-byte replay and full population/orphan/schema checks are
unchanged. Source/code/runtime expectations are independently pinned. Strict
binary-cosine/prior-yearDF25/SIC3/HIST_BINS20000/MIN_WORDS50/strict>threshold method
remains required reproduced TNIC-style, not official external TNIC. Nearest5
fallback receives no original95%strict-gate credit. Original yearly linked10K
95% and member-text90% thresholds, exclusions, three-universe populations and
calendar reporting remain; no return-selected or post2022 evaluation is added.
Unknown clock/source scope, original EX13 dependency, annual prior-period match,
amendments and missing section outcomes remain explicit. Public positional
reader/product names are preserved; recovery adds a necessary independently
authenticated transition prerequisite.

No unchanged suite or new repro was run. Author reports11distinct passing focused
tests across seven serialized invocations, each under60s; repeated tests do not
increase the distinct count. Final identity-chain2passed8.80s, source/PIT/bounds
4passed3.12s and TNIC3passed13.60s were on final frozen code; unchanged activation
1passed6.83s and public consumption1passed4.25s retain their first-freeze evidence.
Static review confirms those checks correspond to the corrected code paths.

Actual original SEC submissions/body/index/EX13/census landing, authenticated
historical SIC observations, qualified member/identity/universe populations,
independent source/license/code/runtime plan and immutable expectations remain
root prerequisites. Exact original95%strict linked10K2019+ and90%member-text2020+
gates are unmeasured/failed. Source transport audit, this review and synthetic
fixtures do not admit actual source quality or clocks. Golden exact text-field
mapping/catalog integration remains Task17's separate implementation scope.
