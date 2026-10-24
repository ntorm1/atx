# Local main follow-up checkpoint, 2026-10-24

The next reviewed development checkpoint builds on local main `163c4f8d`.
Tested source is `55a8155112bdf9f354ab1f1fe79c31444c5302af`. It includes the
strict CPU-counter validation, the European Greek-admission correction,
BEV/strategy field guards, carried-mark metadata/reduction and versioned native
Greek diagnostic semantics. Numerical research backends remain separate.

The European fix derives each bumped sample's exercise regime from its actual
model. An unfloored European price rounded to intrinsic no longer invents an
exercise clamp. A genuinely American rate bump still refuses crossing Rho.
Carried marks retain finite price independently of missing Greeks, restore
metadata with the mark, and reduce final rows so partial/complete transitions
cannot leave stale NaN totals. Volatility-based closing costs require supplied
Vega. Price-based costs can preserve a partial price without inventing Vega.

Combined validation on this exact source:

- Pricing: 62 pass, one process-counter skip and one existing disabled test.
- Portfolio, backtest, strategy and carry controls: 229 pass.
- BEV: 19 pass, one retained historical `Q0-F3` failure,
  `Breakeven.GoldenPathPnlIsPinned`. Its actual `1.3832639438392142` is identical
  to the archived pre-guard baseline; its original pin remains unchanged.
- Full registered Python CTest: 804 pass, 245 environment/fixture skips,
  one existing strict expected failure; no failed tests.
- Native Greek receipt readers: all 24 controls pass, including the original
  v1 receipt and the new European v2 operand receipt.
- The separately reviewed CPU-counter component passes its full 20-test owning
  executable and both owning PCH-off objects. Invalid counter intervals now
  yield unavailable percentages; exact raw snapshots are retained.

This is not a full release qualification. The known BEV pin remains visible,
and the newly developed conditional-scenario and margin guards are subsequent
work. No qualified subsecond fit or worldwide performance leadership is claimed.

Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.
The native receipt `integrated-bev-carry-native-receipt-v1.json` has SHA-256
`e64d5ae2c18d3086afcc2f9024b7377d6500d05ae7ee895fe4f4375aa3517a4e`.
The Python receipt `integrated-bev-carry-python-receipt-v1.json` has SHA-256
`cb3623168a6b6aeac62ba919dac56f01b959ec53e7b9b2ce4120df43e601f7cf`.
Both bind the exact source, binaries and retained logs.

A fresh pre-merge inventory preserves the independently updated local
`docs/superpowers/specs/2026-10-24-atx-vol-risk-engine-design.md` rather than
restoring its older snapshot. The inventory covers 52,032 local files totaling
2,120,108,465 bytes; `main-local-worktree-before-v3.json` has SHA-256
`af59d454ff7440723e8bb26be019b4773d8b35b70fd65ae21acf90561a495131`.
The source range does not overlap those modified or untracked paths. Final
merge/preservation results are recorded in `main-local-worktree-after-v3.json`.
