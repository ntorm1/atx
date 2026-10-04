# R28 follow-up: same P9 request precision clarification

The owner-linked file changed from59,567 to59,756 bytes. Full new snapshot
`p9-al-data-request-v3.md` SHA256
`8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321`
is preserved; the P9 worktree was read only. Goal8.2 incorporates it. The complete
previous requests, all failed results and every original release requirement
remain binding. This revision changes no threshold or permissible source guard.

The three changed passages clarify that S4 is about validation counts, that
equal counts do not prove equal rows, and that B1/A and B12/B2 share Stage I
counts because they share rehearsals. D0 still compares full rows. They also
clarify one git_head versus per-module raw/LF SHA fields, and that Stage I's
code_version/input hashes do not establish its producing code. No single
producing commit has been demonstrated for the three stages lacking code hashes.

Readiness12/appendix3 preserve all46 and23 complete requirement records and add
binding interpretations to existing P9-I01/I02/I05. Existing statuses, gates,
cutoffs, source obligations and consumer seals remain unchanged. Prior audits
still cite their exact v2 request snapshot; none is retroactively re-pinned.
The new D0 kernel already uses full-row comparison, not validation counts.

Independent readiness12 review found Important I1: the main omitted the current
appendix3 SHA binding. Preserve that failed version and review. Readiness13
corrects only this byte binding and review/supersession metadata, using the exact
unchanged appendix3 SHA
`d37d1e36d4d82b0520a9fa16831e30c0eac1e4f95dc9c430fae52aa8b6d33ba6`.
`refresh-readiness-v13.py` reads and authenticates those exact existing serialized
bytes before publishing main13. Original46/P9 23 records remain unchanged.
Its guarded generation completed1s/26,628,096B under0.15GiB, with no cap hit.
Independent I1 rereview PASS/PASS is in `review-readiness-v13-findings.md`;
the earlier rejection remains preserved and no release gate is promoted.
