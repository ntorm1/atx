"""Read an `atx-vol-chain-export` slice and put market IVs beside the fair vol.

The exporter writes fair value (`srPrc`), fair vol (`srVol`) and the nine greeks
per contract, but NOT `bidIV`/`askIV` -- the vendor schema has those columns and
we emit them as the `-99` sentinel, because a bid vol is an inversion of a quote
rather than a property of the fitted surface. This module does that inversion,
against the exact pricing inputs the exporter recorded on each row (`uPrc`,
`rate`, `sdiv`, `years`), so a market vol here is directly comparable to the
`srVol` sitting next to it rather than being a differently-conventioned number.

IMPORT ORDER IS THE POINT OF THIS MODULE LIVING IN THE PACKAGE
--------------------------------------------------------------
`atxvol._core` and `pyarrow` both link vcpkg's `arrow.dll`/`parquet.dll` by base
name, and on Windows whichever loads first claims the process-wide slot. The
package README says to use separate processes; that is stricter than the
measured behaviour. What actually holds is:

    import atxvol          then  import pyarrow   ->  BOTH WORK
    import pyarrow         then  import atxvol    ->  ImportError, _core dies

AND THE TRAP IS NOT SPELLED `pyarrow`. `import pandas` pulls pyarrow in
transitively (`pandas.compat.pyarrow` -> `pyarrow.lib`), so:

    import pandas          then  import atxvol    ->  ImportError, _core dies

which is the shape almost every caller actually writes, and the one that reads
as innocent. It is also why an import sorter must not touch the block below or
the header of anything importing this module: alphabetising `atxvol` under
`numpy`/`pandas` is a silent breakage, and it is how `tests/test_chain.py` was
first written and immediately failed to collect. The rule is therefore:

    `atxvol` FIRST -- above numpy, pandas and pyarrow alike.

Importing `atxvol.chain` necessarily initialises `atxvol` (hence `_core`) before
this module's own `import pandas` / `import pyarrow.parquet`, so any caller
whose FIRST atx import is this module gets the working order for free and cannot
accidentally assemble the broken one. A caller that imports pandas on its own
account must still put `import atxvol` above it.

DEEP-IN-THE-MONEY QUOTES DO NOT INVERT, AND THAT IS NOT A BUG
-------------------------------------------------------------
A deep-ITM American quote routinely sits BELOW intrinsic -- the bid especially,
because a market maker will not pay the exercise value for something the holder
can exercise immediately. Measured on KMX 2026-09-18 (S=62.69): 19 of 50 rows
carry a bid below `max(S-K, 0)`, and the 17.5 call is quoted 43.40/46.60 against
an intrinsic of 45.19. `american_implied_vol` correctly refuses these with
`OutOfRange: price below intrinsic`; there is no volatility that reproduces an
arbitrage. So inversion failures are RECORDED per row and per side, never
silently dropped and never coerced to a number.

For the same reason `otm_only()` exists and is what a vol curve should normally
be built from: puts below the forward, calls above it. The ITM wing of each side
is the same information as the OTM wing of the other, arrived at through a wider
spread and a larger early-exercise correction.
"""

from __future__ import annotations

# `atxvol` (and therefore `_core`) MUST be initialised before pyarrow -- see the
# module docstring. Importing the package explicitly rather than relying on the
# relative import below makes that ordering visible and greppable.
import atxvol  # noqa: F401  (imported for its DLL-load side effect, then used)

import numpy as np
import pandas as pd
import pyarrow.parquet as pq

from . import Side, american_implied_vol

__all__ = [
    "MISSING",
    "load_chain",
    "expiries",
    "slice_expiry",
    "add_market_ivs",
    "otm_only",
    "forward",
    "atm_vol",
    "normalized_strike",
    "within_z",
]

# The vendor's missing marker, as `chain_export` writes it (see
# `atx-vol/scripts/oracle_ingest.py` for the same convention on the ingest side).
# It is a real float in the file, so every read has to mask it out explicitly --
# a `-99` that reaches a plot as a vol is worse than a gap.
MISSING = -99.0

# Columns the inversion needs. Reading a subset matters: the full export is
# ~2M rows x 33 columns, and a slice request should not pay for all of it.
_CORE_COLUMNS = [
    "okey_tk", "okey_yr", "okey_mn", "okey_dy", "okey_xx", "okey_cp",
    "uPrc", "bidPrc", "askPrc", "bidSz", "askSz",
    "srPrc", "srVol", "rate", "sdiv", "years",
]


