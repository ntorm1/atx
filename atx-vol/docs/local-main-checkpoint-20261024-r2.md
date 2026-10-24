# Local main checkpoint, 2026-10-24, second merge

At the user's explicit request, local `C:/atx` main advanced by fast-forward from
`96cd56db9ad034ee6868e3e5a8babb81ac8cb82c` to the tested source
`bbb2ede663c8c86f59d95a18136f5bd6e66f9342`: 59 commits, 134 changed files.
No push was performed. Development continues in the pool worktrees.

The checkpoint carries reviewed Greek availability through cold/cached pricing,
portfolio and derivative-book aggregates, supplied surfaces, chains, sessions,
raw ticks and Python. It also retains independent Greek-reference experiments,
cash PDE batch research and the full negative or unresolved accuracy evidence.
The optional block-factor and empirical vector-seed implementation branches remain
separate; their earlier correctness audits do not constitute a latency promotion.

On the exact source above, combined native controls passed: 58 raw-tick tests,
6 served-session tests and 26 live-publication tests. The full registered Python
CTest passed with 804 passes, 245 environment/fixture skips and one existing strict
expected failure. All 17 independent-reference controls passed. Earlier component
and integration gates remain recorded in `LEDGER.md`; this is a development
checkpoint, not a new full release qualification or a performance claim.

Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.
`integrated-session-tick-receipt-v1.json` binds source, binaries and test artifacts;
SHA-256 `a8296c8ff8366388ddf35cb95ec312d1c26ab03628f4e952345e19473937ce5f`.
Before/after SHA-256 manifests verified all 52,032 existing modified or untracked
files, totaling 2,120,107,199 bytes, unchanged. The staged index stayed empty and
local status stayed unchanged. The two modified tracked files, `.vscode/settings.json`
and the root `CMakeLists.txt`, were untouched by the incoming commit range.
Preservation manifests:

- `main-local-worktree-before-v2.json`:
  `6bd4cbc85b4d2fd0e3cf9e7a3f8682d82d95c83a6d8519847ff44b989596c8cf`.
- `main-local-worktree-after-v2.json`:
  `5993836beadb85b88ea318e4fbb54123b257349e0491b7d7f0f1c89048860b75`.

Open work is explicit. European price/intrinsic rounding can falsely refuse cold
Greek stencils; its model-regime correction is being developed separately. BEV,
strategy and carried-mark consumers have further reviewed or active work awaiting
integration. CPU interval validation needs to reject decreasing counters instead
of treating them as idle. No valid quiet-host subsecond timing has been established:
the latest combined-candidate preflight failed the declared system-idle gate before
launching any timing trials. The independent Greek pilot resolves 7/12 Delta and
1/12 Gamma references; its remaining uncertainty is not candidate accuracy success.
