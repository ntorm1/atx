# AR4 Critical C1 rereview

Date: 2026-09-20. Reviewed fix `29908a7cc80442697061adab07b8f7fe59f788f4`, its implementation report, and the relevant regression source. Confirmed the identity fix remains present after facade commit `c401fb31`. This is the required bounded C1 rereview; I1-I3 remain accepted on the implementer's report under the controller's ruling, with replacement inspected only where necessary for identity transitions.

**Verdict: C1 closed. Critical: none remaining in this scope. Important: none newly found. Minor: none.**

`fundamentals.py` introduces `UNRESOLVED_COMPANYFACTS_CIK_PREFIX = "SEC-COMPANYFACTS-UNRESOLVED-CIK-"` and uses it for archive target fallback IDs. Those IDs no longer collide with the current security master's real `SEC-CIK-*` IDs. The loader still disables undated ticker/entity fallback, retains the source identity when no eligible history exists, copies the per-fact ID to fundamental points, and clears archive point symbols. Candidate evidence retains the same isolated source identity. No security, listing, or bar record is created for it.

Authoritative dated history can still return a real `SEC-CIK-*` ID. There is no blanket prefix exclusion that would discard those valid resolutions. The new regression `test_unresolved_archive_fact_cannot_join_current_sec_identity_or_bars` uses the real current SEC ID, a fact predating its dated history, and a later resolved fact. Its assertions cover actual revision/statement refresh, NULL versus current statement symbols, the security/availability join to bars, and the isolated candidate ID. This directly addresses the downstream path behind C1.

The replacement query now locates prior raw facts by normalized CIK, restricts points to prior issuer identities (including the legacy SEC-CIK fallback), and uses NULL-safe fact-key comparisons. This permits old joinable points to be removed when replacement moves facts into the isolated namespace. The NULL-accession resolved-to-unresolved and legacy-fallback regression sources cover that transition and repeated replacement. The current-ticker lookup in this deletion scope is cleanup evidence only; it does not assign incoming archive identities.

The original brief's submissions-helper suggestion is explicitly superseded by source-identity isolation. No additional identifier-master or submissions redesign is required to close C1.

Validation evidence: implementation report records 55 focused cases passing under the controller's memory guard, plus touched-file lint/type checks. No tests were rerun, no database was opened, and no production data was changed by this review. Closure approves the code correction; existing production raw/derived identities still require the controller's authorized replacement and downstream rebuild before they reflect it.