def load_chain(path, symbol=None, columns=None) -> pd.DataFrame:
    """Read a chain-export parquet, optionally for one underlier only.

    The file is written one row group per underlier with no symbol split across
    groups, so a `symbol` filter is answered by reading only that symbol's row
    groups -- the whole point of that layout. Falls back to a full read plus a
    mask if the footer statistics are missing.
    """
    cols = list(columns) if columns is not None else _CORE_COLUMNS
    pf = pq.ParquetFile(path)
    if symbol is None:
        return pf.read(columns=cols).to_pandas()

    wanted = []
    for i in range(pf.metadata.num_row_groups):
        stats = pf.metadata.row_group(i).column(0).statistics
        if stats is None:  # no statistics -> cannot prune, read everything
            wanted = None
            break
        if stats.min == symbol or stats.max == symbol:
            wanted.append(i)
    if wanted is None:
        frame = pf.read(columns=cols).to_pandas()
    elif not wanted:
        frame = pf.read(columns=cols).to_pandas().iloc[:0]
    else:
        frame = pf.read_row_groups(wanted, columns=cols).to_pandas()
    return frame[frame["okey_tk"] == symbol].reset_index(drop=True)


def _expiry_series(frame: pd.DataFrame) -> pd.Series:
    return pd.to_datetime(
        dict(year=frame["okey_yr"], month=frame["okey_mn"], day=frame["okey_dy"])
    )


def expiries(frame: pd.DataFrame) -> pd.DataFrame:
    """One row per listed expiry: contract count, year fraction, calendar days."""
    out = frame.assign(expiry=_expiry_series(frame))
    grp = out.groupby("expiry", sort=True)
    return pd.DataFrame({
        "n_contracts": grp.size(),
        "years": grp["years"].first(),
        "dte_days": (grp["years"].first() * 365.0).round(1),
    }).reset_index()


def slice_expiry(frame: pd.DataFrame, expiry) -> pd.DataFrame:
    """The one expiry's contracts, sorted by (side, strike).

    `expiry` accepts anything `pd.Timestamp` accepts, and also a bare
    `"YYYY-MM"`, which selects that month's single listed expiry and RAISES if
    the month carries more than one -- a silent pick between two expiries is how
    a curve ends up mixing tenors.
    """
    out = frame.assign(expiry=_expiry_series(frame))
    key = str(expiry)
    if len(key) == 7 and key[4] == "-":  # "YYYY-MM"
        year, month = int(key[:4]), int(key[5:])
        month_rows = out[(out["okey_yr"] == year) & (out["okey_mn"] == month)]
        found = sorted(month_rows["expiry"].unique())
        if not found:
            raise KeyError(f"no expiry listed in {key}")
        if len(found) > 1:
            listed = ", ".join(pd.Timestamp(d).date().isoformat() for d in found)
            raise KeyError(
                f"{key} lists {len(found)} expiries ({listed}); name one exactly "
                f"rather than letting this pick, or a curve silently mixes tenors")
        target = found[0]
    else:
        target = pd.Timestamp(expiry)
    sel = out[out["expiry"] == target]
    if sel.empty:
        raise KeyError(f"no contracts at expiry {target}")
    return sel.sort_values(["okey_cp", "okey_xx"], kind="stable").reset_index(drop=True)


def forward(slice_frame: pd.DataFrame) -> float:
    """The slice's forward under the exporter's own recorded inputs, F = S e^{(r-q)T}.

    Used to split OTM from ITM. Taken from the row values rather than recomputed
    from a curve so it matches whatever the fit actually priced against.
    """
    row = slice_frame.iloc[0]
    return float(row["uPrc"]) * float(np.exp((row["rate"] - row["sdiv"]) * row["years"]))


def atm_vol(slice_frame: pd.DataFrame, fwd: float | None = None) -> float:
    """The slice's reference vol: the fitted `srVol` at the strike nearest the forward.

    One number for the whole slice, deliberately. Using each row's own vol to
    normalise its own strike makes the mapping non-monotonic wherever the smile
    is steep, so two different strikes can land on the same normalised
    coordinate and the axis stops being an ordering.
    """
    f = forward(slice_frame) if fwd is None else fwd
    have = slice_frame[slice_frame["srVol"].notna() & (slice_frame["srVol"] != MISSING)]
    if have.empty:
        raise ValueError("no fitted vol in this slice; cannot form a reference vol")
    return float(have.loc[(have["okey_xx"] - f).abs().idxmin(), "srVol"])


