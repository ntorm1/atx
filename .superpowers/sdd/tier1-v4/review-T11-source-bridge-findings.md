# Task11 source bridge independent review

Reviewer: /root/t5_prebuild_review; implementationc6f40804; 2026-10-04.
Ledger-only interface: scoped Approved. Bridge spec and quality: Needs fixes.
Earlier native I1-I4 remain closed; no actual source/economic acceptance.

Important SB-I1, thirteenf_quality.py233: URL accession substring matching accepts
/Archives/edgar/data/1/000000000120000001999/complete.txt for expected
000000000120000001. Require an exact accession directory component or supported
submission filename; cover overlong, prefixed and valid path examples.

Important SB-I2, thirteenf_quality.py340,398,403: an initial existence check then
partial.replace(output) overwrites evidence created in between. Receipt writing
also allows overwrite. A fixture reproduced destruction of a concurrent output.
Use atomic refuse-existing publication for both output and receipt; preserve
forensic partials on collisions/interruption. A final existence check is not
sufficient. Cover both destination races and interrupted publication.

The ledger helper bounds records, bytes, record size and candidates, retains
UNKNOWN/incomplete diagnostics, checks metadata identity, and opens no referenced
objects. Bridge byte digest/cover total/position parsing remains explicitly
unqualified with null public availability. Two selected regressions passed0.99s
with explicit -n0 -o addopts='' and OPENBLAS1; tiny reproductions confirmed both
findings. No actual source/data/hash/HTTP jobs, edits or live review jobs.

Root dispatched original author for source-bridge fix round1/5. Full Task11
acceptance and original-source completeness remain open.
