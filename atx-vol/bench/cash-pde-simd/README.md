# Private four-lane Thomas research kernel

This target is default-off and links neither kernel into the production library.
It has no PDE driver and no speed or cash-price qualification. The scalar control
and AVX2 kernel own no state: callers own one interleaved pack and provide a lane
mask. Invalid input lanes leave scratch unchanged; a nonfinite arithmetic result
refuses the affected lane and leaves partial scratch that must not be published.

The frozen `vol-rel` reference object's Thomas sweep uses fused negative
multiply-add for both forward numerators and the backsolve, with two separate
divisions. The research kernel preserves those operations explicitly. The scalar
control uses `std::fma` and separate divisions. This matches the observed release
arithmetic; it does not establish parity with every compiler profile. Real PDE
sweep capture is the next gate before driver integration.

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

Admission scans consumed values and result validation scans all scratch values;
these costs must remain in any inclusive kernel measurement until a separately
validated caller contract removes them. A cash PDE batch must also charge
setup, grouping, allocation, layout conversion, jumps, masked Howard iterations
and result publication. See the
[bounded design](../../docs/cash-pde-simd-design-20261024.md).
