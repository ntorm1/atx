# A0 provenance preflight: published pin confirmed; D0 rebuild still unmeasured

The retained pit bridge manifest matches P9's exact requested SHA256
`09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01`.
Seven named JSON manifests (296,141 bytes) were read and checked again, together
with current source files and read-only Git objects at `dec03454`. No Parquet
bodies, source payloads, return rows or network requests were read or issued.
The published files and P9 worktree were unchanged.

The first guard attempt, `logs/p9-a0-manifest-preflight-v1.receipt.json`, refused
before its child because free commit was 0.5162 GiB. Its stdout, stderr and result
do not exist. This is a resource refusal, not a producer or metadata assertion
failure. A fresh v2 guard attempt used unchanged 0.15 GiB admission thresholds
with bounded waiting; it completed 0/1s/65,548,288 peak bytes after memory recovered.
It ran controller v1 and produced `p9-a0-manifest-preflight-v1.json`.

Controller v2 then extended the code comparison to all twelve fixed r4 code
declarations, covering seventeen distinct source files in total. It requires the
exact twelve-name set in both the r4 build and manifest. Fresh guard attempt v3
completed 0/2s/65,740,800 peak bytes with no cap hit, producing
`p9-a0-manifest-preflight-v2.json`. This is the latest preflight; earlier records
remain unchanged. No acceptance or native byte guard was relaxed.

Observed provenance:

- Both the pit export and link-table manifest declare git_head
  `c0dc376f26fbb7b910085009eff6d84b61247a12`. Their identity_table declaration
  matches the `dec03454` source after the explicitly reported line-ending
  comparison. Their common.py declaration matches neither that Git source nor
  its CRLF encoding. A recorded HEAD is not proof that every producing file was
  committed at that HEAD, so a supported single producing commit remains unknown.
- The Stage I manifest identifies `identity-links-v1`, the exact retained r4
  manifest and its declared exported inputs. Its historical validation records
  pass=true. This is the original producer's declaration, not fresh D0 evidence.
- The r4 manifest and build pin declare the same twelve source-file hashes.
  Eight match `dec03454` raw bytes or its CRLF encoding. Four do not:
  `_fundamental_clock.py`, `identity_reconstruction.py`, `market_daily.py` and
  `market_owner_bridge.py`. The complete pairs are preserved in the v2 result.
  The causes of those mismatches have not yet been established. None is treated
  as a line-ending-only discrepancy or waived for a future rebuild.
- The backfill and name manifests contain no code identity, as the request
  predicted. The r4 build pin retains original input, OPS, preparation, document
  review and retained-database identities as leads. Those declared paths have
  not been opened or certified by this preflight.

Next: recover and inspect the exact preparation/OPS/review chain and producing
code versions; design the isolated S1-S4 implementation and resource plan, then
obtain independent review before any actual A/B rebuild. D0 full-row comparison,
all four differential columns, rows 1-11, PIT fixes and synthetic tests remain
unmeasured. The preflight does not authorize overwriting or re-pinning the bridge.

An early inspection printed an overly broad metadata declaration object and
was truncated; subsequent reads selected exact keys and comparisons. No source
body was opened by that display, and no finding relies on the truncated output.
