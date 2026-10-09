#!/usr/bin/env python3
"""Recover a DISCRETE CASH DIVIDEND SCHEDULE from the SpiderRock oracle store.

WHY THIS EXISTS. `atx-vol-chain-export` can now be handed a discrete schedule
(`--dividends <parquet>`) instead of folding every dividend into the single
continuous borrow its carry solve backs out. A continuous borrow can reproduce
the vendor's forward exactly and still misprice an AMERICAN board, because the
early-exercise boundary depends on WHEN each cash dividend lands, not only on
the forward the dividends integrate to. This tool writes that file.

THE ALGORITHM IS NOT INVENTED HERE. It mirrors, rule for rule,
`atx-vol/tools/oracle_dividends.hpp` / `.cpp` (`reconstruct_dividends`), which
is the validated C++ implementation:

  * `ddiv` is the SUM of the cash dividends whose ex-date falls at or before
    THAT option's expiry, so across one underlier's chain it is a step FUNCTION
    of `years`. Differencing consecutive expiries recovers the amounts.
  * Each recovered dividend is placed at the UPPER BRACKET expiry, never at the
    bracket midpoint. That is a MEASUREMENT, not a preference: the header
    records 6.96 ticks MAE on 14,357 SPY rows for the upper bracket versus
    22.28 for the midpoint.
  * TWO tolerances, because "did `ddiv` change?" and "is that change a
    dividend?" are different questions. A |change| <= DDIV_FLAT_TOL is the
    float column's rounding noise and must not refuse a clean underlier; a
    change above that but below MIN_DIVIDEND_JUMP is too large to be noise and
    far too small to be cash, and is refused rather than emitted.
  * FAIL CLOSED PER UNDERLIER, and COUNT the refusals by reason. A group that
    violates the step shape is refused WHOLE and listed by name; nothing is
    partially reconstructed. This exists so a caller can tell "reconstruction
    failed for this name" from "this name pays no dividends" -- the two produce
    the same empty schedule and mean opposite things. Both are reported here,
    separately and by name.

WHAT `ex_date` ACTUALLY IS -- READ THIS BEFORE TRUSTING THE COLUMN. It is NOT
the issuer's declared ex-dividend date, and this tool has no source for that.
It is the EXPIRY DATE OF THE FIRST LISTED EXPIRY WHOSE `ddiv` INCLUDED THE
DIVIDEND: an upper bracket, resolved only to the granularity of that
underlier's expiry ladder. Where the ladder is weekly the bracket is tight;
where it is quarterly or annual the true ex-date can be months earlier. The
column is spelled as a date because that is what the consumer needs, and
because placing the dividend AT that bracket is what priced best -- not because
a real ex-date was recovered.

The one thing the bracket IS exact about is the vendor's own step function: an
ex-date equal to an expiry's calendar day is midnight UTC, which falls before
that expiry's 16:00-ET settlement instant and after the previous expiry's, so
`forward_div_corrected` accrues exactly the dividends `ddiv` said it should, at
every expiry on the ladder.

MERGED JUMPS ARE NOT SPLIT HERE. When no listed expiry separates two ex-dates
the recovered amount is their SUM at the later bracket -- SPY's far-dated ladder
does this, and GS's 2026-06 -> 2028-01 gap recovers a single 10.00 that is two
quarters. Splitting needs the issuer's CADENCE, which the `ddiv` column does not
contain; `split_merged_dividends` in oracle_dividends.hpp is the validated
opt-in that does it, and it is deliberately not run here because its output taus
are a cadence INFERENCE rather than anything the expiry ladder contained, which
is precisely what this file's `ex_date` column promises not to be.

OUTPUT SCHEMA (what `--dividends` consumes):

    underlying : string  the `undSecKey_tk` this schedule belongs to
    ex_date    : string  "YYYY-MM-DD" upper-bracket expiry (see above)
    amount     : double  cash per share, strictly positive

ONE SNAPSHOT PER FILE. The output carries no date column, and `ddiv` is a
per-snapshot property of one underlier, so exactly one `date=<d>/bucket_et=<b>`
partition is read per run -- the same "one session, explicitly" discipline
`atx-vol-chain-export` itself keeps.

usage:
  spiderrock_dividends.py --partition <store>/date=<d>/bucket_et=<b>
                          [--symbols GS,LLY,CAT] [--out divs.parquet]
                          [--fail-on-refusal]

exit: 0 ok | 1 runtime failure or a refusal under --fail-on-refusal
      2 usage | 3 ran but recovered no dividend at all
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import pathlib
import re
import sys
from collections import Counter, defaultdict

import pyarrow as pa
import pyarrow.parquet as pq

# ── The two tolerances (mirrors kDdivFlatTol / kMinDividendJump) ─────────────
#
# A |change| at or below DDIV_FLAT_TOL is FLAT -- the rounding noise a float
# column carries, three orders of magnitude above the ~1e-15 telescoping
# residual measured across the SPY population. A change above that but below
# MIN_DIVIDEND_JUMP is neither noise nor cash, and is refused.
DDIV_FLAT_TOL = 1.0e-12
MIN_DIVIDEND_JUMP = 1.0e-9

# The store columns this tool decodes. `okey_yr`/`okey_mn`/`okey_dy` are the
# option's own expiry date and are what let this emitter name a DATE where the
# C++ reconstructor names only a `years`.
COLUMNS = ["undSecKey_tk", "okey_yr", "okey_mn", "okey_dy", "years", "ddiv"]

# ── Refusal reasons ─────────────────────────────────────────────────────────
#
# The first four are the C++ `DividendRefusal` enumerators, spelled identically
# and tested in the same order (the FIRST violation a group hits is the one
# recorded, and the group is abandoned there).
NON_FINITE_INPUT = "NonFiniteInput"
AMBIGUOUS_DDIV_AT_EXPIRY = "AmbiguousDdivAtExpiry"
NON_MONOTONE_DDIV = "NonMonotoneDdiv"
NON_POSITIVE_JUMP = "NonPositiveJump"

# The last two are EXTENSIONS this emitter needs and the C++ does not, because
# the C++ schedule is keyed by `years` while this file is keyed by a DATE. They
# are listed separately so nobody reads them as part of the mirrored contract.
AMBIGUOUS_EXPIRY_DATE = "AmbiguousExpiryDate"  # one `years`, two expiry dates
DUPLICATE_EX_DATE = "DuplicateExDate"  # two dividends onto one calendar day

REFUSAL_ORDER = [
    NON_FINITE_INPUT,
    AMBIGUOUS_DDIV_AT_EXPIRY,
    NON_MONOTONE_DDIV,
    NON_POSITIVE_JUMP,
    AMBIGUOUS_EXPIRY_DATE,
    DUPLICATE_EX_DATE,
]


class Refused(Exception):
    """One group's first shape violation. Carries the reason and where it hit."""

    def __init__(self, reason: str, years: float) -> None:
        super().__init__(f"{reason} at years={years!r}")
        self.reason = reason
        self.years = years


