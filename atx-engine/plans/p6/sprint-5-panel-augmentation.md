# Sprint 5 — Production Panel Augmentation

**Goal:** promote the test-only `augment_for_alpha101` logic to a production engine function
(`with_alpha101_fields`) and make `atx-impl panel` emit a fully-augmented panel, so all
~60+ factor-catalog entries that require `returns`/`cap`/`IndClass.*`/multi-adv become
reachable through the CLI.

**Owns (exclusive):**
- NEW `atx-engine/include/atx/engine/alpha/augment.hpp`
- `atx-engine/include/atx/engine/alpha/datafields.hpp`
- `atx-impl/src/stage_panel.cpp`
- `atx-impl/tests/alpha101_support.hpp`
- NEW `atx-engine/tests/alpha/augment_test.cpp` (or `atx-impl/tests/augment_test.cpp`)

**Must NOT touch:**
- `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp` — reserved for S7 (CLI flag wiring)
- `atx-impl/src/stage_discover.cpp`, `atx-impl/src/stage_run.cpp` — reserved for S7
- `atx-engine/tests/factory/oracle.hpp` — untouchable by all sprints
- `factory/`, `combine/`, `eval/` paths — outside S5 scope

**Determinism contract: (A) ADDITIVE / opt-in.**
The new production augment fn + the `stage_panel` augmentation step are both new behavior.
Existing `with_datafields` derivation stays byte-identical — `has()` guards make re-derivation
idempotent. The test-only `augment_for_alpha101` DELEGATES to the new fn and must produce
identical fields so every alpha101/capacity/metric-alignment/pseed test stays green.
Pinned goldens unchanged on the default path.

---

## The field-availability gap

### What the raw panel carries today (verified: `stage_panel.cpp:53–56`)

`build_history_panel` is called at line 53 and `write_panel` is called at line 56.
No augmentation step exists between them. The ORATS history panel surfaces 12 base fields:

| # | Field | Source |
|---|-------|--------|
| 1 | open | ORATS OHLCV |
| 2 | high | ORATS OHLCV |
| 3 | low | ORATS OHLCV |
| 4 | close | ORATS OHLCV |
| 5 | volume | ORATS OHLCV |
| 6 | market_cap | ORATS |
| 7 | sector | GICS code (f64 label) |
| 8–12 | (any additional ORATS fields) | loaded from segs |

### What `with_datafields` appends (verified: `datafields.hpp:153–232`)

`with_datafields` begins at line 153 and its final `Panel::create` call is at line 230–231.
It derives exactly three column families:

| Derived column | Derivation | Idempotent guard |
|---|---|---|
| `dollar_volume` | `close * volume` (universe-masked) | `detail::has_field(names, kDollarVolume)` at line 218 |
| `vwap` | `(high+low+close)/3` proxy, or kept if supplied | `detail::has_field(names, kVwap)` at line 184 |
| `adv{d}` (single window) | `ts_mean(dollar_volume, d)` for each requested window | `detail::has_field(names, name)` at line 204 |

### What the test-only `augment_for_alpha101` adds on top (verified: `alpha101_support.hpp:152–241`)

`augment_for_alpha101` signature is at line 155–156. Its body runs from line 157 to
line 241. It adds the following fields (in order) before delegating to `with_datafields`:

| Derived column | Derivation | Idempotent guard (line) |
|---|---|---|
| `returns` | `close[t]/close[t-1]-1`, causal, NaN on day 0/gaps | `detail::has(names,"returns")` at line 186 |
| `cap` | `market_cap` copy; fallback `close*1e8` | `detail::has(names,"cap")` at line 205 |
| `IndClass.sector` | widened f64 copy of `sector` | `detail::has(names,g)` at line 231 |
| `IndClass.industry` | same sector code (stand-in) | `detail::has(names,g)` at line 231 |
| `IndClass.subindustry` | same sector code (stand-in) | `detail::has(names,g)` at line 231 |

Then delegates at line 239–240 to `with_datafields(D, I, names, data, universe, adv_windows)`.

### Gap summary

