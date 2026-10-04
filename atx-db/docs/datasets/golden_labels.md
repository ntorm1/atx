# Separate complete holding-window labels

`golden_labels.build_labels` publishes a physically separate immutable label
generation. Every native feature/name has 5/21/63 horizon rows, including
missing/prohibited-window exclusions. Entry is the next session close; horizon h
contains exactly the following h session returns and exits at feature index
h+1. Every complete window, price, cash and terminal observation must remain
strictly before 2023-01-01. Crossing windows have no usable entry/exit/label.
Unknown returns, absent actions, missing proceeds or residual cash never
become zero. `validate_generation` fully reconstructs every value, NULL,
reason and population, and recomputes coverage evidence.

The fixed `golden_returns.NativeWindows` protocol
`golden-native-windows-v1` requires an independently reviewed Task8
`char_replication_label_source_v1` original source contract, exact original
price/action inventories, full native code pins, per-unit immutable window
inventories and independently reviewed `golden_complete_window_v1` documentary
proofs. Proofs bind the exact feature/security/horizon, calendar window and
all original observations. A self-asserted generic return Parquet is rejected.

Native marks are reread through Task12 `mna_prices.read_vendor_price` using
exact typed VendorPricePin/EconomicPin identities, original securityID/date,
USD raw close, share class, no-FX and raw/unadjusted economic declarations.
The Task8 contract supplies the admitted positive split multiplier and cash
amount for each observation; absent action rows do not establish no action.
Positive cash amounts require `distributions.validate_product` full original
Task13 replay plus the matching event/class/currency/amount and separately
reviewed original ex-date quote. Vendor calendar schedule documentation alone
does not admit any local row, currency, share basis or historical availability.

Cash conversion invokes Task12 `mna_exact.replay_exact` with original
document/SEC issue clock, native economic and price evidence, exact code
contract and auxiliary original bulk-clock/receipt identities. Qualified cash
proceeds replace the prior raw price directly. Every subsequent observation
requires a separately reviewed dated original USD cash-factor observation.
Stock/mixed terminal residuals without a fully admitted continuation convention
remain explicit missing outcomes. Qualified rows retain full proof and source
contract SHA plus availability no earlier than exit-close/evidence clocks.

Source proof inventories remain unprovided on actual data. The code includes
the conditional native calculation and full publication/replay; missing proofs
produce honest NULLs and FAIL coverage, not a claim that the full original
label/source gate is complete. Offline tests use original-shaped synthetic
vendor/actions and documentary pins only.
