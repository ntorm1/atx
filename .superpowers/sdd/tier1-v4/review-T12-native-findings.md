# Task12 native initial independent review

Reviewed package review-T12-native.diff, commits faa18387, 11892c26, a2f97138 and e255b515, task brief, gap-analysis section2 item10, global constraints, reviewer contract, final report and supplied implementation. Reviewer /root/t16_release_foundation authored none of this Task12 implementation.

## Spec Compliance

The implementation provides separate native financial vintages and terminal outcomes; cash/stock proceeds are distinct from proceeds/target-last-close minus1. Original candidate Parquet replay preserves every raw2020+ M&A candidate, including legacy continued flags. Missing sources/economic admission yield NULL outcomes and reasons. The original 2267 denominator, consideration>=80%, exactDLRET>=80%, and 200-deal>=95% handcheck requirements are retained. Actual source/gate/resource/three-universe coverage acceptance remains open; fixture correctness is not source certification.

Missing: trustworthy handling of some conditional financial terms and negative economic evidence, complete independent auxiliary-clock/runtime input admission, and exact physical output-schema validation (findings below). No shared registries or foreign labels are modified by the supplied package. No extra scope concern identified.

Requirement change received during review: the user clarified that the pre2023 restriction applies to evaluation and forward labels, not individual source-backed DLRET values or historical return-based feature construction. Root is recording R21. Current e255b515 still imposes the older restriction through terminal_mna_v2.py:218-222 and mna_exact_v2.py:79-82, plus the registry return-window field. This needs a separately frozen/reviewed source-only raw-DLRET policy; it is a new requirement adjustment, not a retroactive author correctness finding. The former 1003/2267=44.24% metadata ceiling is superseded under that clarification, not converted into measured coverage or PASS. All full-denominator/gate requirements survive. Post2022 evaluation/forward labels remain prohibited.

## Strengths

Native SEC SGML acceptance uses America/New_York-to-UTC conversion and exact filer/accession matching; HTML-only documents retain retrieval floors. Bulk clocks require an SGML crosscheck rather than trusting a syntactic Z suffix. Native vendor reading projects raw close only, streams batches, rejects ambiguous SID/date/ticker identities and checks source bytes before/after. Unsupported ADS/elections/fractional/FX cases and missing candidate sources remain visible. Immutable contract-named builds, complete pending-build replay, output-row replay and explicit handcheck expected fields are useful foundations.

## Issues - Critical / Important / Minor

No Critical finding.

### Important I1 - Conditional cash terms become unconditional proceeds

`atx-db/src/atx_db/alpha_panel/mna_financial_v2.py:38-46,130-153`: the purported unconditional financial parser blocks selected words such as contingent/CVR/subject-to-adjustment but accepts ordinary conditional language. A synthetic native clause saying each Acme common share was converted into the right to receive USD12 in cash **only if the holder satisfies future performance conditions**, with otherwise recognized completion/settlement statements, returned cash_per_share=12 and no reasons. The exact path then has no remaining condition to prevent valuation. Matching the original bytes or independently replaying the same parser cannot remove this economic error.

Fix: require supported affirmative unconditional clause semantics; preserve conditional/qualified clauses as NULL with reasons. Cover if/only-if/provided-that/unless/subject-to conditions and their scope, rather than assuming absence of the current short keyword list establishes unconditional consideration.

### Important I2 - Negated raw-price evidence certifies an exact return

`atx-db/src/atx_db/alpha_panel/mna_price_native_v2.py:75-89`: economic proof checks ordered keywords and quote inclusion without their polarity. Replacing the fixture's raw-basis quote with **close is not the unadjusted raw traded close**, pinning those exact original bytes and that quote in the synthetic independent expected contract, still produced dlret_exact=True and dlret=0.2 through mna_exact_v2.replay_exact. Similar keyword-only conversion-basis matching cannot prove affirmative unchanged split/FX terms.

