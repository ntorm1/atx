# R26 Task17 round2: delegated native family source helper

This is implementation assistance within existing Task17 repair2/5, not an
independent approval, new task/counter, source gate PASS or Task16 round6.
Task16 remains frozen6438c202/cap5/5. Only the new private helper, its direct
test and this report are owned here. Task17 owns all catalog/golden wiring.

## Fixed interface and original native authority

`private_tier1_family_sources.VERSION = tier1-family-sources-v1`.
`admit(name,spec,con)` accepts exactly `text_features`, `classification_tnic`,
`delisting_v2`, `distributions`, `events_buyback`. It returns
`status=admitted_diagnostic`, `products` mapping original relative names to
Paths, native `validation`, complete `dependency_pins`, physical `schemas`,
recommended `views`/keys/clock/qualification descriptions, `spec`, `family`,
and `source_quality_certified=False`. That last flag is global science/source
acceptance, not a replacement for reachable native conditional cell evidence.

* Task12 calls unchanged `mna_publish_v2.validate_publication` on the complete
  independently expected source contract, original candidate Parquet, all
  native SEC objects/receipts, original input manifests, typed vendor price and
  economic proof pins, auxiliary original clock sources and optional native
  handchecks. Native full candidate/population/code/schema/cell replay remains
  mandatory. Fixed products are `mna.parquet` and `events.parquet`; terminal
  keys are security_id/last_session/candidate_available_at. Original
  available_at, dlret_exact, dlret_policy, source failure reasons and scientific
  gates remain visible. Supported exact economics are not forced to NULL.
* Task13 distributions calls unchanged `distributions.replay_rows` and
  `validate_product`. Fixed product `distributions.parquet`, event_id key.
  Original declaration amount, conditional rate, unresolved clock/class and
  quote-currency/history exclusions remain original native fields. Native
  admission, full source catalog partition, source bodies, receipts, recursive
  financial/identity payloads, class/clock original documents and loaded code
  are pinned. Ancillary original records are traversed even when the current
  declaration has no supported event. No invented source completeness PASS.
* Task13 buyback calls unchanged `events_buyback.native_rows` and
  `validate_product`, preserving complete native source and XBRL/identity join
  replay. Fixed `buyback.parquet`; keys accession/source_sha256/event_type/
  evidence. Original publication also records the fixed native parse Parquet;
  this helper checks exact full native parsed rows, parse receipt/population/
  diagnostics and the manifest's complete native input inventory. Both parse
  bytes and receipt are added to consumption pins. If current mutable parse
  intermediates no longer match an old generation, it is rejected, not rescued
  by a caller-selected replacement. First announcement/global-history and all
  native science failures remain unchanged.
* Task15 calls finalized positional `text_features_native.read_native_generation`
  and `iter_native_rows`. Products are text/features.parquet and
  classification_tnic/{pairs,firms,years}.parquet. The full original census,
  SEC bodies/receipts/EX13, identity clocks, historical SIC and reproduced
  original priorDF/SIC3 algorithm replay remain native authority. Source plan,
  every actual source dependency, immutable units/checkpoint/metadata/product
  byte and code are checked. Current Task15 repair1 still requires independent
  review; merely present worktree code never admits actual sources. Native
  section numeric fields and strict/fallback peer_basis are exposed without an
  invented generic text_score. Keys are cik/accession, year/cik/peer_cik,
  year/cik and year respectively; native available_at is preserved.

## Independently expected specification

Common exact identities: `interface_version`, absolute `root`, absolute immutable
`generation` inside root, `manifest_pin={sha256,bytes}`, full `dependency_pins`,
`expected_validation_sha256`, `native` and `independent_review`.
`independent_review={path,expected_sha256}` is itself in complete dependency pins.
Its bounded original JSON must equal schema `atx.tier1-family-review/v1`, status
`scoped_pass`, fixed family/version, canonical `binding_sha256` over version/root/
generation/manifest/native, and complete actual `code_pins`. A generic caller
PASS boolean, pending review, changed review or stale code/plan binding fails.
Controller/reviewer must independently supply these expectations; synthetic
fixture review documents have no authority for actual sources.

Native specs are fixed rather than generic Parquet loaders:

* text: source_root and expected_plan_sha256.
* Task13: admission_path and expected_admission_sha256.
* Task12: contract_path, expected_contract_sha256, candidate_original, objects,
  input_manifests, receipts, exact_inputs, bulk_clocks, clock_receipts and
  handcheck_original. Exact inputs require original vendor and target economic
  paths plus complete native typed pins; stock consideration additionally
  requires original acquirer economics and price pins. Bulk clocks require
  bulk/submission originals and their exact native typed pins.

Dependency inventory must equal the independently supplied inventory exactly,
including code/runtime identities, original proofs and all products. No partial
caller pin map is admission. Original body maps are lazy; fixed bound8MiB per
ordinary original/review/contract and32MiB candidate original avoids materializing
all originals in this helper. Native replay owns its existing population bounds.

## Resources and consumer integrity

Each admission/consumption requires actual external Windows job membership under
<=1GiB, >=40GiB free disk and a primary file-backed DuckDB connection configured
<=128MB/one thread/UTC. `iter_rows` batches at most64, including finally checks
on early close. `consuming` brackets catalog SQL/COPY. Both verify complete
actual dependencies before/after and reject mutable result dictionaries that
redirect fixed products, alter inventories/validation or replace review identity.
No frozen producer, source contract, golden module or shared registry was edited.

## Focused offline checks

All commands use PYTHONPATH=C:/atx/atx-db/src, OPENBLAS_NUM_THREADS=1 and:

```powershell
atx-db/.venv/Scripts/python.exe .superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 1 --disk-path C:/atx --min-free-disk-gb 40 -- atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_private_tier1_family_sources.py -k <selection> -n 0 -o addopts='' -p no:cacheprovider -q
```

Initial terminal:1passed/22.77s, guard25.1s, peak152727552B, no cap event.
Initial `task13 or prerequisite`: prerequisite passed; one test-fixture failure
passed a string to native common.sha256_file before Task13 replay,19.28s. Fixed
the owned fixture to use helper pin; no producer change. Corrected Task13:
1passed/14.54s, guard17.0s, peak130469888B, no cap event, exit0.
Terminal final consumption-guard check:1passed/24.01s, guard26.0s,
peak152469504B, no cap event, exit0. All helper fixtures stopped and lane released.
Guard receipts (local controller observations, not actual-source evidence):
20261004T143605-24728.json,20261004T143645-5504.json,
20261004T143944-21496.json,20261004T144014-7424.json under
C:/Users/natha/AppData/Local/atx-memory-guard/receipts.

Terminal fixture proves source-shaped stock conversion proceeds15 and dlret0.5
through complete unchanged public native replay and bounded reads; changed source
bytes are refused. Prerequisite fixture proves missing complete expected original
candidate pin and pending independent review are refused. Task13 fixture proves
native original declaration amount0.25 remains visible, missing class/clock
qualification stays NULL, and changed body bytes refuse readmission.

No unchanged upstream suite, actual warehouse source, HTTP, actual returns or
consumer job was run. Buyback and repaired text/TNIC delegation are statically
bound to full original APIs; these new direct fixtures do not constitute a
separate positive full buyback/TNIC end-to-end coverage claim. Independent scoped
review of this helper and finalized Task15 code is still required. All original
source/population/manual/statistical/consumer gates remain open; Task17 must wire
individual native cell qualification and all required source-derived serving
definitions. R21/pre2023 and original terminal/distribution conventions remain.
