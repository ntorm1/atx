# Durable task state — reported-quarter EPS source (0320)

The original `integration.patch` is preserved. Root applied the standalone
`integration-final.patch` (SHA-256
`BAC6C8DE5808B5FCF2897E2AB3DADD2F962F1D808762309F07A787F900C0B170`)
to live 0320 files. This draft mirror now contains an isolated follow-up in
`source-fixture-followup.patch` (SHA-256
`5B6332C1AF3EE7E5BA6115F64010B5DECBE89DF8035D707BEFC3430BC4663A23`).
The follow-up changes only live-targeted `press_release.py` and
`test_reported_quarter_eps_source.py`. Static
`git apply --check --whitespace=error` passes against integrated live files.

Root's first focused source/bulk/press-release run reported 28 passes and
three fixture failures: two expected 2026-02-01 when filing date plus 46 hours
is 2026-01-31 22:00, and one omitted a shared quarterly duration heading
above its prior-year column. The follow-up corrects those fixtures while
retaining strict prior-year header qualification.

The follow-up also repairs actual SEC exhibit discovery. The directory
`index.json` uses MIME labels such as `text.gif`; source-owned EX-99 Type
evidence is in the accession's filing-detail HTML Document Format Files table.
The loader now fetches that accession-local index once, accepts only a typed
EX-99 row with a flat document name and href inside the same accession
directory, and rejects MIME/name guesses. Frozen official local index
inspection supports the change; the new follow-up has not been run or applied
by this draft implementer. Root independently reported that the full official
EX-99 table extraction produced 1.39 current, 1.84 prior, and the 2025-12-31
quarter end.

Next: root applies this small follow-up and reruns focused source tests under
its runtime guard, then requests narrow Critical review. No new database,
network, import, or test run occurred in this draft task.
