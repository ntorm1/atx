"""Independent exact oracle for iteration-14 cross-sectional forecast evaluation.

Scope
-----
This script is the *independent* side of checkpoint-14 T7a. It re-derives, from
the frozen design note
``atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md`` alone
(plus the two headers that design explicitly permits reading -- ``random.hpp``
for the ``Xoshiro256pp`` definition, cited at ``random.hpp:83``, and
``decimal.hpp:120-133`` for the widening 64x64->128 multiply), the expected
values of the six oracle scenario families F1..F6 of design section 9.2.

Independence discipline (design sections 8/T7a and 9.2):
  * stdlib only -- ``fractions``, ``decimal``, ``hashlib``, ``json``, ``math``;
  * ``atx-engine/src/eval/cross_section_ic.cpp`` and
    ``atx-impl/src/stage_equity_ic.cpp`` were NOT read (they do not exist yet);
  * every expected value below is computed here in exact ``Fraction`` arithmetic
    (or ``Decimal`` at 50 digits where the design's own formula takes a square
    root), never copied from a native run;
  * all text I/O passes ``encoding="utf-8"`` explicitly;
  * the output file is opened with mode ``"x"`` -- an existing output is a hard
    refusal, never an overwrite.

Bit-exact arithmetic
--------------------
Family F6 is the one place where floating point is irrelevant and *bit-exact
uint64* arithmetic is mandatory: the bootstrap stream key, the splitmix64
seeding, the ``Xoshiro256pp`` output transform and the Lemire bounded draw are
all reproduced here in Python integers masked to 64 bits.

Qualification
-------------
This is a synthetic-fixture arithmetic oracle. It admits no real market data, it
does not execute or import any native code, it measures no strategy performance
and it makes no claim about survivorship, terminal-event classification or
economic correctness. The three evidenced terminal legs of family F3 use the
*amounts* recorded by the required-marks audit on *synthetic* session dates; the
audit's own limit ("neither absence nor the following row authorizes terminal
settlement") is binding and is not weakened here.
"""

from __future__ import annotations

import hashlib
import json
import math
import sys
from decimal import Decimal, localcontext
from fractions import Fraction as Q
from pathlib import Path

# --------------------------------------------------------------------------- #
# Paths and schema
# --------------------------------------------------------------------------- #

SCHEMA = "atx-iteration14-cross-section-oracle-v1"
MEASUREMENT_SCHEMA = "atx-cross-section-ic-measurement-v1"

SELF = Path(__file__).resolve()
ROOT = SELF.parents[2]
OUTPUT = ROOT / "build-equity" / "audits" / "iteration14-cross-section-oracle.json"
DESIGN = ROOT / "atx-engine" / "reviews" / "2026-09-20-iteration14-cross-section-ic-design.md"
RANDOM_HPP = ROOT / "atx-core" / "include" / "atx" / "core" / "random.hpp"
DECIMAL_HPP = ROOT / "atx-core" / "include" / "atx" / "core" / "decimal.hpp"
CROSS_SECTION_HPP = ROOT / "atx-core" / "include" / "atx" / "core" / "stats" / "cross_section.hpp"

DECIMAL_PRECISION = 50  # design section 9.2 F1: "one Decimal.sqrt at 50 digits"

# --------------------------------------------------------------------------- #
# Frozen parameters (design sections 3.10, 3.12, 4.3)
# --------------------------------------------------------------------------- #

K_BOOTSTRAP_SEED = 20260920
BLOCK_LEN_FLOOR = 5
BOOTSTRAP_DRAWS = 2000
MIN_NAMES_PER_DATE = 2
QUANTILE_COUNT = 10
TRADE_BPS = Q(5)              # constexpr EquityAllocationConfig{}.trade_bps
ANNUAL_BORROW_BPS = Q(365)    # deployed equity-book run configuration literal
SHORT_LEG_GROSS = Q(1)        # ruling AR-11: the short leg of the gross-2.0 book
BPS_DENOM = Q(10000)
DAYS_PER_YEAR = Q(365)
NS_PER_DAY = 86_400_000_000_000

HORIZONS = (1, 5, 10, 21, 63)
SIGNALS = ("momentum_252", "momentum_126", "blend_equal")
VARIANTS = ("DropMissingForward", "IncludeAuditedTerminalV1")
RESTRICTIONS = ("full", "ex34")
SAMPLES = ("full", "common")

BOOTSTRAP_STATISTIC_IDS = {
    "IcMean": 0,
    "Icir": 1,
    "RankIcMean": 2,
    "RankIcir": 3,
    "SpreadGross": 4,
    "SpreadNet": 5,
}

DBL_EPSILON = 2.0 ** -52
ULP_MULTIPLIER = 64

M64 = (1 << 64) - 1

QUALIFICATION = (
    "Exact synthetic-fixture arithmetic oracle for cross-sectional forecast "
    "evaluation. No native code was read, imported or executed; no real panel, "
    "price archive or corporate action was admitted; no strategy performance is "
    "measured or claimed."
)


