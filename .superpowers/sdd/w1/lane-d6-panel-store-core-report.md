# W1 D6 panel-store core: initial source slice

This is implementation-first source with seven postimplementation C++ fixtures.
No compilation, runtime test, historical artifact build or market payload read
has occurred. Stage integration is the next slice; this core does not by itself
close D6 or establish its large-union RSS/rank-IC acceptance.

## Storage and precision contract

`data/panel_store.hpp/.cpp` defines explicit ATPSTR2/ATPCHN2 version 2. Immutable
chunks contain one date-major block per named field, an original-f64 adjusted
close channel, separate binary present/tradable blocks and a per-date membership
decision key. Field level basis and f32/f64 precision are explicit. Every output
column belongs to one fixed, sorted positive numeric security-ID union; original
source-axis indices, exact session labels, source parents, membership SHA and
recipe bytes are manifest-bound. The API never derives publication from a session
label. The tradable bit means as-of membership, independently of current data
presence, and requires a positive membership decision strictly before the session.
A member with a missing source row retains membership; its numeric cells are NaN.
Consumers may derive presence AND membership for execution, while missing-held-mark
and coverage checks retain the distinction. Presence alone preserves warm-up data.

Finite f32 storage uses ordinary rounding and rejects overflow/underflow to zero;
infinities are refused. Absent cells and numeric NaNs use canonical quiet NaN
encodings. The `returns` field must retain original f64 precision, including signed
zero. The separate exact-close channel stores original f64 values, allowing the
same endpoint/entry-minus-one arithmetic without reconstructing f32 prices.
Finite return bits are identical only for identical original endpoints and the
same formula. Cold-start boundaries, terminal returns, guards and NaN payloads
are not silently declared equivalent. No rank-IC tolerance is claimed without its
planned numerical comparison.

All numeric row reads widen into caller-owned f64 spans before arithmetic.
`forward_returns` enforces an explicit exclusive maturity boundary, uses one
N-element f64 scratch row and at most one live mapping. It does not invent terminal
returns or apply an unrequested bad-return filter.

## Bounds and publication

Preflight bounds axes, fields, metadata, total payload, chunk bytes and writer
working storage before allocating the chunk. Default chunks span 32 dates;
each is at most 64 MiB. Opening reads at most 8 MiB metadata and checks declared
file extents without reading every numeric cell. Each captured read-only mapping
is SHA-validated and checked for numeric/mask/clock invariants before exposure.
Copied stores share a live mapped-byte/handle budget; chunk owners retain their
mapping independently, and release OS resources before returning budget.
This bounds mappings and decode buffers, not an observed operating-system RSS.

Review corrections add a checked Mapping overload: the opened handle's size is
admitted before CreateFileMapping/MapViewOfFile or mmap, and only that admitted
extent is mapped. A post-open grown-file fixture covers rejection. The existing
one-argument API remains available. f32 range is checked before narrowing, and
the roundtrip fixture explicitly preserves an absent member's membership bit.

The writer exclusively creates a fresh directory and chunk files. A complete
hash-bound manifest is published last by a no-replace hard link; incomplete stores
cannot open. Reads never repair or rewrite artifacts. Expected manifest SHA can
be supplied as an external anchor; content hashes are not source authentication.

Prepared fixtures cover mixed presence/tradability and retained warm-up data,
f32 rounding versus exact-f64 returns, cross-chunk maturity, publish-last/no-replace,
captured-byte corruption/extents, shared handle budget/view lifetime, malformed
axes/clocks/numeric bounds, and earlier-prefix invariance under future mutation.
All are uncompiled/unrun. Root registration needed: `src/data/panel_store.cpp`
and `tests/data/data_panel_store_test.cpp`; no CMake changes were made here.

Pending: actual stage/pipeline adapters, all-time membership-union construction,
bounded historical assembly, explicit legacy dispatch, source review and focused
runtime qualification; then authorized real 2012-2019/D5 inputs and the original
6,624-ID RSS, rank-IC, return-overlap and causality acceptance evidence.