def collapse_expiries(rows: list[tuple[float, float, str]]) -> list[tuple[float, float, str]]:
    """Collapse one group's rows to one point per distinct `years`, ascending.

    Mirrors `collapse_expiries`: finiteness is screened FIRST (a NaN `years`
    would make the sort's strict-weak-ordering contract unsatisfiable, not
    merely produce a bad answer), then equal-`years` rows must agree on `ddiv`
    to within DDIV_FLAT_TOL -- `ddiv` is a property of the EXPIRY, so two
    answers at one expiry is not a step function and nothing differenced out of
    it would mean anything.

    Raises Refused. Returns [(years, ddiv, expiry_date), ...].
    """
    for years, ddiv, _ in rows:
        if not math.isfinite(years) or not math.isfinite(ddiv):
            raise Refused(NON_FINITE_INPUT, years)

    points: list[tuple[float, float, str]] = []
    for years, ddiv, expiry in sorted(rows, key=lambda r: r[0]):
        if points and points[-1][0] == years:
            if abs(points[-1][1] - ddiv) > DDIV_FLAT_TOL:
                raise Refused(AMBIGUOUS_DDIV_AT_EXPIRY, years)
            if points[-1][2] != expiry:
                # Beyond the C++ rules: two calendar expiries priced at one
                # `years` cannot both be named as the bracket, and choosing
                # either would be a guess.
                raise Refused(AMBIGUOUS_EXPIRY_DATE, years)
            continue
        points.append((years, ddiv, expiry))
    return points


def difference_schedule(points: list[tuple[float, float, str]]) -> list[tuple[str, float]]:
    """Difference a collapsed expiry ladder into [(ex_date, amount), ...].

    Mirrors `difference_schedule`. The baseline is 0, so a group whose EARLIEST
    expiry already carries `ddiv > 0` emits that dividend at that expiry: the
    dividend is real and its upper bracket is the first expiry, exactly as for
    any later one. A group whose earliest `ddiv` is negative is refused as
    NonMonotoneDdiv against that same baseline.

    Raises Refused.
    """
    schedule: list[tuple[str, float]] = []
    previous = 0.0
    for years, ddiv, expiry in points:
        jump = ddiv - previous
        if jump < -DDIV_FLAT_TOL:
            raise Refused(NON_MONOTONE_DDIV, years)
        if jump > DDIV_FLAT_TOL:
            if jump < MIN_DIVIDEND_JUMP:
                raise Refused(NON_POSITIVE_JUMP, years)
            schedule.append((expiry, jump))
        # Carried forward even when the change was folded into "flat", so the
        # emitted amounts keep telescoping against the ORIGINAL column values --
        # that is what holds the accrual invariant at ~1e-15 instead of drifting.
        previous = ddiv

    seen: set[str] = set()
    for expiry, _ in schedule:
        if expiry in seen:
            # Beyond the C++ rules, and load-bearing: `--dividends` refuses two
            # dividends on one ex-date, so emitting them would produce a file
            # chain-export rejects whole.
            raise Refused(DUPLICATE_EX_DATE, 0.0)
        seen.add(expiry)
    return schedule


