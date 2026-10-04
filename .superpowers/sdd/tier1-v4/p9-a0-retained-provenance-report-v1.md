# P9 A0 retained provenance and preparation dependencies

The retained rehearsal code is recoverable. All twelve exact snapshot files match
the raw hashes pinned by both the published r4 build and manifest. After CRLF to
LF normalization, all twelve also match `dec03454` and the current source files.
The four apparent r4 mismatches in the earlier manifest-only preflight are mixed
line endings, not evidence of different normalized source code. Native raw-byte
guards remain mandatory. This result does not resolve the alpha-panel
`common.py` provenance mismatch or identify a single producing commit.

The original preparation chain is incomplete at its named paths:

| Dependency | Current observation | Qualification |
| --- | --- | --- |
| `session5/preparation`, its JSON and retained DuckDB | All absent | Fresh preparation required; no reconstruction performed |
| Original input receipt `data/tmp/v2/0.6/ri2_a.json` | Absent | Path recovered from historical `prep-vendor.json`; original r4 build retains four input descriptors |
| Four original retained source paths | Three present; original staged TickerHistory absent | Stat only; no source payload hash verification |
| Original OPS manifest | Exact published hash matches | Five present-declared exports exist, stat only; sixth `security_identity_evidence` export was already declared missing |
| Original document-review manifest | Exact published hash matches | All 367 original body paths named in r4's pin are absent; metadata alone cannot replace these bodies |

The directory snapshot has exactly the twelve declared code files. Metadata/code
bytes and observed path states were independently checked again before result
publication. The audit read no Parquet, database, ZIP or HTML bodies, performed no
network request, wrote no source or published data, and ran no arm or D0 comparison.
Its evidence is `p9-a0-retained-provenance-v2.json`; its versioned controller is
`p9-a0-retained-provenance-v2.py`.

The first controller assumed every OPS export had a file and failed on the
historically missing export (`KeyError: file`). Preserve v1 and its failed receipt:
exit 1, 1 second, 66,535,424 peak job bytes, no result JSON. Version 2 explicitly
retains declared-missing exports and rejects unknown states. It completed with
exit 0 in 2 seconds at 66,449,408 peak job bytes, under the original 0.15 GiB
metadata guard, with no cap hit or threshold override. Its guard receipt is
`logs/p9-a0-retained-provenance-v2.receipt.json`.

The original successful vendor command names the missing receipt; it is evidence
of the old command, not evidence that the receipt or source bytes survive today.
The original build pins remain unchanged: preparation
`2b23a7b1d7f6ae4e23dff7a67bc2943c7da071d53a97a63b9cc19604a7835954`,
retained database
`543b76bbbdb0c49a8673be0ba16a500d32e29ab281d14740bda092770f0a3cd0`,
OPS `fcfb46c68bb447703a2b7e46a994523a13d89d4474ea2b75d00c6a9a8d616165`,
and document review
`1e9625618935ec4db928a75910d2b677a8b8f4fbeba2e56eb95994f119866a58`.

Next: inspect retained cache/archival candidates for byte-identical missing
originals without network acquisition or pin substitution. The Downloads
TickerHistory file exists (3,617,973,507 bytes), but its hash has not been compared
by this audit. A recovered input receipt would have to reproduce the exact four
original descriptors and preserve their original clocks with explicit recovery
provenance. No filesystem timestamp today can substitute for those clocks.

S1-S4 must be implemented in an isolated checked code copy: current
`alpha_panel/identity_links.py` is explicitly included by the frozen financial
candidate's `fund_batch_release.code_paths`, and `common.py` is also protected.
Do not modify either shared file to run the diagnostic. A, B1, B2 and B12 must use
the same diagnostic code, including S1 off in A. S4 can record failed validation
and continue only in isolated diagnostic roots; it grants no source, production,
coverage, predictive or consumer admission. D0 and all arm measurements remain
unperformed, and no release gate changes from this metadata audit.
