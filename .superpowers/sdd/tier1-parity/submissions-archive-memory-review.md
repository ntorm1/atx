# SA1 independent static review

Decision: **no Critical or Important findings** in the frozen candidate. It may
proceed to the planned focused checks and retained-source measurement. This is
not runtime acceptance, a qualified memory profile, or production qualification.

Reviewed the SA1 brief/report and lower-memory audit; the new
`_submissions_archive.py`; the archive integration diff in `sec_submissions.py`;
the new archive tests; and the focused bulk/resume test additions. Compared the
delegated ZIP operations with the installed CPython 3.12 stdlib source. No Python,
tests, database/archive opens, network requests or heavy workloads were run.
CompanyFacts archive17 was left untouched. This is SA1's one independent review.

## Correctness and resource assessment

- Central framing is bounded by the directory extent and individual unsigned
  16-bit name/extra/comment lengths. The synthetic one-entry ZIP keeps filename,
  Unicode extra-field and ZIP64 member interpretation with stdlib; its offset
  adjustment is zero before the real archive's concatenation adjustment is
  applied. End-record parsing uses the same ZIP64 fixed-trailer convention as
  the installed stdlib. Count/framing disagreement, multi-disk layout and
  invalid local offsets fail before publishing an index.
- Every physical entry contributes an offset, including entries overwritten by
  duplicate normalized names. SQL selects the last name while the ordinal test
  reproduces stdlib's stable descending-offset overlap boundary, including
  identical local-header offsets. Payload reads delegate local-header/name,
  decompression and CRC checks, with an additional expanded-length check.
- The directory, sorted names and membership lookup reside in SQLite. The
  Python loop retains one record at a time; SQLite receives a fixed cache,
  disabled mmap and file-backed temporary storage. Ordinary `ZipFile` is never
  constructed on the complete source. Per-member bytes/JSON, pandas frames and
  the existing batch50 buffer remain separate memory risks and are explicitly
  outside the claimed directory improvement.
- Reuse requires independently hashing the source and completed index, matching
  source stat/parser identity and the index receipt, then matching selected
  central bytes against the source. Partial files do not authorize reuse. The
  index-then-receipt replacements cannot authorize a mismatched interrupted
  publication. The documented trust model correctly excludes coordinated
  malicious cache-and-receipt replacement; the digest is corruption detection,
  not external authentication or historical-vintage evidence.
- Normal success, exceptions during construction, iterator completion and
  context exit close their source, SQLite and payload-reader handles. Failed
  cache validation closes its temporary read connection; build cleanup targets
  only that invocation's temporary paths. Persistent cache files are retained.
- The integration preserves sorted unique main traversal, history lookup,
  recent-before-history precedence, original batch boundaries and the existing
  all-forms/all-CIK/history resume proof. Prefix and remainder cursors replace
  in-memory name collections without narrowing the full-scope denominator.
  Resume source identity comes from the fresh source hash, not cache metadata.

## Remaining acceptance

Run the planned archive and affected bulk/resume checks plus scoped lint in the
next authorized resource slot. Measure the actual retained 991,042-entry source
without eager whole-archive `ZipFile`, reconcile main/history/byte denominators
and selected member digests, and record native peak. Full submissions acceptance
still requires its actual dataset/stage ledgers, all 985,667 main members, zero
missing referenced history, `scope_complete=true`, and verified-prior plus new
rows reconciled to covered rows. Fixtures or a directory-only result cannot
establish the write/normalization profile or complete the production goal.

Review anchor: HEAD `5bfe05ad0d36cd8fca30788de63cc5629e0ded92` plus the uncommitted
candidate. SHA-256:

- `_submissions_archive.py`: `56e1a8c41e4f6d760c50d649e4cc7a0e815089e4bd6be95706ac6bb170ec64a6`
- `sec_submissions.py`: `8caf2ea7b582da0f6c642a2a04f37df5efc5da487d15c649fdb1dc78c169f1ac`
- `test_submissions_archive.py`: `469ce05e492ea1a53186cbeba057af46bf4f63fec6ce76070d6c3bf47229b5ac`

Only this review file was written. No commit, code change, source mutation or
stash action was performed; `stash@{0}` remains untouched.