class OracleError(RuntimeError):
    """Raised when a hand-derived checkpoint disagrees with the computed value."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise OracleError(message)


# --------------------------------------------------------------------------- #
# Number packaging
# --------------------------------------------------------------------------- #

def to_decimal(value: Q) -> Decimal:
    return Decimal(value.numerator) / Decimal(value.denominator)


def rat(value: Q) -> dict:
    """An exactly representable rational expectation."""
    return {
        "kind": "exact_rational",
        "exact": f"{value.numerator}/{value.denominator}",
        "binary64": float(value),
    }


def irr(value: Decimal) -> dict:
    """An irrational expectation carried as a 50-digit Decimal."""
    return {
        "kind": "decimal_50",
        "decimal": str(value),
        "binary64": float(value),
    }


def whole(value: int) -> dict:
    """An exact-integer expectation; no tolerance is admitted."""
    return {"kind": "exact_integer", "integer": int(value)}


def u64(value: int) -> dict:
    """A 64-bit unsigned expectation; carried as a decimal STRING as well as an
    integer because values above 2**53 do not survive a JSON float reader."""
    require(0 <= value <= M64, f"u64 out of range: {value}")
    return {"kind": "exact_u64", "integer": int(value), "decimal_string": str(value)}


def unrat(packed: dict) -> Q:
    """Recover the exact Fraction from a rat()-packed expectation."""
    require(packed.get("kind") == "exact_rational", "unrat: not an exact rational")
    num, den = packed["exact"].split("/")
    return Q(int(num), int(den))


def nonfinite(tag: str) -> dict:
    return {"kind": "nonfinite", "value": tag}


# --------------------------------------------------------------------------- #
# Section 3.4 -- Pearson, exact
# --------------------------------------------------------------------------- #

def pearson_exact(xs, rs) -> dict:
    """Design section 3.4, verbatim.

        n = |U|; if n < 2 -> 0.0
        ma = mean(x), mb = mean(r)          (means accumulated first, then divided)
        cov = sum (x-ma)(r-mb); va = sum (x-ma)^2; vb = sum (r-mb)^2
        IC  = (va == 0 || vb == 0) ? 0.0 : cov / sqrt(va*vb)

    Exact rationals are used for cov/va/vb; the single square root is taken in
    Decimal at DECIMAL_PRECISION digits. Accumulation order is irrelevant under
    exact arithmetic, so the design's fixed ascending-index order is reproduced
    without being a source of divergence here.
    """
    require(len(xs) == len(rs), "pearson: mismatched vector lengths")
    n = len(xs)
    if n < 2:
        return {
            "n": n,
            "cov": Q(0),
            "va": Q(0),
            "vb": Q(0),
            "leg_scale": Decimal(0),
            "ic": Decimal(0),
            "ic_is_exact_rational": True,
            "branch": "n_lt_2_early_return",
        }
    ma = sum(xs, Q(0)) / n
    mb = sum(rs, Q(0)) / n
    cov = sum(((x - ma) * (r - mb) for x, r in zip(xs, rs)), Q(0))
    va = sum(((x - ma) * (x - ma) for x in xs), Q(0))
    vb = sum(((r - mb) * (r - mb) for r in rs), Q(0))
    if va == 0 or vb == 0:
        return {
            "n": n,
            "cov": cov,
            "va": va,
            "vb": vb,
            "leg_scale": Decimal(0),
            "ic": Decimal(0),
            "ic_is_exact_rational": True,
            "branch": "constant_series_returns_zero",
        }
    with localcontext() as ctx:
        ctx.prec = DECIMAL_PRECISION
        leg = (to_decimal(va) * to_decimal(vb)).sqrt()
        ic = to_decimal(cov) / leg
    return {
        "n": n,
        "cov": cov,
        "va": va,
        "vb": vb,
        "leg_scale": leg,
        "ic": ic,
        "ic_is_exact_rational": False,
        "branch": "normal",
    }


# --------------------------------------------------------------------------- #
# Section 3.5 -- averaged ranks, exact
# --------------------------------------------------------------------------- #

def rank_exact(values):
    """core::stats::rank semantics (cross_section.hpp:102-136), exactly.

    Ascending sort with (value, then original index) as the total order; each run
    of *bit-equal* values takes the mean of the positions it occupies, normalized
    by n-1. n == 1 yields 0.0.

    Fixture values are chosen dyadic (or built from math.nextafter) so exact
    rational equality here coincides with bitwise f64 equality natively.
    """
    n = len(values)
    if n == 0:
        return []
    if n == 1:
        return [Q(0)]
    order = sorted(range(n), key=lambda i: (values[i], i))
    out = [Q(0)] * n
    denom = Q(n - 1)
    lo = 0
    while lo < n:
        hi = lo + 1
        while hi < n and values[order[hi]] == values[order[lo]]:
            hi += 1
        avg_pos = (Q(lo) + Q(hi - 1)) / 2
        norm = avg_pos / denom
        for p in range(lo, hi):
            out[order[p]] = norm
        lo = hi
    return out


# --------------------------------------------------------------------------- #
# Section 3.9 -- series statistics, at DECIMAL_PRECISION digits
# --------------------------------------------------------------------------- #

def series_stats(values) -> dict:
    """ic_mean / ic_sd / icir / naive_t of design section 3.9, with the explicit
    n == 0 and n == 1 branches (I-11): never NaN."""
    n = len(values)
    if n == 0:
        return {
            "n": 0,
            "summary_reportable": 0,
            "mean": Decimal(0),
            "sd": Decimal(0),
            "icir": Decimal(0),
            "naive_t": Decimal(0),
            "branch": "n_zero",
        }
    if n == 1:
        return {
            "n": 1,
            "summary_reportable": 0,
            "mean": Decimal(values[0]),
            "sd": Decimal(0),
            "icir": Decimal(0),
            "naive_t": Decimal(0),
            "branch": "n_one",
        }
    with localcontext() as ctx:
        ctx.prec = DECIMAL_PRECISION
        total = Decimal(0)
        for v in values:
            total += Decimal(v)
        mean = total / Decimal(n)
        ss = Decimal(0)
        for v in values:
            diff = Decimal(v) - mean
            ss += diff * diff
        sd = (ss / Decimal(n - 1)).sqrt()
        icir = Decimal(0) if sd == 0 else mean / sd
        naive_t = icir * Decimal(n).sqrt()
    return {
        "n": n,
        "summary_reportable": 1,
        "mean": mean,
        "sd": sd,
        "icir": icir,
        "naive_t": naive_t,
        "branch": "normal",
    }


# --------------------------------------------------------------------------- #
# Section 3.10 -- bit-exact bootstrap RNG replication
# --------------------------------------------------------------------------- #

def rotl64(x: int, k: int) -> int:
    x &= M64
    return ((x << k) | (x >> (64 - k))) & M64


def splitmix64_next(state: int):
    """atx::core::detail::splitmix64_next (random.hpp).

    ADVANCES ``state`` first and RETURNS the mixed value of the advanced state.
    Returns ``(return_value, advanced_state)``; the design's seed derivation uses
    the RETURN VALUE and discards the mutated state.
    """
    state = (state + 0x9E3779B97F4A7C15) & M64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return (z ^ (z >> 31)) & M64, state


class Xoshiro256pp:
    """atx::core::Xoshiro256pp (random.hpp:83): four splitmix64 steps seed the
    256-bit state; output transform rotl(s0+s3, 23) + s0."""

    def __init__(self, seed: int) -> None:
        s = seed & M64
        state = []
        for _ in range(4):
            value, s = splitmix64_next(s)
            state.append(value)
        self.state = state

    def next_u64(self) -> int:
        s = self.state
        result = (rotl64((s[0] + s[3]) & M64, 23) + s[0]) & M64
        t = (s[1] << 17) & M64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = rotl64(s[3], 45)
        return result


def umul_64_to_128(a: int, b: int):
    """decimal.hpp:120-133 -- widening 64x64 -> 128 product, (hi, lo)."""
    product = (a & M64) * (b & M64)
    return (product >> 64) & M64, product & M64


def draw_below(rng: Xoshiro256pp, n: int):
    """Lemire nearly-divisionless bounded draw, design section 3.10, verbatim.

    One 64-bit output per attempt; threshold ``(0ULL - n) % n``; the HIGH half of
    the widening product is the result; the retry loop is statically bounded at
    64 tries, after which ``x % n`` is returned and a fallback is counted.
    """
    require(n >= 1, "draw_below: n must be >= 1")
    fallbacks = 0
    x = rng.next_u64()
    hi, lo = umul_64_to_128(x, n)
    if lo < n:
        thresh = ((1 << 64) - n) % n
        tries = 0
        while lo < thresh:
            if tries == 64:
                fallbacks = 1
                return x % n, fallbacks
            x = rng.next_u64()
            hi, lo = umul_64_to_128(x, n)
            tries += 1
    return hi, fallbacks


def stream_key(statistic_id: int, horizon_index: int, signal_index: int,
               variant_id: int, restriction_id: int, sample_id: int) -> int:
    """Design section 3.10 / ruling R-C: six disjoint byte-aligned fields."""
    for name, value in (("statistic_id", statistic_id), ("horizon_index", horizon_index),
                        ("signal_index", signal_index), ("variant_id", variant_id),
                        ("restriction_id", restriction_id), ("sample_id", sample_id)):
        require(0 <= value <= 255, f"stream key field {name} out of byte range: {value}")
    return (K_BOOTSTRAP_SEED
            ^ (statistic_id << 8)
            ^ (horizon_index << 16)
            ^ (signal_index << 24)
            ^ (variant_id << 32)
            ^ (restriction_id << 40)
            ^ (sample_id << 48)) & M64


def block_len(h: int) -> int:
    """L_h = max(block_len_floor, ceil(h/2)); ceil(h/2) is integer (h+1)/2."""
    return max(BLOCK_LEN_FLOOR, (h + 1) // 2)


def bootstrap_reportable(draws: int, n: int, h: int):
    """draws >= 1 && n >= 20 && floor(n / L_h) >= 10 (design sections 3.10, 4.3)."""
    length = block_len(h)
    if draws < 1:
        return 0, "bootstrap-draws-zero", length
    if n < 20:
        return 0, "series-shorter-than-twenty", length
    if n // length < 10:
        return 0, "series-too-short-for-block-length", length
    return 1, None, length


def first_draw_starts(key: int, n: int, count: int):
    """The first ``count`` draw start indices of one bootstrap stream."""
    seed_x, _discarded = splitmix64_next(key)
    rng = Xoshiro256pp(seed_x)
    starts = []
    fallbacks = 0
    for _ in range(count):
        start, fb = draw_below(rng, n)
        starts.append(start)
        fallbacks += fb
    return seed_x, starts, fallbacks


def resample_indices(starts, n: int, length: int):
    """Design section 3.10: block j contributes S[(s_j + k) mod n] for
    k = 0..L-1; blocks concatenated in draw order, truncated to exactly n."""
    out = []
    for start in starts:
        for k in range(length):
            out.append((start + k) % n)
    return out[:n]


# --------------------------------------------------------------------------- #
# Section 3.12 -- decile machinery, exact
# --------------------------------------------------------------------------- #

def quantile_buckets(signal_by_index, quantiles: int):
    """Step 2/3 of design section 3.12: order by (x descending, instrument index
    ascending), then q = floor(p * Q / n) with p the 0-based ordered position."""
    items = sorted(signal_by_index.items(), key=lambda kv: (-kv[1], kv[0]))
    n = len(items)
    buckets = {q: [] for q in range(quantiles)}
    positions = {}
    for p, (idx, _value) in enumerate(items):
        q = (p * quantiles) // n
        buckets[q].append(idx)
        positions[idx] = p
    return buckets, positions


def decile_weights(buckets, quantiles: int):
    """Ruling AR-11 gross-2.0 book: +1/n_top on decile 0, -1/n_bottom on the last
    decile, zero elsewhere. A complete turnover therefore reads oneway == 2.0."""
    top = buckets[0]
    bottom = buckets[quantiles - 1]
    weights = {}
    for idx in top:
        weights[idx] = Q(1, len(top))
    for idx in bottom:
        weights[idx] = weights.get(idx, Q(0)) - Q(1, len(bottom))
    return weights


def oneway_turnover(weights_now, weights_prev):
    """oneway(t) = 0.5 * sum_i | w_i(t) - w_i(t-h) |."""
    names = set(weights_now) | set(weights_prev)
    total = sum((abs(weights_now.get(i, Q(0)) - weights_prev.get(i, Q(0))) for i in names), Q(0))
    return total / 2, total


def trade_drag(turnover: Q) -> Q:
    """(trade_bps / 1e4) * 2 * turnover_h -- constant per horizon."""
    return (TRADE_BPS / BPS_DENOM) * 2 * turnover


def borrow_drag(days: int) -> Q:
    """(annual_borrow_bps / 1e4) * (days_h(t) / 365.0) * short_leg_gross."""
    return (ANNUAL_BORROW_BPS / BPS_DENOM) * (Q(days) / DAYS_PER_YEAR) * SHORT_LEG_GROSS


def session_key(year: int, month: int, day: int) -> int:
    """Midnight-UTC session key in nanoseconds since the Unix epoch."""
    import datetime

    delta = datetime.date(year, month, day) - datetime.date(1970, 1, 1)
    return delta.days * NS_PER_DAY


def days_forward(key_t: int, key_t_plus_h: int) -> int:
    """(session_keys[t+h] - session_keys[t]) / 86_400_000_000_000, INTEGER."""
    require(key_t_plus_h > key_t, "session keys must be strictly increasing")
    return (key_t_plus_h - key_t) // NS_PER_DAY


# --------------------------------------------------------------------------- #
# Fixtures
# --------------------------------------------------------------------------- #

NAN = "nan"

# --- F1/F2 panel: 6 dates x 8 names, every cell admitted and finite ---------- #

F1_SIGNALS = [
    [Q(8), Q(7), Q(6), Q(5), Q(4), Q(3), Q(2), Q(1)],
    [Q(1), Q(3), Q(5), Q(7), Q(2), Q(4), Q(6), Q(8)],
    [Q(2), Q(2), Q(5), Q(5), Q(9), Q(9), Q(1), Q(1)],   # ties in both directions
    [Q(4), Q(1), Q(7), Q(3), Q(8), Q(6), Q(2), Q(5)],
    [Q(5), Q(5), Q(5), Q(5), Q(5), Q(5), Q(5), Q(5)],   # constant -> IC exactly 0
    [Q(3), Q(1), Q(4), Q(1), Q(5), Q(9), Q(2), Q(6)],
]

F1_PRICES = [
    [Q(100), Q(50), Q(200), Q(25), Q(80), Q(40), Q(160), Q(10)],
    [Q(101), Q(51), Q(198), Q(26), Q(79), Q(41), Q(158), Q(11)],
    [Q(102), Q(49), Q(202), Q(24), Q(82), Q(39), Q(162), Q(9)],
    [Q(103), Q(52), Q(196), Q(27), Q(78), Q(42), Q(164), Q(12)],
    [Q(104), Q(48), Q(204), Q(23), Q(83), Q(38), Q(156), Q(8)],
    [Q(105), Q(53), Q(194), Q(28), Q(77), Q(43), Q(166), Q(13)],
]

F1_SESSION_KEYS = [
    session_key(2013, 4, 1),   # Mon
    session_key(2013, 4, 2),   # Tue
    session_key(2013, 4, 3),   # Wed
    session_key(2013, 4, 4),   # Thu
    session_key(2013, 4, 5),   # Fri
    session_key(2013, 4, 8),   # Mon  (Fri -> Mon is 3 calendar days)
]

# --- F3 terminal-leg evidence (design sections 2.3 / 3.8, rulings AR-1/AR-2) -- #
#
# Amounts are the required-marks audit's; the SESSION DATES of the DELL cases are
# synthetic, chosen only to place one observation at the record date and one
# after it (design section 9.1 cases 8a/8b). No real archive row is admitted.

DELL_RECORD_DATE = session_key(2013, 10, 28)

# --- F4 coverage panel: 3 dates x 8 names ----------------------------------- #

F4_IDS = [1001, 1002, 37648, 35715, 39970, 146189, 1003, 1004]
F4_EX34_IDS = [37648, 35715, 39970, 146189]  # the fixture's members of the 34

F4_MASK = [
    [1, 1, 1, 1, 1, 1, 0, 1],
    [1, 1, 1, 1, 1, 1, 1, 0],
    [1, 1, 1, 1, 1, 1, 1, 1],
]

F4_SIGNALS = [
    [Q(1), Q(2), Q(3), NAN, Q(4), Q(5), Q(6), Q(7)],
    [Q(1), Q(2), Q(3), Q(8), Q(4), Q(5), Q(6), Q(7)],
    [Q(1), Q(2), Q(3), Q(8), Q(4), Q(5), Q(6), Q(7)],
]

# ``None`` is an absent mark; a non-positive value is a mark that is NOT repaired.
F4_PRICES = [
    [Q(100), Q(50), Q(200), Q(25), Q(80), Q(40), Q(160), Q(10)],
    [Q(101), Q(51), Q(198), Q(26), None, None, Q(161), None],
    [Q(102), Q(-49), None, Q(24), None, None, Q(162), Q(8)],
]

F4_TERMINAL = [
    [0, 0, 1, 0, 1, 1, 0, 0],
    [0, 0, 0, 0, 1, 1, 0, 0],
    [0, 0, 0, 0, 0, 0, 0, 0],
]

F4_TERMINAL_EVIDENCED = [
    [0, 0, 1, 0, 1, 0, 0, 0],
    [0, 0, 0, 0, 1, 0, 0, 0],
    [0, 0, 0, 0, 0, 0, 0, 0],
]

# --- F5 net-spread fixture: 4 names, 3 dates, Q = 2, h = 1 ------------------ #

F5_SIGNALS = [
    [Q(4), Q(3), Q(2), Q(1)],
    [Q(1), Q(2), Q(3), Q(4)],   # complete decile turnover against date 0
    [Q(4), Q(3), Q(2), Q(1)],
]

F5_PRICES = [
    [Q(100), Q(50), Q(200), Q(25)],
    [Q(101), Q(51), Q(198), Q(27)],
    [Q(102), Q(52), Q(196), Q(28)],
]

F5_SESSION_KEYS = [
    session_key(2013, 4, 5),   # Fri
    session_key(2013, 4, 8),   # Mon -> 3 calendar days
    session_key(2013, 4, 9),   # Tue -> 1 calendar day
]


# --------------------------------------------------------------------------- #
# Family F1 -- IC family
# --------------------------------------------------------------------------- #

def forward_returns(prices, t: int, h: int, names):
    """r_i(t,h) = P_i(t+h)/P_i(t) - 1, defined iff t+h < T and both marks are
    finite and > 0 (design section 3.3)."""
    out = {}
    for i in names:
        p0 = prices[t][i]
        p1 = prices[t + h][i]
        if p0 is None or p1 is None or p0 <= 0 or p1 <= 0:
            continue
        out[i] = p1 / p0 - 1
    return out


def f1_cases():
    cases = []
    names = list(range(8))
    dates = len(F1_PRICES)

    pearson_series = {}
    rank_series = {}

    for h in (1, 5):
        pear_values = []
        rank_values = []
        for t in range(dates - h):
            rets = forward_returns(F1_PRICES, t, h, names)
            used = sorted(rets)  # ascending instrument index; every cell admitted
            xs = [F1_SIGNALS[t][i] for i in used]
            rs = [rets[i] for i in used]
            require(len(used) == 8, "F1 fixture must have full coverage")

            pear = pearson_exact(xs, rs)
            rx = rank_exact(xs)
            rr = rank_exact(rs)
            rank_p = pearson_exact(rx, rr)

            pear_values.append(pear["ic"])
            rank_values.append(rank_p["ic"])

            cases.append({
                "id": f"f1_ic_h{h}_t{t}",
                "family": "F1",
                "native_export": "expected",
                "description": (
                    f"Pearson and rank IC on the 6x8 fully-admitted panel at date {t}, "
                    f"horizon {h}; every cell eligible, finite and priced."
                ),
                "formula": "section 3.4 pearson(x, r); section 3.5 rank_IC = pearson(rank(x), rank(r))",
                "inputs": {
                    "date_index": whole(t),
                    "horizon": whole(h),
                    "n_used": whole(len(used)),
                    "instrument_indices": [whole(i) for i in used],
                    "signal": [rat(v) for v in xs],
                    "price_t": [rat(F1_PRICES[t][i]) for i in used],
                    "price_t_plus_h": [rat(F1_PRICES[t + h][i]) for i in used],
                    "forward_return": [rat(v) for v in rs],
                },
                "expected": {
                    "pearson_ic": (rat(Q(0)) if pear["ic_is_exact_rational"] else irr(pear["ic"])),
                    "rank_ic": (rat(Q(0)) if rank_p["ic_is_exact_rational"] else irr(rank_p["ic"])),
                    "cov": rat(pear["cov"]),
                    "va": rat(pear["va"]),
                    "vb": rat(pear["vb"]),
                    "signal_ranks": [rat(v) for v in rx],
                    "return_ranks": [rat(v) for v in rr],
                    "pearson_branch": pear["branch"],
                    "rank_branch": rank_p["branch"],
                },
                "bound_legs": {
                    "pearson_ic": float(pear["leg_scale"]),
                    "rank_ic": float(rank_p["leg_scale"]),
                },
            })

        pearson_series[h] = pear_values
        rank_series[h] = rank_values

        stats = series_stats(pear_values)
        rstats = series_stats(rank_values)
        cases.append({
            "id": f"f1_series_h{h}",
            "family": "F1",
            "native_export": "expected",
            "description": (
                f"ic_mean / ic_sd / icir / naive_t over the {len(pear_values)} emitted "
                f"dates at horizon {h} (design section 3.9)."
            ),
            "formula": "ic_mean = mean(IC_t); ic_sd = sqrt(sum (IC_t - ic_mean)^2 / (n-1)); "
                       "icir = ic_mean/ic_sd (0 when sd == 0); naive_t = icir * sqrt(n)",
            "inputs": {
                "horizon": whole(h),
                "n": whole(stats["n"]),
                "ic_series": [irr(v) for v in pear_values],
                "rank_ic_series": [irr(v) for v in rank_values],
            },
            "expected": {
                "summary_reportable": whole(stats["summary_reportable"]),
                "ic_mean": irr(stats["mean"]),
                "ic_sd": irr(stats["sd"]),
                "icir": irr(stats["icir"]),
                "naive_t": irr(stats["naive_t"]),
                "rank_ic_mean": irr(rstats["mean"]),
                "rank_ic_sd": irr(rstats["sd"]),
                "rank_icir": irr(rstats["icir"]),
                "rank_naive_t": irr(rstats["naive_t"]),
                "naive_t_validity": "invalid-under-overlapping-horizons",
            },
            "bound_legs": {
                "ic_mean": float(max((abs(v) for v in pear_values), default=Decimal(0))),
                "ic_sd": float(max((abs(v) for v in pear_values), default=Decimal(0))),
                "icir": float(abs(stats["icir"])),
                "naive_t": float(abs(stats["naive_t"])),
                "rank_ic_mean": float(max((abs(v) for v in rank_values), default=Decimal(0))),
                "rank_ic_sd": float(max((abs(v) for v in rank_values), default=Decimal(0))),
                "rank_icir": float(abs(rstats["icir"])),
                "rank_naive_t": float(abs(rstats["naive_t"])),
            },
        })

    # --- perfectly ordered cross-section: IC and rank IC are exactly 1 ------- #
    xs = [Q(1), Q(2), Q(3), Q(4)]
    rs = [2 * x + 1 for x in xs]
    pear = pearson_exact(xs, rs)
    rank_p = pearson_exact(rank_exact(xs), rank_exact(rs))
    require(pear["ic"] == Decimal(1), "F1 perfect-affine Pearson IC must be exactly 1")
    require(rank_p["ic"] == Decimal(1), "F1 perfect-affine rank IC must be exactly 1")
    cases.append({
        "id": "f1_pearson_perfect_affine_is_one",
        "family": "F1",
        "native_export": "expected",
        "description": "4 names, r a strictly increasing affine image of x (design section 9.1 case 1).",
        "formula": "section 3.4",
        "inputs": {"signal": [rat(v) for v in xs], "forward_return": [rat(v) for v in rs]},
        "expected": {
            "pearson_ic": irr(pear["ic"]),
            "rank_ic": irr(rank_p["ic"]),
            "cov": rat(pear["cov"]), "va": rat(pear["va"]), "vb": rat(pear["vb"]),
        },
        "bound_legs": {"pearson_ic": float(pear["leg_scale"]), "rank_ic": float(rank_p["leg_scale"])},
    })

    # --- constant signal cross-section: exactly 0.0, never NaN --------------- #
    xs = [Q(3), Q(3), Q(3), Q(3)]
    rs = [Q(1, 100), Q(2, 100), Q(-1, 100), Q(4, 100)]
    pear = pearson_exact(xs, rs)
    require(pear["branch"] == "constant_series_returns_zero", "F1 constant-signal branch")
    cases.append({
        "id": "f1_pearson_constant_signal_is_zero_not_nan",
        "family": "F1",
        "native_export": "expected",
        "description": "va == 0 branch of design section 3.4 (design section 9.1 case 3).",
        "formula": "IC = (va == 0.0 || vb == 0.0) ? 0.0 : cov / sqrt(va*vb)",
        "inputs": {"signal": [rat(v) for v in xs], "forward_return": [rat(v) for v in rs]},
        "expected": {"pearson_ic": rat(Q(0)), "va": rat(pear["va"]), "vb": rat(pear["vb"]),
                     "pearson_branch": pear["branch"]},
        "bound_legs": {"pearson_ic": 0.0},
    })

    # --- n < 2 early return -------------------------------------------------- #
    pear = pearson_exact([Q(7)], [Q(1, 10)])
    require(pear["branch"] == "n_lt_2_early_return", "F1 n<2 branch")
    cases.append({
        "id": "f1_pearson_fewer_than_two_names_is_zero_not_nan",
        "family": "F1",
        "native_export": "expected",
        "description": "the n < 2U early return (design section 9.1 case 3b).",
        "formula": "if (n < 2) return 0.0",
        "inputs": {"signal": [rat(Q(7))], "forward_return": [rat(Q(1, 10))],
                   "n": whole(1), "min_names_per_date": whole(MIN_NAMES_PER_DATE)},
        "expected": {"pearson_ic": rat(Q(0)), "pearson_branch": pear["branch"],
                     "date_emitted": whole(0), "dates_below_min_names_increment": whole(1)},
        "bound_legs": {"pearson_ic": 0.0},
    })

    # --- tie handling: averaged ranks ---------------------------------------- #
    tied = [Q(1), Q(1), Q(2), Q(2)]
    ranks = rank_exact(tied)
    require(ranks == [Q(1, 6), Q(1, 6), Q(5, 6), Q(5, 6)],
            f"F1 tie-averaged ranks disagree with the hand value: {ranks}")
    cases.append({
        "id": "f1_rank_tied_values_average_positions",
        "family": "F1",
        "native_export": "expected",
        "description": "x = {1,1,2,2} -> ranks {0.5/3, 0.5/3, 2.5/3, 2.5/3} exactly "
                       "(design section 9.1 case 4; cross_section.hpp:123-136).",
        "formula": "each run of bit-equal values takes mean(position)/ (n-1)",
        "inputs": {"signal": [rat(v) for v in tied]},
        "expected": {"ranks": [rat(v) for v in ranks]},
        "bound_legs": {"ranks": 1.0},
    })

    # --- one-ulp-apart values are two ranks, not a tie ----------------------- #
    base = 1.0
    up = math.nextafter(base, 2.0)
    ulp_values = [Q(base), Q(up), Q(2.0), Q(3.0)]
    require(ulp_values[0] != ulp_values[1], "nextafter must produce a distinct value")
    ulp_ranks = rank_exact(ulp_values)
    require(ulp_ranks == [Q(0), Q(1, 3), Q(2, 3), Q(1)],
            f"F1 one-ulp ranks disagree with the hand value: {ulp_ranks}")
    cases.append({
        "id": "f1_rank_one_ulp_apart_are_distinct_ranks",
        "family": "F1",
        "native_export": "expected",
        "description": "bitwise f64 equality, not a tolerance (design sections 3.5 / 9.1 case 5). "
                       "The second value is math.nextafter(1.0, 2.0).",
        "formula": "equality inside rank() is bitwise == on f64",
        "inputs": {
            "signal": [rat(v) for v in ulp_values],
            "signal_binary64_hex": [float(v).hex() for v in ulp_values],
        },
        "expected": {"ranks": [rat(v) for v in ulp_ranks]},
        "bound_legs": {"ranks": 1.0},
    })

    return cases, pearson_series, rank_series


# --------------------------------------------------------------------------- #
# Family F2 -- quantile spread
# --------------------------------------------------------------------------- #

def spread_case(case_id, description, signal_by_index, returns_by_index, quantiles,
                native_export="expected", extra_expected=None):
    n = len(signal_by_index)
    if n < quantiles:
        return {
            "id": case_id,
            "family": "F2",
            "native_export": native_export,
            "description": description,
            "formula": "design section 3.12 step 1: reject the date when n < Q",
            "inputs": {
                "quantile_count": whole(quantiles),
                "n_used": whole(n),
                "instrument_indices": [whole(i) for i in sorted(signal_by_index)],
                "signal": [rat(signal_by_index[i]) for i in sorted(signal_by_index)],
            },
            "expected": {
                "spread_emitted": whole(0),
                "dates_below_quantile_count_increment": whole(1),
            },
            "bound_legs": {},
        }
    buckets, positions = quantile_buckets(signal_by_index, quantiles)
    means = {}
    for q, members in buckets.items():
        require(members, f"{case_id}: bucket {q} is empty")
        means[q] = sum((returns_by_index[i] for i in members), Q(0)) / len(members)
    spread = means[0] - means[quantiles - 1]
    max_bucket_mean_abs = max(abs(v) for v in means.values())
    expected = {
        "spread": rat(spread),
        "mean_top_decile": rat(means[0]),
        "mean_bottom_decile": rat(means[quantiles - 1]),
        "bucket_means": [rat(means[q]) for q in range(quantiles)],
        "bucket_sizes": [whole(len(buckets[q])) for q in range(quantiles)],
        "bucket_members": [[whole(i) for i in buckets[q]] for q in range(quantiles)],
        "ordered_positions": [whole(positions[i]) for i in sorted(positions)],
        "spread_emitted": whole(1),
    }
    if extra_expected:
        expected.update(extra_expected)
    return {
        "id": case_id,
        "family": "F2",
        "native_export": native_export,
        "description": description,
        "formula": "order by (x desc, index asc); q = floor(p*Q/n); equal weight inside "
                   "each bucket; spread = mean_0 - mean_{Q-1}",
        "inputs": {
            "quantile_count": whole(quantiles),
            "n_used": whole(n),
            "instrument_indices": [whole(i) for i in sorted(signal_by_index)],
            "signal": [rat(signal_by_index[i]) for i in sorted(signal_by_index)],
            "forward_return": [rat(returns_by_index[i]) for i in sorted(returns_by_index)],
        },
        "expected": expected,
        "bound_legs": {
            "spread": float(max_bucket_mean_abs),
            "mean_top_decile": float(max_bucket_mean_abs),
            "mean_bottom_decile": float(max_bucket_mean_abs),
            "max_bucket_mean_abs": float(max_bucket_mean_abs),
        },
    }


def f2_cases():
    cases = []
    names = list(range(8))
    rets = forward_returns(F1_PRICES, 0, 1, names)

    # Q = 4 on an 8-name cross-section: exactly divisible (design section 9.2 F2).
    signal = {i: F1_SIGNALS[0][i] for i in names}
    case = spread_case(
        "f2_spread_q4_eight_names",
        "Q = 4 on the 8-name date-0 cross-section, exactly divisible; buckets of two.",
        signal, rets, 4)
    require(unrat(case["expected"]["spread"]) == Q(-23, 800),
            "F2 canonical spread disagrees with the hand value -23/800")
    cases.append(case)

    # Tie-break by ascending instrument index (design section 9.1 case 18).
    tie_signal = {0: Q(3), 1: Q(5), 2: Q(5), 3: Q(1), 4: Q(4), 5: Q(4), 6: Q(2), 7: Q(0)}
    tie_case = spread_case(
        "f2_tie_break_by_ascending_instrument_index",
        "ties at two bucket boundaries; the total order is (x desc, instrument index asc).",
        tie_signal, rets, 4)
    require([w["integer"] for w in tie_case["expected"]["bucket_members"][0]] == [1, 2],
            "F2 tie-break top bucket must be {1,2}")
    require([w["integer"] for w in tie_case["expected"]["bucket_members"][3]] == [3, 7],
            "F2 tie-break bottom bucket must be {3,7}")
    cases.append(tie_case)

    # Exactly Q names -> one per bucket (design section 9.1 case 16).
    exact_signal = {i: F1_SIGNALS[0][i] for i in range(4)}
    exact_rets = {i: rets[i] for i in range(4)}
    exact_case = spread_case(
        "f2_exactly_q_names_one_per_bucket",
        "n == Q == 4: q = floor(p*Q/n) = p, one name per bucket.",
        exact_signal, exact_rets, 4)
    require(all(w["integer"] == 1 for w in exact_case["expected"]["bucket_sizes"]),
            "F2 exactly-Q bucket sizes must all be 1")
    cases.append(exact_case)

    # Fewer than Q names -> no spread, counted (design section 9.1 case 17).
    few_signal = {i: F1_SIGNALS[0][i] for i in range(3)}
    few_rets = {i: rets[i] for i in range(3)}
    cases.append(spread_case(
        "f2_fewer_than_q_names_emits_no_spread",
        "n == 3 < Q == 4: the date is rejected for spread purposes and counted.",
        few_signal, few_rets, 4))

    # The pre-registered Q = 10 on exactly 10 names.
    ten_signal = {i: Q(10 - i) for i in range(10)}
    ten_prices_t = [Q(100 + i) for i in range(10)]
    ten_prices_th = [Q(100 + i) + Q(i - 4, 4) for i in range(10)]
    ten_rets = {i: ten_prices_th[i] / ten_prices_t[i] - 1 for i in range(10)}
    cases.append(spread_case(
        "f2_spread_q10_ten_names",
        "the pre-registered Q = 10 on exactly 10 names; decile 0 is the highest signal.",
        ten_signal, ten_rets, QUANTILE_COUNT))

    return cases


# --------------------------------------------------------------------------- #
# Family F3 -- terminal leg
# --------------------------------------------------------------------------- #

def terminal_case(case_id, description, *, ticker, instrument_id, obs_key,
                  raw_t, factor_t, marks, horizon, consideration, special,
                  record_date_key, evidenced, native_export="expected"):
    """``marks`` maps an offset u in (0, h] to (raw_close, cumulReturnFactor) or
    None for an absent mark. P = raw_close * cumulReturnFactor."""
    price_t = raw_t * factor_t
    valid = [u for u in sorted(marks) if marks[u] is not None
             and marks[u][0] * marks[u][1] > 0]
    inputs = {
        "ticker": ticker,
        "instrument_id": whole(instrument_id),
        "horizon": whole(horizon),
        "session_key_t": u64(obs_key),
        "raw_close_t": rat(raw_t),
        "cumul_return_factor_t": rat(factor_t),
        "price_t": rat(price_t),
        "forward_marks": {
            str(u): (None if marks[u] is None else {
                "raw_close": rat(marks[u][0]),
                "cumul_return_factor": rat(marks[u][1]),
                "price": rat(marks[u][0] * marks[u][1]),
            }) for u in sorted(marks)
        },
        "consideration": rat(consideration),
        "special_dividend": rat(special),
        "record_date_session_key": (None if record_date_key is None else u64(record_date_key)),
        "terminal": whole(1),
        "terminal_evidenced": whole(1 if evidenced else 0),
    }
    if not evidenced:
        return {
            "id": case_id, "family": "F3", "native_export": native_export,
            "description": description,
            "formula": "design section 3.8: a terminal cell with no evidenced consideration "
                       "DROPS and increments n_terminal_unevidenced; no return is invented",
            "inputs": inputs,
            "expected": {
                "cell_used": whole(0),
                "n_terminal_unevidenced_increment": whole(1),
                "n_terminal_applied_increment": whole(0),
                "forward_return_defined": whole(0),
            },
            "bound_legs": {},
        }
    if not valid:
        return {
            "id": case_id, "family": "F3", "native_export": native_export,
            "description": description,
            "formula": "design section 3.8 (M-8): t_last is searched in the half-open window "
                       "(t, t+h] only; no valid mark there means the cell has no terminal path "
                       "and drops as an ordinary missing forward return",
            "inputs": inputs,
            "expected": {
                "cell_used": whole(0),
                "t_last_found": whole(0),
                "n_dropped_missing_forward_increment": whole(1),
                "n_terminal_applied_increment": whole(0),
                "forward_return_defined": whole(0),
            },
            "bound_legs": {},
        }
    u_last = max(valid)
    raw_last, factor_last = marks[u_last]
    price_last = raw_last * factor_last
    include_special = (record_date_key is not None and obs_key <= record_date_key)
    applied_special = special if include_special else Q(0)
    leg = (consideration + applied_special) / raw_last - 1
    ret = (price_last / price_t) * (1 + leg) - 1
    price_ratio_abs = abs(price_last / price_t)
    consideration_ratio_abs = abs((consideration + applied_special) / raw_last)
    leg_scale = float(price_ratio_abs * consideration_ratio_abs)
    return {
        "id": case_id, "family": "F3", "native_export": native_export,
        "description": description,
        "formula": "terminal_leg = (consideration + special)/raw_close(t_last) - 1; "
                   "r = (P(t_last)/P(t)) * (1 + terminal_leg) - 1; "
                   "special applies iff session_keys[t] <= record_date_ns",
        "inputs": inputs,
        "expected": {
            "cell_used": whole(1),
            "t_last_offset": whole(u_last),
            "t_last_found": whole(1),
            "special_dividend_included": whole(1 if include_special else 0),
            "special_dividend_applied": rat(applied_special),
            "terminal_leg": rat(leg),
            "forward_return": rat(ret),
            "n_terminal_applied_increment": whole(1),
            "n_terminal_unevidenced_increment": whole(0),
        },
        "bound_legs": {
            "terminal_leg": leg_scale,
            "forward_return": leg_scale,
            "price_ratio_abs": float(price_ratio_abs),
            "consideration_ratio_abs": float(consideration_ratio_abs),
        },
    }


def f3_cases():
    cases = []

    hnz = terminal_case(
        "f3_terminal_hnz_evidenced_no_special",
        "HNZ 37648: consideration USD 72.50 against a last raw close of 72.49; no special "
        "dividend. The audit's amounts; synthetic session offsets.",
        ticker="HNZ", instrument_id=37648, obs_key=session_key(2013, 6, 3),
        raw_t=Q("72.00"), factor_t=Q(1),
        marks={1: (Q("72.49"), Q(1)), 2: None}, horizon=2,
        consideration=Q("72.50"), special=Q(0), record_date_key=None, evidenced=True)
    leg = unrat(hnz["expected"]["terminal_leg"])
    ret = unrat(hnz["expected"]["forward_return"])
    require(leg == Q(1, 7249), f"F3 HNZ leg disagrees with 1/7249: {leg}")
    require(ret == Q(1, 144), f"F3 HNZ return disagrees with 1/144: {ret}")
    cases.append(hnz)

    dell_at = terminal_case(
        "f3_terminal_dell_at_record_date_includes_special",
        "DELL 35715 shape: observation AT the record date 2013-10-28, so the USD 0.13 "
        "special dividend is included (ruling AR-2; design section 9.1 case 8a). The "
        "session dates are synthetic; only the amounts and the record date are the audit's.",
        ticker="DELL", instrument_id=35715, obs_key=session_key(2013, 10, 28),
        raw_t=Q("13.90"), factor_t=Q(1),
        marks={1: None, 2: (Q("13.86"), Q(1))}, horizon=2,
        consideration=Q("13.75"), special=Q("0.13"),
        record_date_key=DELL_RECORD_DATE, evidenced=True)
    leg = unrat(dell_at["expected"]["terminal_leg"])
    require(leg == Q(1, 693), f"F3 DELL-at-record leg disagrees with 1/693: {leg}")
    cases.append(dell_at)

    dell_after = terminal_case(
        "f3_terminal_dell_after_record_date_excludes_special",
        "the same shape one session later: session_keys[t] > 2013-10-28, so the "
        "consideration alone applies (ruling AR-2; design section 9.1 case 8b).",
        ticker="DELL", instrument_id=35715, obs_key=session_key(2013, 10, 29),
        raw_t=Q("13.90"), factor_t=Q(1),
        marks={1: None, 2: (Q("13.86"), Q(1))}, horizon=2,
        consideration=Q("13.75"), special=Q("0.13"),
        record_date_key=DELL_RECORD_DATE, evidenced=True)
    leg = unrat(dell_after["expected"]["terminal_leg"])
    require(leg == Q(-1, 126), f"F3 DELL-after-record leg disagrees with -1/126: {leg}")
    cases.append(dell_after)

    molx = terminal_case(
        "f3_terminal_molx_evidenced_zero_leg",
        "MOLX 39970: consideration USD 38.68 equals the last raw close, so the leg is "
        "exactly zero -- an exactly-zero quantity whose bound comes from the contributing "
        "legs, never from a relative test against zero.",
        ticker="MOLX", instrument_id=39970, obs_key=session_key(2013, 12, 2),
        raw_t=Q("38.50"), factor_t=Q(1),
        marks={1: (Q("38.68"), Q(1))}, horizon=1,
        consideration=Q("38.68"), special=Q(0), record_date_key=None, evidenced=True)
    leg = unrat(molx["expected"]["terminal_leg"])
    require(leg == Q(0), f"F3 MOLX leg must be exactly zero: {leg}")
    cases.append(molx)

    scaled = terminal_case(
        "f3_terminal_leg_uses_raw_close_not_adjusted_close",
        "the same DELL shape with cumulReturnFactor 1 at t and 5/4 at t_last: the price "
        "ratio uses the adjusted closes while the leg denominator stays the RAW close "
        "(design section 3.8: 'terminal_leg is computed against raw_close').",
        ticker="DELL", instrument_id=35715, obs_key=session_key(2013, 10, 28),
        raw_t=Q("13.90"), factor_t=Q(1),
        marks={1: None, 2: (Q("13.86"), Q(5, 4))}, horizon=2,
        consideration=Q("13.75"), special=Q("0.13"),
        record_date_key=DELL_RECORD_DATE, evidenced=True)
    leg = unrat(scaled["expected"]["terminal_leg"])
    require(leg == Q(1, 693), "F3 raw-vs-adjusted leg must be unchanged by the factor")
    cases.append(scaled)

    cases.append(terminal_case(
        "f3_terminal_unevidenced_drops_and_counts",
        "the PCS shape (ruling AR-1 / R-A): flagged terminal, terminal_evidenced = 0, no "
        "admissible consideration. The cell DROPS and increments n_terminal_unevidenced. "
        "PCS is never applied; its MetroPCS/T-Mobile terms are stock-plus-cash and the "
        "section 3.8 formula has no successor-share leg at all.",
        ticker="PCS", instrument_id=146189, obs_key=session_key(2013, 4, 26),
        raw_t=Q("11.00"), factor_t=Q(1),
        marks={1: None, 2: None}, horizon=2,
        consideration=Q(0), special=Q(0), record_date_key=None, evidenced=False))

    cases.append(terminal_case(
        "f3_terminal_no_valid_mark_in_forward_window_drops",
        "evidenced terminal ID whose forward window (t, t+h] contains no finite positive "
        "mark: t_last does not exist, so the cell drops as an ordinary missing forward "
        "return (M-8; design section 9.1 case 8c).",
        ticker="HNZ", instrument_id=37648, obs_key=session_key(2013, 6, 10),
        raw_t=Q("72.00"), factor_t=Q(1),
        marks={1: None, 2: None}, horizon=2,
        consideration=Q("72.50"), special=Q(0), record_date_key=None, evidenced=True))

    return cases


# --------------------------------------------------------------------------- #
# Family F4 -- coverage counters (exact integers)
# --------------------------------------------------------------------------- #

def f4_counters(t: int, h: int, restriction: str):
    """Design section 3.7, read literally.

    n_dropped_missing_forward = n_signal_finite - n_with_forward  (GROSS: a
    terminal-applied cell has no section 3.3 forward return, so it appears in
    both this counter and n_terminal_applied; the design states both formulas
    without netting one against the other).

    n_used = n_with_forward + n_terminal_applied.
    """
    ex34 = set(F4_EX34_IDS)
    names = [i for i in range(len(F4_IDS))
             if restriction == "full" or F4_IDS[i] not in ex34]

    eligible = [i for i in names if F4_MASK[t][i] == 1]
    signal_finite = [i for i in eligible if F4_SIGNALS[t][i] is not NAN]

    with_forward = []
    for i in signal_finite:
        p0 = F4_PRICES[t][i]
        p1 = F4_PRICES[t + h][i]
        if p0 is not None and p1 is not None and p0 > 0 and p1 > 0:
            with_forward.append(i)

    terminal_applied = []
    terminal_unevidenced = []
    for i in signal_finite:
        if i in with_forward:
            continue
        if F4_TERMINAL[t][i] != 1:
            continue
        if F4_TERMINAL_EVIDENCED[t][i] == 1:
            marks = [u for u in range(t + 1, t + h + 1)
                     if F4_PRICES[u][i] is not None and F4_PRICES[u][i] > 0]
            if marks and F4_PRICES[t][i] is not None and F4_PRICES[t][i] > 0:
                terminal_applied.append(i)
        else:
            terminal_unevidenced.append(i)

    excluded_audited = [i for i in eligible if F4_IDS[i] in ex34]
    used_basis_excluded = [i for i in (with_forward + terminal_applied) if F4_IDS[i] in ex34]

    return {
        "eligible": eligible,
        "signal_finite": signal_finite,
        "with_forward": with_forward,
        "terminal_applied": terminal_applied,
        "terminal_unevidenced": terminal_unevidenced,
        "excluded_audited": excluded_audited,
        "used_basis_excluded": used_basis_excluded,
        "counters": {
            "n_eligible": len(eligible),
            "n_signal_finite": len(signal_finite),
            "n_with_forward": len(with_forward),
            "n_dropped_missing_forward": len(signal_finite) - len(with_forward),
            "n_terminal_applied": len(terminal_applied),
            "n_terminal_unevidenced": len(terminal_unevidenced),
            "n_excluded_audited": len(excluded_audited),
            "n_used": len(with_forward) + len(terminal_applied),
        },
    }


F4_HAND_CHECKPOINTS = {
    ("f4_counters_t0_h1_full"): {
        "n_eligible": 7, "n_signal_finite": 6, "n_with_forward": 3,
        "n_dropped_missing_forward": 3, "n_terminal_applied": 0,
        "n_terminal_unevidenced": 1, "n_excluded_audited": 4, "n_used": 3},
    ("f4_counters_t0_h2_full"): {
        "n_eligible": 7, "n_signal_finite": 6, "n_with_forward": 2,
        "n_dropped_missing_forward": 4, "n_terminal_applied": 1,
        "n_terminal_unevidenced": 1, "n_excluded_audited": 4, "n_used": 3},
    ("f4_counters_t1_h1_full"): {
        "n_eligible": 7, "n_signal_finite": 7, "n_with_forward": 3,
        "n_dropped_missing_forward": 4, "n_terminal_applied": 0,
        "n_terminal_unevidenced": 1, "n_excluded_audited": 4, "n_used": 3},
    ("f4_counters_t0_h1_ex34"): {
        "n_eligible": 3, "n_signal_finite": 3, "n_with_forward": 2,
        "n_dropped_missing_forward": 1, "n_terminal_applied": 0,
        "n_terminal_unevidenced": 0, "n_excluded_audited": 0, "n_used": 2},
    ("f4_counters_t0_h2_ex34"): {
        "n_eligible": 3, "n_signal_finite": 3, "n_with_forward": 2,
        "n_dropped_missing_forward": 1, "n_terminal_applied": 0,
        "n_terminal_unevidenced": 0, "n_excluded_audited": 0, "n_used": 2},
    ("f4_counters_t1_h1_ex34"): {
        "n_eligible": 3, "n_signal_finite": 3, "n_with_forward": 2,
        "n_dropped_missing_forward": 1, "n_terminal_applied": 0,
        "n_terminal_unevidenced": 0, "n_excluded_audited": 0, "n_used": 2},
}


def f4_cases():
    cases = []
    for t, h in ((0, 1), (0, 2), (1, 1)):
        for restriction in ("full", "ex34"):
            case_id = f"f4_counters_t{t}_h{h}_{restriction}"
            result = f4_counters(t, h, restriction)
            hand = F4_HAND_CHECKPOINTS[case_id]
            for key, value in hand.items():
                require(result["counters"][key] == value,
                        f"{case_id}: {key} computed {result['counters'][key]}, hand value {value}")
            require(result["counters"]["n_used"]
                    == result["counters"]["n_with_forward"] + result["counters"]["n_terminal_applied"],
                    f"{case_id}: n_used identity failed")
            require(result["counters"]["n_dropped_missing_forward"]
                    == result["counters"]["n_signal_finite"] - result["counters"]["n_with_forward"],
                    f"{case_id}: n_dropped_missing_forward identity failed")
            cases.append({
                "id": case_id,
                "family": "F4",
                "native_export": "expected",
                "description": (
                    f"every design-section-3.7 counter at date {t}, horizon {h}, "
                    f"{'unrestricted' if restriction == 'full' else 'under the _ex34 restriction'}."
                ),
                "formula": "section 3.7 counter table, read literally; "
                           "n_dropped_missing_forward = n_signal_finite - n_with_forward (gross); "
                           "n_used = n_with_forward + n_terminal_applied",
                "inputs": {
                    "date_index": whole(t),
                    "horizon": whole(h),
                    "restriction": restriction,
                    "instrument_ids": [whole(v) for v in F4_IDS],
                    "ex34_ids": [whole(v) for v in F4_EX34_IDS],
                    "mask": [whole(v) for v in F4_MASK[t]],
                    "signal": [(nonfinite("nan") if v is NAN else rat(v)) for v in F4_SIGNALS[t]],
                    "price_t": [(None if v is None else rat(v)) for v in F4_PRICES[t]],
                    "price_t_plus_h": [(None if v is None else rat(v)) for v in F4_PRICES[t + h]],
                    "terminal": [whole(v) for v in F4_TERMINAL[t]],
                    "terminal_evidenced": [whole(v) for v in F4_TERMINAL_EVIDENCED[t]],
                },
                "expected": {k: whole(v) for k, v in result["counters"].items()},
                "diagnostics": {
                    "eligible_indices": [whole(i) for i in result["eligible"]],
                    "used_indices": [whole(i) for i in sorted(result["with_forward"]
                                                              + result["terminal_applied"])],
                    "n_excluded_audited_used_basis": whole(len(result["used_basis_excluded"])),
                    "n_excluded_audited_basis_note": (
                        "PRIMARY expectation n_excluded_audited counts ELIGIBLE cells whose id is "
                        "in the audited set, matching the counter's placement in the section-3.7 "
                        "eligible-family table. The used-basis count is a NON-BINDING diagnostic "
                        "for the alternative reading; the design does not state which basis the "
                        "native counter uses."
                    ),
                },
                "bound_legs": {},
            })
    return cases


# --------------------------------------------------------------------------- #
# Family F5 -- net cost drag
# --------------------------------------------------------------------------- #

def turnover_case(case_id, description, signal_prev, signal_now, quantiles,
                  expected_l1, native_export="expected"):
    prev_buckets, _ = quantile_buckets(signal_prev, quantiles)
    now_buckets, _ = quantile_buckets(signal_now, quantiles)
    w_prev = decile_weights(prev_buckets, quantiles)
    w_now = decile_weights(now_buckets, quantiles)
    oneway, l1 = oneway_turnover(w_now, w_prev)
    require(l1 == expected_l1,
            f"{case_id}: sum|dw| computed {l1}, hand value {expected_l1}")
    drag = trade_drag(oneway)
    return {
        "id": case_id,
        "family": "F5",
        "native_export": native_export,
        "description": description,
        "formula": "w_i = +1/n_top on decile 0 and -1/n_bottom on decile Q-1 (ruling AR-11); "
                   "oneway(t) = 0.5 * sum_i |w_i(t) - w_i(t-h)|; "
                   "trade_drag_h = (trade_bps/1e4) * 2 * turnover_h",
        "inputs": {
            "quantile_count": whole(quantiles),
            "trade_bps": rat(TRADE_BPS),
            "signal_prev": [rat(signal_prev[i]) for i in sorted(signal_prev)],
            "signal_now": [rat(signal_now[i]) for i in sorted(signal_now)],
        },
        "expected": {
            "weights_prev": [rat(w_prev.get(i, Q(0))) for i in sorted(signal_prev)],
            "weights_now": [rat(w_now.get(i, Q(0))) for i in sorted(signal_now)],
            "sum_abs_weight_change": rat(l1),
            "oneway_turnover": rat(oneway),
            "decile_one_way_turnover": rat(oneway),
            "trade_drag": rat(drag),
            "trade_drag_bps": rat(drag * BPS_DENOM),
        },
        "bound_legs": {
            "oneway_turnover": float(l1),
            "decile_one_way_turnover": float(l1),
            "trade_drag": float(drag),
            "sum_abs_weight_change": float(l1),
        },
        "notes": "decile_one_way_turnover lives on the [0, 2] gross-2.0 scale (NEW-4); it is "
                 "NOT comparable to AutocorrSummary::implied_one_way_turnover = 1 - rho_rank.",
    }


def days_case(case_id, description, start, end, h, native_export="expected"):
    days = days_forward(start, end)
    drag = borrow_drag(days)
    return {
        "id": case_id,
        "family": "F5",
        "native_export": native_export,
        "description": description,
        "formula": "days_h(t) = (session_keys[t+h] - session_keys[t]) / 86400000000000 "
                   "(INTEGER division); borrow_drag_h(t) = (annual_borrow_bps/1e4) * "
                   "(days_h(t)/365.0) * short_leg_gross with short_leg_gross = 1.0",
        "inputs": {
            "horizon": whole(h),
            "session_key_t": u64(start),
            "session_key_t_plus_h": u64(end),
            "annual_borrow_bps": rat(ANNUAL_BORROW_BPS),
            "short_leg_gross": rat(SHORT_LEG_GROSS),
        },
        "expected": {
            "days_forward": whole(days),
            "borrow_drag": rat(drag),
        },
        "bound_legs": {"borrow_drag": float(drag)},
    }


def f5_cases():
    cases = []

    complete_prev = {i: v for i, v in enumerate(F5_SIGNALS[0])}
    complete_now = {i: v for i, v in enumerate(F5_SIGNALS[1])}
    complete = turnover_case(
        "f5_complete_decile_turnover_costs_twenty_bps",
        "4 names, Q = 2, a COMPLETE turnover of both legs: sum|dw| == 4 exactly, "
        "oneway == 2, trade_drag == (5/1e4)*2*2 == 0.0020 == 20 bps (ruling AR-11; "
        "design section 9.1 case 19b).",
        complete_prev, complete_now, 2, Q(4), native_export="expected")
    require(unrat(complete["expected"]["trade_drag"]) == Q(2, 1000),
            "F5 complete turnover must cost exactly 20 bps")
    cases.append(complete)

    half_now = {0: Q(4), 1: Q(1), 2: Q(3), 3: Q(0)}
    half = turnover_case(
        "f5_half_decile_turnover_costs_ten_bps",
        "the companion half-turnover fixture: one name retained in each leg, "
        "sum|dw| == 2, oneway == 1, trade_drag == 0.0010 == 10 bps.",
        complete_prev, half_now, 2, Q(2), native_export="expected")
    require(unrat(half["expected"]["trade_drag"]) == Q(1, 1000),
            "F5 half turnover must cost exactly 10 bps")
    cases.append(half)

    cases.append(days_case(
        "f5_days_friday_to_monday_is_three",
        "Fri 2013-04-05 -> Mon 2013-04-08 at h = 1: three CALENDAR days, not one "
        "observation (design section 9.1 case 19c).",
        session_key(2013, 4, 5), session_key(2013, 4, 8), 1))
    cases.append(days_case(
        "f5_days_tuesday_to_wednesday_is_one",
        "Tue 2013-04-09 -> Wed 2013-04-10 at h = 1: one calendar day.",
        session_key(2013, 4, 9), session_key(2013, 4, 10), 1))
    cases.append(days_case(
        "f5_days_holiday_weekend_is_four",
        "Fri 2013-05-24 -> Tue 2013-05-28 at h = 1 across the Memorial Day holiday: "
        "four calendar days (design section 9.1 case 19c, three-session-gap companion).",
        session_key(2013, 5, 24), session_key(2013, 5, 28), 1))
    cases.append(days_case(
        "f5_days_horizon_five_spans_seven_days",
        "Mon 2013-04-01 -> Mon 2013-04-08 at h = 5: seven calendar days > 5 observations, "
        "so the observation-count convention is provably not in use (design section 9.1 case 19d).",
        session_key(2013, 4, 1), session_key(2013, 4, 8), 5))

    # --- the per-date net series (ruling AR-3: no rebalance grid) ------------ #
    h = 1
    names = list(range(4))
    emitted = []
    for t in range(len(F5_PRICES) - h):
        rets = forward_returns(F5_PRICES, t, h, names)
        signal = {i: F5_SIGNALS[t][i] for i in names}
        buckets, _ = quantile_buckets(signal, 2)
        means = {q: sum((rets[i] for i in members), Q(0)) / len(members)
                 for q, members in buckets.items()}
        emitted.append({
            "t": t,
            "gross": means[0] - means[1],
            "weights": decile_weights(buckets, 2),
            "means": means,
            "days": days_forward(F5_SESSION_KEYS[t], F5_SESSION_KEYS[t + h]),
        })
    require(emitted[0]["gross"] == Q(-1, 50),
            f"F5 net-series gross(0) disagrees with -1/50: {emitted[0]['gross']}")

    oneways = []
    for k in range(1, len(emitted)):
        if emitted[k]["t"] - h == emitted[k - 1]["t"]:
            ow, _ = oneway_turnover(emitted[k]["weights"], emitted[k - 1]["weights"])
            oneways.append(ow)
    require(len(oneways) == 1, "F5 net-series must have exactly one turnover observation")
    turnover = sum(oneways, Q(0)) / len(oneways)
    require(turnover == Q(2), f"F5 net-series turnover must be 2: {turnover}")
    td = trade_drag(turnover)

    rows = []
    for row in emitted:
        bd = borrow_drag(row["days"])
        cd = td + bd
        rows.append({
            "t": row["t"], "gross": row["gross"], "days": row["days"],
            "borrow_drag": bd, "cost_drag": cd, "net": row["gross"] - cd,
        })
    require(rows[0]["cost_drag"] == Q(23, 10000),
            f"F5 cost_drag(0) disagrees with 0.0023: {rows[0]['cost_drag']}")
    require(rows[0]["net"] == Q(-1, 50) - Q(23, 10000), "F5 net(0) identity failed")

    cases.append({
        "id": "f5_net_is_gross_minus_per_date_drag",
        "family": "F5",
        "native_export": "expected",
        "description": (
            "the per-date net series (ruling AR-3): 4 names, Q = 2, h = 1, session keys "
            "Fri/Mon/Tue so days_h(t) is 3 then 1. trade_drag is a horizon CONSTANT; "
            "borrow_drag varies by calendar span; the net series has exactly the same "
            "length and the same emitted-date set as the gross series."
        ),
        "formula": "net_h(t) = gross_h(t) - cost_drag_h(t); "
                   "cost_drag_h(t) = trade_drag_h + borrow_drag_h(t)",
        "inputs": {
            "horizon": whole(h),
            "quantile_count": whole(2),
            "trade_bps": rat(TRADE_BPS),
            "annual_borrow_bps": rat(ANNUAL_BORROW_BPS),
            "short_leg_gross": rat(SHORT_LEG_GROSS),
            "session_keys": [u64(k) for k in F5_SESSION_KEYS],
            "signals": [[rat(v) for v in row] for row in F5_SIGNALS],
            "prices": [[rat(v) for v in row] for row in F5_PRICES],
        },
        "expected": {
            "dates_emitted": whole(len(rows)),
            "decile_one_way_turnover": rat(turnover),
            "trade_drag": rat(td),
            "days_forward": [whole(r["days"]) for r in rows],
            "gross": [rat(r["gross"]) for r in rows],
            "borrow_drag": [rat(r["borrow_drag"]) for r in rows],
            "cost_drag": [rat(r["cost_drag"]) for r in rows],
            "net": [rat(r["net"]) for r in rows],
            "net_series_length_equals_gross": whole(1),
            "borrow_day_convention": "calendar_days_from_session_keys",
        },
        "bound_legs": {
            "decile_one_way_turnover": float(turnover),
            "trade_drag": float(td),
            "borrow_drag": float(max(r["borrow_drag"] for r in rows)),
            "cost_drag": float(max(r["cost_drag"] for r in rows)),
            "net": float(max(r["cost_drag"] for r in rows)),
            "gross": float(max(abs(r["gross"]) for r in rows)),
        },
        "diagnostics": {
            "net_cancellation_leg": float(max(abs(r["gross"]) + r["cost_drag"] for r in rows)),
            "net_cancellation_leg_note": (
                "NON-BINDING. The design's declared F5 bound leg for the net is "
                "trade_drag + borrow_drag, which omits |gross|; net is a difference of the "
                "two, so |gross| + cost_drag is the cancellation-aware leg. The declared "
                "bound governs the verdict; this is recorded so the looser reading is visible."
            ),
        },
        "notes": "section 12.8c: the per-date net mixes a per-date financing term with a "
                 "period-average trading term and is not a per-date realized cost.",
    })

    return cases


# --------------------------------------------------------------------------- #
# Family F6 -- bootstrap reproducibility
# --------------------------------------------------------------------------- #

F6_DRAW_COUNT = 8
F6_HORIZON_INDEX = 0
F6_HORIZON = HORIZONS[F6_HORIZON_INDEX]          # h = 1 -> L_h = 5
F6_PRIMARY_N = {0: 189 - F6_HORIZON, 1: 189 - max(HORIZONS)}   # 188 full, 126 common
F6_AUXILIARY_N = (20, 25, 32, 50, 126, 188)


def f6_cases():
    cases = []
    length = block_len(F6_HORIZON)
    require(length == 5, f"F6 block length at h=1 must be 5, got {length}")

    for sample_id in (0, 1):
        key = stream_key(
            statistic_id=BOOTSTRAP_STATISTIC_IDS["IcMean"],
            horizon_index=F6_HORIZON_INDEX,
            signal_index=0,
            variant_id=0,
            restriction_id=0,
            sample_id=sample_id,
        )
        primary_n = F6_PRIMARY_N[sample_id]
        seed_x, starts, fallbacks = first_draw_starts(key, primary_n, F6_DRAW_COUNT)
        require(fallbacks == 0, "F6 primary stream must take no modulo fallback")
        require(all(0 <= s < primary_n for s in starts), "F6 draw starts out of range")

        blocks = math.ceil(primary_n / length)
        seed_check, all_starts, fb2 = first_draw_starts(key, primary_n, blocks)
        require(seed_check == seed_x and fb2 == 0, "F6 stream must be reproducible")
        require(all_starts[:F6_DRAW_COUNT] == starts,
                "F6 first-8 starts must prefix the first draw's start list")
        resample = resample_indices(all_starts, primary_n, length)
        require(len(resample) == primary_n, "F6 resample must be truncated to exactly n")

        reportable, reason, _ = bootstrap_reportable(BOOTSTRAP_DRAWS, primary_n, F6_HORIZON)
        require(reportable == 1, "F6 primary n must be reportable at h = 1")

        auxiliary = []
        for n in F6_AUXILIARY_N:
            aux_seed, aux_starts, aux_fb = first_draw_starts(key, n, F6_DRAW_COUNT)
            require(aux_seed == seed_x, "F6 auxiliary stream seed must match")
            require(aux_fb == 0, "F6 auxiliary stream must take no modulo fallback")
            auxiliary.append({
                "n": whole(n),
                "draw_starts": [whole(s) for s in aux_starts],
            })

        cases.append({
            "id": f"bootstrap_f6_sample{sample_id}",
            "family": "F6",
            "native_export": "required",
            "description": (
                "the first 8 draw start indices of the stream "
                f"(statistic_id=IcMean=0, horizon_index=0, signal_index=0, variant_id=0, "
                f"restriction_id=0, sample_id={sample_id}); design section 9.1 case 13b / "
                "section 9.2 family F6. Asserting BOTH sample ids is what verifies that "
                "ruling R-C's sample_id field actually separates the two streams."
            ),
            "formula": (
                "stream_key = 20260920 ^ (stat<<8) ^ (hidx<<16) ^ (sig<<24) ^ (var<<32) "
                "^ (restr<<40) ^ (sample<<48); seed_x = splitmix64_next(stream_key) "
                "(advance state FIRST, use the RETURN value, discard the mutated state); "
                "rng = Xoshiro256pp(seed_x) (four further splitmix64 steps, random.hpp:83); "
                "draw_below consumes one 64-bit output per attempt, threshold (0ULL - n) % n, "
                "HIGH half of the widening product is the result"
            ),
            "inputs": {
                "kBootstrapSeed": u64(K_BOOTSTRAP_SEED),
                "statistic_id": whole(BOOTSTRAP_STATISTIC_IDS["IcMean"]),
                "statistic_name": "IcMean",
                "horizon_index": whole(F6_HORIZON_INDEX),
                "horizon": whole(F6_HORIZON),
                "signal_index": whole(0),
                "variant_id": whole(0),
                "restriction_id": whole(0),
                "sample_id": whole(sample_id),
                "n": whole(primary_n),
                "bootstrap_draws": whole(BOOTSTRAP_DRAWS),
                "draw_count_asserted": whole(F6_DRAW_COUNT),
            },
            "expected": {
                "stream_key": u64(key),
                "seed_x": u64(seed_x),
                "block_len": whole(length),
                "blocks_per_draw": whole(blocks),
                "draw_starts": [whole(s) for s in starts],
                "modulo_fallbacks": whole(0),
                "reportable": whole(reportable),
                "unreportable_reason_text": reason,
                "first_draw_resample_indices_head": [whole(v) for v in resample[:16]],
                "first_draw_resample_indices_tail": [whole(v) for v in resample[-4:]],
                "first_draw_resample_length": whole(len(resample)),
            },
            "auxiliary_draw_starts_by_n": auxiliary,
            "auxiliary_note": (
                "The design names the two stream TUPLES but not the series length n the "
                "engine fixture uses. The primary n is the stage-1 series length for this "
                "sample (section 4.4: n = 189 - h = 188 full, n_common = 126). The auxiliary "
                "table carries the same stream's first 8 starts for other n so a smaller "
                "synthetic engine fixture can still be matched bit-exactly."
            ),
            "bound_legs": {},
        })

    # The two streams must not coincide -- this is the whole point of ruling R-C.
    starts0 = [w["integer"] for w in cases[0]["expected"]["draw_starts"]]
    starts1 = [w["integer"] for w in cases[1]["expected"]["draw_starts"]]
    require(cases[0]["expected"]["stream_key"]["integer"]
            != cases[1]["expected"]["stream_key"]["integer"],
            "F6 sample_id must change the stream key")
    require(starts0 != starts1, "F6 sample_id must change the drawn start indices")

    # Reportability of the frozen stage-1 grid, computed from the same rule.
    grid = []
    for idx, h in enumerate(HORIZONS):
        for sample, n in (("full", 189 - h), ("common", 189 - max(HORIZONS))):
            rep, reason, length_h = bootstrap_reportable(BOOTSTRAP_DRAWS, n, h)
            grid.append({
                "horizon": whole(h), "horizon_index": whole(idx), "sample": sample,
                "n": whole(n), "block_len": whole(length_h),
                "floor_n_over_l": whole(n // length_h),
                "reportable": whole(rep),
                "unreportable_reason_text": reason,
            })
    null_horizons = sorted({row["horizon"]["integer"] for row in grid
                            if row["reportable"]["integer"] == 0})
    require(null_horizons == [63],
            f"section 4.4 predicts exactly h = 63 unreportable; got {null_horizons}")
    cases.append({
        "id": "f6_reportability_grid_stage1",
        "family": "F6",
        "native_export": "expected",
        "description": "the frozen reportability rule evaluated over the stage-1 grid; "
                       "section 4.4 predicts exactly one null horizon, h = 63, in both "
                       "the full-sample and common-sample columns.",
        "formula": "L_h = max(5, ceil(h/2)); reportable = draws>=1 && n>=20 && floor(n/L_h)>=10",
        "inputs": {
            "T": whole(189),
            "horizons": [whole(h) for h in HORIZONS],
            "bootstrap_draws": whole(BOOTSTRAP_DRAWS),
            "common_sample_dates": whole(189 - max(HORIZONS)),
        },
        "expected": {
            "grid": grid,
            "predicted_null_horizons": [whole(h) for h in null_horizons],
        },
        "bound_legs": {},
    })

    return cases


# --------------------------------------------------------------------------- #
# Section 3.13 -- common-sample prefix (checked here, reported as F6 metadata)
# --------------------------------------------------------------------------- #

def common_sample_block():
    t_count = 189
    common = t_count - max(HORIZONS)
    require(common == 126, "common_sample_dates must be 126")
    return {
        "rule": "in_common_sample(t) = (t < common_sample_dates); "
                "common_sample_dates = T - max(H)",
        "T": 126 + max(HORIZONS),
        "max_horizon": max(HORIZONS),
        "common_sample_dates": common,
        "void_rule": (
            "common_prefix_gaps is ONE usize per (horizon, sample), shared by that block's "
            "IC and spread families. Any gap voids the ENTIRE common block for that horizon: "
            "every *_common column -- ic_mean_common and spread_*_mean_common INCLUDED -- is "
            "emitted as null (JSON) / \"\" (CSV) with summary_reportable = 0 and "
            "unreportable_reason == 2 (\"common-sample-prefix-incomplete\"). The full-sample "
            "block for the same horizon is unaffected. Falling back to a per-horizon "
            "intersection is forbidden (rulings R-B and NEW-1)."
        ),
        "pinned_unreportable_reason_codes": {
            "2": "common-sample-prefix-incomplete",
            "3": "series-too-short-for-block-length",
        },
        "unpinned_reason_codes_note": (
            "Only 2 and 3 are pinned by the design (sections 3.13 and 9.1 case 14). The "
            "integer codes for \"bootstrap-draws-zero\" and \"series-shorter-than-twenty\" "
            "are NOT stated anywhere in the design; the comparator therefore checks the two "
            "pinned codes and records any other code as unpinned rather than failing on it."
        ),
    }


# --------------------------------------------------------------------------- #
# Document assembly
# --------------------------------------------------------------------------- #

def file_evidence(path: Path) -> dict:
    entry = {"path": str(path).replace("\\", "/"), "exists": path.exists()}
    if path.exists():
        data = path.read_bytes()
        entry["sha256"] = hashlib.sha256(data).hexdigest()
        entry["bytes"] = len(data)
    return entry


def build_document() -> dict:
    f1, pearson_series, rank_series = f1_cases()
    cases = f1 + f2_cases() + f3_cases() + f4_cases() + f5_cases() + f6_cases()

    ids = [c["id"] for c in cases]
    require(len(ids) == len(set(ids)), "duplicate oracle case id")

    per_family = {}
    for case in cases:
        per_family.setdefault(case["family"], []).append(case["id"])

    return {
        "schema": SCHEMA,
        "status": "exact-synthetic-math-verified",
        "measurement_schema_expected_from_native": MEASUREMENT_SCHEMA,
        "measurement_marker_expected_from_native": "CROSS_SECTION_IC_MEASUREMENT ",
        "qualification": QUALIFICATION,
        "independence": {
            "native_cpp_source_read": False,
            "cross_section_ic_cpp_read": False,
            "stage_equity_ic_cpp_read": False,
            "permitted_headers_read": [
                "atx-core/include/atx/core/random.hpp (Xoshiro256pp, design N-7)",
                "atx-core/include/atx/core/decimal.hpp:120-133 (umul_64_to_128, design N-5)",
                "atx-core/include/atx/core/stats/cross_section.hpp (rank / quantile_sorted "
                "semantics cited by design sections 3.5 and 3.10)",
            ],
            "stdlib_only": True,
            "build_or_test_executed_by_this_script": False,
            "real_panel_or_archive_admitted": False,
            "expected_values_derived_in_exact_arithmetic": True,
        },
        "frozen_parameters": {
            "signals": list(SIGNALS),
            "horizons": list(HORIZONS),
            "quantile_count": QUANTILE_COUNT,
            "min_names_per_date": MIN_NAMES_PER_DATE,
            "bootstrap_draws": BOOTSTRAP_DRAWS,
            "bootstrap_seed": K_BOOTSTRAP_SEED,
            "block_len_rule": "max(5, ceil(h/2))",
            "block_lens": [block_len(h) for h in HORIZONS],
            "bootstrap_percentiles": [2.5, 97.5],
            "percentile_convention": "nearest-rank, round-half-up "
                                     "(cross_section.hpp:75-86 quantile_sorted)",
            "reportable_rule": "draws>=1 && n>=20 && floor(n/L_h)>=10",
            "statistic_ids": dict(BOOTSTRAP_STATISTIC_IDS),
            "stream_key": "seed ^ (stat<<8) ^ (horizon_index<<16) ^ (signal_index<<24) "
                          "^ (variant_id<<32) ^ (restriction_id<<40) ^ (sample_id<<48)",
            "stream_key_unused_bits": "0-7 and 56-63",
            "sample_ids": {"full": 0, "common": 1},
            "variants": list(VARIANTS),
            "restrictions": list(RESTRICTIONS),
            "samples": list(SAMPLES),
            "trade_bps": 5.0,
            "trade_bps_source": "constexpr EquityAllocationConfig{}.trade_bps "
                                "(equity_allocation.hpp:40)",
            "annual_borrow_bps": 365.0,
            "annual_borrow_bps_source": "deployed equity-book run config literal, NOT "
                                        "replay.hpp whose default is 0.0",
            "short_leg_gross": 1.0,
            "priced_book_gross": 2.0,
            "decile_weights": "+1.0/n_top, -1.0/n_bottom",
            "borrow_day_convention": "calendar_days_from_session_keys",
            "borrow_day_formula": "(session_keys[t+h]-session_keys[t])/86400000000000 "
                                  "integer division",
            "alignment": "signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1",
            "naive_t_validity": "invalid-under-overlapping-horizons",
            "turnover_source": "rho_rank",
            "trial_count_declared": 30,
            "evidenced_terminal_ids": [37648, 35715, 39970],
            "terminal_unevidenced_ids": [146189],
            "evidenced_non_terminal_ids": [150340, 351548],
            "pcs_statement": "PCS is never applied; admission remains rejected",
        },
        "common_sample": common_sample_block(),
        "bound_policy": {
            "eps": DBL_EPSILON,
            "ulp_multiplier": ULP_MULTIPLIER,
            "rule": "Bounds are derived from the contributing legs, never an absolute floor. "
                    "An exactly-zero expectation is never tested relatively against zero.",
            "families": {
                "F1": {
                    "kind": "relative_with_leg",
                    "formula": "64 * DBL_EPSILON * max(|actual|, |expected|, leg_scale)",
                    "leg_scale": "sqrt(va*vb) for an IC; max |IC_t| for a series statistic",
                    "note": "The design declares leg_scale = sqrt(va*vb) for the IC family. "
                            "sqrt(va*vb) carries the units of cov while the IC is "
                            "dimensionless, so this bound is generous whenever the fixture's "
                            "dispersion is large. It is implemented exactly as declared; "
                            "the comparator additionally records a NON-BINDING "
                            "relative-only residual for visibility.",
                },
                "F2": {
                    "kind": "relative_with_leg",
                    "formula": "64 * DBL_EPSILON * max(|actual|, |expected|, max_bucket_mean_abs)",
                },
                "F3": {
                    "kind": "relative_with_leg",
                    "formula": "64 * DBL_EPSILON * max(|actual|, |expected|, "
                               "price_ratio_abs * consideration_ratio_abs)",
                    "note": "legs, not zero: the MOLX leg is exactly 0.0 and is bounded by "
                            "the contributing price and consideration ratios.",
                },
                "F4": {
                    "kind": "exact_integer",
                    "formula": "exact equality -- integers have no tolerance",
                },
                "F5": {
                    "kind": "mixed",
                    "formula": "days_forward: exact integer equality. Drags and net: "
                               "64 * DBL_EPSILON * max(|actual|, |expected|, "
                               "trade_drag + borrow_drag)",
                },
                "F6": {
                    "kind": "exact_integer",
                    "formula": "exact equality on the stream key, the seed and every draw "
                               "start index -- bit-exact uint64 arithmetic, no tolerance",
                },
            },
        },
        "families": {
            "F1": "IC family -- Pearson and rank IC per date, and ic_mean / ic_sd / icir / "
                  "naive_t over the date series (design sections 3.4, 3.5, 3.9)",
            "F2": "quantile spread -- bucket assignment, bucket means and the decile spread "
                  "(design section 3.12 steps 1-5)",
            "F3": "terminal leg -- design section 3.8 with ruling AR-2's record-date condition",
            "F4": "coverage counters -- every design section 3.7 counter as an exact integer",
            "F5": "net cost drag -- gross-2.0 decile turnover, calendar borrow days, per-date "
                  "cost drag and net spread (design sections 3.12, rulings AR-3 / AR-11)",
            "F6": "bootstrap reproducibility -- the section 3.10 stream recipe reproduced "
                  "bit-exactly for both sample ids (rulings AR-6 / R-C)",
        },
        "native_export_policy": {
            "required": "named by the design as natively exported (section 9.1 case 13b / "
                        "section 9.2 F6). A missing required case is a comparator FAILURE and "
                        "is recorded as not_measured_natively -- never as passing.",
            "expected": "design section 9.2 states the native side emits 'one line per oracle "
                        "scenario', so every other case is expected natively. A missing "
                        "expected case is also a failure by default; the comparator's "
                        "--optional-case flag can downgrade one to not_measured_natively, and "
                        "records every such downgrade in its output.",
        },
        "case_count": len(cases),
        "cases_by_family": per_family,
        "cases": cases,
        "evidence": {
            "oracle_script": file_evidence(SELF),
            "design_note": file_evidence(DESIGN),
            "random_header": file_evidence(RANDOM_HPP),
            "decimal_header": file_evidence(DECIMAL_HPP),
            "cross_section_header": file_evidence(CROSS_SECTION_HPP),
        },
        "decimal_display_precision": DECIMAL_PRECISION,
    }


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    selftest_only = "--selftest" in argv
    for arg in argv:
        if arg not in ("--selftest",):
            print(f"unknown argument: {arg}", file=sys.stderr)
            return 2

    document = build_document()

    if selftest_only:
        print(json.dumps({
            "schema": SCHEMA,
            "status": "selftest-only",
            "case_count": document["case_count"],
            "cases_by_family": {k: len(v) for k, v in document["cases_by_family"].items()},
        }, indent=2))
        return 0

    if OUTPUT.exists():
        print(json.dumps({
            "schema": SCHEMA,
            "status": "refused",
            "reason": "output already exists; refusing to overwrite",
            "out": str(OUTPUT).replace("\\", "/"),
        }, indent=2))
        return 1

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, indent=2, allow_nan=False)
        stream.write("\n")

    print(json.dumps({
        "schema": SCHEMA,
        "status": document["status"],
        "case_count": document["case_count"],
        "cases_by_family": {k: len(v) for k, v in document["cases_by_family"].items()},
        "out": str(OUTPUT).replace("\\", "/"),
        "oracle_sha256": hashlib.sha256(OUTPUT.read_bytes()).hexdigest(),
        "qualification": QUALIFICATION,
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
