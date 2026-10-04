# Independent review: bounded additional native-cache metadata lookup

Spec verdict: **PASS**. Quality verdict: **PASS** for the corrected v3 controller and its narrowly stated metadata result. No corrections required. Earlier v1 failure and v2 unstable observations remain evidence, not successful snapshots or source admission.

Reviewed the three controller scripts, selected v2/v3 result fields and changed-state comparisons, all three exact guard receipts, v1 stderr and v2/v3 stdout, and `p9-a0-extra-store-candidates-report-v1.md`. No controller, source scan, body/ledger read, hash job, fixture, HTTP operation or git index operation was performed by this reviewer. Only repository controller code and retained small metadata evidence were read.

The controllers require the external guard and preserve existing output. Their fixed original build metadata is independently pinned, bounded to1MiB and rechecked; its complete declared367-document SHA set is validated. Discovery stays under the fixed raw root, rejects reparse paths, bounds directory depth/count/entries and time, prunes objects subtrees, and stats only exact plain/gzip SHA names in newly identified stores. They do not open bodies or native ledger payloads.

The retained attempts are reported faithfully:

- v1 failed at the generic final metadata-state check: exit1/3s/27,545,600B, no result. Its directory st_size comparison was unsuitable as a regular-file length invariant.
- v2 completed0/3s/27,447,296B but explicitly returned UNSTABLE_METADATA_OBSERVATION_NO_SNAPSHOT_CLAIM. All75 retained differences affect directory byte-length only; presence, kind, inode and mtime comparisons remain equal. This does not establish a payload mutation or source writer.
- v3 completed0/4s/28,905,472B and reports zero changed named states, with stable_final_recheck=true. It excludes directory st_size, retaining directory device/inode/mtime plus independently repeated sorted child-name/directory-type/symlink digests. Regular-file byte lengths remain checked, and the original build bytes are independently rechecked before publication. All three receipts retain0.15GiB/no cap/no threshold override.

The v3 result directly supports190 visited directories, six native ledger-parent stores, two additional stores (`sec_13dg`, `sec_144`),367 original SHA names and zero candidate paths at the bounded observations. One Databento symbology directory is explicitly depth-pruned. The four previously checked stores are discovery observations here; their earlier object lookups are not represented as simultaneous new checks. No object subtree traversal, other storage layout, external root or backup medium is implied.

The report correctly confines the stable result to repeated bounded metadata observations. It does not claim an atomic global snapshot, global original-body absence, payload integrity, complete source coverage, restored files, D0/A0 acceptance or a gate promotion. Payload/network/restoration counts remain zero. A missing original remains an actual arm dependency; another concrete retained location could support a separately bounded lookup, with any candidate requiring its own original-byte authentication before use.
