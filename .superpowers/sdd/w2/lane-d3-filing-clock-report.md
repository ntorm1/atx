# W2-D3 bounded filing-clock and true-share producer slice

Status: source implemented; runtime/compilation pending. This is not full D3 acceptance.

## Frozen packet

- `044185e90f07ac11dabc38826d60f425581779ef`: actual exporter, new sealed-clock resolver, private V4 admission decoder.
- `17fddf8e`: root-review repair for conflicted SUE history and negative identity clocks.
- `6db088f66c5d3376fae6856f73e2af4c2e35ef73`: postimplementation producer/decoder fixtures.
- `3f45713b4121f4f02c5476376f277c88b01f6e58`: V4-only finite-statistics refusal for overflowed seasonal differences, plus fixture.

Root must register `atx-engine/src/data/fundamental_clock_artifact.cpp` and the owning new data fixture. No CMake edits, compilation, test execution, real payload reads, downloads, warehouse access, or exports were performed in pool4. `git diff --check` passed. Synthetic fixture code is not evidence of runtime success.

## Actual producer and compatibility

`tools/export_fundamental_fields.py` now accepts explicit `--filing-clock-rule acceptance-v2`, `--filing-clocks`, `--filing-clocks-sealed-manifest`, and `--acceptance-delay-seconds`. The current default stays `legacy-filed-day-v1`; legacy static and dated V2/V3 readers/writers retain their numeric path. The historical `+46h` label was inaccurate: the old interval writer stamps midnight UTC of filed+1, or +24h. That recipe remains explicit reproduction. Only V4 uses exact +46h fallback.

New `tools/pit_fundamental_clock.py` resolves issuer CIK and accession independently; an accession's filing-agent prefix is never inferred to be issuer CIK. Each event is ordered by its resolved nanosecond availability. Simultaneous events are applied together; contradictory values for one concept/period become an unqualified knowledge tombstone, never a lexical accession winner. A later qualified correction replaces the same cell causally.

The existing fiscal formulas are reused. The V4 calculation view contains finite cells, while authoritative provenance retains tombstones. A default-off SUE guard is enabled only in V4 and withholds SUE if a derived current/history seasonal difference is nonfinite. No general exception suppression or new fiscal formula was introduced.

Qualification conservatively covers all retained accounting cells, not just exact final arithmetic dependencies. Retention is explicitly 2,928 days from the latest known statement period: old cells and their qualifiers expire together. Dense TTM (up to 380 days), year-ago balances (365 +/-20 days), and eight prior quarterly seasonal differences plus their one-year lag fit well within this bound. Sparse history has no implied fixed quarterly calendar: data outside the bound is unavailable and cannot be used to complete a history. This can reduce qualified coverage because unused retained concepts also qualify the snapshot. No coverage claim follows from this policy.

## Clock and vintage input contract

A bounded sealed JSON projection has schema `atx.sec-filing-clocks/v1` and `records`. Each record supplies:

- issuer `cik`, exact `accession`, `filed` date;
- optional timezone-aware `accepted_at`, separately evidenced `published_at`, and `revision_available_at`;
- `revision_status`: `original-confirmed`, `revision-confirmed`, or `unverified`;
- source SHA256, source locator and optional original acceptance string.

Its receipt has schema `atx.sec-filing-clocks-sealed/v1`, exclusive seal, payload SHA256, and `facts_payload_sha256` matching the exact sealed CompanyFacts ZIP. The separate CompanyFacts seal proves the bounded payload, not historical fact vintage by itself. Source/vintage statuses remain caller-declared evidence; this exporter does not authenticate absent original filings or independently prove dissemination.

All clock evidence is pre-2020. Naive timestamps, contradictory publication/revision order, mismatched issuer/accession filed dates, conflicting duplicate evidence, invalid units/periods, and mismatched payload hashes are refused or explicitly audited according to their type. Malformed clock evidence is a hard error. Invalid numeric fact rows are audited and not admitted.

Availability rules:

| Kind | Base time | Numeric default eligibility |
| --- | --- | --- |
| observed public | observed publication, at/after acceptance | qualified fact vintage AND observed retained knowledge |
| modeled acceptance | acceptance + explicitly declared 0..604800 seconds | withheld by default |
| modeled filed fallback | UTC midnight(filed) +46 hours | withheld by default |

