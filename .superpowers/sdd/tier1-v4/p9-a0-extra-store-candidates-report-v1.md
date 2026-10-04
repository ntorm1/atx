# Additional retained original-body store lookup

A bounded metadata walk under `atx-db/data/raw` found six native
`fetch-ledger.jsonl` parent directories within depth0..3. Four were already
checked in the previous recovery audit. The two additional stores are
`sec_13dg` and `sec_144`. Neither contains a plain/gzip object at any of the367
exact original document SHA names. No ledger or body payload was read.

This is not an exhaustive backup search or source-quality admission. The walk
visited190 directories, pruned every objects subtree, and did not recurse into
one depth-limited Databento symbology directory. Non-native storage layouts,
other roots and backup media remain outside this lookup. The earlier four-store
observations retain their own timestamps; this does not claim a simultaneous
new body lookup across all six stores.

Preserve the versioned observations:

- v1 failed at its final metadata-state check, exit1/3s/27,545,600B, and wrote
  no result. It compared directory st_size as if that were a regular-file byte
  length; its generic failure message did not identify the changing fields.
- v2 exposed all changed fields without claiming a stable result. It completed
  exit0/3s/27,447,296B with status UNSTABLE_METADATA_OBSERVATION_NO_SNAPSHOT_CLAIM.
  All75 differences were directory sizes; their presence, kind, inode and mtime
  were unchanged. This does not demonstrate a source writer or payload mutation.
- v3 compares directory device/inode/mtime and the exact sorted child
  name/type/symlink digests, while preserving regular-file byte-length checks.
  Both original metadata bytes and all observed states are checked again before
  publication. It completed exit0/4s/28,905,472B, with zero changed states. Its
  current result is `p9-a0-extra-store-candidates-v3.json`.

All three used unchanged0.15GiB metadata guards with no cap hit/override.
No files were restored, no HTTP request occurred, no original file was changed,
and no A0/D0/coverage/evaluation/consumer gate was promoted. The missing originals
remain an actual arm-build dependency. An optional user question about a backup
location remains pending; authorized implementation continues independently.
