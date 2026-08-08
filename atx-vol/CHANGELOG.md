# atx-vol — changelog

Breaking behavioural changes are recorded here with their migration. Anything
that silently changes a NUMBER a caller already depends on belongs in this file.

## 1.1.0

Vol-derivatives production sprint, Phase 1 (correctness). Grows only with
changes that move a number a caller could already be marking with.

### Fixed — the eSSVI alternate driver's `validate_no_arb` is no longer a dead knob (FIT-C1/FIT-C5, Task C-8)

`essvi_calib_surface`[`_sequential`] unconditionally stamped `out_diag->
n_butterfly_viol = 0` and never ran a calendar check, regardless of
`CalibOpts::validate_no_arb` (default `true`) — the knob's documented
contract ("run the static-arb validators at the end and bail on a
violation", `calib.hpp`) was dead on this path. A genuinely calendar- or
butterfly-arbitrageable surface (e.g. from an un-projected HINGE_QUAD
wing-residual layer — the per-slice Roper projector stays out of port
scope, see the PORT NOTE on `fit_wing_residual`) was served silently.

`validate_no_arb` now runs a real post-fit audit (`arb_check_total_
surface_all` over the surface's assembled slices, `[-0.5, 0.5]` / 64-grid
— the same window the surface-recovery tests already independently
certify clean), populates `FitDiag` with the real counts, and returns
`Unavailable` on a nonzero count. This is INDEPENDENT of
`essvi_alt_driver_theta_project` (FT-C9a's opt-in theta-bump repair, still
default off, still the only thing that MOVES a fitted level) — the audit
runs after that repair, so enabling both knobs is repair-then-audit.

`FitDiag::n_calendar_viol` (new field — `FitDiag` carries no arity pin, so
this is a plain append) reports the real calendar-violation count;
`n_butterfly_viol` is no longer unconditionally zero on the eSSVI path.
Both read 0 when `validate_no_arb` is false (audit not run — not itself a
"verified clean" claim).

The `fit_slice_curve` (canonical PricerFitter) Essvi branch is UNCHANGED
on the default (`residual_disable == true`) path — bit-identical. When a
caller has opted into the HINGE_QUAD/C2Bspline wing residual, the served-
slice butterfly check now scans the full quoted range +/- 0.5 (mirroring
FT-C2's raw-SVI fix) instead of the fixed `[-0.6, 0.6]` band, catching wing
arb the un-projected residual can carry past it.

**Migration**: a caller of `essvi_calib_surface`/`_sequential` relying on
the default `validate_no_arb == true` that was previously served a
(silently) arbitrageable surface now gets `Err(Unavailable)` instead —
inspect `FitDiag::n_calendar_viol`/`n_butterfly_viol` for the real counts,
or set `validate_no_arb = false` to keep the historical permissive
behavior (e.g. a deliberately-biased fixture used to measure a KNOWN
disease, as `essvi_deam_test.cpp`'s raw-route tests now do explicitly).
The per-slice `essvi_fit_slice`/`fit_slice_curve` (PricerFitter) paths are
unaffected either way.

### Fixed — the wing clamp now trusts a Latency-mode surface's OWN certified band, not a wider one nobody certified (FIT-C7, Task C-6)

`DerivConfig::wing_clamp_k == 0` (the default) resolved unconditionally to
`strip::kCertifiedWingHalfBand` (0.5) — the band the fit pipeline's
independent risk validator certifies for a **Balanced**-quality surface
(`risk_validation_config`, `pricer_fitter.cpp`). A surface fit at
**Latency** quality is certified only to `±0.35`; a default-config quote
against it was reading `[0.35, 0.5]` as trusted when nothing in the fit
pipeline ever validated that range — precisely the uncertified extrapolation
the clamp exists to keep out.

`atx::vol::certified_wing_half_band(FitQualityMode)` (new,
`surface_policy.hpp`) is the canonical mode-keyed band (Latency 0.35,
Balanced 0.50 — unchanged, Accuracy 0.60). The `PricedSurface`/`SurfaceRef`-
native entry points (`var_swap_fair_strike`, `vol_swap_fair_strike`,
`deriv_price`, `deriv_greeks`, `deriv_price_on_ref`, `deriv_greeks_on_ref`)
each take a new trailing `surface_certified_wing_band` parameter (default
`std::nullopt`): a caller that knows the surface's own build quality mode
resolves it through `certified_wing_half_band` and passes it in, and
`wing_clamp_k == 0` now resolves to THAT band instead of the mode-blind
default. The templated legacy-surface entry points (`VolSurface`/
`EssviSurface`/`SviSurface`) carry no such provenance and are unaffected —
they keep reading `strip::kCertifiedWingHalfBand` exactly as before.
`DerivQuote::resolved_wing_clamp` (new field, arity pin 15 -> 16) records
the band a quote actually resolved, so a caller can inspect it directly
instead of inferring it from `DerivFlags::WingClamped` alone. A bumped
`deriv_greeks` evaluation prices through an adapter with no provenance of
its own, so `pin_center_scheme` now also pins the CENTER's resolved band
into every bump's config — without this, a Latency-certified center's
vega/gamma/etc. would difference against bumps silently read at the wider
mode-blind band.

**Migration**: every existing call site is unaffected — `surface_certified_
wing_band` defaults to `std::nullopt`, which resolves to the SAME `0.5` this
code always used, bit for bit (also regression-tested). A caller pricing a
`PricedSurface`/`SurfaceRef` it knows was fit at Latency or Accuracy quality
should start passing `certified_wing_half_band(quality_mode)` explicitly;
doing so for a Latency-mode surface **moves the mark** — a steepening wing
between `±0.35` and `±0.5` was previously read as trusted smile and now
reads flat at the `±0.35` band edge instead, which can only lower a
variance-swap fair strike (never raise one, by the same monotonic-tightening
identity an explicit `wing_clamp_k` override already has). Accuracy-mode
surfaces widen from 0.5 to 0.6 and move the other direction. Balanced-mode
surfaces are bit-identical either way (0.5 either way).

**Wired, not just available** (review fix round 1): the paragraph above
originally undersold this as an opt-in a caller had to reach for. It is now
also wired into every live production path that prices or marks a swap lot
against a stamped-provenance surface, so a Latency/Accuracy-fit surface's
own band is read automatically, with no caller-side opt-in required:

- `MarketSnapshot::provenance(uid)` (already plumbed end-to-end from
  `FittedSurface::quality_mode()` through `SurfaceArchive`) now feeds a new
  `certified_wing_band_for(snapshot, uid)` helper (`backtest.hpp`), used by
  `step_swap_lots` (`backtest.cpp`) to mark every open swap lot's PnL against
  its OWN surface's certified band, and by `DeclarativeStrategy` (`strategy.cpp`)
  to size and fair-strike every new swap leg the same way.
- `solve_cycle_swap` (`swap_leg.hpp`/`.cpp`) takes a new trailing
  `surface_certified_wing_band` parameter (default `std::nullopt`, so any
  caller not yet threading provenance is unaffected bit for bit) and forwards
  it to both the fair-strike solve and the entry-vega greeks; `SwapSignalProbe`
  passes it through as well.
- `price_deriv_book` (`deriv_book.hpp`/`.cpp`) takes a new trailing
  `WingBandResolver` callback (default empty, same no-op guarantee) so a
  caller holding per-uid provenance from any source can apply it per row.

A legacy archive with no independently-admitted `SurfaceProvenance` resolves
`FitQualityMode::Balanced` (`legacy_surface_provenance()`), the mode-blind
default band, so none of the above changes a mark for archives that predate
provenance stamping. It changes marks only for Latency/Accuracy-stamped
surfaces flowing through these paths, per the monotonic-tightening/-widening
direction described above.

### Fixed — two silent kind x engine mismatches now fail loud (PV-5)

`deriv_price` already rejected an engine that names no pricing formula for a
capped kind (`StripLogContract`/`VolCarrLee` on `CappedVarSwap`/
`CappedVolSwap`), but missed the same mismatch on the two uncapped kinds:

- `DerivKind::VarSwap` + `DerivConfig::engine == VolCarrLee` silently ran the
  variance strip anyway — `price_var_swap` never read `cfg.engine` at all, so
  every engine choice on a `VarSwap` behaved exactly like `Auto`.
- `DerivKind::VolSwap` + `DerivConfig::engine == StripLogContract` silently
  fell into the same unaged Carr-Lee branch `Auto`/`VolCarrLee` take — that
  branch only tested `cfg.engine != RvDistributionProxy`, not which engine it
  actually was.

Both combinations now return `InvalidArgument` ("engine cannot price a var
swap" / "engine cannot price a vol swap") at dispatch, before any pricing
runs. The full matrix `deriv_price` enforces: `VarSwap` -> {`Auto`,
`StripLogContract`}; `VolSwap` -> {`Auto`, `VolCarrLee` (unaged only, as
before), `RvDistributionProxy`}; `CappedVarSwap`/`CappedVolSwap` -> {`Auto`,
`RvDistributionProxy`} (unchanged).

**Migration**: `cfg.engine` defaults to `Auto`, so no default-config caller is
affected — this changes zero prices. A caller who explicitly pinned
`VolCarrLee` on a `VarSwap` contract, or `StripLogContract` on a `VolSwap`
contract, was silently getting the SAME number `Auto` already returns for
those inputs (a strip quote / an unaged Carr-Lee quote respectively) under an
engine name that did not describe what ran; it should switch to `Auto` (or
the matching valid engine) — no mark changes for anyone reading the number
`Auto` already gave them.

### Fixed — an interior bad node silently contributed 0 with no trace (PV-4)

The variance strip's OTM integrand treats a non-finite or non-positive
surface IV as 0 at that node. Only the two ENDPOINT nodes of the whole grid
were ever checked (`bad_first`/`bad_last`, driving `StripTruncatedLeft`/
`Right`) — a bad node strictly inside the grid, including the `k = 0`
put-call-parity kink the C-3 panel split reads as its own node, contributed 0
to the integral with no trace anywhere in the returned quote.

`var_swap_fair_strike` now counts interior bad nodes. One or more sets the
new `DerivFlags::InteriorBadNodes = 1u << 13` (still priced — a handful of
gap quotes is business as usual on a real fitted surface). More than
`max(2, n_nodes/100)` returns `Internal` instead of a quote: a surface with
that many holes across its middle is broken, not sparse, and a number built
mostly from zero-substitutions is worse than refusing to answer. Exempted
when the strip is wholly unusable — BOTH true grid endpoints (`bad_first`
and `bad_last`) read non-finite, e.g. a query T under the legacy short-T
extrapolation guard — that is the pre-existing, deliberately tolerated
"surface has nothing to say at this T" corner (`deriv_greeks` relies on it
to roll a theta/charm bump past expiry and get a NaN greek back, not a
failed call), a different failure from PV-4's target of an otherwise-usable
surface with a hole in it. (Review fix round 1: the exemption is decided on
the strip's own two endpoints, not on a fresh ATM read — an ATM-coupled
exemption would silently re-open PV-4's own named case, a single bad node
that happens to sit at the k = 0 kink.)

**Migration**: nothing changes for a clean surface — a well-formed fitted
surface was never producing interior bad nodes, so this is new accounting,
not a new failure mode. A caller whose surface adapter has genuine mid-grid
gaps will now see `InteriorBadNodes` (informational, same price as before) or,
past the threshold, an `Internal` error where it previously got a quote
computed mostly from zeros with no signal that anything was wrong.

### Fixed — `DerivDiscreteCorrection::Diffusion1OverN` was wrong (PV-1, PV-8)

The discrete-monitoring correction for the future implied-variance leg
multiplied `K_var_future` by `(1 + 1/n_obs_total)`: the wrong functional form
(the Broadie-Jain (2008) leading-order diffusion-drift term is ADDITIVE, not
multiplicative) applied with the wrong divisor (the contract's total
observation count, not the future leg's own remaining fixings).

Corrected formula, applied at all four call sites (`price_var_swap`,
`price_vol_swap_distribution`, `price_capped_var_swap`,
`price_capped_vol_swap`):

```
K_var_future += (T_resid / n_remaining) * (r_bar - q_bar - K_var_future/2)^2
n_remaining = n_obs_total - n_obs_done
r_bar - q_bar = ln(F/S) / T_resid   (read from the same CurveSet the strip
                                     already resolves F from)
```

**Magnitude**: for a daily-monitored (n=252) index contract at sigma=20%,
r-q=5%, T=1Y, the old code added ~1.6 variance points (`K_var/n`); the
corrected addend is ~0.036 variance points — smaller by about two orders of
magnitude. `DerivFlags::DiscreteCorrApplied` still marks whenever the
correction ran.

**Migration**: `discrete_correction_mode` defaults to `None`, so this changes
nothing for a caller who left it there. Anyone who had already opted into
`Diffusion1OverN` was marking discrete-monitored variance/vol swaps
materially rich (by roughly the magnitude above) and should re-mark against
the corrected engine.

Also fixed (PV-8): xi (vol-of-vol) auto-calibration was resolving against the
ALREADY-corrected strip mean whenever this mode was on, so
`resolve_vol_of_vol`'s "reproduces Carr-Lee exactly" guarantee silently broke
under the mode. xi is now always resolved against the UNCORRECTED strip mean;
the correction applies only to the mean actually fed to the distribution
model afterward.

Not covered: the residual O(1/n) jump term (Broadie-Jain sec 4) — jump-
diffusion discrete-monitoring bias needs the `FullMc` engine (reserved,
LIT-3).

### Fixed — short-tenor variance strip was under-resolved; `DerivFlags::LowT` now fires (PV-2, PV-3)

The E2 adaptive-wing logic only WIDENED the strip's span for a high-vol/
long-dated tenor (`kh = max(tier_span, width_sigmas*sigma_atm*sqrt(T))`) and
rescaled the node count to match — it never checked the OPPOSITE direction.
The tier grids are sized for a roughly-1Y reference vol scale, so a short-
tenor quote (e.g. T = 1 trading day) sits comfortably inside the tier's span
floor but resolves it far more coarsely than its own `sigma_atm*sqrt(T)`
calls for. The quadrature error this starves is dominated by the near-ATM
curvature the strip integrates through (the `price/(df*K)` integrand's kink
at `k = 0`), not by truncated wings.

`var_swap_fair_strike` now enforces the mirror rule after span resolution:
`dk <= sigma_atm*sqrt(T) / 4`, raising the node count when it does not
(rounded up to the next `4m+1` so the Richardson half-grid estimate stays
populated). `sigma_atm` is the same ATM-vol read the span logic already
resolves — no second surface read. A caller-pinned `strip_nodes` is never
overridden (pin semantics are load-bearing for `deriv_greeks`' grid pinning);
a pinned grid that violates the floor raises the now-live `DerivFlags::LowT`
instead of being silently corrected. `LowT` was declared in 1.0.0 with no
writer anywhere in the engine (PV-3); it now fires whenever the floor
engaged, or a pinned grid could not be corrected to satisfy it.

**Magnitude** (flat sigma=20%, T=1/252, truth K_var=0.04 exactly):

| Tier     | Nodes (pre→post) | K_var (pre→post)         | Move                    |
|----------|-------------------|----------------------------|-------------------------|
| Fast     | 97 → 637          | 0.042423 → 0.040000        | -6.06% (~-24.2 var pts) |
| Standard | 257 → 957         | 0.0400156 → 0.0400000      | -3.91bp (~-0.16 var pt) |
| High     | 769 → 1273        | ~0.04000000 (both)         | ~0 (already accurate)   |
| Audit    | 2049 → 2049       | 0.0400000002 (unchanged)   | none (floor unneeded)   |

The node counts in that column are this fix's own arithmetic. The kink-split
entry below provisions 16 further intervals of apportionment headroom, so the
counts 1.1.0 actually ships at this tenor are **653 / 973 / 1289 / 2049**
(pinned in `StripResolution.PanelSpacingRespectsCeilingAtTheFloorBoundary`).
The K_var column is unaffected — both grids are far past convergence here.

**Migration**: this moves the DEFAULT Fast-tier mark at short tenors — the
old default was a verified quadrature bug (PV-2), not an intentional choice,
so per the sprint's correctness-first rule it is corrected rather than kept
for compatibility. A caller marking short-dated (sub-week) variance/vol
swaps at `DerivQuality::Fast` was pricing them materially rich/cheap by
roughly the magnitude above and should re-mark against the corrected engine.
Standard/High/Audit move by ≤4bp at the same tenor and are unaffected at
ordinary (multi-week+) tenors, where the floor was already satisfied. A
caller that pins `strip_nodes` explicitly is unaffected numerically (the pin
still holds exactly) but may now see `DerivFlags::LowT` where it previously
never could.

### Fixed — the variance strip's Simpson panels now straddle no C1 kink (LIT-10)

The strip's OTM integrand `OTM(K)/(df*K)` is only PIECEWISE smooth. It kinks
in C1 at `k = 0` — put-call parity makes the two branches agree in value at
`K = F` but their `K`-derivatives differ by the discount factor, a slope jump
of exactly 1, some 25x the integrand's own ATM value at a 3M 20-vol — and at
`±wing_clamp_k` whenever the clamp binds, where `d(iv)/dk` drops to zero.

Composite Simpson is O(h^4) on a smooth panel but only O(h^2) on one that
STRADDLES such a kink, and the Richardson `|I_h - I_2h|/15` estimate assumes
the h^4 law. Before this change `k = 0` landed on a panel boundary only
because every DEFAULT grid happens to be symmetric with `4m+1` nodes — an
accident of the defaults, never asserted, and broken by any caller-pinned
asymmetric span. The clamp edges sat mid-panel even on the defaults.

`var_swap_fair_strike` now splits the composite integration at every interior
kink (`detail/strip_grid.hpp`'s `plan_strip_split`), apportioning the resolved
node budget across the sub-intervals in proportion to length so the kinks are
panel boundaries BY CONSTRUCTION on any grid. The total span and total node
count are unchanged, so `strip_k_lo_used`/`strip_k_hi_used`/`strip_nodes_used`
keep their meaning and `deriv_greeks`' grid pinning replays a quote exactly.
The estimate is populated whenever every panel is `4m+1`, which every default
budget is; a starved caller-pinned budget gives up the clamp edges first, the
estimate next, and the `k = 0` alignment last.

**C-2's resolution floor is enforced per panel, not per nominal `dk`.** The
floor (`dk <= sigma_atm*sqrt(T)/4`) was sized against the spacing of one
uniform lattice. The split retires that lattice — integer apportionment cannot
divide a span evenly, so a panel's own spacing runs above the nominal
`span/(n-1)` (1.6% at Standard's 256 intervals). Left alone that would have
made the floor's guarantee approximate: a tenor whose nominal `dk` sat just
under the ceiling cleared the check while a panel breached it. `dk_floor_nodes`
now provisions the excess analytically — the apportionment bound is
`dk_i < span/(intervals - 4*n_panels)`, so requiring
`intervals >= span/dk_max + 4*n_panels` makes `dk_i < dk_max` hold for **every**
panel, exactly. `DerivFlags::LowT` is likewise decided on the widest panel
rather than the nominal `dk`, which is what makes it honest for a
caller-pinned count (never overridden, so flagging is all it can do).

Cost: at most 16 intervals (`kMaxStripPanels == 4`). At ordinary tenors it is
inert — every tier still resolves exactly its default budget (97/257/769/2049),
so **no default mark moves by even one ulp** on this account. Where the floor
already engaged, node counts rise by 16 plus `4m+1` rounding (at `T = 1/252`,
`sigma = 20%`: Fast 637→653, Standard 957→973, High 1273→1289, Audit unchanged),
which only makes those quotes more accurate.

**Magnitude — default grids move by at most 2.8e-7 relative**, and toward
truth (3M skew fixture, `make_skew_surface(0.20, -0.40, 0.35)`; flat fixture
is truth `= 0.04` exactly):

| Tier     | flat K_var move | skew K_var move | Accuracy         |
|----------|-----------------|-----------------|------------------|
| Fast     | 8.5e-16 rel     | 3.1e-16 rel     | unchanged grid   |
| Standard | 2.4e-9 rel      | 2.8e-7 rel      | 8.5x more exact  |
| High     | 1.1e-15 rel     | 0 (exact)       | unchanged grid   |
| Audit    | 2.5e-12 rel     | 4.2e-9 rel      | ~unchanged       |

Fast and High do not move at all: their proportional apportionment happens to
reproduce the un-split uniform spacing exactly (96 intervals over four equal
panels; 768 over 1.5/0.5/0.5/1.5).

**Asymmetric-pin values are corrected, not merely nudged.** A pinned
`k_min_log = -0.714, k_max_log = 0.686, strip_nodes = 101` on the flat
sigma = 20%, T = 3M fixture returned `0.0402613` against a truth of `0.04` —
off by 2.6e-4 (6.5e-3 relative), which is exactly the `J*h^2/6*(2/T)` straddle
term. It now returns `0.0400000082`, matching the symmetric reference to
1.4e-13. Across a 16-step sweep sliding a 257-node grid through one panel, the
worst case improves from 1.8e-4 to 1.4e-9.

**The error estimate is now an estimate.** Measured `integration_error_est`
over the true error on the 3M skew fixture: the default Standard grid went
0.689 → 1.000; a symmetric 101-node pin (whose `k = 0` is a full-grid boundary
but an ODD HALF-grid index, so the /15 difference was measuring the half
grid's own straddle) went 574 → 1.158; an asymmetric 101-node pin went
0.133 → 1.161.

**Migration**: nothing to do for a caller on default grids — the moves above
are far below any mark's resolution. A caller that pins an ASYMMETRIC span was
being served an O(h^2) quote and a meaningless error estimate; it should
re-mark. `integration_error_est` was the one number that could previously be
wrong by four orders of magnitude while looking plausible, and any consumer
gating on it will now see a much smaller (and truthful) value.

### Added — `DerivConfig::carr_lee_form`: opt-in Remark 6.4/6.5 convexity refinement (LIT-4, Task C-5)

`vol_swap_fair_strike`'s naive Carr-Lee formula (`K_vol ~= sqrt(2 pi / T) *
C_ATMF(T) / (F * df)`) reads only the ATMF point of the smile — it is Carr &
Lee's own Prop. 6.1 bound (a) / Remark 6.3, the approximation their rrvd.pdf
explicitly declines to endorse (Remark 6.5). Under equity skew it is biased
LOW relative to the true fair vol-swap value (the paper's own Heston BCC
example, Sec. 6.5, cites >40 vol bp at 6M for ρ ≈ −0.7). Because
`resolve_vol_of_vol`'s xi auto-calibration inverts against this same naive
number, the bias was propagating into every distribution-model consumer
(capped var/vol swaps, mid-life vol swaps) too.

New `enum class CarrLeeForm { Naive = 0, Refined = 1 }` and
`DerivConfig::carr_lee_form` (appended field, arity pin raised 12 → 13)
select between the naive formula (default) and the paper's Remark 6.4/6.5
refinement evaluated against the variance strip's own `K_var`:

```
K_vol_refined = K_vol_naive *
    (1 + T*(K_var - K_vol_naive^2) / (8 + 2*T*K_vol_naive^2))
```

**This is NOT the T-dropped paraphrase a from-summary read of Remark 6.4
suggests.** The paper states `VOL0 ≈ IV0*(1 + (VAR0²−IV0²)/(8+2·IV0²))` for
UN-annualized total-horizon quantities (IV0, VAR0 scale like σ√T); restating
it directly against this codebase's ANNUALIZED `K_vol`/`K_var` without
re-inserting `T` silently assumes `T == 1` always. The formula above is the
annualization-consistent substitution (`IV0 = K_vol_naive·√T`,
`VAR0² = K_var·T`, divided back through by `√T`); it collapses to the
T-dropped paraphrase exactly at `T == 1` and to `K_vol_naive` exactly
whenever `K_var == K_vol_naive²` (no convexity to recover). See
`task-C-5-report.md` for the full re-derivation.

**Wiring**: `resolve_vol_of_vol`'s three callers (mid-life vol swap, capped
var swap, capped vol swap) already have the strip's `K_var` in hand, so
`Refined` is free there. The standalone `vol_swap_fair_strike` entry does
not run a strip under `Naive` — under `Refined` it now pays for one
`var_swap_fair_strike` evaluation (propagating that call's own error
contract) and, since a strip now genuinely ran, populates
`uncapped_var_dec`/`integration_error_est`/the strip-grid fields instead of
leaving them at "no strip ran" (0.0 / NaN).

**Direction, verified against `resolve_vol_of_vol`'s own closed form (not
assumed from the task brief's paraphrase, which has this backwards): larger
K_vol_target (Refined, sitting closer to `sqrt(K_var)`) needs LESS inferred
lognormal dispersion to explain a SMALLER Jensen gap, so `vol_of_vol_used`
and every cap option value it drives move DOWN under Refined, not up** — the
naive formula's own approximation shortfall was being misattributed by the
auto-calibrator as real vol-of-vol, inflating `xi` (and cap prices) above
what the surface's actual convexity supports; `Refined` corrects part of
that inflation back down, same direction as the `K_vol` fix itself.

**Magnitude** (skewed fixture, `atm_vol=0.20`, ATM skew slope −0.40 bp/Δk at
the 3M pillar, ρ = −0.7, convexity 0.35, T = 0.5 = 6M, r=2%, q=1%):

| Quantity                         | Naive         | Refined       | Move             |
|-----------------------------------|---------------|---------------|------------------|
| `K_vol` (vol-swap fair strike)    | 0.199833458   | 0.199924092   | +0.906 vol bp    |
| `sqrt(K_var)` (Jensen bound)       | 0.217316377   | (unchanged)   | —                |
| xi (`vol_of_vol_used`, CappedVarSwap, T=6M, cap=0.05) | 1.158412278 | 1.155276537 | −0.271% rel |
| `cap_option_value_dec`            | 0.0141013933  | 0.0140619539  | −0.280% rel      |

The refinement recovers ≈0.9 vol bp of the naive-vs-Jensen-bound gap here
(≈174.8 vol bp total on this fixture) — a small, leading-order correction by
design (the paper does not endorse either approximation as globally
accurate; Remark 6.5), not a full close of LIT-4's cited >40bp bias. A
`Refined` caller should expect single-digit-vol-bp-scale `K_vol` moves and
sub-percent `vol_of_vol_used`/cap-value moves at ordinary skew, growing with
`T` and with the size of the naive-vs-`sqrt(K_var)` gap.

**Known residual, undisturbed by this change (review finding I-1):** Remark
6.5's `IV0` slot is the paper's ATM IMPLIED VOL (`sigma_atmf`) itself; this
implementation feeds it `K_vol_naive` (`carr_lee_k_vol`'s own ATMF-straddle
output), a SEPARATE, already-approximate stand-in for `sigma_atmf` biased low
by `sigma_atmf^3*T/24` (annualized) — on the fixture above, ~1.667 vol bp,
larger than the +0.906 vol bp this refinement adds. A `Refined` strike
therefore still lands below `sigma_atmf`, not just below the paper's true
`VOL0`. This is forced, not an oversight: `CarrLee.RefinementVanishesOnFlat`
pins refined == naive bit-exact whenever `K_var == K_vol_naive^2`, which only
holds with `K_vol_naive` (not `sigma_atmf`) as the refinement's base point —
substituting `sigma_atmf` would move `Refined` even on a flat surface and
break that pin. Fixing the underlying straddle-proxy bias is a separate,
un-scoped change (it would move the `Naive` default too).

**Migration**: `carr_lee_form` defaults to `Naive`, so this changes zero
prices for every existing caller — the v1.1 default is byte-compatible with
every pre-C-5 quote. **Planned 2.0 default: `Refined`.** A caller who wants
the closer (still not exact) approximation today can opt in explicitly; a
2.0 upgrade will move every unaged-vol-swap and distribution-model mark by
the small magnitude above, growing with skew and tenor, and should be
re-marked against the new default at that time.

## 1.0.0

The first release with a stability promise. Everything below happened during the
production-v1 release sprint, on the way from an internal library to one that
installs into a prefix and can be depended on.

**What 1.0.0 actually promises** is a *tier*, not the tree: the 57 headers
`atx/vol/vol.hpp` includes are frozen for 1.x, and the manifest that says which
those are is machine-checked (`kTierA` in `atx-vol/tests/vol_umbrella_test.cpp`).
(The set said 56 until the release audit re-derived it. Since the release gate's
pre-flight, the *count* is machine-checked too —
`VolUmbrella.TierCountsMatchTheReadmeTable` asserts 57 against the live manifest,
alongside Tier-B 31 and `detail/` 28 — so this digit can no longer rot silently
the way it did.)
Everything else — Tier-B, `detail/`, `tools/`, `research/` — is public-but-
unfrozen or internal. The full policy, with the counts and the tests that
enforce it, is the *API stability policy* section of `README.md`. Read it before
depending on a header: this release moved a lot of them, deliberately, precisely
so the frozen set could be small and honest.

Because that reshaping is the release, **this section is mostly breaking
changes**. They are grouped by what a caller has to do about them.

### BREAKING — the public surface was tiered, and headers moved

Nothing was deleted in the tiering itself; every relocation is a `git mv`.

* **12 headers demoted to `detail/`** (`#include "atx/vol/X.hpp"` →
  `"atx/vol/detail/X.hpp"`): `parallel_for`, `pricing_executor`, `counters`,
  `phase_profile`, `prepared_fitting`, `prepared_policy`, `prepared_portfolio`,
  `strip_grid`, `run_archive_schema`, `backtest_series_columns`,
  `risk_surface_validation`. `listed_quote_key` was demoted and then **returned
  to Tier-B**, because `listed_opra.hpp` names `ListedQuoteKey` in a public
  signature — a caller could not use that parameter without naming a type the
  tier says carries no promise.
* **6 headers → `atx-vol-tools`** (`"atx/vol/X.hpp"` → `"atx/vol/tools/X.hpp"`):
  `run_report`, `surface_db_admin`, `surface_db_build`, `surface_db_exit_codes`,
  `surface_db_populate`, `tearsheet`.
* **9 headers → `atx-vol-research`** (`"atx/vol/X.hpp"` →
  `"atx/vol/research/X.hpp"`): `backtest_driver`, `dispersion_backtest`,
  `dispersion_run`, `dispersion_workflow`, `listed_definitions_cache`,
  `listed_dispersion_pipeline`, `listed_dispersion_reconciliation`,
  `run_archive`, `run_diagnostics`. The split line is **driver vs vocabulary**:
  headers that *compose* a research run moved; the dispersion domain vocabulary
  they are written in stayed public.
* **`atx/vol/curve.hpp` → `atx/vol/rates_curve.hpp`** (Tier-A). No symbol
  renamed; the rates vocabulary just collided visually with the vol-smile family
  (`vol_curve.hpp` / `spline_curve.hpp`).
* **`spy_fixture.hpp`** moved the other way — out of `tests/support/` and onto
  the public surface as Tier-B `atx/vol/spy_fixture.hpp` — because the shipped
  Python module and a bench reached into `tests/` for it.
* **The umbrella is now exactly Tier-A.** 7 headers joined it (`adjusted_greeks`,
  `corpus`, `priced_surface_view`, `query_pricing`, `spline_curve`, `surface_db`,
  `surface_policy`) and 14 left it for Tier-B (`batch`, `c8_calib`, `cstar`,
  `cstar_calib`, `essvi_calib`, `svi_calib`, `historical_projection`,
  `listed_dispersion`, `listed_dispersion_schedule`,
  `listed_dispersion_strategy`, `listed_opra`, `occ_ess`, `panel`, `s3`).
  Reaching those 14 now needs a direct include. `curve_selector` and
  `dense_slice` are deliberately NOT in the joined set — both were in the
  umbrella throughout 0.1.0 and describe no change for an upgrading caller — and
  `portfolio` / `portfolio_risk` are not in the Tier-B set, because they were
  **removed outright rather than demoted** (see REMOVED below).
* **`Surface<Slice>`, `SviSurface`, `EssviSurface`, `C8Surface`, `CStarSurface`**
  moved to `detail/legacy_surface.hpp`, `detail/legacy_c8_surface.hpp`,
  `detail/legacy_cstar_surface.hpp`. The **namespace did not change** — they are
  still `atx::vol::` — so the migration is one added include. The canonical
  pipeline is `CurveSurface` (fit) → `PricedSurface`/`PricedSurfaceView` (serve)
  → `SurfaceSet` (portfolio), and public headers may no longer name the demoted
  containers even in prose.
* **The templated `derivatives.hpp` entries are now instantiated for
  `VolSurface`.** `var_swap_fair_strike`, `vol_swap_fair_strike`, `deriv_price`
  and `deriv_greeks` are templates on the surface type whose bodies live in
  `derivatives.cpp`; every instantiation used to be on a demoted container, so
  these Tier-A declarations could only be linked against by including a
  `detail/` header. `VolSurface` — which answers `iv(k_log, T)`, the template's
  whole requirement — joins the instantiation set, and the demoted pair stays
  for source compatibility. Purely additive: no existing call changes.

### BREAKING — error model: batch entries report how many lanes they wrote

Ten `Status`-returning batch entries now return `Result<std::size_t>`, carrying
the number of lanes written and defined only on success. `Result<T>` itself is
unchanged (`tl::expected<T, atx::core::Error>`); error codes and messages are
byte-identical.

`black76_price_batch`, `black76_price_from_lnfk_batch`,
`black76_value_and_vega_batch`, `implied_vol_batch`, `black76_greeks_batch`,
`essvi_w_batch` (`batch.hpp`); `american_price_batch`,
`american_price_batch_resolved`, `american_greeks_batch`
(`american_batch.hpp`); `american_implied_vol_batch` (`american_iv.hpp`,
Tier-A).

*Migration.* Inline uses (`if (f(...))`, `ASSERT_TRUE(f(...))`) bind unchanged —
both types are `expected`. Only declaration-form sites move:
`const Status st = f(...)` → `const Result<std::size_t> st = f(...)`, and
forwarding sites change `return st;` → `return Err(st.error());`. Output spans
and the per-lane `std::span<Status>` channel are untouched.

Two smaller shape changes: `configure_pricing_executor` returns
`[[nodiscard]] Status` instead of a discardable `bool`, and
`ticker_seed_profile` returns `std::optional<ProfileKind>` instead of taking an
out-param (`if (ticker_seed_profile(t, kind))` → `if (const auto k =
ticker_seed_profile(t); k.has_value())`).

### BREAKING — positional aggregate initialisation is no longer supported

`AlOpts`, `RunConfig`, `SessionInputs` and `SurfaceParityReport` were reordered
and pinned with a field-count `static_assert`. **Use designated initializers.**
`AlOpts{3, 3, 1, 1.0e-1}` is now wrong — `n_quad_price` moved from last to
third. Each of the four headers states that this is the last layout change
allowed; post-1.0 knobs append at the end with no positional promise. Python is
unaffected: keyword names, arity and signature are unchanged.

`RunConfig`'s pin moved **15 → 16** later in the same sprint when `cancel` was
added (see *Embedding* below). It was INSERTED beside `step_observer`, its
semantic group — not appended — which is precisely the freedom the new convention
buys and the old one forbade. Named initialisation is unaffected by construction;
a positional one would have rebound, which is why none is allowed to exist.

It then moved **16 → 17** when the release branch merged `main` (2026-08-02),
which brought the backtest-replay work's `RunConfig::prefetch_depth`
(`std::size_t`, default `2`). This one is **appended at the end**, the form the
convention prescribes for a new knob. Two notes a caller may care about:

* **It changes no output at any value.** `prefetch_depth` is purely an I/O
  schedule — how many future snapshots may be in flight — never which bytes are
  deserialised nor the order the economics consume them.
  `Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead` pins that
  bit-identity, and the SPY-dispersion NAV determinism legs reproduce their
  anchors bit-exactly across the merge that introduced it.
* **The default is `2`, not the historical single-step `1`.** It arrived from
  `main` at `1` and was moved to `2` in this release (v1 closeout sprint Task
  4.8, plan item 6.7) on a paired measurement: one binary with the depth
  alternated inside a single session, 12 interleaved rounds on the 135-session
  SPY-dispersion replay, medians and win-counts only — `1 → 2` **+15.2 % (11/12
  rounds)**, then `1 → 4` +19.8 % (10/12) and `1 → 8` +19.6 % (10/12), while `2 → 4` (+1.9 %,
  7/12) and `4 → 8` (+1.6 %, 7/12) are washes. The curve is a step, not a ramp:
  overlapping the first load is the whole win, so `2` is the cheapest default
  that takes it. **A run that wants the old shape sets `1` explicitly and gets
  it bit-for-bit** — by the note above, no value of this field moves a number.
  The cost of the new default is one extra in-flight snapshot and a private
  cache of `4` slots instead of `3`.
  `0` is still normalised to `1` — "no look-ahead" is expressed by
  `prefetch_snapshots = false`, not by a zero depth. A caller-supplied
  `snapshot_cache` must retain at least `depth + 2` entries or the LRU drops a
  completed prefetch before its step reaches it (costing throughput, never
  correctness); `run_backtest`'s private cache is sized from the field
  automatically.

Python's ARITY and keyword names are unaffected by both moves — the binding is a
hand-kept `def_readwrite` list, and `prefetch_depth` is exposed through it
(`python/src/bindings/backtest.cpp`) as an attribute, not as a constructor
keyword. A Python caller who never touches the attribute therefore picks up the
new default exactly as a C++ caller does;
`python/tests/test_backtest.py::test_run_config_prefetch_depth_round_trips`
asserts it.

### REMOVED

* **The deprecated `VolSurface`-bound portfolio engine**: `portfolio.hpp`,
  `portfolio_risk.hpp` and 34 symbols (`PortfolioLeg`, `LegKind`, `AggMode`,
  `bulk_price`, `scenario_pnl`, `project_compare`, …). Replacements, honestly:
  multi-shock scenarios → `scenario_grid.hpp`; theoretical legs →
  `contract_projection.hpp`; factor attribution → `pnl_attribution.hpp`; `ByUid`
  aggregation → `reduce_risk_buckets`. **Stock/cash legs, bulk selection, and
  the ByUidExpiry / ByGroupId aggregation views have no canonical counterpart.**
  Deleting the header also resolved a latent ODR conflict: `atx::vol::LaneStatus`
  had two different definitions, and the `american_batch.hpp` one survives.
  `scenario_grid.hpp` and `pnl_attribution.hpp` were promoted to Tier-A.
* **The `SurfaceArchive` v1 writer/reader** (`write_surface_archive[_file]`,
  `class SurfaceArchive`, `SurfaceArchiveWriteOpts`,
  `archive_identity_from_header`) and the `atx-vol-archive-v1` library.
  ATXVSA2 is the only shipped surface-archive format. The retired format's
  on-disk record declarations are kept as reference — `RunDir::run_identity_hash`
  still recognises such a file by its magic. Note this header used to declare
  symbols a plain `atx::vol` link could not resolve.
* `calib_pool.hpp` (`calibrate_pool`, `CadenceQueue`); `vola_parity.hpp`;
  `arb_project_calendar_essvi_total`; the four `derivatives.hpp` unit
  constexprs (`var_dec_to_points`, `var_points_to_dec`, `vol_dec_to_points`,
  `vol_points_to_dec`); `dispersion_build_schedule`, `dispersion_run_backtest`,
  `dispersion_verify`, `DispersionVerifyReport`.

### Numbers that moved

These change results without changing a signature — the category this file
exists for.

* **Deep-OTM put premia.** Black-76 puts are priced from `Φ(-d)` instead of
  `1 - Φ(d)` in `black76_aux`, `black76_value_and_vega`, `black76_greeks`, the
  implied-vol Halley loop and the AVX2 kernels. Far-wing values move; they were
  catastrophically cancelling to zero or negative.
* **AVX2 P&L.** The vector kernel adopts the scalar association tree, so
  `total == sum(terms)` holds and a position's P&L no longer depends on its
  batch index — a contract `simd/pnl_batch.hpp` already claimed.
* **Archive bytes and content identity are now reproducible.** Slice-params
  padding is no longer memcpy'd into archive records, so the same fitted slice
  produces the same `payload_crc32c` every time. Archives written by earlier
  builds are not byte-reproducible by this one.
* **`VolSurface::iv` returns NaN** for non-finite or non-positive `T` (was
  `+inf`).
* **A moved-from `PricedSurfaceView` is structurally empty** and answers no
  queries; it previously answered and could index out of bounds.
* **Vol-time is fail-closed.** `trading_hours_between`, `vol_time_years`,
  `time_to_expiry_years` and `tenor_years` return `Result<double>` instead of
  `double`, and `VolTimeCalendar` requires an explicit coverage window
  (`us_default()` covers 2024-01-01..2028-12-31). Out-of-window queries are
  `ErrorCode::OutOfRange` rather than a silently credited 7.5h session.
* **`all_symbols` / `universe_at`** lost their `index_symbol = "SPY"` default;
  the argument is required.
* **Corrupt archives that used to be accepted now fail with `ParseError`** —
  the LinearVariance node axis is validated and slice payload extents must be
  monotone and disjoint. Backtests fail closed on backwards snapshot timestamps.
* **`PortfolioPricer`'s returning `price()` / `pnl_explain()` are genuinely
  concurrent-const-safe** (per-call workspace), at the cost of the cross-call
  cached workspace.
* **`BacktestResult::validate()`** is new and enforced at `run_backtest` and the
  three TSV/CSV writers: every column must be empty or exactly `size()` long. A
  producer handed a skewed result now returns `InvalidArgument`.
* **Loose dispersion result TSVs are off by default**, behind
  `DispersionRunConfig::emit_tsv_diagnostics` (spec key of the same name,
  default `false`). Retained-input and evidence TSVs are unaffected.
* **`surface_insert_vol_slice(..., with_no_arb_check = true)` now actually
  checks.** The parameter used to be accepted and discarded, leaving
  `InsertedSliceHandle::no_arb_status == 0` unconditionally. It now runs a dense
  butterfly/calendar sweep over the resolved slice and reports through
  `no_arb_status` (`kNoArbStatusButterfly` / `kNoArbStatusCalendar` /
  `kNoArbStatusNotEvaluated`) plus the `kFlagNoArbWarning` provenance bit. It is
  a report, never a rejection — the handle is still returned, with the same
  numeric contents. The default (`false`) path is unchanged and still costs
  nothing, so no shipped caller's numbers move.

### NEW — every public batch kernel is now reachable from Python

Three `batch.hpp` entries were bound, which completes the set — verified by
enumerating `batch.hpp`'s six entries against the module rather than by
inspection, because an earlier draft of this section claimed completeness while
`black76_price_from_lnfk_batch` was still unbound:

* `black76_greeks_batch(F, K, T, sigma, r, df, side)` — a dict of SoA numpy
  columns (`delta`/`gamma`/`vega`/`theta`/`rho`/`vanna`/`volga`/`charm`/`price`).
* `black76_value_and_vega_batch(F, K, T, sigma, df, side, sqrt_t=-1.0)` —
  `(value, vega)` for one expiry slice (`T` and `sqrt_t` shared, as in the C++
  signature).
* `black76_price_from_lnfk_batch(F, K, T, sqrt_t, sigma, df, ln_fk, side)` — the
  bind-step shortcut for a caller that already holds `ln(F/K)` and `sqrt(T)`.
  `sqrt_t` is **required and has no sentinel** here: the scalar kernel consumes
  it verbatim, so there is no negative value meaning "recompute". Its scalar
  companion `black76_price_from_lnfk` is bound alongside it, so the batch's
  bit-exactness is checkable from the same interpreter.

Binding-only: no kernel changed, and all three go through the validated
`batch.hpp` entry points rather than the raw `simd::` kernels.

Three notes for callers:

* **None returns a per-lane `status` column, and that is the NaN + per-lane
  convention rather than a departure from it.** A parallel status exists where
  the kernel HAS a per-lane failure channel a binding must not erase
  (`implied_vol_batch`'s `span<Status>`; the American batch's `LaneStatus`).
  These three have none — `black76_greeks`, `black76_value_and_vega` and
  `black76_price_from_lnfk` are `noexcept` total functions whose degenerate lanes
  collapse to the documented intrinsic result — so an all-`STATUS_OK` column
  would advertise a diagnostic carrying no information. Only a malformed *call*
  raises.
* **Greeks and value+vega agree with their scalars to the SIMD gate, not
  bit-for-bit; from-lnFK is bit-identical.** The first two dispatch to AVX2 at
  `n >= 4`; from-lnFK has no vector kernel. The gate is per output column —
  ~1e-6 absolute + 1e-7 relative on prices and Greeks, ~1e-5 absolute on the
  fused batch's `vega`.
* **`side` now also accepts a single `Side`, broadcast across the batch**, on
  every binding that takes a `side` column (the American batches included). Pure
  widening: a per-lane integer column behaves exactly as before, and a float
  column is still refused with `ErrorCode::InvalidArgument` rather than
  truncated onto `Side::Call`.

### NEW — embedding: a diagnostic sink and cooperative cancellation

The two things a library has to offer before a host can embed it: give up the
process's streams, and be stoppable.

* **`atx/vol/log.hpp` (Tier-B) — diagnostic sink.** `install_log_sink(sink, user)`
  routes every diagnostic atx-vol emits to a callback carrying a `LogLevel`, a
  `LogStream` and one newline-free line. **All 13 library stream writes across 5
  source files now go through it**; no `fprintf`/`printf`/`cerr`/`cout` to a
  process stream remains in library code.
  **With no sink installed, output is byte-identical to 1.0.0-pre**: the same
  text on the same stream, so this is not a behavioural change for any existing
  consumer. The stream is carried on the record rather than derived from the
  level, precisely so the two Info-level sites that historically wrote to
  *different* streams both stay unchanged.
  The callback must not throw (the emit path is `noexcept`), must not re-enter
  atx-vol, and **must tolerate concurrent invocation** — pricing-pool workers
  emit, so records arrive on threads the host never created, and record order
  across threads is not defined. Install once, before the first emitting call.
* **`ErrorCode::Cancelled` (atx-core)** — appended last, so no existing
  enumerator's `u16` value moved.
* **Cooperative cancellation on the four long-running entries.** A `CancelToken`
  (`atx/vol/types.hpp`) is a non-owning view of a caller-owned
  `std::atomic<bool>`; a default-constructed one never cancels and costs one
  branch per poll. Plumbed as `RunConfig::cancel` (**this is the 15 → 16 field
  above**), `CorpusConfig::cancel`, `SurfaceDbPopulateConfig::cancel`, and — for
  the run-dir-only entry that has no caller-supplied config — a defaulted
  trailing parameter on `dispersion_run_projected_var`.
  Cancellation is a **clean early return with `ErrorCode::Cancelled`, never a
  partial write**. Each entry polls at the top of a loop iteration, before that
  iteration writes anything: `run_backtest` returns no result at all (and writes
  no files in any case); `build_corpus` leaves no manifest, so the corpus never
  claims to be complete; `populate_surface_db` leaves a **valid database holding
  a prefix of the dates**, because each date is committed atomically with a
  generation-bumped manifest — stop a long backfill and re-run to resume;
  `dispersion_run_projected_var` writes its artifacts only after the work it
  cancels, so the run dir is untouched. The two fan-out entries (`build_corpus`,
  `populate_surface_db`) additionally poll at the **top of each fit task**, so a
  stop drains the queued fits instead of running them to completion — the cancel
  shortens the run rather than only declining to publish its index. A fit already
  **in flight** is never abandoned: the call returns once the boards already
  running finish.

### Packaging, versioning and ABI

* **`find_package(atx-vol)` works from an install prefix.**
  `cmake --install <build> --prefix P` ships headers, static archives and
  `atx-volConfig.cmake`; the exported targets are `atx::vol`, `atx::core`,
  `atx::tsdb`, `atx::vol-tools`, `atx::vol-research`, with `atx::vol::tools` /
  `atx::vol::research` recreated so in-tree source compiles against the install
  unchanged. `Result<T>` is still `tl::expected<T, atx::core::Error>` and
  `tl-expected` installs into the same prefix.
* **The version is single-sourced** from `project(atx VERSION ...)` through a
  generated `atx/vol/detail/version_generated.hpp`. `atx::vol::version()` no
  longer carries its own literal. New: `ATX_VOL_VERSION_{MAJOR,MINOR,PATCH}`,
  `ATX_VOL_VERSION_STRING`, `ATX_VOL_VERSION_NUM(a,b,c)` and `ATX_VOL_VERSION`
  for preprocessor feature-gating, plus `atx::vol::kVersionString`.
* **Package compatibility is now `SameMajorVersion`** (was `SameMinorVersion`,
  correct only while the version was 0.y.z). A `find_package(atx-vol 1.0)`
  consumer accepts any 1.z.
* **atx-vol 1.x is distributed static-only**, with no `ATX_VOL_API` export
  macro — see the *Linkage and distribution policy* section of `README.md` for
  why (header-inline instrumentation globals get one instance per image on
  Windows). `BUILD_SHARED_LIBS` now fails configure with the reason instead of
  being silently ignored, and `cmake --install` refuses a shared build.
* **The `ATX_VOL_COUNTERS` / `ATX_VOL_PROFILE` ODR trap is closed.** Those
  options change the definition of inline entities in
  `atx/vol/detail/counters.hpp` and `atx/vol/detail/phase_profile.hpp`; a
  consumer that disagreed with the library used to silently read a plane nobody
  wrote. The configuration is now part of an inline namespace name, so a
  mismatch fails to **link**, naming both sides. No struct layout changed and no
  computed value moves. `ATX_VOL_PROFILE_CONCAT[_INNER]` are renamed
  `ATX_VOL_PROFILE_DETAIL_CONCAT[_INNER]` and defined in both configurations.
* **Archive format naming is unified on the on-disk magic**: the live format is
  **ATXVSA2** (magic `ATXVSA20`) and the retired one is **ATXVSA03** (magic
  `ATXVSA03`). The old "v1" / "v3" ordinals are gone from the headers — they
  named the same format both ways. Comment-only; no identifier changed.

### REMOVED / NEW — the bespoke strangle-vs-varswap strategy is now a declarative spec (swap-lane DSL sprint)

**Removed.** `StrangleVsVarswapStrategy` + `StrangleVarswapConfig`
(`strangle_varswap.hpp/.cpp`), the 600-line
`atx-vol-strangle-varswap-driver`, and their test suite. The class was a
one-analysis special: its cycle lifecycle, swap sizing and signal mirror were
generic machinery trapped in bespoke code.

**New, in its place — the grammar now expresses the whole analysis:**

* `LifecycleSpec::Holding::FixedExpiryRestrike` — a cycle fixes ONE expiry
  (ceil-snapped onto `StrategySpec::session_ts`; the legs' shared
  `tenor.target_T` is the cycle tenor) and every entry tick restrikes the
  option legs at it; keep-strikes on soft resolution failures with the
  `skipped_restrikes` / `unopened_entry_steps` counters; `Entry` is the
  restrike cadence.
* `StrategySpec::swap_legs` (`SwapLegSpec` + `SwapSizeSpec`:
  `FixedQty` / `TargetVega` / `MatchGroupVega`) — one fair-struck swap leg per
  cycle, opened on the cycle-open step only, every refusal counted in
  `skipped_swap_cycles`. Empty `swap_legs` is bit-identical to the old
  grammar (the additive-lane rule).
* `swap_leg.hpp` — the reusable toolkit: `swap_contract_for_lot` (the engine's
  `SwapLot`→`DerivContract` transcription), `solve_cycle_swap` (fixing-count +
  bridge-priced fair strike + vega-targeted qty, fail-soft by contract), and
  `SwapSignalProbe` (the engine-accrual mirror behind the
  `swap_delta/gamma/vega/theta/rho` signal columns, NaN discipline included).
* `DeclarativeStrategy::signals` — emitted only when the spec carries swap
  legs: the probe's five columns + `options_vega` (the old `strangle_vega`,
  renamed lane-agnostic; `tools/render_strangle_vs_varswap.py` reads either
  spelling) + cumulative `skipped_restrikes` / `skipped_swaps`.

**Migration.** `examples/varswap_compare_example.cpp` is the old driver's
replacement: the full comparison as a ~20-line spec. Old config fields map
1:1 (`target_abs_delta` → the strangle selectors, `tenor_years` →
`tenor.target_T`, `contracts` → `FixedContracts`, `enable_swap_leg` → the
`swap_legs` vector).

**Why the numbers are trustworthy.** The deletion sat behind a track-parity
gate: old vs new through the same engine on the full XOM 2026 fixed-db window
(137 sessions, both legs, delta-hedged) — per-row `nav`/`pnl_total` within
7e-9 dollars, `swap_pv`/`swap_pnl` bit-identical, identical lot-id watermarks
and skip counters; plus synthetic-corpus parity including a dark-session
keep-strikes run. The example reproduces the reference track to the cent
(combined −7499.69 / swap −6065.38 / strangle −1434.31).

### FIXED — calendar level repairs no longer fabricate slice levels from extrapolated-wing crossings (fit fidelity budget + tradeable-overlap band)

**The defect.** Every parametric calendar repair — `arb_project_calendar_svi_pair`
/ `_essvi_pair` / `_c8_pair` at the `fit_slice_curve` serving seam, and the
surface-level `arb_project_calendar_svi` / `_essvi` in `run_surface_parity`'s
`CalendarRepair::Project` branch — closed w(k)-crossings by shifting or scaling
the longer slice's LEVEL (`a` / `theta`) by the WORST-CASE deficit over a fixed
k-band (±0.6 at the pair seam, **±3.0** in the Project branch). For slices
quoted to |k| ≲ 0.1–0.3 (every weekly and most mid-tenor chains), nearly that
whole band is extrapolation on BOTH slices: the closed forms' wings there are
unidentified by any quote, so the "crossing" was fiction — and the repair
converted it into a real ATM error, slice after slice, since each repaired slice
becomes the next pair's floor. The result passed every downstream gate
(shape-preserving shifts keep butterflies clean; calendar is clean by
construction; risk admission checks geometry, not fidelity to quotes) and was
stored. On the sp100-2026 database this served XOM 2026-02-18 mid-tenors at up
to **+25 vol pts over their own quotes** (43d ATM: quotes 28.4, served 53.4; the
SVI `a` shifted 0.0091 → 0.0331 by a k=±0.6 wing deficit), and — because the
fitted wing shapes flip day to day — produced the ±8–11 pt one-day ATM
spike-reverts on XOM/CVX that surfaced as artificial P&L spikes in the
strangle-vs-varswap backtest.

**The fix, structural.**
1. **Pair projections act only on the tradeable overlap** of the two slices'
   data-supported k-ranges (∩ the risk band) — the exact rule
   `SplineVolCurve::project_calendar` already applied; the parametric branches
   now follow it. `fit_curve_surface` tracks each committed slice's observation
   k-range and hands it to the next pair (`prev_data_k_range`, previously
   populated only for SplineVol). A crossing with no traded witness no longer
   moves any level.
2. **Every level repair carries a fidelity budget**
   (`kCalendarRepairMaxAtmShiftFrac = 0.10` of the slice's pre-repair ATM total
   variance, floor `1e-6`): a repair that would move the ATM further returns
   `Unavailable` and leaves the slice/surface untouched (all five projectors are
   now transactional). Failure flows into the existing honest paths: the slice
   is dropped (soft) or the risk candidate walks the family ladder.
3. **`CalendarRepair::Project` repairs over the certified band** (±0.5,
   `static_assert`-tied to both `RiskSurfaceValidationConfig` and
   `strip::kCertifiedWingHalfBand`), not the ±3.0 diagnostic grid — repair now
   covers exactly what admission checks. The ±3.0 grid remains as the
   `calendar_arb_free` DIAGNOSTIC and may now honestly report unrepaired wing
   crossings.
4. **The SVI fallback ladder reaches the dense rung** (`kFromSvi` gains
   `LinearVariance`, which the risk path substitutes with `ConvexDense`) — the
   one family list that did not descend to the direct-variance curve its own
   contract promises. Invisible while the fabricated repair force-admitted
   every SVI candidate; load-bearing now that rejection is honest.

**Effect on existing callers.** Numbers move wherever a calendar repair used to
fire on out-of-overlap or beyond-budget crossings — deliberately: those numbers
were fabrications. Boards whose parametric candidates genuinely cannot
reconcile quotes with in-band calendar monotonicity now serve the dense model
via the ladder (XOM 2026-02-18 does exactly this) or drop the offending slice.
Re-fit of the poisoned XOM cell: every tenor within ~1 vol pt of its own
quotes (worst tenor was +25.0). Gates: `ArbProjectCalendarPair.*Refuses*`,
`ArbProjectCalendarSvi/Essvi.Refuses*`,
`VolCurve.SviPairProjectionActsOnlyOnTheTradeableOverlap` /
`SviPairProjectionStillRepairsInsideTheOverlap`.

**Databases built before this fix carry the fabricated levels** — the carry-over
fingerprint does not cover the fitter, so a resume will NOT repair them (see
"Re-running at the corrected --r does NOT repair the database" in
docs/surface-db-build.md for the identical mechanism). Rebuild affected
symbols' partitions from the hive, or rebuild into a fresh root.

**Validated on a full XOM+CVX 2026 rebuild** (release binary, fresh root
`scratch-fitfix-2026`, 140 sessions): CVX 140/140, XOM 137/140 with three
honestly-rejected boards (2026-02-13 / 02-18 / 06-11: butterfly- or
calendar-inadmissible on every ladder rung; the backtest engine's documented
dark-day drop handles them). Daily 3M ATM fit noise: XOM 3.28 → **0.93**
vol pts/day, CVX 5.28 → **0.92** — both now indistinguishable from AAPL/MSFT —
with the spike-revert autocorrelation gone (lag-1 −0.50 → −0.17) and the
largest one-day ATM move 11.6 → 2.6 pts. The strangle-vs-varswap reference
run's legs land on the same scale (strangle −$1,434, swap −$6,065 over the
window). An AAPL one-cell control rebuild serves IVs identical to ~1e-10:
surfaces whose repairs never fired are numerically untouched.

### CHANGED — the variance strip now reads flat-vol tails beyond the certified wing band (wing clamp)

**What changed.** `var_swap_fair_strike` (and everything that prices through it:
`deriv_price` on every swap kind, `deriv_greeks`, the backtest swap lane's daily
marks) now clamps its SURFACE READS to a wing trust band. Strip nodes beyond the
band keep their true strikes but price under the BAND-EDGE vol — flat-vol tails
over the wings, never a truncated span, so the log-contract replication stays
complete. The band is `DerivConfig::wing_clamp_k`: `0` (the default) selects the
certified validation band `strip::kCertifiedWingHalfBand = 0.5` — the |k| ≤ 0.5
band `RiskSurfaceValidationConfig` actually certifies, `static_assert`-tied so
the two cannot drift — `> 0` is an explicit half-band, `< 0` restores the old
unclamped reads, `NaN` is `InvalidArgument`. A new provenance flag
`DerivFlags::WingClamped` (structural: "tails were in effect", set even when the
smile is flat and the clamp moves nothing) records it on every quote. The strip
span, node count, adaptive widening and truncation flags are untouched: this
clamps *reads*, not the grid.

**Why.** A parametric eSSVI/SVI slice serves an *unbounded linear-in-|k|*
extrapolation at any k, no quote ever disciplines it beyond roughly the ATM
region, and the fit pipeline certifies nothing outside |k| ≤ 0.5 — yet the
Standard strip integrated it to ±1.5 with 1/K weighting. On the sp100-2026 XOM
corpus that fiction read 82 vol at k = −1.0 (swinging ±22 vol pts/day), inflated
the 3M fair strike to ~38 vol against a ~30 ATM and ~29.6 realized, and put
~98% of the daily mark variance beyond |k| = 0.25 — the mechanism behind the
XOM 2026 reference run's swap leg bleeding −$42k with ±$90k single-day marks
and sign-flipping FD gamma. With the default clamp the same corpus prices
~33.9 with ~3.5 vol pts/day of mark noise, in line with the ATM's own 3.3.

**Effect on existing callers.** This moves K_var on ANY surface with wing
structure beyond ±0.5 — deliberately. A flat or near-flat surface is unchanged
(band-edge vol == wing vol), so the analytic `K_var == σ²` contracts all hold;
a caller whose wings ARE quote-disciplined and who wants them integrated raw
sets `wing_clamp_k = -1.0` (or any negative) and gets the pre-clamp number
bit-for-bit. Gate: `WingClamp` (tests/derivatives_test.cpp), including a
node-for-node flat-tail oracle at 1e-12.

**The XOM 2026 reference run, re-taken under the clamp** (same corpus, gen 225,
same window/config; artifacts in `xom-strangle-varswap-2026-wingclamp/`):
strangle **+$1,501.64 — bit-identical**, the option lane never touches
`DerivConfig`; variance swap **−$10,436.00** (was −$42,468.19); per-cycle swap
P&L +$32,009 / −$35,471 / −$6,974 against strangle +$80,743 / −$50,173 /
−$29,068 — the two legs finally live on the same scale. Daily swap-mark noise
$18.0k (was $24.5k); what remains is the in-band sp100-2026 fit noise (ATM
itself moves 3.3 vol pts/day on this corpus), which is the refit's job, not the
pricer's. `swap_gamma`'s sign still flips day to day — that is a CONVENTION
fact, not a defect: marks read the surface at fixed log-forward-moneyness, so
the smile floats with a spot bump and a var swap's FD gamma is numerical noise
around zero.

### NEW — strangle-vs-varswap comparison backtest + the XOM 2026 reference run (strangle-vs-varswap sprint, Tasks 1-5)

**What shipped.** `strangle_varswap.hpp`/`.cpp` add
`StrangleVsVarswapStrategy`, an `IStrategy` that runs one fixed-expiry,
daily-restriked 40Δ strangle against one uncapped variance swap struck fair and
sized to that strangle's **entry** vega, both on a single clock and
delta-hedged daily. Equal vega is enforced in DOLLAR terms (per-share vega ×
contracts × multiplier) at each cycle open, and the swap's strike comes from
the `deriv_price` bridge's `fair_strike_dec` — not the `PricedSurface`
`var_swap_fair_strike` — so strike and engine mark share one carry and the swap
opens at a bitwise `+0.0` PV. Per-step `swap_delta/gamma/vega/theta/rho`,
`strangle_vega` and cumulative `skipped_restrikes`/`skipped_swaps` counters ride
out as strategy signals. `examples/strangle_varswap_driver.cpp` (target
`atx-vol-strangle-varswap-driver`, `ATX_BUILD_EXAMPLES`) drives it over a
`SurfaceDb` and writes `track.tsv`; `tools/render_strangle_vs_varswap.py`
renders the five-panel comparison report.

**Effect on existing callers.** Additive only — a new header, a new example
target, a new Python tool, and one new test module. No library type, engine
path or existing test changed; `write_backtest_pnl_tsv`'s frozen column set is
deliberately *not* widened (its `{name, order}` is `static_assert`-pinned to the
RunArchive registry that feeds `ra_schema_hash()`), so `swap_pv`/`swap_pnl`
reach the TSV as signal columns instead and the schema hash is untouched.

**Operational contracts worth knowing.** A live variance swap makes the engine
fail the whole run closed on any missing board — `unpriced = ExcludeAndReport`
governs option lots only and is no escape for the swap lane. The driver
therefore probes every partition in the window and builds its clock from the
sessions where the symbol's surface exists; a partition the manifest lists whose
*archive will not open* is reported as an inconsistent database (exit 1), not as
a dark session, so a broken db can never silently shorten a measured window. The
final cycle of a corpus whose calendar the tenor outruns may legitimately run
one-legged (`skipped_swaps ≥ 1`, no swap lot). `swap_theta` is legitimately NaN
within one bump width of expiry while the swap is live, so liveness is read off
`swap_vega`.

**The reference run** (`XOM`, `sp100-2026` gen 225, 2026-01-02 → 2026-07-24,
139 sessions, 1 dark, three 91d cycles) is recorded for provenance:
strangle **+$1,501.64**, variance swap **−$42,468.19**, combined
**−$40,966.55**. The equal-vega identity held to 0 ULP at all three cycle opens,
`reconcile_nav` held on every row, and the leg split closes back to NAV to
1.5e-11.

**Read that run's economics with the corpus in mind.** The two legs' daily P&Ls
are essentially *uncorrelated* on this data (ρ = +0.038), and the swap's path is
the *noisier* of the two (daily σ $24,478 vs $21,739). That is a property of the
corpus, not of the strategy: each leg is independently reproducible from the raw
surface — the strangle from the 40Δ/ATM implied-vol move (R² = 0.69) and the
swap from a 1/K²-weighted replication of the variance strip (R² = 0.81) — but in
this corpus those two regions of the surface are themselves only ρ = +0.24
related, because the deep put wing carries ~27 vol points/day of fit noise
(vs 3.3 at ATM) that is *anti*-correlated with the ATM move (ρ = −0.39), and
every strike's daily IV change has lag-1 autocorrelation ≈ −0.5 — the signature
of independent per-day fit noise rather than a coherent vol path. A variance
swap integrates exactly that region. Treat the sp100-2026 wings as unfit for
strip-sensitive P&L attribution until they are refit.

### NEW — vol-derivatives production surface: capped/mid-life swaps, greeks, dated fixings, DerivBook (derivatives-production sprint, Tasks 1-10)

**What shipped.** `derivatives.hpp` gains two capped product kinds
(`DerivKind::CappedVarSwap`, `CappedVolSwap`) plus a mid-life dispatch for
`VolSwap` contracts with `0 < n_done < n_total` (previously priced only at the
two exact-aged endpoints, inception and full accrual). All three price their
model leg against a lognormal distribution for the future realized-variance
leg (Gauss-Hermite / split-domain Gauss-Legendre quadrature,
`detail/rv_lognormal.hpp`): closed-form for the capped variance swap, kinked
split-domain quadrature for the capped vol swap, plain Gauss-Hermite for the
smooth mid-life sqrt payoff. A new `DerivConfig::vol_of_vol` knob (0 =
auto-calibrate so the lognormal's `E[sqrt(W)]` reproduces the surface's own
Carr-Lee `K_vol`) drives all three. `deriv_greeks` differentiates every
product kind via sticky-strike finite differences, with the center's resolved
strip grid and any auto-calibrated vol-of-vol pinned into every bump so a
bumped evaluation cannot land on a different quadrature scheme than the
center. `RealizedTracker::observe_dated` adds a strictly-ascending-timestamp
fixing entry point for daily-fixing drivers. New `deriv_book.hpp` prices a
book of swap positions against a `SurfaceSet` (additive companion to
`portfolio_pricer.hpp`'s option book, combined via `combine_totals`), and
`backtest.hpp`'s strategy-aware engine gains an additive variance/vol-swap
lane (`PortfolioState::swap_lots`, held to expiry, no early close in v1).

**Effect on existing callers.** Additive only. Every new type/field defaults
to the prior behavior: `DerivKind::VarSwap`/`VolSwap` dispatch is unchanged,
`DerivConfig::vol_of_vol = 0.0` auto-calibrates but that resolver is reachable
only from the new capped/mid-life dispatch paths, and
`PortfolioState::swap_lots` defaults empty (an empty-swap-lots book prices,
accrues and settles exactly as it did before this lane existed). One field's
OBSERVABLE SENTINEL does change: `DerivQuote::integration_error_est` was
unconditionally `NaN` (documented as "this port does not yet run the
Richardson half-step refinement"); `var_swap_fair_strike` now populates it
with a real Richardson half-grid estimate (`|I_h - I_2h|/15`) whenever the
resolved strip node count is `4m+1` — every quality-tier default and the E2
adaptive-wing rescale land there. A caller gating on `(x == x)` to mean "not
estimated" now sees a real number on those grids; a caller-pinned
`strip_nodes` that isn't `4m+1` still leaves it `NaN`, unchanged.

**Not shipped.** The RV distribution-affine / Monte-Carlo QE pricing engines
and the discrete-monitoring full-Monte-Carlo correction remain reserved and
actively return `NotImplemented`. CBOE variance-future marking
(`DerivMarkingConvention::CboeVarianceFuture`) is declared but unenforced — no
pricing path reads `DerivContract::marking` yet. `BacktestDb` refuses (rather
than silently drops) a run, checkpoint, or append that actually carries swap
data — its checkpoint and series schema predate the swap lane; schema support
is a deferred follow-on. Swap-lot entry is frictionless (zero cost, no
spread/impact) in v1, and `DerivBook` prices its positions single-threaded.

### BREAKING — `DispersionConfig::target_vega` is now dollars per VOL POINT (E1 / AN-P1-1)

**What changed.** `build_dispersion_book` (the projected / surface dispersion
route) sized the index leg as

```
straddle_qty = target_vega / (straddle_vega * multiplier)
```

where `DispersionLeg::straddle_vega` is a per-share `dP/dsigma`, i.e. vega per
**unit** vol. The listed route (`build_listed_dispersion_roll`) has always sized
off `vega_per_contract_per_vol_point = vega_per_unit_vol * multiplier * 0.01`,
i.e. vega per **vol point**. The same conceptual knob therefore carried two
conventions 100x apart: `target_vega = 10000` built a projected-route book
carrying \$10,000 of vega per 1.00 of sigma (\$100 per vol point) while the
listed schedule built one carrying \$10,000 per vol point.

Cross-route PnL and parity comparisons were meaningless unless the caller knew
to rescale by hand.

**The canonical unit is now dollars of gross index vega per ONE VOL POINT** — a
0.01 move in sigma. This is the industry convention and the unit the listed
route already used. `build_dispersion_book` divides by
`straddle_vega * multiplier * 0.01`, textually matching the listed route.

**Effect.** For an unchanged `target_vega`, projected-route books GROW BY EXACTLY
100x — contract counts, gross notional, premium, PnL and NAV all scale by 100.
`K`, `T`, `sigma`, `straddle_vega`, `call_mark` and `put_mark` are unchanged
bit-for-bit; only `straddle_qty` (and everything downstream of it) moves.

Note: the sprint plan's parenthetical said projected books would "shrink 100x".
That is the wrong direction — dividing by an extra factor of 0.01 makes the
quantity larger. The plan's normative formula (divide by
`straddle_vega * multiplier * 0.01`) and its cross-route test gate are both what
is implemented here.

**Migration.** Any caller tuned against the old projected-route behaviour must
DIVIDE its `target_vega` / `gross_index_vega` by 100 to keep the same book size.
This includes `DispersionBacktestConfig::gross_index_vega` and the
`gross_index_vega` key in `run_spec.tsv` for surface-route backtest runs.
Callers that were already feeding the listed route's number now get a book that
matches it, which is the point.

**Migration also applies to RISK LIMITS, and silently if you skip it.**
`DispersionRiskLimits::max_gross_vega` / `max_gross_notional` and
`DispersionRunConfig::capital` (`dispersion_run.cpp`, `strategy.hpp`) are
compared against a book that is now 100x larger. Following the migration above
(divide `gross_index_vega` by 100) DOES fix them, because
`measure_book`/`binding_limit` scale with the book — but a spec that sets a
limit and is NOT migrated starts CLAMPING or HALTING with no error: the same
limit value now binds at 1/100th of the intended book size. The failure mode is
a book that quietly stops trading, not a diagnostic. Migrate the limits with the
target, or set them to 0 (unlimited) while you do.

**Affected surfaces.** `DispersionConfig::target_vega`,
`DispersionBacktestConfig::gross_index_vega`, the `gross_index_vega` run-spec
key, `dispersion_run_surface_backtest`, `dispersion_run_projected_var`, and
every artifact they emit. The listed schedule route
(`gross_index_vega_target_per_vol_point`) is UNCHANGED — it was already correct.

**Golden replay pins.** The 82-session and 135-session `surface_backtest.tsv`
reproducibility pins move as a direct consequence (position scale -> NAV scale).
Re-pinning is a single coordinated event owned by the sprint controller; see the
disp-hotpath STATUS doc.

**Test gate.** `ListedDispersionSchedule.ProjectedAndListedRoutesAgreeOnVegaUnit`
builds both routes over the SAME three `PricedSurface`s at the same tenor,
multiplier and target, and asserts the two index-leg dollar-vega-per-vol-point
figures agree to 5%. Before this change it failed by exactly 100x (projected
100 vs listed 10000).

### FOLLOW-UP — the X3 gross-vega limit now honours the multiplier (C-2)

E1 above migrated the SIZING to dollars per vol point but left the X3 risk probe
(`measure_book`, `dispersion_strategy.cpp`) summing a bare
`|straddle_vega * straddle_qty|`. That expression discards the `multiplier` the
function is handed AND the per-vol-point scale, so
`DispersionRiskLimits::max_gross_vega` was compared in the advertised unit only
at the historical `multiplier == 100`; elsewhere the measured exposure was off by
exactly `100 / multiplier` (10x under-reported at 1,000, 10x over-clamped at 10).
`multiplier` is a real typed run-spec key on this branch, so non-100 books are
reachable from production.

**Effect.** `max_gross_vega`, and the `risk_clamp_scale` / `risk_breach_reason`
telemetry it drives, are UNCHANGED at `multiplier == 100` (the default, and the
82-session golden's value — which also configures no limits at all, so the golden
path never measures). At any other multiplier the measured gross vega changes by
`multiplier / 100`; a spec that pins both a non-100 multiplier and a
`max_gross_vega` must restate the cap in dollars per vol point.

The conversion now lives once, in `contract_vega_per_vol_point` (dispersion.hpp).
Projected sizing, the listed schedule's `vega_per_contract_per_vol_point` column
and its round-trip validator all adopt it with the same operand association, so
those three are bit-identical. Guarded by
`Strategy.DispersionGrossVegaLimitIsDollarsPerVolPointAtNonHistoricalMultiplier`,
whose oracle (`2 * target_vega`, for any multiplier) is hand-derived from the
sizing contract rather than re-evaluated from the code.
