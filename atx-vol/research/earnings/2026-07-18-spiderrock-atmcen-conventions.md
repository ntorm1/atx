# SpiderRock ATM Earnings-Censored IV Conventions — Reproduction Research

**Date:** 2026-07-18. **Objective:** reproduce `atmCenI_*` and `iEMove` from an OPRA
snapshot + estimated earnings dates, matching SpiderRock `tbltickerhistoryv3`.

## Primary sources
- Live Volatility Surfaces (Connect): https://docs.spiderrockconnect.com/docs/next/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/
- Volatility Time Calculation: https://docs.spiderrockconnect.com/docs/next/Documentation/PlatformFeatures/Analytics/VolTimeCalc/
- Option Pricing: https://docs.spiderrockconnect.com/docs/next/Documentation/PlatformFeatures/Analytics/OptionPricing/
- Historical VOL2G Data Dictionary (PDF, `tbltickerhistoryv3` fields): https://spiderrock.net/wp-content/uploads/2021/04/6.-Historial-VOL2G-Data-Dictionary-Product-Overview_v1.pdf
- Historical Volatility Surfaces Data Dictionary (PDF): https://spiderrock.net/wp-content/uploads/2021/04/5.-Historical-Volatility-Surfaces-Data-Dictionary-Product-Overviews_v1.pdf

[PDF] = verbatim from dictionary PDF text extraction. [doc] = Connect HTML docs via summarizer.

## GOAL 1 — eMove / iEMove / atmCen and the censoring formula
Model [doc]: **σ²T = σ_E²·n + σ_C²·T**; σ=ATM vol, σ_E=eMove (per-event move vol),
σ_C=censored vol, T=time-to-expiry, n=earnings events before expiry.
Censor: `σ_C = sqrt((σ²T − σ_E²·n)/T)` per listed expiry, then **censored ATM values
interpolated on a fixed-term grid** and stored `atmCenI_5d…504d` [PDF]. eMove calibration
[doc]: "Given a collection of expiration ATM volatilities, along with an earnings count
grid and an eMove guess, we can compute a fitness value ... by computing the deviation
from a smooth term curve model." Result → `iEMove`, copied to `eMove` on curve record.
**Our internal model is algebraically identical.** σ_E² = per-event variance = eMove².

## GOAL 2 — VolTimeCalc
[doc]: α=0.7; trading budget 1890/yr (252×7.5h; 7.5h = RTH 6.5h + 1h post-close);
non-trading 6870/yr (8760−1890). T = (TradingHrsRem)·α/1890 + (NonTradingHrsRem)·(1−α)/6870.
**Earnings get NO extra vol-time weight** — handled entirely as the σ_E²·n add-on.
atx-vol `vol_time.hpp` already matches α/1890/6870/formula EXACTLY. MEDIUM confidence on
exact holiday calendar + intraday per-minute decay shape (not published).

## GOAL 3 — ATM definition & forward
[doc]: `atmStrike` = value where call and put price ≈ equal (**forward/parity ATMF**,
not spot-ATM, not literal 50-delta). `atmVol` = **fitted spline anchor** continuously
re-fit, NOT a raw interpolated smile point. Forward `fUPrc = uPrc·e^(rT) − ddiv`;
`carry = rate − sdiv`; **sdiv IMPLIED** by minimizing call/put surface mismatch (market
correction to discrete-div / implied hard-to-borrow); `ddiv` = cumulative discrete divs.
KNOB: naive `rate − q` forward with textbook divs + no borrow shifts every atmCenI.

## GOAL 4 — Smile model & fixed-tenor interpolation
[doc]: **SRCubic** — cubic spline on vol MULTIPLES (29 skew coeffs skewC00..28 + atmVol),
X=moneyness (std lognormal/normal), Y=vol as multiple of ATM vol [PDF]. Grid nodes in
std-dev units [-25..0..25], knotShift recenters on min-vol; exponential-decay wings.
atx-vol has SplineVol = faithful SRCubic port. For the ATM TERM STRUCTURE we only need
`atmVol` at the parity strike, NOT the full 29-coeff smile. Fixed-tenor interp: censored
ATM interpolated to fixed CALENDAR-day grid; boundary = flat extrapolation (first/last
expiry value). Interp SPACE (variance vs vol, calendar vs vol-time) NOT stated — a KNOB.

