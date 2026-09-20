# AR6 review corrections

Review: `ar6-price-source-review.md`, two Important findings, no Critical
findings. The existing source audit, raw bytes, numeric transformation and
historical timing/identity limitations remain unchanged. No source scan or
production write is part of this fix.

## AR6-R1: replace stale bars for quarantined incoming keys

The plain loader now registers selected original source keys before canonical
filtering. Within the existing transaction, it invalidates that source's
matching vendor/date bars even when the incoming canonical frame is empty.
The positive vendor identifier drives invalidation, so an earlier bar with a
different canonical security ID cannot survive a conflicting reimport.
Nonpositive/missing-ID placeholder keys additionally require their display
symbol, avoiding blanket deletion of unrelated placeholders on that date.

Raw replacement still stores every incoming conflict row. Other sources,
vendors and dates remain isolated. Both full and price-projection-only paths
have a regression fixture that first loads accepted bars, changes the incoming
canonical identity, then reimports a quarantined positive key; it verifies the
old bar disappears while unaffected bars and original conflicts remain.

## AR6-R2: stable vendor identity and explicit competing-link fallback

Existing link reuse now requires exactly one distinct canonical security for
that vendor ID. Competing links fall back to the stable vendor ID namespace;
warehouse `source_loaded_at`, insertion order and timestamp ties select no
winner. Multiple in-frame current-symbol candidates follow the same fallback
instead of choosing the last source row. This remains unresolved issuer
identity, not a new historical CIK inference.

Positive vendor collision keys now contain only the stable vendor ID in both
adapters. Their display metadata uses an explicit latest `(trade_date, symbol)`
ordering; historical ticker changes cannot change the synthetic key. Negative
or missing-ID placeholders retain the existing symbol-based namespace.

Two reversed-order fixtures cover competing prior links with reversed load
timestamps and a colliding vendor line renamed from OLD to NEW. The latter
asserts one `TBLTICKERHISTORY-202` key for both display symbols and idempotent
collision handling. Existing plain/bulk collision expectations were updated
from vendor-plus-symbol suffixes to the stable positive vendor key.

## Scope and validation

The bulk current-symbol candidate map still uses current metadata and load-time
ordering; that separate historical identity prerequisite remains documented.
No split/return semantics, downstream consumers, registry, activation or jobs
were edited. The exact guarded operator handoff and T11 truth/memory corrections
are a separate documentation commit/report.

All 32 cases in `tests/test_ticker_history.py`,
`tests/test_ticker_history_bulk.py` and `tests/test_ticker_history_price_source.py`
passed once with `-n 0 -q` in the controller-granted database slot. The first
4 GiB guard request refused before launching pytest because physical headroom
was 5.664 GiB, below its required 6 GiB. The run then passed under a smaller
3 GiB aggregate cap, preserving the guard's 2 GiB preflight margin. Native peak
job memory accounting was 0.706451 GiB; final physical headroom was 5.940 GiB.
Both receipts are retained as `ar6-review-fix-focused-memory.json` and
`ar6-review-fix-focused-memory-3g.json`. No guard was bypassed and no production
database was opened.

Touched-file Ruff, dictionary `--check` and `git diff --check` passed. No new
helper was introduced, and the previously checked helper/script were unchanged.
No full suite, duplicate test run or independent re-review was performed.
