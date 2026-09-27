# Fast IC implementation and first runtime attempt

Clean source and configured provenance: `20bf677b4aaeee8e6059b4a5863ececbf96a594b`.
The fixed 48-alpha library, three-horizon kernel and streaming composition are
implemented. Corporate-action expansion remains deferred by owner instruction.

The two focused targets built in **27.729s with three compiler workers**:
eight C++ compilations, two archive links and two executable links. Existing
PCH and dependency objects were reused. CMake regeneration took 10.9s; no full
release build or global compiler-flag change was made.

All **11 native cases passed in 1.344s**, including execution of every actual
DSL, causal historical masks, strict endpoint counts, legacy kernel parity,
fixed-family composition and frozen TRAIN orientation under reversed validation
returns. The sampled guard completed in 1.641s with 19.1 MiB peak process-tree RSS.
This qualifies implementation contracts, not profitability.

The metadata-only plan took 0.281s. Actual maximum compiled slots: seven;
maximum lookback: 314 prior sessions. Conservative complete working envelopes:
1,487,603,721 bytes TRAIN and 1,030,839,264 bytes validation.

The first real fast-IC attempt used the registered full 2020–2022 TRAIN and
2023–2024 validation roles, 2,000 minimum paired names, a 1,536 MiB internal/RSS
limit, 768 MiB free-memory floor and 180s time cap. Root stopped the owned process
early after **105.609s** because candidate timings showed that the serial run
would exceed the cap. The guard records `process-error`; the separate verified
PID/creation-time operator receipt explains this deliberate stop.

Ten TRAIN candidates completed; an eleventh started. Completed candidate times
were **6.091–11.509s**. Sampled peak RSS was **956,231,680 bytes** (912 MiB), with
at least 1,522,909,184 bytes system free. No TRAIN composition, orientation
artifact, validation candidate or combined portfolio completed. Partial results
remain retained and do not change the library, weights or selection policy.
Three older strict-book attempts also remain zero completed.

Next runtime work is the existing bounded shared VM worker pool and separate
VM/IC/composition timers. Any compiler optimization will be narrowly scoped;
no long compilation or extended research run is authorized by this receipt.

The exact 32-file, 2,323,119-byte evidence packet, including role preparation,
manifest pins, build/test receipts and partial candidate outputs, is indexed at
`fast-ic-qualification-20260926/index.json`. Its `.gitattributes` preserves bytes.