Fix: admit a narrow explicitly affirmative documentary statement/semantics and reject negation, alternatives and contradictory basis statements. An externally expected hash/quotation is identity proof; it must not override unsupported economic content. Add negative/contradictory quotation fixtures that remain unqualified.

### Important I3 - Auxiliary clock originals are not independently frozen

`atx-db/src/atx_db/alpha_panel/mna_publish_v2.py:51-53,153-157` and `mna_exact_v2.py:41-52`: the immutable producer/exact contracts freeze financial source pins but have no expected bulk-clock inventory or matching-SGML source/receipt bindings. bulk_clocks is supplied separately and forwarded to replay; mna_native_v2.py:49-89 validates those objects against caller-supplied BulkClockPin/NativePin hashes. It does not compare those auxiliary pins with independently frozen expected ones. An auxiliary JSON/submission can therefore be replaced with freshly self-matching pins, so long as it asserts the financial document's already expected clock/accession/CIK. The complete source/receipt contract claims do not cover these load-bearing clock originals.

Fix: include exact auxiliary clock and matching-submission identities, source provenance/receipts, and typed pin inventories in the independent producer/exact contract before computation. Reject missing/extra/swapped auxiliary inputs and independently bind them during public replay/resume. Merely checking the caller's new SHA is insufficient.

### Important I4 - Public full-schema validation accepts wrong physical types

`atx-db/src/atx_db/alpha_panel/mna_publish_v2.py:328-345`: validation reads rows and compares canonical JSON but never checks actual Arrow/Parquet field types against the explicitly declared publication schemas. Canonicalization maps Decimal/date/timestamp values to strings, so values with different physical types can match. A synthetic published mna.parquet was rewritten with cash_per_share as VARCHAR/string "12" instead of DECIMAL(30,12); after refreshing its output hash, validate_publication accepted it. This same validator authorizes existing-build reuse and pending-build resume.

Fix: independently derive exact physical schemas (including UTC timestamp units/timezone, decimal scale/precision, list types and field inventory), verify them before row comparison, and recheck product/manifest identities across admission. Cover refreshed-hash type forgery and empty tables. Keep row/key/native replay as an additional check.

### Important I5 - Complete reader identity omits the runtime readers

`atx-db/src/atx_db/alpha_panel/mna_native_v2.py:104-110`, `mna_exact_v2.py:24-26`, and `mna_publish_v2.py:42-44`: complete expected_code binds eight local Python modules only. Actual original/output Parquet reads and SQL publication depend on PyArrow/compiled parquet and DuckDB/compiled engine, and SEC acceptance conversion depends on timezone data. None of those reader/runtime identities is included in the independent immutable expectation or checked before/after. The same contract/build ID can consequently be admitted under changed reader/timezone implementations despite the report's complete-reader claim.

Fix: declare and independently bind the actual loaded reader entrypoints/binaries and relevant timezone semantics/data or a versioned reviewed immutable runtime contract. Verify the complete dependency set before computation and public admission; abbreviated or changed runtime identities must reject.

## Validation evidence

Only three new narrow synthetic acceptance checks ran, in one temporary pytest file outside the workspace, using OPENBLAS_NUM_THREADS=1 and:

`atx-db/.venv/Scripts/python.exe -m pytest <temporary>/test_named_risks.py -n0 -o addopts= -p no:cacheprovider -q`

Result: 3 passed in0.81s. These tests intentionally assert the observed erroneous acceptance for I1/I2/I4; they are reproductions, not passing product-quality gates. The publication fixture uses C.connect128MB/one-thread. No author test suite was rerun. All hashes involved synthetic fixture bytes or local implementation identities; no actual source/data/hash/metadata/HTTP jobs were run. The temporary test was removed automatically. This review report is the only persistent file written; no commits/subagents/live jobs.

## Assessment

**Task quality: Needs fixes.** Economic conditions and negative price evidence can become exact values, and public/auxiliary/runtime admission is weaker than the promised independent full contract. Actual source acceptance, full gate measurement, manual200 corpus, guarded throughput and revised source-only raw-return policy remain separate unfinished requirements.