Every rule floors actual availability by any observed publication/revision availability. Missing evidence does not qualify fact vintage. A modeled-clock override can relax only clock qualification, never vintage. An observed current event cannot launder old modeled/unverified accounting cells. True-share events qualify their own fact independently of unrelated accounting history.

The input record/audit binds raw acceptance, normalized acceptance, publication, revision status, source locator/hash and actual resolved availability. Output manifest binds input/output hashes, declared lag, retained-history policy, admission semantics, and a canonical recipe SHA256. A future caller deliberately using `AllowModeledClockV2` must bind that policy name in its run identity; the ordinary artifact dispatch always uses `ObservedPublicV1`.

## V4 wire/admission format

Magic: `ATX-FUNDAMENTAL-INTERVALS\t4`. The unchanged 12 D1 identity columns are followed by:

`filed_ns, clock_kind, accepted_ns, published_ns, revision_available_ns, fact_vintage_qualified, knowledge_clock_qualified, clock_policy`

Then the unchanged 12 raw accounting columns follow. Identity markers have zero clock fields and empty policy/numerics. All known-at clocks remain strict `< decision` through the existing D1 aligner. Link interval, priority and separately known retirement fields are preserved.

`fundamental_clock_artifact.hpp` exposes lightweight policy/config/audit declarations. The private CPP checks exact geometry, finite numerics, policy-specific clock arithmetic, pre-2020 boundaries and bounded retained allocation estimates before admitting rows. It validates absent-axis rows too. Unqualified numeric values become NaN while identity is retained; malformed clocks are errors. Audit counters distinguish markers, observed/modeled rows, unqualified vintage/history, withheld/admitted rows and absent axes, and publish only on success.

The legacy aligner treats nonfinite fields as absent. Therefore an unqualified/conflicting V4 snapshot does **not revoke** a previously admitted value. That older value may remain under its **original** availability/period/staleness clocks; the withheld event never refreshes those clocks. The source knowledge tombstone prevents a new aggregate from being qualified, but it is not an engine revocation record. Full field-level retraction semantics are outside this slice.

## True shares and open prerequisites

`shares.entity-v1.jsonl` emits `dei:EntityCommonStockSharesOutstanding` separately from the unchanged weighted-average diluted/basic `shares_outstanding` accounting field. Exact duplicates collapse; conflicting entity values are withheld, never added as presumed classes. Unqualified vintages have missing numeric shares with reason and clock evidence. Observation date and unrebased observation-date split basis remain explicit. No line-share allocation, class summation, split inference, or market cap is produced.

This matters because [SEC API documentation](https://www.sec.gov/search-filings/edgar-application-programming-interfaces) describes CompanyFacts as standardized entity-level facts. Class allocation needs separate evidence. The [SEC webmaster FAQ](https://www.sec.gov/about/webmaster-frequently-asked-questions) distinguishes acceptance from subsequent dissemination and does not provide a guaranteed first-public timestamp; a fixed acceptance lag is a model, not observed publication.

Still open: real clock/vintage acquisition and 94.6% acceptance match evidence; dated class/split/raw-price inputs and PIT line cap; SI dated-identity/float/DT C details; coverage denominators and the original 95% cap /80% book-equity gates; actual reexports; broad D3 causal harness. Neither a strict admission policy nor synthetic fixtures substitutes for those deliverables.

## Bounded qualification commands (not run here)

- Python owning producer: `python -m unittest discover -s atx-engine/tests/tools -p test_pit_fundamental_clock.py` (10 new cases).
- Existing legacy fiscal formulas: `python -m unittest discover -s atx-engine/tests/tools -p test_export_fundamental_fields.py`.
- New C++ owning file: `tests/data/fundamental_clock_artifact_test.cpp`, filter `FundamentalClockV4.*` (6 cases), plus existing `SecurityLinkIntervals.*` for V2/V3.

New fixtures exercise same-day accession separation, exact modeled clocks/revision floors, inherited qualification, simultaneous conflict and later correction, expiration, separate true/diluted shares, synthetic actual exporter publication and hash binding, derived SUE overflow, strict engine boundaries, modeled-vintage separation, negative clocks, resource limits, and retained-history staleness. No long numerical or performance run is needed for this slice.