| State | Fields available | Factor-catalog coverage |
|---|---|---|
| Raw `panel.bin` (today) | ~12 base OHLCV+meta | ~20 of 101 alphas evaluable |
| After `with_datafields` (single adv) | +3 (`dollar_volume`, `vwap`, `adv{d}`) | subset grows modestly |
| After `augment_for_alpha101` (test only) | +5 more (`returns`, `cap`, `IndClass.*`) + multi-adv | ~101 of 101 evaluable |
| After S5 (`with_alpha101_fields` production) | same as row above, in production | full catalog reachable via CLI |

---

## Tasks

### S5-0 — Production augment fn in NEW `augment.hpp` *(do first; unblocks S5-1 and S5-2)*

**Root cause:** `augment_for_alpha101` (`alpha101_support.hpp:155–241`) is `inline` in a test
header. Production code and `stage_panel` cannot call it. The same derivation logic must exist
in an engine-level header under `atx/engine/alpha/`.

**Fix:** Create `atx-engine/include/atx/engine/alpha/augment.hpp` with a single public function:

```cpp
[[nodiscard]] atx::core::Result<Panel>
with_alpha101_fields(const Panel& base,
                     std::span<const atx::u16> adv_windows);
```

Move the body verbatim from `alpha101_support.hpp:157–241`:
- `returns` derivation (lines 186–202), idempotent via `has()` guard.
- `cap` derivation (lines 205–220), idempotent via `has()` guard.
- `IndClass.sector/.industry/.subindustry` derivation (lines 222–236), idempotent via `has()` loop.
- Delegation to `with_datafields(...)` (lines 239–240).

The function takes a `Panel` by const-ref, reconstructs the mutable vector representation
internally (same pattern as `augment_for_alpha101`), and delegates to `with_datafields` for
the dollar_volume/vwap/adv family. Allocation is cold-path; no special strategies needed.
Header-only (`inline`); NaN-canonical; deterministic.

Document the three `IndClass.*` stand-ins explicitly in the header comment: `industry` and
`subindustry` alias the GICS sector code — not a claim of finer-grained GICS fidelity.
Leave a `// I5-HOOK:` comment marking the location Sprint 7 / a future sprint will replace
with true industry data.

**Determinism:** ADDITIVE. Byte-identical derivation lifted directly from the test helper;
existing `with_datafields` output untouched.

**Accept:** `augment.hpp` compiles standalone; `with_alpha101_fields` adds exactly
{`returns`, `cap`, `IndClass.sector`, `IndClass.industry`, `IndClass.subindustry`} + the
`with_datafields` set; re-calling on an already-augmented panel is idempotent (no duplicate
columns).

---

### S5-1 — Delegate test helper to production function *(no test regressions)*

**Root cause:** `augment_for_alpha101` (`alpha101_support.hpp:155–241`) duplicates logic that
will now live in `augment.hpp`. The two must stay identical in behavior or existing alpha101 /
capacity / metric-alignment / pseed-illiq tests will see field-value drift.

**Fix:** Rewrite `augment_for_alpha101` at `alpha101_support.hpp:155` to a thin wrapper:

```cpp
[[nodiscard]] inline atx::core::Result<Panel>
augment_for_alpha101(const Panel& base, std::span<const atx::u16> adv_windows) {
    return atx::engine::alpha::with_alpha101_fields(base, adv_windows);
}
```

Include `augment.hpp` at the top of `alpha101_support.hpp` (replacing the now-redundant
derivation block).

**Determinism:** The wrapper delegates identically; field set + values are byte-for-byte
equivalent to the pre-delegation path. All existing tests compile unchanged and stay green.

**Accept:** Run alpha101 / capacity / metric-alignment / pseed-illiq suites (or their
synthetic CI equivalents). Zero regressions. `augment_for_alpha101` output == `with_alpha101_fields`
output field-by-field on the synthetic panel.

---

### S5-2 — `stage_panel` emits an augmented panel + multi-adv engine half

**Root cause:** `stage_panel.cpp:53–56` — `build_history_panel` (line 53) is immediately
followed by `write_panel` (line 56). No augmentation step exists. `RunConfig` has no
`augment_panel` flag or `adv_windows` list — those are S7's to declare in `config.hpp`.

