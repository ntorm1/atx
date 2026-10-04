# Task13 native qualification continuation: independent review

Reviewer: /root/t12_terminal_events (GPT6.1 Sol), independent of Task13 authorship.
Scope solely commits 62896107 and cda1e08b, packaged review-T13-native-qualified.diff.
Prior cb318dd9 R2 engineering approval remains historical; old addressed findings
were not reopened. Read Task13 brief/report, binding gap item11, prior R2 findings,
Task4 requirements, R21 and global constraints. Graph tools were unavailable;
used exact-path static reads. No producer edits, commits, subagents, actual source,
data/hash/HTTP jobs or external preflight execution. This report is the only
workspace file written. Temporary synthetic fixture directories were removed.

## Spec compliance

The continuation implements a genuine conditional route from original
receipt-backed declaration/class/acceptance objects and independently pinned
symbol/identity inputs to event-level annual rates. Missing evidence records
retain NULL/reasons; claimed tampered originals reject. Physical output schemas,
full native population/key/row replay and diagnostic counts remain present.
Quote currency is honestly separate and false. Source-explicit validity avoids
an invented forever carry or arbitrary expiry. Cross-partition prior history,
complete cancellation/carry inventory, vendor/member-line cash denominators and
actual 200-event corpus remain openly incomplete; this continuation does not
claim otherwise or turn successful document partitions into complete history.
R21 does not exempt PIT or native-source semantics; no prohibited evaluation or
forward-label computation was introduced.

However, native replay currently verifies reproducibility of several false
qualification decisions. The four Important findings below prevent scoped
engineering approval. Actual gates remain 0 PASS /3 FAIL-unmeasured and native
buyback handchecks 0/200; neither this review nor fixtures certify task completion.

## Important findings

### I1: New fixed-rate qualifier promotes conditional and misassociated cadence text

Locations: atx-db/src/atx_db/alpha_panel/distributions_qualification.py:79-87,109;
production promotion distributions.py:179-188. The new qualifier replays the
older extractor and checks only special=False, a recognized cadence and USD;
it does not establish an unconditional fixed declaration or associate cadence
with the dividend. In the supplied original-format fixture, append
`, subject to regulatory approval` or `, if earnings targets are achieved` to
the regular quarterly USD0.60/share declaration: qualify returns
native_fixed_regular_event and annual rate2.4. An independently hashed original,
receipts and full production admission using the first sentence also build and
pass the supplied full public replay checks with the same qualified rate.
Separately, prefix the declaration with `At the annual meeting`: the first
cadence token becomes annual, and the newly qualifying route emits0.6 instead
of2.4 for an explicitly quarterly dividend.

Fix: qualify only a fully supported affirmative, unconditional declaration;
reject unsupported qualifications/ambiguities with NULL/reasons, and bind
regular status/cadence to the dividend clause instead of the first word anywhere
in a sentence. Add conditional, unrelated annual-meeting and conflicting-cadence
regressions through production and public replay. This finding concerns the new
certification of these extracted values, not reopening the prior local-unit fix.

### I2: Native clock accession and filing membership are only caller assertions

Locations: distributions_qualification.py:13-25;
distributions_ancillary.py:33-41,69-71. accepted_clock accepts an accession merely
appearing anywhere in a blob, then takes the first Accepted/acceptance stamp.
It never matches the authoritative native accession/issuer field or proves the
index/header belongs to the declaration/class document. The ancillary adapter
compares accession strings in the supplied nested document metadata; matching
original bytes/receipts do not repair that semantic gap.

Reproduction: replace the supplied index fixture with an SGML original having
`<ACCESSION-NUMBER>0000000900-20-999999`, the same acceptance stamp, and a TEXT
section containing `Related accession decl` (or cover for the class clock).
Keep the document metadata accession decl/cover and independently pin every
changed original/receipt/admission. The supplied production fixture and full
public positive checks still pass with native_fixed_regular_event. A header
with a 20200101100000 stamp and unrelated native accession similarly yields
2020-01-01T15:00:00Z whenever the requested accession occurs in related prose.