def normalized_strike(slice_frame: pd.DataFrame, fwd: float | None = None,
                      vol: float | None = None) -> np.ndarray:
    """z = ln(K/F) / (sigma * sqrt(T)) — strike in standard deviations of the move.

    The natural window for looking at a smile: it is tenor-independent and
    comparable across names, so "within 2" means the same thing on a 28-day KMX
    slice as on a 2-year SPX one, whereas a fixed strike or moneyness window
    does not.
    """
    f = forward(slice_frame) if fwd is None else fwd
    s = atm_vol(slice_frame, f) if vol is None else vol
    T = float(slice_frame["years"].iloc[0])
    denom = s * np.sqrt(T)
    if not np.isfinite(denom) or denom <= 0.0:
        raise ValueError(f"degenerate sigma*sqrt(T) = {denom!r}; cannot normalise")
    return (np.log(slice_frame["okey_xx"].to_numpy(dtype=float) / f) / denom)


def within_z(slice_frame: pd.DataFrame, z_max: float, fwd: float | None = None,
             vol: float | None = None) -> pd.DataFrame:
    """Keep strikes with |z| <= `z_max`, z as `normalized_strike` defines it."""
    z = normalized_strike(slice_frame, fwd, vol)
    return slice_frame[np.abs(z) <= float(z_max)].reset_index(drop=True)


def _invert(price, spot, strike, T, r, q, side) -> tuple[float, str]:
    """One inversion. Returns (vol, reason) with vol NaN whenever reason is set."""
    if not np.isfinite(price) or price <= 0.0:
        return float("nan"), "no_quote"
    try:
        return float(american_implied_vol(float(price), float(spot), float(strike),
                                          float(T), float(r), float(q), side)), ""
    except Exception as exc:  # atxvol raises AtxError; keep the vendor's own words
        text = str(exc)
        if "below intrinsic" in text:
            return float("nan"), "below_intrinsic"
        if "above" in text and "bound" in text:
            return float("nan"), "above_bound"
        return float("nan"), text.split(":")[0].strip().lower() or "failed"


def add_market_ivs(slice_frame: pd.DataFrame) -> pd.DataFrame:
    """Add `bid_iv` / `ask_iv` / `mid_iv` and their per-row failure reasons.

    Inverted at the row's OWN recorded pricing inputs (`uPrc`, `rate`, `sdiv`,
    `years`) so the result is on the same footing as `srVol` beside it. A row
    that cannot be inverted gets NaN and a reason string, never a fabricated
    number -- see the module docstring on deep-ITM quotes.
    """
    out = slice_frame.copy()
    for col in ("srVol", "srPrc"):
        out[col] = out[col].mask(out[col] == MISSING)

    sides = np.where(out["okey_cp"].str.startswith("C"), Side.CALL, Side.PUT)
    results = {"bid": [], "ask": [], "mid": []}
    reasons = {"bid": [], "ask": [], "mid": []}
    for i, row in enumerate(out.itertuples(index=False)):
        prices = {"bid": row.bidPrc, "ask": row.askPrc,
                  "mid": (row.bidPrc + row.askPrc) / 2.0
                         if row.bidPrc > 0 and row.askPrc > 0 else float("nan")}
        for tag, px in prices.items():
            vol, why = _invert(px, row.uPrc, row.okey_xx, row.years,
                               row.rate, row.sdiv, sides[i])
            results[tag].append(vol)
            reasons[tag].append(why)
    for tag in ("bid", "ask", "mid"):
        out[f"{tag}_iv"] = results[tag]
        out[f"{tag}_iv_reason"] = reasons[tag]
    return out


def otm_only(slice_frame: pd.DataFrame, fwd: float | None = None) -> pd.DataFrame:
    """Keep the out-of-the-money leg of each strike: puts below F, calls above.

    The ITM wing carries the same information through a wider spread and a
    larger early-exercise correction, so a curve built from both legs plots two
    noisy copies of one smile.
    """
    f = forward(slice_frame) if fwd is None else fwd
    is_call = slice_frame["okey_cp"].str.startswith("C")
    keep = (is_call & (slice_frame["okey_xx"] >= f)) | (~is_call & (slice_frame["okey_xx"] < f))
    return slice_frame[keep].reset_index(drop=True)
