# Private four-lane Thomas research kernel

This target is default-off and links neither kernel into the production library.
Its private PDE batch driver is compiled inside the reference TU only when the
research option is enabled; it calls a borrowed kernel callback. No production
entry point dispatches to the driver. There is no speed or cash-price
qualification. The scalar control and AVX2 kernel own no state: callers own one
interleaved pack and provide a lane mask. Invalid input lanes leave scratch
unchanged; a nonfinite arithmetic result refuses the affected lane and leaves
partial scratch that must not be published.

The frozen `vol-rel` reference object's Thomas sweep uses fused negative
multiply-add for both forward numerators and the backsolve, with two separate
divisions. The research kernel preserves those operations explicitly. The scalar
control uses `std::fma` and separate divisions. This matches the observed release
arithmetic; it does not establish parity with every compiler profile.

```powershell
./scripts/atx-build.ps1 configure -Preset vol-rel -DATX_VOL_BUILD_CASH_THOMAS_RESEARCH=ON
./scripts/atx-build.ps1 build atx-vol-cash-thomas-research-tests -Preset vol-rel -Jobs 3
./build-vol-rel/bin/atx-vol-cash-thomas-research-tests.exe
```

The six initial tests cover 320 grid-size/policy/lane-mask combinations, every
`cp`/`dp`/solution bit, noncontiguous and empty masks, signed-zero boundaries,
unused poisoned operands, independent invalid lanes, nonfinite arithmetic and
malformed shapes. All six pass after a failing stub. Owning three-TU PCH-off
checks pass. Runtime red/green receipts are under
`calendar/cash-thomas-simd-v1/`; the original scalar disassembly is under
`calendar/cash-pde-simd-inventory-v1/`, both beneath the r7 evidence root.

The next component gate now captures the unchanged reference solver directly.
An optional caller-owned observer sees read-only spans after each scalar Thomas
sweep. The test copies the first fresh seed and following Howard solve at expiry,
then the same two solves after the first interior jump when present. It retains
196 sweeps per grid across all 50 original cases, or 588 sweeps over three grids.
Each copied input is replayed through both research kernels; all original
`cp`/`dp`/solution bits match. Captures are in-process copies reproduced by the
archived executable and canonical input source, not a fitted runtime fixture.

All 150 complete observed/null solver outputs also agree in price, delta, gamma
and every existing work/solver field. The default exporter remains byte-identical
to the pre-observer replay, SHA256
`d4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc`.
Seven component tests pass after the observer's runtime red; the complete default
replay passes separately. Owning observer source, replay test and both existing
reference/export consumers pass PCH-off checks. This establishes the bounded
scalar-sweep replay gate.

The next private component executes complete PDE solves. It groups equal
canonical event-time bits, maturity bits, exercise style and space/time schedules
in original input order, then splits each calendar class into at most four lanes.
No strike, volatility, cash amount or coefficient is rounded or quantized. It
reuses the reference TU's original model, setup, terminal payoff and scalar jump
helpers. All working curves stay interleaved between jumps; each jump gathers a
lane into owned scratch and scatters it back. The original jump's allocations
remain. Howard's policies, convergence checks and typed refusals are independent
per lane. A completed lane swaps only its own values and retires from the current
step; a refused lane never rejoins. The immutable calendar remains valid when its
first lane refuses. There is no hidden scalar retry.

All 150 complete results match the original scalar solver in every binary64
field and every integer work/result field, through both scalar and AVX2 callbacks.
The optional per-lane census also matches every coefficient bit, Thomas sweep,
continuation-run count and length. Generic controls cover independent callback
refusal, invalid inputs, unsupported implicit rows at negative rates, anchor
refusal, mixed cash models, European exercise, canonical event coalescing and a
one-ulp event-time difference that must remain a separate group. Fourteen total
kernel/capture/driver tests pass. The initial driver stub fails its five controls;
the retained first red exposed a missing size guard in the test, corrected before
the subsequent safe red run. No production behavior was changed to make a test
pass.

The original 17 calendar classes require 21 actual four-lane bundles. Native work
agrees with the earlier input-only scheduling inventory:

| Grid | Vector sweeps | Useful lane-sweeps | Tail slots | Retired slots |
|---|---:|---:|---:|---:|
| 400 x 200 | 5,543 | 11,703 | 8,506 | 1,963 |
| 800 x 400 | 10,965 | 22,977 | 16,700 | 4,183 |
| 1600 x 800 | 21,953 | 45,688 | 33,290 | 8,834 |

Each row satisfies `4 * vector_sweeps = useful + tail + retired`. These are work
counts, not latency results. Evidence is retained in
`calendar/cash-pde-batch-driver-v1/`. The all-case accuracy frontier is unchanged:
43/44 spot-liquidator cases pass the finest-grid comparison and DSPYp remains
uncertain. The six other-model cases remain separate.

Owning reference and batch-test translation units pass PCH-off checks. The
default export remains byte-identical to the frozen pre-driver replay above.

Admission scans consumed values and result validation scans all scratch values;
these costs must remain in any inclusive kernel measurement until a separately
validated caller contract removes them. A cash PDE batch must also charge
setup, grouping, allocation, layout conversion, jumps, masked Howard iterations
and result publication. See the
[bounded design](../../docs/cash-pde-simd-design-20261024.md).