**Fix:** In `stage_panel.cpp`, between the `ATX_TRY(auto hp, ...)` call (line 53) and the
`ATX_TRY(auto wd, write_panel(...))` call (line 56), insert the augmentation block:

```cpp
// SPRINT7-WIRES: cfg.augment_panel and cfg.adv_windows are declared in
// config.hpp by Sprint 7. Until then, fall back so the file compiles today.
#if defined(ATX_PANEL_AUGMENT)   // Sprint 7 turns this on via its build profile
    const std::vector<atx::u16> adv_wins =
        cfg.adv_windows.empty()
            ? std::vector<atx::u16>{static_cast<atx::u16>(cfg.adv_window)}
            : cfg.adv_windows;
    ATX_TRY(hp.panel, atx::engine::alpha::with_alpha101_fields(
                           std::move(hp.panel), adv_wins));
#endif // SPRINT7-WIRES
```

The `#if defined(ATX_PANEL_AUGMENT)` guard means the file compiles today without any
`config.hpp` changes and produces byte-identical output on the default (no-flag) build.
Sprint 7 enables the guard as part of its non-default profile rather than a golden re-baseline.

Also add a `// SPRINT7-WIRES:` marker comment at the top of `run_panel` documenting the two
`RunConfig` fields S7 will declare (`cfg.augment_panel: bool`, `cfg.adv_windows: vector<u16>`)
so the S7 implementer knows exactly which two stubs to fill.

Include `atx/engine/alpha/augment.hpp` in `stage_panel.cpp`.

**Determinism:** The `#if` guard is off by default. Existing `panel.bin` bytes are unchanged
on any build that does not define `ATX_PANEL_AUGMENT`. The augmentation path is new behavior,
not a regression path.

**Accept:** `stage_panel` compiles without `ATX_PANEL_AUGMENT` and produces the same output
as today (existing golden/digest tests pass). When built with `-DATX_PANEL_AUGMENT` and a
synthetic panel, the output carries the full augmented field set.

---

### S5-3 — Panel provenance sidecar `.meta.txt`

**Root cause:** downstream stages (`discover`, `sweep`, `report`) have no way to know which
`UniverseConfig` parameters and adv-window list were baked into a given `panel.bin`. This
causes silent double-screening and makes it impossible to assert "this panel was augmented
with adv5/20/60." Touching the serializer binary format risks breaking readers of existing
panels.

**Fix:** In `stage_panel.cpp`, after `write_panel` succeeds (line 56), write a sidecar
`<panel_out>.meta.txt` (plain UTF-8, key=value lines). Example content:

```
atx_panel_meta_v1
built_utc=<ISO-8601 timestamp>
panel_bin=<cfg.panel_out>
universe_min_adv_usd=<cfg.min_adv_usd>
universe_top_n_by_adv=<cfg.top_n_by_adv>
universe_min_price=<cfg.min_price>
universe_require_sector=<cfg.require_sector>
adv_windows=<comma-separated list, or "none" if not augmented>
augmented=false       # becomes true when Sprint 7 enables ATX_PANEL_AUGMENT
engine_digest=<hp.digest hex>
dates=<panel.dates()>
instruments=<panel.instruments()>
fields=<panel.num_fields()>
```

The sidecar is written only when `!cfg.panel_out.empty()` (same guard as the main output).
No changes to `write_panel`, `serialize_panel`, or any binary format. The `StageResult.kvs`
map already surfaces most of these values; the sidecar makes them file-persistent.

**Determinism:** Sidecar does not affect `panel.bin` bytes. Existing consumers of `panel.bin`
are unaffected. Old panels remain readable; the sidecar is optional for readers.

**Accept:** `run_panel` writes `<panel_out>.meta.txt`; file parses as key=value; required
keys all present; no change to `panel.bin` digest.

---

## New test file: `augment_test.cpp`

Place under `atx-engine/tests/alpha/` or `atx-impl/tests/` (whichever directory is auto-globbed
by the build). Use the `ATS_TEST(...)` framework. Required test cases:

| Test | What it proves |
|---|---|
| `WithAlpha101Fields_AddsExpectedColumns` | synthetic panel → output has exactly {`returns`, `cap`, `IndClass.sector`, `IndClass.industry`, `IndClass.subindustry`, `dollar_volume`, `vwap`, `adv{d}`} for a single requested window; no duplicates |
| `WithAlpha101Fields_Idempotent` | calling `with_alpha101_fields` twice on the same panel produces identical output to calling once |
| `WithAlpha101Fields_ReturnsCorrectValues` | verify `returns[d,n] == close[d,n]/close[d-1,n]-1` on a hand-checked synthetic row; `returns[0,n]` is NaN |
| `WithAlpha101Fields_CapFallback` | panel without `market_cap` → `cap == close*1e8`; panel with `market_cap` → `cap == market_cap` |
| `WithAlpha101Fields_MultiAdv` | requesting `{5, 20, 60}` → panel carries `adv5`, `adv20`, `adv60`; each equals `ts_mean(dollar_volume, d)` on the synthetic fixture |
| `DelegationIdentity` | `augment_for_alpha101(base, windows)` output == `with_alpha101_fields(base, windows)` field-by-field (field names identical, all values bitwise equal) |

All tests use a deterministic synthetic panel (e.g. `make_synth_orats_panel` from
`alpha101_support.hpp`). No real-data dependency. No env-gating required for CI.

---

## Sequencing

1. **S5-0** (production `augment.hpp`) — write first; S5-1 and S5-2 depend on it.
2. **S5-1** (delegate test helper) — do immediately after S5-0; run existing tests before
   moving on to confirm zero regressions.
3. **S5-2** (stage_panel augment path) — after S5-0; depends on `augment.hpp` being available.
4. **S5-3** (sidecar) — independent of S5-0/S5-1; can be done in parallel with S5-2 or
   immediately after. Small; write in the same commit as S5-2 for cohesion.

S5-1 is the regression gate. Do not proceed to S5-2/S5-3 until the alpha101/capacity/pseed
suites are green on the delegating wrapper.

---

## Risks / guardrails

| Risk | Mitigation |
|---|---|
| `augment_for_alpha101` delegation produces different values | S5-1 `DelegationIdentity` test catches any bit-level divergence before commit |
| `stage_panel` guard leaks into existing CI builds | `#if defined(ATX_PANEL_AUGMENT)` is OFF by default; existing CI never defines it |
| `IndClass.*` stand-ins silently pass wrong factor values | Documented in `augment.hpp` header comment + `// I5-HOOK:` marker; no pretense of GICS-industry fidelity |
| Serializer format drift via sidecar | Sidecar is a separate file; `panel.bin` digest unchanged; no `serialize_panel.hpp` edits |
| `with_datafields` signature mismatch | `augment.hpp` delegates to the same overload at `datafields.hpp:153–156`; no signature changes to `datafields.hpp` |
| S7 RunConfig fields not yet declared | `#if defined(ATX_PANEL_AUGMENT)` guard + `// SPRINT7-WIRES:` comments let the file compile today with no stubs |

---

## Bench / acceptance

- **All pre-existing alpha101 / capacity / metric-alignment / pseed-illiq tests green** before
  and after S5-1 delegation rewrite.
- **Augment test suite (`augment_test.cpp`):** 6 test cases, 0 failures, 0 skips.
- **`stage_panel` default-build digest unchanged:** run `atx-impl panel` on a fixed input and
  confirm the `panel.bin` hash matches the pre-S5 baseline.
- **Multi-adv correctness:** `adv20` on the synthetic panel equals the hand-computed rolling
  mean of `dollar_volume` over a 20-day window (spot-check at least 3 cells in the test).
- **Sidecar presence:** `.meta.txt` exists alongside the output `panel.bin`; required keys parse.

No performance claims in S5 (augmentation is a cold path). Sprint 7 runs the real-panel
end-to-end bench once `ATX_PANEL_AUGMENT` is live.

## Out of scope (S7)

`--adv-windows` / `--augment-panel` CLI flags; `RunConfig.adv_windows` / `RunConfig.augment_panel`
field declarations; the discover-stage augment call; true GICS-industry/subindustry data ingestion
(deferred to a future I5 sprint).