def reconstruct(rows: list[tuple[float, float, str]]) -> tuple[list[tuple[str, float]], int]:
    """One group's schedule plus its distinct-expiry count. Raises Refused."""
    points = collapse_expiries(rows)
    return difference_schedule(points), len(points)


def accrued(schedule: list[tuple[str, float]], ex_date: str) -> float:
    """The invariant reconstruction is FOR: the sum of the scheduled dividends
    with ex-date at or before `ex_date` is what a row at that expiry reports as
    `ddiv` (mirrors `accrued_dividend`, in date space)."""
    return sum(amount for when, amount in schedule if when <= ex_date)


# ── The store ───────────────────────────────────────────────────────────────

_PARTITION_RE = re.compile(r"^date=(\d{4}-\d{2}-\d{2})$")
_BUCKET_RE = re.compile(r"^bucket_et=(\d{4})$")


def parse_partition(path: pathlib.Path) -> tuple[str, str] | None:
    """Split `<store>/date=<d>/bucket_et=<b>` into (date, bucket_et), or None.

    The partition strings are taken VERBATIM from the directory that is actually
    opened and are never re-derived from row contents, mirroring the C++ cohort
    reader -- a grouping key built from them cannot drift from the data.
    """
    bucket = _BUCKET_RE.match(path.name)
    date = _PARTITION_RE.match(path.parent.name) if bucket else None
    if not bucket or not date:
        return None
    return date.group(1), bucket.group(1)


def read_partition(
    path: pathlib.Path, keep: set[str] | None
) -> tuple[dict[str, list[tuple[float, float, str]]], dict[str, int]]:
    """Read every *.parquet in one partition into per-underlier row lists.

    Each file is opened with `ParquetFile.read`, NOT `pq.read_table`: the store
    path carries `date=`/`bucket_et=` hive components AND the file carries its
    own `date` column, and the dataset reader refuses that collision outright
    ("Field date has incompatible types"). Reading the file directly sidesteps
    a partitioning inference this tool does not want in the first place.

    A row is DROPPED (and counted) when a column it needs is null, non-finite,
    or -- for the expiry triple -- not a real calendar date. That mirrors the
    cohort reader's `rows_bad_input` screen, which is why the NonFiniteInput
    guard below can never fire on this path and is still kept: it is what stops
    a future caller from differencing NaNs into a schedule.
    """
    files = sorted(path.glob("*.parquet"))
    if not files:
        # A partition with no files would otherwise reconstruct zero underliers
        # and read as "nobody pays dividends", which is the one answer this tool
        # must never give by accident.
        raise FileNotFoundError(f"no *.parquet under '{path}'")

    groups: dict[str, list[tuple[float, float, str]]] = defaultdict(list)
    tally = {"files": len(files), "rows_read": 0, "rows_kept": 0, "rows_bad_input": 0}
    for file in files:
        table = pq.ParquetFile(file).read(columns=COLUMNS)
        tally["rows_read"] += table.num_rows
        tickers = table["undSecKey_tk"].to_pylist()
        years_col = table["years"].to_pylist()
        ddiv_col = table["ddiv"].to_pylist()
        yy = table["okey_yr"].to_pylist()
        mm = table["okey_mn"].to_pylist()
        dd = table["okey_dy"].to_pylist()
        for i, ticker in enumerate(tickers):
            if not ticker or (keep is not None and ticker not in keep):
                continue
            years, ddiv = years_col[i], ddiv_col[i]
            if years is None or ddiv is None or not math.isfinite(years) or not math.isfinite(ddiv):
                tally["rows_bad_input"] += 1
                continue
            try:
                expiry = dt.date(int(yy[i]), int(mm[i]), int(dd[i])).isoformat()
            except (TypeError, ValueError):
                tally["rows_bad_input"] += 1
                continue
            groups[ticker].append((float(years), float(ddiv), expiry))
            tally["rows_kept"] += 1
    return groups, tally


# ── The CLI ─────────────────────────────────────────────────────────────────


