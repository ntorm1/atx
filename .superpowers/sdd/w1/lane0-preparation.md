# W1 scaffold preparation (no W1 implementation started)

Freeze the W0 gate SHA before creating these stubs and dispatch/source-list edits.
The current source inventory has none of the new implementation files below.

| Owner | Lane 0 implementation stub |
|---|---|
| I1 | atx-impl/src/prereg.cpp |
| I1 | atx-impl/src/run_manifest.cpp |
| E1 | atx-engine/src/combine/signal_cube.cpp |
| L1 | atx-engine/src/learn/panel_dataset.cpp |
| B1 | atx-engine/src/cost/cost_surface.cpp |
| B1 | atx-engine/src/cost/spread_estimators.cpp |
| B1 | atx-engine/src/cost/borrow_tiers.cpp |

W0 trial_clusters is already header-only; the plan explicitly permits skipping its stub.
R1 owns existing headers/allocation source and requires no listed new implementation file.
D1/D5 preparation is running separately; migrations must be authored without applying them
to the other session's warehouse, and new modules must avoid tier1-parity ownership.

Contracts to settle in lane briefs before implementation:

- E1: distinguish signal quantization error from cached-stat precision. A float32 stat cache
  cannot generally reproduce arbitrary float64 downstream arithmetic bit for bit. State the
  reference representation and versioned compatibility path for the exact walk-forward claim;
  do not choose exactly representable fixtures merely to hide this distinction. Compare IC
  against independently computed full-precision values under the specified tolerance.
- E1/X1: an IC outcome belongs to its label-maturity/availability clock for causal decisions.
  Forward returns may change when future prices change; they must be inaccessible before
  maturity. Register this clock in the causality adapter rather than calling a future label a
  feature known on its signal date.
- L1: carry label-maturity metadata through all residualization and missing-indicator paths.
- B1: authoritative one-way cost units and observed-versus-modeled provenance; production
  fitness/optimizer/replay/capacity wiring remains with A4/R3/B2/B3 as the DAG specifies.
- I1: public combiner consumers must carry the actual return horizon; W0's new inference API
  intentionally cannot infer it from values. Keep raw/IID legacy reproduction explicit.

These are implementation constraints and preparation notes, not passed acceptance items.
