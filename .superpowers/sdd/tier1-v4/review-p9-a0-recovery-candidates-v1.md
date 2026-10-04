# Independent A0 recovery-candidate inspection review

Spec verdict: **PASS for this bounded recovery-lead inspection only.**

Quality verdict: **PASS for this bounded recovery-lead inspection only.** No
Important or Minor corrective findings. No restore, D0, source-quality, producer
attribution or arm/consumer acceptance is approved.

Reviewed the complete controller, bounded selected result fields, retained
stdout/empty stderr and completed guard receipt; checked the specifically named
native object-path definitions in `sec_http.py:873-881`. No controller was rerun
and no source body, vendor candidate, archive, hash job, test, HTTP, cleanup,
Git/index mutation or data restoration occurred during this review.

1. **Informational — fixed original metadata and candidate scope.**
   `p9-a0-recovery-candidates-v1.py:46-57` authenticates the exact original build
   SHA `1b4c1ae22de46cf21d5eaadf52c50fbe81d2be1c79b3b29a6f0bc48c9e3eee30`,
   bounds its metadata at 1 MiB and requires all367 original document entries.
   Lines58-70 validate each raw SHA and inspect only its two native object
   names in exactly four named stores. The `.gz` and legacy plain-object forms
   match unchanged `FetchLedgerStore.object_path`/raw-object definitions; the
   gzip filename names the RAW digest, not the compressed byte digest.
   No wildcard, recursive store scan, HTML/gzip decode or ledger admission is
   introduced. Reparse paths and non-regular existing files refuse inspection.

2. **Informational — zero named document candidates is correctly limited.**
   Result/stdout report367 entries inspected, zero entries with a named
   candidate, zero candidate objects. All four stores are explicitly listed.
   This does not establish global document absence: no other store, backup
   medium or alternate object/ledger population was searched or certified.
   The controller preserves original document path/SHA declarations while
   treating pathname/stat as leads; no document body bytes were read or
   authenticated. Successful-end checks revalidate every observed named state.

3. **Informational — the one vendor candidate has genuine byte-match evidence.**
   Lines72-96 inspect only `C:/Users/natha/Downloads/TickerHistory3.parquet`,
   bound it at4 GiB and stream it with1 MiB blocks. Pre-open stat, opened handle
   identity/length/mtime, post-read handle identity, reopened path state and
   complete byte count are checked. Reported bytes are3,617,973,507; expected
   and observed SHA both equal
   `0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae`.
   This supports recovery of those original bytes at the observed candidate
   location, not restoration to the original staged path or proof of prices,
   schema, currency/share basis, historical availability, economics or quality.
   Original staged-path absence in the prior audit remains unchanged.

4. **Informational — integrity, resource evidence and failure behavior.**
   A50-second internal deadline is checked during bounded candidate enumeration
   and each vendor stream block. Lines97-100 recheck named states and exact
   original build bytes before exclusive new-result publication. A failure
   cannot produce the successful result. The retained original external guard
   receipt completed exit0/8s, peak30,564,352B under0.15 GiB, no cap/hits;
   result elapsed7.359s and stdout counters agree. No changed resource floor,
   nested guard, native data builder or replay assertion is inferred.

5. **Informational — no source or goal promotion.**
   Lines105-118/result explicitly retain status
   `A0_CACHE_RECOVERY_CANDIDATES_INSPECTED_NO_RESTORE`, files_restored0,
   HTML/gzip payload reads0, network_attempts0 and gate_promotion=false. The
   vendor stream is disclosed separately rather than described as stat-only.
   Qualifications explicitly exclude global quiescence, whole-store search and
   historical source admission. Neither the prior twelve-code snapshot result
   nor any producing source/pin/output is changed by this inspection.

Any later restoration or A0 reproduction still requires separately authorized
controller work, original native raw-byte/code/source/preparation/receipt trust,
complete documentary/OPS provenance, reviewed PIT/differential implementation
and original resource admission. These dependencies remain open.
