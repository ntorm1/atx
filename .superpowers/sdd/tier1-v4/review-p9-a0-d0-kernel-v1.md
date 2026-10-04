# P9 A0 D0 kernel independent review v1

Reviewed frozen checkpoint `49770435`: private_identity_differential.py, its direct tests, p9-a0-d0-kernel-report-v1.md and the exact v1 .out/.err/.receipt.json evidence. Static review only; no tests, actual source reads, hashes, staging or implementation changes performed. The only outside-code check was identity_links.py:29-40 to verify the original security_company_links TABLE_KEYS.

Spec verdict: PASS for the standalone conditional diagnostic kernel.

Quality verdict: PASS within this bounded helper scope.

## Numbered findings

1. No Important or Minor corrections identified in the reviewed implementation. This verdict does not approve actual D0, producer provenance, restored source admission, differential arms or downstream acceptance.

## Checked invariants

- private_identity_differential.py:94-118 compares ordered names and DuckDB native types, then the entire remaining row in both EXCEPT ALL directions. Multiplicity is preserved; mismatched schemas return explicit failed evidence and NULL difference counts. PIT and link_table retain every column. Only r4's exact created_at column is excluded, and its presence is required.
- Lines 120-136 use the original four security_company_links TABLE_KEYS, matching identity_links.py:39. Duplicate key groups and NULL-key rows independently prevent r4 D0 success even for identical multisets. Non-key content remains in the comparison.
- Lines 26-59 stream raw bytes in 1MiB blocks and authenticate explicit independent SHA256 pins before and in finally after comparison, including early return and SQL errors. Path/reparse and regular-file checks plus pre/post stat identity checks reject ordinary input replacement or mutation. The covering mutation fixture changes a physical input during EXCEPT ALL and observes refusal, rather than accepting a computed result.
- Lines 74-87 enforce one thread, UTC, at most128MiB database memory, a file-backed connection and configured spill storage. Large rowsets remain in disk-backed SQL; Python retains schemas and scalar counts only. The outer guard, isolated spill directory and complete orchestration/code manifests remain caller obligations, explicitly documented rather than claimed implemented.
- Lines 89-99/138-140 create unique temporary views and clean only their own names in finally. No input write, producer mutation, directory cleanup or whole-row Python materialization occurs.
- Every result carries source_quality_certified=False. The report clearly limits the helper to synthetic implementation checks and lists missing actual source restoration, reviewed orchestration, same-code arm controls, actual D0/four comparison tables and PIT repair tests. Capsule-plan controls are not implemented by this kernel and receive no review credit here.

## Evidence and dependencies

The exact .out reports3 passed in0.85s; .err is empty. The v1 receipt records completed/returncode0,2s,64,602,112 peak job bytes,0 cap-hit events, unchanged0.6GiB cap and40GiB disk floor (observed40.172637939453125GiB), with no test override. These are author-run physical synthetic fixtures, not reviewer reruns or actual D0 evidence.

Actual use still requires independently reviewed full-manifest/input/code binding, original missing preparation/receipt/documents, isolated bounded invocation and all mandated A0 arms and acceptance gates. Identical arbitrary inputs can legitimately produce diagnostic d0_pass=True; that flag alone establishes neither the correct published population nor source qualification. Schema equality here covers exposed column names/types, not original Parquet bytes or provenance, which remain independently pinned caller inputs.
