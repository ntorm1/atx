# Native filing text

Conditional implementation only; no actual source population has qualified.
`text_features_native.publish_native(source_root, output_root, plan,
expected_plan_sha256, unit_size=32)` derives original annual filings directly
from receipt-authenticated SEC submissions JSON and full pinned monthly native
member/identity Parquet. The fitting population is the complete original SEC
annual cohort, independent of future member selection. Native full quarterly
SEC master indexes and their actual receipt clocks prove complete cohort
closure by exact accession/form/date/owner replay. Missing quarterly indexes
leave TNIC fitting unqualified; caller completeness flags cannot replace them.
The independent source plan must include original
submissions completeness/native member/identity review evidence, upstream
manifest hashes, exact original source/code byte hashes and full expected
yearly counts. Reviewed assertions and synthetic fixtures do not qualify
production sources. Original primary bodies are indispensable: old section
landings, whose source implementation discarded bodies, cannot alone replay.

Generation: `generations/<identity>/text/features.parquet`. Keys: `(cik,
accession)` including amendments and missing sources with reasons. All native
inventory keys are replayed; source exclusions never reduce member denominators.
`source-plan.json`, `manifest.json`, `checkpoint.json` and bounded text JSONL
units authenticate source/code/schema/phase outputs. Resume requires an
independently captured checkpoint SHA256 and replays completed units. Publication
uses an exclusive lock, atomic directory rename then atomic activation pointer;
interruptions preserve prior activation. A retained lock after a process kill
requires controller reconciliation; no automatic stale lock removal occurs.
An immutable `transitions/<generation>.json` intent pins the exact target and
previous pointer bytes/value as rollback history. A retry after rename requires
both independently expected manifest and `expected_activation_transition_sha256`;
it completes only that transition and rejects an intervening activation.

Native identity retains `available_at` and accepts strict/dated/name relationships
effective on the monthly cell and known strictly before that cell's22UTC cutoff.
Exactly one distinct supported CIK must agree with any supplied member CIK;
otherwise the original cell remains ambiguous/conflicting/unresolved. Supported
identity resolves NULL member CIKs for text coverage. All cells remain in coverage;
unresolved identity cells prevent TNIC gate qualification.
Accepted clocks additionally require the original index's semantic accession and
filer/archive-owner CIKs plus corresponding original submissions records. A
transport receipt alone cannot qualify the clock.

Public admission includes source_root/runtime and complete source/code pins.
`iter_native_rows` rechecks the immutable plan, original dependencies, actual code
and runtime before/after each bounded batch and in finally on explicit early
close/downstream failure. Callers must close an iterator when stopping early.
Current text and replacement EX13 are capped before token/profile allocation;
current plus retained issuer profiles cannot exceed500000words. SIC originals
are bounded16MiB; all original accession indexes are bounded5MB.

| Columns | Type/unit | Definition/source/clock |
|---|---|---|
| cik, accession, form | BIGINT/string | Original SEC issuer/accession/form |
| filing_date, report_date | DATE | Original filing and fiscal period end |
| available_at, acceptance_clock, vintage_risk | TIMESTAMPTZ UTC/string/string | Authenticated original EDGAR index Accepted time in New York, cross-checked against raw submission clock; missing/ambiguous proof stays NULL |
| source_path, source_index, source_sha256 | string/BIGINT/string | Canonical original submissions index and original primary SHA256 |
| prior_accession, prior_gap_days | string/BIGINT days | Unique original same-family prior annual period: previous fiscal year and both period/filing gaps180..550; accepted strictly earlier |
| prior_filing_date, prior_report_gap_days | DATE/BIGINT days | Original prior filing date and fiscal-period gap, separately retained |
| doc_bytes, doc_words, doc_fog | BIGINT bytes/words, DOUBLE | Original full primary body length and heuristic readability |
| business/risk/mdna_chars, words, sentences | BIGINT counts | Extracted original annual sections, primary or authenticated EX13 |
| business/risk/mdna_fog, pct_complex | DOUBLE index/fraction | Gunning Fog with vowel-group heuristic; below50words stays NULL |
| business/risk/mdna_sim_cosine, sim_jaccard | DOUBLE fraction | Exact token-frequency cosine and exact token-set overlap, no CRC32 collisions |
| business/risk/mdna_sim_minedit | DOUBLE fraction | Explicit sentence-sequence edit proxy from legacy implementation; not exact word minimum edit; >6000sentences stays NULL with reason |
| business/risk/mdna_words_chg | DOUBLE log ratio | Log current/prior section word count |
| reason, comparison_reason, section_reason, section_comparison_reason, section_edit_reason, metric_basis | string | Missingness and explicit metric approximation labels |
| business/risk/mdna_source, method, scope, n_sections | string/string/string/BIGINT | Primary/EX13, extraction method, original Item scope, complete parsed section count |

Monthly PIT: latest accepted original at strictly before22:00UTC of the member
session;400day filing staleness. Daily consumers must apply strictly before
22:00UTC of preceding session. All coverage years2019..2026 remain in manifest,
including zero/missing populations; original text gate>=90% full member cells
2020+, and yearly2020..2025 reporting required. Delisted member lines remain.
No sentiment dictionary is used or license inferred.

SEC submissions contain both real UTC and ET wall-clock values mislabeled Z,
as documented by the existing native sec_filings clock resolver. A Z suffix
alone cannot qualify availability. Explicit original index-byte/receipt
acceptance proofs derive the clock; DST-ambiguous/nonexistent times remain
missing. Disagreement is preserved as vintage risk.

The plan freezes Python/DuckDB/PyArrow/NumPy/tzdata versions. Optional independently
pinned `universe_flags` and exact `expected_universe_cells` enable complete
yearly reports on `univ_all_common` and `univ_r3000p` after native Task2 landing;
absence of those populations remains an actual dependency, never a reduced
denominator. Standard native source plans must include full upstream manifest
identities. Original source-plan/checkpoint bytes are hashed in the manifest.

`read_native_generation(source_root, generation, expected_manifest_sha256,
expected_plan_sha256)` independently checks all sources/code before and after,
rejects orphan products/keys/incomplete phases and fully reconstructs and compares
every immutable product byte. `iter_native_rows(admission, product)` yields
bounded rows and checks unchanged product/manifest bytes on both sides. Native
reader status retains gate failure; a serving adapter cannot turn fixture
success into source qualification.

Measured actual coverage: **unmeasured/FAIL**. Offline conditional text fixture:
2/2member cells2020; not production evidence. Bounded units:32filings(default),
64max,40MiB source documents,16MiB submissions units,500000retained profile
words per issuer; exceedances fail explicitly. Production runtime must run via
the required memory guard. Scratch database files are isolated and never served.