def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        prog="spiderrock_dividends.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--partition",
        required=True,
        type=pathlib.Path,
        help="one <store>/date=YYYY-MM-DD/bucket_et=HHMM directory",
    )
    ap.add_argument(
        "--symbols",
        default="",
        help="comma-joined undSecKey_tk filter; empty = every underlier present",
    )
    ap.add_argument(
        "--out",
        type=pathlib.Path,
        default=None,
        help="output Parquet path (parent dirs created); omitted = report only",
    )
    ap.add_argument(
        "--fail-on-refusal",
        action="store_true",
        help="exit 1 if any underlier was refused (default: report and continue)",
    )
    return ap.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if not args.partition.is_dir():
        print(f"error: --partition '{args.partition}' is not a directory", file=sys.stderr)
        return 2
    partition = parse_partition(args.partition)
    if partition is None:
        print(
            f"error: --partition must be a <store>/date=YYYY-MM-DD/bucket_et=HHMM "
            f"directory, got '{args.partition}'",
            file=sys.stderr,
        )
        return 2
    date, bucket = partition
    requested = [s.strip().upper() for s in args.symbols.split(",") if s.strip()]
    keep = set(requested) if requested else None

    try:
        groups, tally = read_partition(args.partition, keep)
    except (OSError, pa.ArrowInvalid) as why:
        print(f"error: --partition '{args.partition}': {why}", file=sys.stderr)
        return 1

    schedules: dict[str, list[tuple[str, float]]] = {}
    no_dividends: list[str] = []
    refused: list[tuple[str, str, float]] = []
    counts: Counter[str] = Counter()
    expiries_seen: dict[str, int] = {}
    for underlier in sorted(groups):
        try:
            schedule, n_expiries = reconstruct(groups[underlier])
        except Refused as why:
            refused.append((underlier, why.reason, why.years))
            counts[why.reason] += 1
            continue
        expiries_seen[underlier] = n_expiries
        if schedule:
            schedules[underlier] = schedule
        else:
            no_dividends.append(underlier)

    # ── The report ──────────────────────────────────────────────────────────
    out = sys.stderr
    # ASCII only, deliberately: this prints to a Windows console whose default
    # code page cannot encode box-drawing glyphs, and a report that raises
    # UnicodeEncodeError is a report nobody reads.
    print("\n-- spiderrock dividends " + "-" * 31, file=out)
    print(f"partition        date={date} bucket_et={bucket}  ({tally['files']} file(s))", file=out)
    print(
        f"rows             {tally['rows_read']:,d} read, {tally['rows_kept']:,d} kept, "
        f"{tally['rows_bad_input']:,d} dropped (null/non-finite ddiv|years|expiry)",
        file=out,
    )
    absent = sorted(set(requested) - set(groups)) if requested else []
    print(
        f"underliers       {len(groups)} reconstructed"
        + (f", {len(requested)} requested, {len(absent)} absent from the store" if requested else ""),
        file=out,
    )
    if absent:
        print(f"  absent         {' '.join(absent)}", file=out)

    n_events = 0
    for underlier in sorted(schedules):
        schedule = schedules[underlier]
        n_events += len(schedule)
        body = "  ".join(f"{when} {amount:.4f}" for when, amount in schedule)
        print(
            f"  {underlier:<8} {len(schedule)} div over {expiries_seen[underlier]:>3} expiries"
            f"   {body}",
            file=out,
        )
    # Stated by NAME, not merely counted: "pays no dividends" is a real answer
    # and must never be confused with the refusals below it.
    if no_dividends:
        print(
            f"  pays none      {' '.join(no_dividends)}   (ddiv flat across the ladder -- "
            f"an ANSWER, not a refusal)",
            file=out,
        )
    if refused:
        print(f"refused          {len(refused)} underlier(s) -- NO partial schedule emitted", file=out)
        for underlier, reason, years in refused:
            print(f"  {underlier:<8} {reason} at years={years:.6f}", file=out)
    print(
        "refusal tally    "
        + "  ".join(f"{reason}={counts.get(reason, 0)}" for reason in REFUSAL_ORDER),
        file=out,
    )
    print(f"dividends        {n_events} across {len(schedules)} underlier(s)", file=out)
    print("-" * 56, file=out)

    if args.out is not None:
        rows = sorted(
            (underlier, when, amount)
            for underlier, schedule in schedules.items()
            for when, amount in schedule
        )
        table = pa.table(
            {
                "underlying": pa.array([r[0] for r in rows], pa.string()),
                "ex_date": pa.array([r[1] for r in rows], pa.string()),
                "amount": pa.array([r[2] for r in rows], pa.float64()),
            }
        )
        args.out.parent.mkdir(parents=True, exist_ok=True)
        pq.write_table(table, args.out)
        print(f"wrote {len(rows)} rows -> {args.out}", file=out)

    if args.fail_on_refusal and refused:
        return 1
    if n_events == 0:
        # Ran, and recovered nothing. Distinguished from success because an
        # empty schedule file is exactly what omitting --dividends already does.
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