## GOAL 5 — atmCen st/lt/decay parametric form
[PDF verbatim]: `atmCenI_st`="short term (5 day) model atm volatility (censored using
iEMult)"; `atmCenI_lt`="long term (504 day) model atm volatility"; `atmCenI_decay`=
"model decay parameter"; `atmCenI_5d…504d`="Interpolated N day atm vol (censored using
iEMult)". So TWO objects: a 3-param MODEL {st,lt,decay} and the interpolated grid.
`atmCenH_*` = same censored with historical multiplier `hEMult`. Exact form NOT published
→ inferred **atmCen_model(T) = lt + (st − lt)·exp(−decay·T)** (matches 3 named params;
standard mean-reverting term-vol form). NOTE: censoring uses `iEMult`/`hEMult` (a
multiplier from iEMove/hEMove) — verify whether multiplier == eMove itself by back-solving.

## GOAL 6 — iEMove calibration
[PDF]: `iEMove`="implied earnings move (all earnings events)" = **global term-structure
fit**, not a single straddling pair. `iEFitCode` enum
{None,Minimum,CenterError,LeftError,RightError,SplitError,DecentError,LeftBound,RightBound,
MaxSteps,CenterFlat}; `iEFitError`="term surface fit error"; `expiryCount`="number of
actual expirations involved"; `iEMoveAvg/Std/Min/Max`=distribution across today's fits;
`iEMoveCnt`=number of surface term fits today; `eMoveExpAdj1/eMoveYrsAdj1(+Adj2)`="number
of expirations / trading years (+/-) the next [2nd] earn date was moved to best fit market
term structure"; `hEMove`="historical realized avg earnings move" (last 8–12 moves, w/max
clipping). Mechanism: many term fits, iEMove = best-fit, effective earnings date allowed
to slide ±k expiries / ±Δ yrs. **Big divergence from our two-expiry closed form.**

## GOAL 7 — American vs European IV, pricer, dividends
[doc, Option Pricing]: single-name equity priced/implied **American**. Patchwork:
closed-form European regions; **CRR binomial** for American early exercise; **Vellekoop &
Nieuwenhuis 2006 modified binary trees w/ splicing for DISCRETE dividends**; Ju-Zhong /
BAW for futures. Expiration-day: switch American→European, rate=sdiv=carry=0. Three-rate
model rate/sdiv(implied)/carry=rate−sdiv. atx-vol uses Andersen-Lake de-Am — CRR is the
exact vendor pricer → convention knob (CRR vendor-compat vs ALO best-accuracy).

## GOAL 8 — Field dictionary & units (tbltickerhistoryv3)
[PDF]: `earnFlag`: '0'=is earnings date, '-1/1'=before/after. `expiryCount`=# expiries.
`hEMove`=historical realized avg earnings move. `iEMove`=forward-implied-vol earnings move.
`ccVar`=close-close daily variance; `hlVar`=high-low (Parkinson) daily variance;
**`rvVar`="N.A for now" — NOT populated, do NOT use**. atmCenI grid may omit 10d in the
history table (appended separately). Units (inferred, MEDIUM): vols decimal (0.30=30%);
moves decimal fraction (0.05=5%); term columns keyed CALENDAR days but model T is VOL-TIME
years. Verify empirically against known tickers.

## Prioritized convention knobs (impact on hitting atmCenI < 0.2 vol pt)
1. **ATM extraction = fitted parity-ATMF atmVol w/ IMPLIED sdiv/borrow, American IV**
   (CRR + discrete-div splicing). Wrong forward or European IV shifts every tenor. HIGHEST.
2. **eMove objective = global multi-expiry smooth-curve fit w/ earnings-date adjustment**,
   not two-expiry closed form. Sets how much variance stripped per event. HIGH.
3. **Vol-time clock in variance accounting** — ensure T in σ²T=σ_E²n+σ_C²T and in term
   interpolation is vol-time (α=0.7). HIGH if currently calendar/BUS-252.
4. **Term-interp space + st/lt/decay fit** — variance-vs-vol, calendar-vs-vol-time; fit
   lt+(st−lt)e^(−decay·T); confirm _Nd = raw interp vs parametric read. MEDIUM-HIGH.
5. **nEarnCnt grid** matches SpiderRock earnings calendar incl. ±expiry/±yr slack. MEDIUM.
6. **hEMove max-clipping** (and any iEMove floor/cap). MEDIUM.
7. **Units sanity** (decimal vol/move) and rvVar=N/A. LOW effort, high downside.

## Unavailable → best inference
- Censored term equation: use `atmCen(T)=lt+(st−lt)·exp(−decay·T)`.
- eMove objective: global LSQ deviation from smooth censored curve over all fittable
  expiries, w/ earnings-date offset search (iEFitCode states).
- Intraday vol-time decay shape & holiday calendar: per-minute decay in 7.5h budget, US
  options holiday calendar, weekends = non-trading only.
- `iEMult`/`hEMult` definition: likely iEMove/hEMove used directly as per-event vol; verify
  by back-solving one ticker.
