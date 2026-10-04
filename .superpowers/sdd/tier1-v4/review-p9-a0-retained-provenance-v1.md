# Independent retained A0 provenance metadata/controller review

Spec verdict: **PASS for the bounded metadata inspection only.**

Quality verdict: **PASS for the bounded metadata inspection only.** No Important
or Minor corrective findings. This is not source qualification, a D0 result,
producing-commit attribution, arm approval or acceptance of identity/index data.

Reviewed the complete v2 controller, selected complete metadata fields in its v2
result, both retained run receipts, v1 failure stderr, v2 stdout/empty stderr and
the prior manifest-preflight report. No audit was rerun; no source payload,
Parquet footer, database, ZIP, filing body, HTTP, test, Git/index mutation or
Task1 implementation change occurred during this review.

## Numbered observations and checked invariants

1. **Informational — original raw code guards remain binding.**
   `p9-a0-retained-provenance-v2.py:88-116` checks exactly the twelve declared
   names in both original build and manifest, exactly that snapshot directory
   inventory, and each retained raw SHA before any normalization. All twelve
   reported retained raw hashes equal their independently pinned declarations:

   | Retained file | Declared and matching retained raw SHA256 |
   |---|---|
   | _fundamental_clock.py | f3a778f933231a50c5849e275adde3119111a4efcf8a2d5c1872e680159d1abe |
   | _submissions_archive.py | 12d48c0334fa0013726200967809f9fb0cb2eee75113debd8cd78d0eba89f09f |
   | calendar.py | 31c54d6737b0fe0a676451c8549e9ac52cdba393f4d9adbda19c14b1c82feaaa |
   | historical_identity.py | 6ab1b60553f977780313af5d5735e197045e732e89d90eec95d94a999b5df7c4 |
   | historical_identity_sources.py | 4bd17a8e819d421355cfb4c740a95ed85a399e8ef99c463d44e544b3935ea323 |
   | identity_links.py | 2d51da02d6676002d2e0353793647075ab38311d3618b01a2ca32d2dbdcc1139 |
   | identity_reconstruction.py | 036625d0d28eacac67be6886f57382f01a2b70a99d92c6b9ec64382a1d288a0b |
   | market_daily.py | 94a8b46783b6460eb9688d831bcb8134b1be58e83da82bc15e5ae8d8d42fa601 |
   | market_owner_bridge.py | 0ee847467625099261774cb357801665c0113eef3f3cd9a943790d81856ae94e |
   | ticker_history.py | 98bcab1af1a98df9617313d6d01ca23f9423f6907f2aafc3d38c7715239bbf88 |
   | universe_us_listed.py | 3a5bafc7386a8f19243ac52a3e3c682aa5e3acd7fc5544c14806664715da5b21 |
   | warehouse.py | 74f25da75eb7646c22470fcdd61ac683b922d9d0cd4360bf7ae2538f63550eef |

   Every reported LF-normalized SHA matches both `dec03454` and the current file
   for that name (12/12 each). The four previously unexplained raw-versus-raw-or-
   uniformly-CRLF discrepancies are explained by mixed endings: respectively
   32 CRLF/3 bare LF, 2126/8, 1839/17 and 2761/17 in `_fundamental_clock`,
   `identity_reconstruction`, `market_daily` and `market_owner_bridge`. This
   establishes normalized code continuity for these twelve files only. Current
   raw bytes still differ for three reported names (`identity_links`,
   `market_daily`, `market_owner_bridge`); normalization does not admit those
   bytes under the original raw guards or identify a single producing commit.

2. **Informational — bounded successful final authentication is supported.**
   Lines49-72 enforce regular files, a 2 MiB individual limit, 8 MiB retained
   total, read-time stat stability and exact expected metadata SHA. Lines170-177
   re-read every held small file, recheck every observed path state and recheck
   complete snapshot membership before writing a result. This is a successful-
   end verification, not a Python `finally` on every failure path; interrupted
   inspection cannot mint a successful result. Historical Git objects are
   read-only comparisons with timeout/size bounds. No original raw guard is
   replaced by LF equivalence.

3. **Informational — absent preparation/receipt remains absent.**
   Lines121-128 and the result record the original preparation directory,
   `preparation.json`, `retained.duckdb` and original `ri2_a.json` receipt as
   absent. The receipt input comparison remains NULL, not true. Declared
   preparation/database SHA values are retained as declarations, not verified
   payload identity or recovered preparation. D0 requires a genuine newly
   pinned preparation/source chain and complete differential measurement.

4. **Informational — presence denominators and limitations are accurate.**
   Lines129-169 stat only the four fixed original sources (3/4 present; original
   staged TickerHistory3 missing), the five OPS exports declared present (5/5
   observed regular files) and the 367 named review document paths (0/367
   present). One sixth OPS record was already declared missing:
   `security_identity_evidence`; it has no invented file/bytes/SHA and remains
   separately recorded. Thus 5/5 is the present-declared bucket, not six-source
   completion. All these observations have payload_hashed=false; declarations,
   lengths, timestamps or path presence do not verify source/document/OPS bytes,
   clocks, coverage, economics or completeness.

5. **Informational — failed attempt and successful retry are distinct.**
   Retained v1 failed exit1/1s at missing OPS `row['file']` (stderr line140),
   peak66,535,424B, no cap hit and no result. V2 explicitly branches on the
   originally missing status before accessing payload fields (lines147-159),
   refuses unexpected payload declarations/unknown statuses and preserves the
   missing row. Its retained receipt completed exit0/2s, peak66,449,408B under
   the unchanged 0.15 GiB guard, no hits; stdout matches the v2 result summary.
   The failed attempt is not erased or mislabeled as a source failure/PASS.

6. **Informational — no promotion is supported or attempted.**
   Lines191-205/result explicitly retain d0_reproduction_measured=false,
   arms_run=[], supported_single_build_commit=NULL, payload body reads0,
   network_attempts0, source_quality_certified=false and gate_promotion=false.
   The exact revised P9 request pin (59,567B,
   d2182547eaafda33d33c51502c07a4622f36d38fb2ca219fbef4eea95019ab02) is enforced.
   The prior exported bridge pin and declarations remain historical evidence;
   neither reproducing outputs nor correcting identity/PIT claims follows from
   this metadata audit. Original seals, thresholds and task caps are unchanged.

Dependencies remain: authentic complete preparation/receipt/source/document/OPS
inputs and original byte identities; complete producing code/config/runtime
provenance; separately reviewed A0 differential/PIT implementation; controller
resource admission and actual D0/arm measurements. No approval to run them is
provided by this review.