Fix: require one authoritative original accession/issuer binding, reject
conflicting/repeated native clock identities, and independently validate the
original index/header-to-document membership (including original SEC document
URI/filing identity). Unsupported index/header layouts must remain unqualified.
Cover related-accession prose, wrong issuer/header, multiple clocks and swapped
original filing chains with genuinely independent pins.

### I3: Lowering availability to native acceptance skips declaration chronology

Locations: distributions.py:176-188; distributions_qualification.py:88-112.
The only future-declaration check runs against the receipt retrieval floor
before qualification. qualify then replaces that floor with earlier acceptance
without checking the declaration date against the resulting clock.

Reproduction: in the supplied production fixture change the original sentence
from `declared on January 4, 2021` to `declared on January 8, 2021`, retaining
its January4 10:00 New York acceptance, correctly pinned original bytes and
receipts, and the valid January4 price/identity evidence. All production/public
positive checks still pass: the rate2.4 is published available January4 15:00Z
for a January8 declaration. Standalone qualify likewise accepted a January4
declaration at a January1 clock; an initial adapter probe of that variant stayed
NULL only because its synthetic price rows were unavailable on January1, so it
was not counted as a positive production reproduction.

Fix: enforce chronology after all native clocks/link evidence are reconstructed
and before replacing availability or publishing qualification. Refuse an
unsupported future declaration rather than exposing its rate earlier. Include
producer/public tests where the price/link evidence is valid for the earlier
clock so missing linkage cannot mask the defect.

### I4: New New York timezone normalization is outside frozen executable identity

Locations: distributions_qualification.py:9,26-31;
distributions_native.py:18-34,53-58. The new clock uses mutable ZoneInfo search
paths/cache, while code_pins hashes local source files and selected reader files
but no actual timezone bytes/normalizer identity. The frozen admission remains
unchanged if the normalization used by computation changes.

Reproduction with the real ZoneInfo implementation: point a temporary TZPATH's
America/New_York at tzdata's Etc/UTC TZif bytes, clear ZoneInfo cache, and qualify
the same original fixture. code_pins is identical, but available_at changes
from2021-01-04 15:00Z to10:00Z. Restore TZPATH/cache afterward. A separate temporary
replacement of the Q.ZoneInfo entrypoint also built/publicly replayed a qualified
2.4 rate at10:00Z under unchanged on-disk expected code pins.

Fix: freeze and use explicitly admitted timezone bytes and actual normalizer
implementation/loaded entrypoints, rechecking before/after computation and public
replay. Reject changed/missing timezone proof rather than inherit ambient TZPATH
or cached semantics. Add unchanged-source/admission timezone-drift regression.
This is the new acceptance-normalization dependency, not a broad reopening of
unchanged readers.

## Quality assessment and validation

Spec compliance: Needs fixes. Task quality: Needs fixes. No Critical findings;
four Important findings above. The missing-proof fallback, independently pinned
ancillary record objects, complete schema/key/row replay, original receipt checks,
and explicit separation of event rate from quote currency/carry/gates are useful.
The six author fixtures cover the intended happy path and missing/ADS/tamper
cases but omit the semantic and final-clock adversaries reproduced here.

Only targeted synthetic originals ran, via inline `python -` from C:/atx/atx-db
with OPENBLAS_NUM_THREADS=1 and PYTHONPATH=src. Imported the existing
 tests/test_distributions_qualification.py fixture helpers without editing them;
changed the exact sentence/index inputs described above in memory and invoked
its production test with TemporaryDirectory and pytest.MonkeyPatch.context.
Conditional and wrong-native-accession variants completed every supplied
production/public assertion; future-date variant also completed all assertions.
Timezone variant's expected-clock assertion failed as anticipated, then direct
validate_product returned conditionally_qualified_events=1 for its10:00Z rate.
Standalone checks reproduced the annual-meeting cadence error, wrong header,
future declaration, and actual TZif substitution. All fixture jobs completed
under60s; DuckDB adapters used128MB/one thread. No pytest suite, broad unchanged
reruns, actual-source acquisition/hashing or return evaluation occurred. No live
review tool sessions remain.
