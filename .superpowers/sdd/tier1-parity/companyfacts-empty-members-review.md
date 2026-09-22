# Companyfacts exact empty archive member review

Fresh independent static review on `feat/tier1-parity`, 2026-09-20. Reviewed the uncommitted `atx-db/src/atx_db/fundamentals.py` diff, new `atx-db/tests/test_companyfacts_empty_members.py`, and the paired empty-member brief and implementation report. Supporting reads covered target discovery, source receipts, quality checks, dataset/activation handling, and the existing ZIP, resilience, CIK spelling and issuer-cleanup tests. Graph tools were unavailable in the exposed catalog, so discovery used scoped source reads.

The reviewer performed no imports, tests, database connections, runtime probes, network requests or live-job operations. No implementation edits or commits were made. This review artifact is the only file written by the reviewer.

## Findings

- **Critical: none.**
- **Important: none.**

The patch is acceptable on static review. No implementation correction or second review pass is requested.

## Correctness and preservation

At `fundamentals.py:918-940`, the fetcher derives the requested canonical CIK member name, requires that exact member to exist, reads that member, and emits the private marker only for exact raw `b"{}"` bytes under the existing full-match CIK filename pattern. Archive discovery at lines 338-348 retains its existing exact top-level filename rules. Nested paths, abbreviated or suffixed filenames, and case variants do not gain placeholder status. Target normalization and deduplication remain unchanged. The marker cannot arise from the ordinary HTTP JSON path.

At lines 1084-1125, only this marker bypasses normal payload validation and normalization. Whitespace variants of `{}`, nonempty missing/null/non-object facts, non-object payloads and malformed JSON still enter the existing failure path. Archive-member payload CIK mismatch validation remains in place for ordinary payloads. A missing selected member still produces `FileNotFoundError`. Filename identity provides the marker's source identity; the implementation does not invent a payload CIK, issuer name or financial fact.

At lines 1126-1144, the marker branch records the unavailable observation and continues before identifier resolution, CIK spelling inventory, `_replace_facts`, unresolved-candidate collection or successful-payload accounting. Consequently it neither removes nor reinserts the placeholder CIK's retained `sec_company_facts`, `fundamental_points` or identifier candidates. Canonical and legacy raw CIK spellings take the same preservation path. The receipt write is outside the skippable source-error catch, so SQL or filesystem metadata failures propagate rather than becoming a successful skip.

The committed per-load spelling cache and issuer-bounded cleanup are unchanged. Usable siblings still take the existing transactional replacement path, and canonical spelling advancement still occurs only after replacement returns. Placeholder CIKs neither create nor advance that cache. Post-loop candidate replacement is restricted to unresolved CIKs collected from successfully normalized siblings; the placeholder does not enter that collection.

The existing optional global derived-surface refresh at line 1230 remains unchanged. Activation and the focused preservation tests explicitly disable it. The preservation/no-replacement conclusion concerns the placeholder branch and the three retained source tables; it does not claim that this patch disables a caller's separately requested derived refresh.

## Accounting, provenance and coverage

The returned counts reconcile as `completed_targets = loaded_targets + empty_target_count` and `target_count = completed_targets + unavailable_target_count + failed_target_count`. Unavailable observations do not contribute rows, points, loaded CIKs or completed payloads. The code preserves this relation for all-placeholder, placeholder-plus-usable, placeholder-plus-authoritative-empty, and source-error combinations. Any real failed target retains `failed_targets` outcome precedence. Without failures, usable siblings yield `loaded_with_unavailable`; unavailable targets without usable rows yield `source_unavailable`. Valid empty payloads retain their distinct counts and authoritative replacement semantics.

The receipt retains the existing archive-member URL and cache identity, archive checksum, run ID and allowlist fingerprint. It explicitly identifies the member, the two-byte member size, zero observed rows and the unavailable reason. `raw_source_files.byte_count` remains the whole cached archive size, consistent with its existing contract; `archive_member_bytes` separately identifies the member size. Retained financial rows keep their old row contents and run lineage rather than acquiring the current archive's identity. The receipt describes the latest source observation under the existing receipt replacement model, not evidence that retained rows were supplied by that observation.

The new availability quality check is a warning with unavailable-count evidence. The existing `rows_loaded` threshold and status rule are unchanged, and `listed_security_coverage_verified` remains false. No activation source change is present. Its existing rule rejects actual source failures or no valid targets while permitting an unavailable-only source observation to finish execution. A completed activation stage therefore still does not certify source completeness, issuer eligibility, or listed-security coverage.

## Evidence and validation limits

The supplied inventory records 62 exact two-byte placeholders among 20,390 main members, no other members of two bytes or less, and zero retained fact rows/CIKs for those 62 at inspection time. The implementation's preservation behavior remains necessary for future retained history. The archive2 inspection separately records the first placeholder's prior source error, 146 loaded CIKs, 19 authoritative empty outcomes and retained attempt data; those records do not establish a completed archive load.

The new file contributes 19 parameterized cases covering retained canonical and legacy identities, unavailable metadata and quality warning, forbidden issuer-write/identity paths, fatal receipt failure, usable siblings, eight excluded invalid representations, both HTTP error modes, valid-empty replacement, and activation's usable/error/unavailable combinations. Existing target-discovery and missing-member tests preserve the filename-selection and absent-member contracts. The source inspection also checks placeholder-plus-valid-empty accounting, which does not have its own new combined fixture.

Root confirms that the focused command over `test_companyfacts_empty_members.py`, `test_companyfacts_zip.py`, `test_companyfacts_resilience.py` and `test_companyfacts_cik_spellings.py` passed all 56 cases with exit code 0. The reviewer read the retained log and memory receipt: the 2.5 GiB guarded job reported native peak memory of 0.6872520446777344 GiB and empty stderr. These are root-run results, not reviewer execution. Production archive completion, production throughput and coverage remain outside this static review's claims.
