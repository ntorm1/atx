# Freeze preparation: READY, final release still pending

GPT-6 Astra, R15-FREEZE-PREP. Work only in pool-2. No commit, build, test,
statistical run, trial append, lease action, ref mutation or cleanup. This is
preparation evidence, not a freeze verdict. Exact metadata is in
`freeze-preparation.json`; neither file existed before this agent created it.

Hash snapshot began 2026-10-17T18:21:18.872464Z at
`71226ef9be8816e2c5e6b0ded657249d9c81b712`, branch
`feat/platform-p9-20261003`. The preparation ledger boundary is progress.md
lines 1050-1147, through R15-FREEZE-PREP; whole-file SHA256
`e7c70c64af50ea17403c80adea163f60e788035f7ad8a7738eacd1e292d48bab`.
Later documents and rulings are outside this snapshot.

All 54 frozen executable hashes match p9mv-frozen-exes.json and the 27+27
p9-3f/p9-3g receipt entries, source
`5f48a25d3730789914941fbc7aa42f4458cea6b1`; re-pin remains 2078f877.
CRT version 10.0.26100.7623 and SHA256
`2c08e77f86b0953cffa505bdbe64e87ecb7c088db65d5acb9a2eee65a32565ef`
match. Both D-COV manifests and model.atxcov payloads, v15 and role manifest
match the adoption-start identities. Payload hashes used 1 MiB streamed reads.
All six reference listings and 126 named file sizes/hashes match, without
extra or missing files. V's final 100 output files and 41 source files match
their committed listing/audit by hash only. No output statistics were parsed.
Default research-store and lo3 index.sqlite remain absent; the five named
absent field directories also remain absent. No mismatch was found.

Trial ledger: 140 lines, SHA256
`086ca77e40806bb8f71039bd5e4b824325dcb2124b845b81ffd5784e364e8f5d`.
Original 133 raw lines retain SHA256
`27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
Canonical accounting: Nc69, Nhand130, inherited M110, Ntot240, history0;
eight historical window reruns add zero. Only unchanged pure accounting
function definitions were selected from backtest_integrity.py via AST;
no module-side reader, append function or statistical calculation ran.
Historical prefixes and accounting records preserve L63/124/234,
H64/125/235, M65/126/236, C66/127/237, MV67/128/238, X68/129/239,
then V69/130/240 (Nc/Nhand/Ntot; M110 throughout).

Seven cell records, their Appendix A evidence, three independent hand DERIV
records and PM dispositions/releases are present. L/H/X accepted,
M/C/MV/V rejected, S/W lapsed0; seven count1 trials. Eleven relevant
completion/DERIV/accounting commits have only sprint-document/evidence
paths and no mypy_cache path; exact scopes are saved in the JSON.
The four R15-APPENDIX-SPLIT notes at c3e3735f preserve each historical
checkpoint and the 61=12+25+9+15 historical admissions/P9+0 distinction.
G-P1 evidence remains UNAWARDED; G-P5 PARTIAL, G-P10 print-only with
(d) PARTIAL, and G-P11 required. Documentation completion awards no gate.

main=`e696e208ef622aeb8a9ffbcc4bd3e4ea16a74548` and
origin/main=`8786863866bcecfbfe3d6f7a90a132bc85a6915c` match session start.
Tracked dirt at the snapshot is PM progress.md only; five owner PNGs are
untracked and were never opened, hashed or staged. Read-only lease -Status
shows P9 owners dead except pool32 alive. Foreign pool35 is now free versus
alive at RESUME15 start: an observed outside change, no action by this agent.
Pools8/24 and pool23 binaries were untouched; all leases remain untouched.

Read standing contracts, prereg G, FREEZE, evidence map, gate/latest cells
handoffs, two-track/platform report and PM drafts. Adoption-print.md,
od3-read.md and rulings-index-final.md did not yet exist at the document
check; final-report.md and freeze-verdict.md were uncommitted PM drafts.
Their final review is deferred, not asserted. OD3-DB remains unanswered;
the required default is not read in P9 / G-B8 not evaluated. The exact
dsr-years PM_DOC_METADATA exception is R15-ADOPT-PM-METADATA, not a
classifier change or scientific retry. Historical GATE-REV exact refused
argv is unavailable and must not be reconstructed.

Commands: git rev-parse HEAD main origin/main; git branch --show-current;
git status --short; git diff-tree --no-commit-id --name-only -r for the
named evidence commits; powershell -NoProfile -File
scripts/lease-worktree.ps1 -Status; explicit Python312 -B inline metadata
checks. Graph tools were absent, so targeted rg/source/document reads were
used. Ordinary missing-path/mapping lookup errors are listed in the JSON;
there was no permission refusal. No owner PNG byte access or atx-db access.

Final release must recheck the final ledger, completed adoption/OD3/index,
changed PM documents, branch/refs/status/leases and their actual snapshot.
Unchanged hash evidence may be reused only with this head/time/scope
explicit. freeze-final-state.md and the exact-path commit remain separately
gated. This preparation modifies only its two own evidence files.
