"""Independent exact oracle for the iteration-15 point-in-time universe builder (v1).

Scope
-----
The *independent* side of checkpoint-15 T3 (design section 8.1, ruling R15-15). It
re-derives, from the FROZEN design note
``atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md`` (Revision 3,
SHA-256 ``be4b3b670eaced4f0b6f5cee8a7d5e01dc8b201ae1ecd4ef8b3377bcdb41020f``) alone, the
expected values of the nine oracle families F1..F9 of design section 8.1, on fixtures that
follow the engine test cases of design section 9.1 so the native marker lines and this
document share case ids and inputs.

Independence discipline (design sections 8.1 and 9.1):
  * stdlib only -- ``fractions``, ``hashlib``, ``json``, ``struct``, ``math``;
  * ``atx-engine/src/data/point_in_time_universe.cpp``, its header and its test were NOT
    read (they are written by T1 in parallel); every rule below is restated from the design
    TEXT -- sections 2.3, 3.3, 3.4, 3.5, 4.3-4.7, 6;
  * every real is produced by IEEE binary64 arithmetic exactly as design section 3.5 pins it
    (``dv = close * volume`` as ONE Python float product stored before use; the even-count
    median as ``(a + b) * 0.5`` -- two float ops) and exported BOTH as a JSON number and as
    ``float.hex()``; everything else is an exact integer (DR15-4 / DR15-15: every median in a
    marker is ``<name>_x2 = 2 * median``, an exact integer);
  * the output file is opened with mode ``"x"`` -- an existing output is a hard refusal;
  * all text I/O passes ``encoding="utf-8"`` explicitly.

Native marker this document binds to (design section 8.2)
----------------------------------------------------------
    POINT_IN_TIME_UNIVERSE_MEASUREMENT {"schema":"atx-point-in-time-universe-measurement-v1",
      "case_id":"...","family":"F1","inputs":{...same keys as the oracle case...},
      "values":{...same keys as expected...}}

Qualification
-------------
Synthetic-fixture arithmetic oracle. It admits no real market data, executes no native code,
measures no strategy and makes no claim about survivorship completeness, instrument type or
economic correctness. A passing comparison means the native arithmetic agrees with the frozen
design's rules on the fixtures listed, and nothing more.

  python build-equity/audits/iteration15_universe_oracle.py            # writes the document
  python build-equity/audits/iteration15_universe_oracle.py --selftest # derives, writes nothing
"""

from __future__ import annotations

import hashlib
import json
import math
import struct
import sys
from fractions import Fraction as Q
from pathlib import Path

# --------------------------------------------------------------------------- #
# Paths and schema
# --------------------------------------------------------------------------- #

SCHEMA = "atx-iteration15-universe-oracle-v1"
MEASUREMENT_SCHEMA = "atx-point-in-time-universe-measurement-v1"
MARKER = "POINT_IN_TIME_UNIVERSE_MEASUREMENT "

SELF = Path(__file__).resolve()
ROOT = SELF.parents[2]
OUTPUT = ROOT / "build-equity" / "audits" / "iteration15-universe-oracle-v1.json"
DESIGN = ROOT / "atx-engine" / "reviews" / "2026-09-20-iteration15-point-in-time-universe-design.md"
# Revision 3, frozen (design header and section 5.8). The document refuses to build when the
# on-disk note hashes to anything else: a changed design is a new pre-registration.
DESIGN_SHA256 = "be4b3b670eaced4f0b6f5cee8a7d5e01dc8b201ae1ecd4ef8b3377bcdb41020f"

# --------------------------------------------------------------------------- #
# Frozen parameters (design section 6)
# --------------------------------------------------------------------------- #

K_ADV_WINDOW = 63
K_MIN_VALID = 57                     # ceil(0.9 * 63)
K_MIN_RAW_PRICE_EXCLUSIVE = 1.0      # raw close > 1.0, strict (R15-5)
K_MIN_ADV_USD = 0.0
K_TOP_N = (1000, 2000, 3000)
K_BAND_BP = (0, 1000)
K_SESSION_KEY_END_EXCLUSIVE = 1_577_836_800_000_000_000   # 2020-01-01T00:00:00Z
NS_PER_DAY = 86_400_000_000_000
M64 = (1 << 64) - 1
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
BIN_MAGIC = b"ATXPITU1"
BIN_VERSION = 1

# Enum integers pinned by the design (section 3.1, DR15-14).
STATUS_ADD, STATUS_KEEP = 0, 1                      # PitMemberStatus
DROP_RANK, DROP_LAST_BAR = 0, 1                     # PitDropKind
EXIT_RANK_DROP, EXIT_LAST_BAR_WITHIN_WINDOW, EXIT_WINDOW_END = 0, 1, 2   # PitExitKind
NOT_RANKED = 0                                      # rank reported for an ineligible target

QUALIFICATION = (
    "Synthetic-fixture arithmetic oracle for the point-in-time universe rules of the frozen "
    "iteration-15 design. It admits no real market data, executes no native code, and makes "
    "no claim about survivorship completeness, instrument type, alpha or economic "
    "correctness. Agreement means the native arithmetic matches the design's rules on the "
    "fixtures listed, nothing more."
)

NAN = float("nan")


class OracleError(RuntimeError):
    """Raised when a hand-derived checkpoint disagrees with the computed value."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise OracleError(message)


# --------------------------------------------------------------------------- #
# Number packaging (DR15-4 / DR15-5 / DR15-15)
# --------------------------------------------------------------------------- #

def whole(value: int) -> dict:
    """An exact-integer expectation; no tolerance is admitted."""
    require(isinstance(value, int) and not isinstance(value, bool), f"whole: {value!r}")
    return {"kind": "exact_integer", "integer": int(value)}


def u64(value: int) -> dict:
    """A 64-bit unsigned expectation; carried as a decimal STRING as well as an integer
    because values above 2**53 do not survive a JSON float reader."""
    require(0 <= value <= M64, f"u64 out of range: {value}")
    return {"kind": "exact_u64", "integer": int(value), "decimal_string": str(value)}


def real(value: float) -> dict:
    """A binary64 expectation: the JSON number (``float`` -- integral values serialise with
    ``.0``) plus ``float.hex()``; compared as ``Fraction(float)`` exact equality."""
    require(isinstance(value, float) and math.isfinite(value), f"real: {value!r}")
    return {"kind": "binary64", "binary64": value, "hex": value.hex(),
            "exact": f"{Q(value).numerator}/{Q(value).denominator}"}


def text(value: str) -> dict:
    return {"kind": "string", "string": str(value)}


# --------------------------------------------------------------------------- #
# Calendar (design section 3.1: Howard Hinnant's civil algorithms, integer only)
# --------------------------------------------------------------------------- #

def days_from_civil(y: int, m: int, d: int) -> int:
    y -= 1 if m <= 2 else 0
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def civil_from_days(z: int):
    z += 719468
    era = (z if z >= 0 else z - 146096) // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + (3 if mp < 10 else -9)
    return (y + (1 if m <= 2 else 0), m, d)


def date_to_nanos(iso: str) -> int:
    y, m, d = (int(part) for part in iso.split("-"))
    return days_from_civil(y, m, d) * NS_PER_DAY


def nanos_to_date(key: int) -> str:
    y, m, d = civil_from_days(key // NS_PER_DAY)
    return f"{y:04d}-{m:02d}-{d:02d}"


def year_of(key: int) -> int:
    return civil_from_days(key // NS_PER_DAY)[0]


def month_of(key: int) -> int:
    y, m, _ = civil_from_days(key // NS_PER_DAY)
    return y * 12 + m


# --------------------------------------------------------------------------- #
# Design section 3.5 -- the median rule, bit-for-bit
# --------------------------------------------------------------------------- #

def median_binary64(values) -> float:
    """``std::sort`` ascending; odd -> v[m/2]; even -> (v[m/2-1] + v[m/2]) * 0.5 in binary64."""
    v = sorted(values)
    m = len(v)
    require(m > 0, "median of an empty set")
    if m % 2 == 1:
        return v[m // 2]
    a = v[m // 2 - 1]
    b = v[m // 2]
    s = a + b          # one binary64 addition
    return s * 0.5     # one binary64 multiplication


def median_x2_of_integers(values) -> int:
    """DR15-15: medians of integer series are integer or .5; export 2 * median exactly."""
    v = sorted(int(x) for x in values)
    m = len(v)
    require(m > 0, "median of an empty integer set")
    if m % 2 == 1:
        return 2 * v[m // 2]
    return v[m // 2 - 1] + v[m // 2]


# --------------------------------------------------------------------------- #
# Design section 2.3 -- bar validity
# --------------------------------------------------------------------------- #

def bar_is_valid(close: float, volume: float) -> bool:
    return (math.isfinite(close) and close > 0.0
            and math.isfinite(volume) and volume >= 0.0)


# --------------------------------------------------------------------------- #
# Design section 3.1 -- select_monthly_rank_sessions (DR15-8, reading A)
# --------------------------------------------------------------------------- #

def select_monthly_rank_sessions(session_keys, start_key: int, end_inclusive_key: int):
    """A session is a rank session iff the NEXT attached session's UTC calendar month differs
    (the month's last session BY DATA); the last attached session is never selected; then the
    ``start_key <= key <= end_inclusive_key`` filter."""
    keys = list(session_keys)
    require(all(keys[i] < keys[i + 1] for i in range(len(keys) - 1)), "keys not ascending")
    out = []
    for i in range(len(keys) - 1):
        if month_of(keys[i]) != month_of(keys[i + 1]):
            if start_key <= keys[i] <= end_inclusive_key:
                out.append(keys[i])
    return out


# --------------------------------------------------------------------------- #
# Design sections 3.3 / 3.4 / 4.3 / 4.4 / 4.5 / 4.6 -- the builder, restated
# --------------------------------------------------------------------------- #

class Name:
    __slots__ = ("id", "slot", "first_seen", "first_bar", "last_bar", "last_close",
                 "last_shares", "last_gics", "ring", "valid_sessions")

    def __init__(self, ident: int, slot: int, first_seen: int, window: int):
        self.id = ident
        self.slot = slot
        self.first_seen = first_seen
        self.first_bar = None
        self.last_bar = None
        self.last_close = NAN
        self.last_shares = NAN
        self.last_gics = NAN
        self.ring = [NAN] * window
        self.valid_sessions = []


class Builder:
    """The design's ``PitUniverseBuilder``, restated from sections 3.3 and 3.4 only."""

    def __init__(self, window, min_valid, min_price, min_adv, top_n, band_bp,
                 end_exclusive=K_SESSION_KEY_END_EXCLUSIVE):
        require(window > 0 and min_valid > 0 and min_valid <= window, "bad window config")
        require(list(top_n) == sorted(set(top_n)) and all(n > 0 for n in top_n), "top_n")
        require(list(band_bp) == sorted(set(band_bp)), "band_bp")
        self.window = int(window)
        self.min_valid = int(min_valid)
        self.min_price = float(min_price)
        self.min_adv = float(min_adv)
        self.top_n = tuple(int(n) for n in top_n)
        self.band_bp = tuple(int(b) for b in band_bp)
        self.end_exclusive = end_exclusive
        self.cuts = [(ti, bi) for ti in range(len(self.top_n)) for bi in range(len(self.band_bp))]
        self.names = []            # slot order
        self.slot_of = {}
        self.session_keys = []
        self.ids_with_valid_bar = []
        self.members = [set() for _ in self.cuts]          # member_bits per cut (slots)
        self.ever = [set() for _ in self.cuts]
        self.first_member_r = [dict() for _ in self.cuts]
        self.last_member_r = [dict() for _ in self.cuts]
        self.last_drop_kind = [dict() for _ in self.cuts]
        self.rebalances = []       # dicts: rank_key, effective_key, ordinal, per-cut results

    # -- section 3.3 ------------------------------------------------------------------
    def observe_session(self, key: int, rows):
        """rows: list of (id, close, volume, shares, gics) in feed order."""
        if self.session_keys:
            require(key > self.session_keys[-1], "session out of order")
        require(key < self.end_exclusive, "session at/after validation boundary")
        ids = [r[0] for r in rows]
        require(all(isinstance(i, int) and i > 0 for i in ids), "id must be a positive i64")
        require(len(set(ids)) == len(ids), "duplicate id in session")
        s = len(self.session_keys)
        k = s % self.window
        for name in self.names:                     # sweep: absent IDs read NaN this session
            name.ring[k] = NAN
        valid_count = 0
        for ident, close, volume, shares, gics in rows:
            slot = self.slot_of.get(ident)
            if slot is None:
                slot = len(self.names)
                self.slot_of[ident] = slot
                self.names.append(Name(ident, slot, s, self.window))
            name = self.names[slot]
            if bar_is_valid(close, volume):
                dv = close * volume                 # ONE binary64 product, stored before use
                name.ring[k] = dv
                name.last_bar = s
                if name.first_bar is None:
                    name.first_bar = s
                name.last_close = close
                name.last_shares = NAN if shares is None else shares
                name.last_gics = NAN if gics is None else gics
                name.valid_sessions.append(s)
                valid_count += 1
        self.session_keys.append(key)
        self.ids_with_valid_bar.append(valid_count)
        if self.rebalances and self.rebalances[-1]["effective_key"] == 0:
            self.rebalances[-1]["effective_key"] = key          # R15-4 / DR15-3
            self.rebalances[-1]["effective_ordinal"] = s

    # -- section 3.4 ------------------------------------------------------------------
    def rebalance(self, key: int) -> dict:
        require(self.session_keys and key == self.session_keys[-1], "rank session has no data")
        if self.rebalances:
            require(key > self.rebalances[-1]["rank_key"], "rank key not increasing")
        rank_ordinal = len(self.session_keys) - 1
        r = len(self.rebalances)
        ranked = []
        for name in self.names:
            valid = [x for x in name.ring if not math.isnan(x)]
            valid_count = len(valid)
            adv = median_binary64(valid) if valid_count > 0 else NAN
            bar_on_rank = name.last_bar == rank_ordinal
            eligible = (bar_on_rank and name.last_close > self.min_price
                        and valid_count >= self.min_valid and adv >= self.min_adv)
            if eligible:
                ranked.append({"slot": name.slot, "security_id": name.id, "adv": adv,
                               "valid_count": valid_count, "raw_close": name.last_close,
                               "gics": name.last_gics})
        # Total order: adv descending, then first-seen slot ascending (adv >= 0 always here,
        # so negating the float is an exact reversal).
        ranked.sort(key=lambda row: (-row["adv"], row["slot"]))
        for position, row in enumerate(ranked):
            row["rank"] = position + 1
        by_slot = {row["slot"]: row for row in ranked}
        result = {"index": r, "rank_key": key, "rank_ordinal": rank_ordinal,
                  "effective_key": 0, "effective_ordinal": None,
                  "ids_seen": len(self.names),
                  "ids_with_valid_bar": self.ids_with_valid_bar[rank_ordinal],
                  "eligible": len(ranked), "ranked": ranked, "cuts": []}
        for c, (ti, bi) in enumerate(self.cuts):
            n = self.top_n[ti]
            k_band = n + (n * self.band_bp[bi]) // 10000
            incumbents = self.members[c]
            chosen = []
            chosen_slots = set()
            for row in ranked:                                   # 3a keep
                if row["slot"] in incumbents and row["rank"] <= k_band:
                    if len(chosen) == n:
                        break
                    chosen.append((row, STATUS_KEEP))
                    chosen_slots.add(row["slot"])
            for row in ranked:                                   # 3b fill
                if len(chosen) == n:
                    break
                if row["slot"] in chosen_slots:
                    continue
                status = STATUS_KEEP if row["slot"] in incumbents else STATUS_ADD
                chosen.append((row, status))
                chosen_slots.add(row["slot"])
            drops = []                                           # 3c drops, slot ascending
            for slot in sorted(incumbents):
                if slot in chosen_slots:
                    continue
                name = self.names[slot]
                kind = DROP_LAST_BAR if (name.last_bar is None or name.last_bar < rank_ordinal) \
                    else DROP_RANK
                drops.append((slot, kind))
                self.last_drop_kind[c][slot] = kind
            chosen.sort(key=lambda pair: pair[0]["rank"])        # 3f output order
            churn = {"adds": sum(1 for _, st in chosen if st == STATUS_ADD),
                     "drops_rank": sum(1 for _, kd in drops if kd == DROP_RANK),
                     "drops_last_bar": sum(1 for _, kd in drops if kd == DROP_LAST_BAR),
                     "kept": sum(1 for _, st in chosen if st == STATUS_KEEP),
                     "members": len(chosen)}
            gics_missing = sum(1 for row, _ in chosen if math.isnan(row["gics"]))
            valid_total = sum(row["valid_count"] for row, _ in chosen)
            new_members = set(chosen_slots)
            for slot in new_members:
                self.ever[c].add(slot)
                self.first_member_r[c].setdefault(slot, r)
                self.last_member_r[c][slot] = r
            self.members[c] = new_members
            result["cuts"].append({
                "top_n": n, "band_bp": self.band_bp[bi], "k_band": k_band,
                "members": [(row["security_id"], row["rank"], st, row["slot"])
                            for row, st in chosen],
                "drops": [(self.names[slot].id, kind, slot) for slot, kind in drops],
                "churn": churn, "gics_missing_members": gics_missing,
                "valid_observations_total": valid_total,
            })
        self.rebalances.append(result)
        return result

    # -- section 4.3 ------------------------------------------------------------------
    def coverage_by_year(self):
        years = sorted({year_of(k) for k in self.session_keys})
        rows = []
        for year in years:
            ordinals = [s for s, k in enumerate(self.session_keys) if year_of(k) == year]
            ids_seen = sum(1 for name in self.names
                           if any(year_of(self.session_keys[s]) == year
                                  for s in name.valid_sessions))
            rebs = [rb for rb in self.rebalances if year_of(rb["rank_key"]) == year]
            row = {"year": year, "sessions": len(ordinals), "ids_seen": ids_seen,
                   "ids_with_valid_bar_median_x2":
                       median_x2_of_integers(self.ids_with_valid_bar[s] for s in ordinals),
                   "rebalances": len(rebs),
                   "eligible_median_x2": median_x2_of_integers(rb["eligible"] for rb in rebs)
                   if rebs else None,
                   "cuts": []}
            for c in range(len(self.cuts)):
                row["cuts"].append({
                    "members_median_x2": median_x2_of_integers(
                        rb["cuts"][c]["churn"]["members"] for rb in rebs) if rebs else None,
                    "gics_missing_members_median_x2": median_x2_of_integers(
                        rb["cuts"][c]["gics_missing_members"] for rb in rebs) if rebs else None,
                })
            rows.append(row)
        return rows

    # -- section 4.4 ------------------------------------------------------------------
    def union_by_year(self):
        emitted = [rb for rb in self.rebalances if rb["effective_key"] != 0]
        years = sorted({year_of(rb["effective_key"]) for rb in emitted})
        rows = []
        cumulative = [set() for _ in self.cuts]
        for year in years:
            row = {"year": year, "cuts": []}
            for c in range(len(self.cuts)):
                distinct = set()
                for rb in emitted:
                    if year_of(rb["effective_key"]) == year:
                        distinct.update(m[3] for m in rb["cuts"][c]["members"])
                cumulative[c].update(distinct)
                row["cuts"].append({"distinct": len(distinct), "cumulative": len(cumulative[c])})
            rows.append(row)
        return rows

    # -- section 4.5 ------------------------------------------------------------------
    def exits(self, c: int):
        require(self.rebalances, "exits without a rebalance")
        final_members = self.members[c]
        rows = []
        for slot in sorted(self.ever[c], key=lambda sl: self.names[sl].id):
            name = self.names[slot]
            if slot in final_members:
                kind = EXIT_WINDOW_END
            else:
                kind = EXIT_RANK_DROP if self.last_drop_kind[c][slot] == DROP_RANK \
                    else EXIT_LAST_BAR_WITHIN_WINDOW
            rows.append({"security_id": name.id, "first_seen": name.first_seen,
                         "first_bar": name.first_bar, "last_bar": name.last_bar,
                         "first_member_rebalance": self.first_member_r[c][slot],
                         "last_member_rebalance": self.last_member_r[c][slot],
                         "exit_kind": kind})
        return rows

    # -- section 4.6 ------------------------------------------------------------------
    def survivorship(self):
        final = len(self.session_keys) - 1
        first_of_year, last_of_year = {}, {}
        for s, k in enumerate(self.session_keys):
            y = year_of(k)
            first_of_year.setdefault(y, s)
            last_of_year[y] = s
        cuts = []
        for c in range(len(self.cuts)):
            ever = self.ever[c]
            ended = sum(1 for slot in ever if self.names[slot].last_bar < final)
            per_year = []
            for y in sorted(first_of_year):
                rb = next((rb for rb in self.rebalances
                           if rb["effective_ordinal"] == first_of_year[y]), None)
                if rb is None:
                    continue    # no rebalance is effective on this year's first session
                members = [m[3] for m in rb["cuts"][c]["members"]]
                exited = sum(1 for slot in members if self.names[slot].last_bar < last_of_year[y])
                per_year.append({"year": y, "members_at_first_rebalance": len(members),
                                 "exited_within_year": exited})
            cuts.append({"ever_members": len(ever), "ended_before_window_end": ended,
                         "censored": len(ever) - ended, "per_year": per_year})
        return cuts


# --------------------------------------------------------------------------- #
# Section 4.7 -- membership.bin (little-endian, FNV-1a-64 trailer)
# --------------------------------------------------------------------------- #

def fnv1a64(data: bytes) -> int:
    h = FNV_OFFSET
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME) & M64
    return h


def encode_membership_bin(builder: Builder) -> bytes:
    out = bytearray(BIN_MAGIC)
    out += struct.pack("<I", BIN_VERSION)
    out += struct.pack("<I", builder.window)
    out += struct.pack("<I", builder.min_valid)
    out += struct.pack("<d", builder.min_price)
    out += struct.pack("<I", len(builder.top_n))
    for n in builder.top_n:
        out += struct.pack("<I", n)
    out += struct.pack("<I", len(builder.band_bp))
    for b in builder.band_bp:
        out += struct.pack("<I", b)
    out += struct.pack("<I", len(builder.rebalances))
    for rb in builder.rebalances:
        require(rb["effective_key"] > 0, "encode refuses a pending rebalance (Internal)")
        out += struct.pack("<q", rb["rank_key"])
        out += struct.pack("<q", rb["effective_key"])
        for cut in rb["cuts"]:
            rows = sorted(cut["members"], key=lambda m: m[0])     # security_id ascending
            out += struct.pack("<I", len(rows))
            for sid, _rank, _st, _slot in rows:
                out += struct.pack("<q", sid)
            for _sid, rank, _st, _slot in rows:
                out += struct.pack("<I", rank)
    out += struct.pack("<Q", fnv1a64(bytes(out)))
    return bytes(out)


def decode_membership_bin(blob: bytes) -> dict:
    require(len(blob) >= 8 + 4 + 4 + 4 + 8 + 4 + 4 + 4 + 8, "short buffer")
    require(blob[:8] == BIN_MAGIC, "bad magic")
    body, trailer = blob[:-8], struct.unpack("<Q", blob[-8:])[0]
    require(fnv1a64(body) == trailer, "trailer mismatch")
    pos = 8

    def take(fmt):
        nonlocal pos
        size = struct.calcsize(fmt)
        require(pos + size <= len(body), "short buffer")
        value = struct.unpack(fmt, body[pos:pos + size])
        pos += size
        return value[0] if len(value) == 1 else value

    version = take("<I")
    require(version == BIN_VERSION, "bad version")
    image = {"adv_window": take("<I"), "min_valid_observations": take("<I"),
             "min_raw_price_exclusive": take("<d")}
    t = take("<I")
    image["top_n"] = [take("<I") for _ in range(t)]
    b = take("<I")
    image["band_bp"] = [take("<I") for _ in range(b)]
    r = take("<I")
    image["rebalances"] = []
    for _ in range(r):
        entry = {"rank_session_key": take("<q"), "effective_session_key": take("<q"), "cuts": []}
        for _c in range(t * b):
            n = take("<I")
            ids = [take("<q") for _ in range(n)]
            ranks = [take("<I") for _ in range(n)]
            entry["cuts"].append({"ids": ids, "ranks": ranks})
        image["rebalances"].append(entry)
    require(pos == len(body), "trailing bytes")
    image["fnv1a64"] = trailer
    return image


# --------------------------------------------------------------------------- #
# Fixture encoding shared with the native side (documented in the output)
# --------------------------------------------------------------------------- #

INPUT_ENCODING = {
    "config": "adv_window, min_valid_observations, min_raw_price_exclusive, min_adv_usd, "
              "top_n[] (ascending), band_bp[] (ascending) -- PitUniverseConfig",
    "sessions": "EITHER first_session_date + session_count (session s has key "
                "date_to_nanos(first_session_date) + s * 86400e9, consecutive UTC midnights) "
                "OR session_dates[] (explicit YYYY-MM-DD, ascending); ordinals are 0-based",
    "rank_sessions": "session ordinals at which rebalance(key_of(ordinal)) is called, after "
                     "that session's observe_session and before the next",
    "names": "fed to observe_session in list order among the names PRESENT on a session; "
             "each name: id (positive i64), bars[] of [from_ordinal, to_ordinal, close, "
             "volume] inclusive segments, overrides[] of [ordinal, close, volume] applied "
             "after the segments (a name is present on an override ordinal), absent[] "
             "ordinals removed last, shares and gics constants for every present bar",
    "non_finite": "closes/volumes carried as the strings \"nan\", \"inf\", \"-inf\"; every "
                  "other value is a JSON number read as binary64",
    "unspecified": "a name is absent from every session its bars/overrides do not cover; "
                   "shares/gics \"nan\" mean the spans are empty (NaN)",
    "target_security_id": "F1/F2 only: the name whose eligible/rank/adv63_usd/"
                          "valid_observations are exported",
}


def num(value):
    if isinstance(value, str):
        return {"nan": NAN, "inf": math.inf, "-inf": -math.inf}[value]
    return float(value)


def session_keys_of(inputs: dict):
    if "session_dates" in inputs:
        keys = [date_to_nanos(d) for d in inputs["session_dates"]]
    else:
        base = date_to_nanos(inputs["first_session_date"])
        keys = [base + s * NS_PER_DAY for s in range(inputs["session_count"])]
    require(all(keys[i] < keys[i + 1] for i in range(len(keys) - 1)), "session keys ascending")
    return keys


def expand_name(spec: dict, session_count: int):
    """ordinal -> (close, volume) for every session the name is present on."""
    bars = {}
    for from_s, to_s, close, volume in spec.get("bars", []):
        for s in range(int(from_s), int(to_s) + 1):
            bars[s] = (num(close), num(volume))
    for s, close, volume in spec.get("overrides", []):
        bars[int(s)] = (num(close), num(volume))
    for s in spec.get("absent", []):
        bars.pop(int(s), None)
    require(all(0 <= s < session_count for s in bars), f"name {spec['id']}: ordinal out of range")
    return bars


def run_fixture(inputs: dict) -> Builder:
    cfg = inputs["config"]
    builder = Builder(cfg["adv_window"], cfg["min_valid_observations"],
                      cfg["min_raw_price_exclusive"], cfg["min_adv_usd"],
                      cfg["top_n"], cfg["band_bp"])
    keys = session_keys_of(inputs)
    names = [(spec["id"], expand_name(spec, len(keys)), num(spec.get("shares", "nan")),
              num(spec.get("gics", "nan"))) for spec in inputs["names"]]
    rank_at = set(int(s) for s in inputs.get("rank_sessions", []))
    for s, key in enumerate(keys):
        rows = [(ident, bars[s][0], bars[s][1], shares, gics)
                for ident, bars, shares, gics in names if s in bars]
        builder.observe_session(key, rows)
        if s in rank_at:
            builder.rebalance(key)
    return builder


def config(top_n, band_bp, window=K_ADV_WINDOW, min_valid=K_MIN_VALID):
    return {"adv_window": window, "min_valid_observations": min_valid,
            "min_raw_price_exclusive": K_MIN_RAW_PRICE_EXCLUSIVE, "min_adv_usd": K_MIN_ADV_USD,
            "top_n": list(top_n), "band_bp": list(band_bp)}


def name(ident, bars, overrides=(), absent=(), shares="nan", gics="nan"):
    return {"id": ident, "bars": [list(b) for b in bars], "overrides": [list(o) for o in overrides],
            "absent": list(absent), "shares": shares, "gics": gics}


def fixture(top_n, band_bp, names, session_count, rank_sessions, window=K_ADV_WINDOW,
            min_valid=K_MIN_VALID, target=None, first_session_date="2013-01-02"):
    inputs = {"config": config(top_n, band_bp, window, min_valid),
              "first_session_date": first_session_date, "session_count": session_count,
              "rank_sessions": list(rank_sessions), "names": names}
    if target is not None:
        inputs["target_security_id"] = target
    return inputs


def case(case_id: str, family: str, inputs: dict, expected: dict, note: str,
         design_refs) -> dict:
    return {"case_id": case_id, "family": family, "design": list(design_refs), "note": note,
            "inputs": inputs, "expected": expected}


# ---- exporters ---------------------------------------------------------------------------

def export_cut(cut: dict) -> dict:
    return {
        "members": [whole(m[0]) for m in cut["members"]],
        "ranks": [whole(m[1]) for m in cut["members"]],
        "status": [whole(m[2]) for m in cut["members"]],
        "drops": [[whole(d[0]), whole(d[1])] for d in cut["drops"]],
        "churn": {k: whole(v) for k, v in cut["churn"].items()},
    }


def export_target(builder: Builder, rb: dict, target: int) -> dict:
    row = next((r for r in rb["ranked"] if r["security_id"] == target), None)
    out = {"eligible": whole(1 if row else 0),
           "rank": whole(row["rank"] if row else NOT_RANKED),
           "eligible_count": whole(rb["eligible"]),
           "ranked_ids": [whole(r["security_id"]) for r in rb["ranked"]]}
    if row:
        out["adv63_usd"] = real(row["adv"])
        out["valid_observations"] = whole(row["valid_count"])
    return out


# --------------------------------------------------------------------------- #
# F1 -- ADV window, gaps, ties, median (design 9.1 cases 1, 2, 2b, 3, 4, 5)
# --------------------------------------------------------------------------- #

def f1_cases():
    cases = []
    refs = ("section 2.3", "section 3.4 step 1", "section 3.5", "section 9.1 #1-#5")

    # 9.1 #1 -- late entrant at session 10; 63 / 62 / 56 observations at ranks 72 / 71 / 65.
    for rank_s, expect_valid, expect_eligible, tag in ((72, 63, 1, "rank72_63valid_eligible"),
                                                        (71, 62, 1, "rank71_62valid_eligible"),
                                                        (65, 56, 0, "rank65_56valid_ineligible")):
        inputs = fixture([1], [0], [name(30, [[10, rank_s, 2.0, 100.0]])], rank_s + 1, [rank_s],
                         target=30)
        b = run_fixture(inputs)
        rb = b.rebalances[0]
        values = export_target(b, rb, 30)
        require(values["eligible"]["integer"] == expect_eligible, f"F1 late entrant {tag}")
        if expect_eligible:
            require(values["valid_observations"]["integer"] == expect_valid, f"F1 {tag} valid")
            require(values["adv63_usd"]["binary64"] == 200.0, f"F1 {tag} adv")
        cases.append(case(f"f1_late_entrant_{tag}", "F1", inputs, values,
                          f"9.1 #1: ID enters at session 10; rank at {rank_s} -> "
                          f"{expect_valid} valid observations, eligible={expect_eligible}", refs))

    # 9.1 #2 -- 63 bars with 7 missing -> 56, ineligible; 6 missing -> 57, adv = median of 57.
    seven = [5, 12, 19, 26, 33, 40, 47]
    inputs = fixture([1], [0], [name(7, [[0, 62, 2.0, 100.0]], absent=seven)], 63, [62], target=7)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 7)
    require(values["eligible"]["integer"] == 0 and values["eligible_count"]["integer"] == 0,
            "F1 gap 7 missing must be ineligible")
    cases.append(case("f1_gap_seven_missing_56_ineligible", "F1", inputs, values,
                      "9.1 #2: 63 sessions, absent on 7 -> 56 valid < 57 -> ineligible", refs))

    six = seven[:-1]
    # dv on session s = close (s + 2) * volume 1 = s + 2, so the 57 valid values are
    # {2..64} minus {7,14,21,28,35,42}; sorted index 28 is 34 (28 values lie below it).
    inputs = fixture([1], [0], [name(7, [[s, s, float(s + 2), 1.0] for s in range(63)],
                                     absent=six)], 63, [62], target=7)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 7)
    require(values["eligible"]["integer"] == 1, "F1 gap 6 missing must be eligible")
    require(values["valid_observations"]["integer"] == 57, "F1 gap 6: 57 valid")
    require(values["adv63_usd"]["binary64"] == 34.0, "F1 gap 6: median of the 57 is 34")
    cases.append(case("f1_gap_six_missing_57_eligible_median_of_57", "F1", inputs, values,
                      "9.1 #2: absent on 6 -> 57 valid; dv(s) = s + 2; median of the 57 "
                      "remaining values is 34.0 (hand-derived)", refs))

    # 9.1 #2b -- every invalid shape is a missing bar; volume 0 is valid.
    overrides = [[1, "nan", 100.0], [2, "inf", 100.0], [3, 0.0, 100.0], [4, -1.0, 100.0],
                 [5, 2.0, -1.0], [6, 2.0, "nan"], [7, 2.0, 0.0]]
    inputs = fixture([1], [0], [name(11, [[0, 62, 2.0, 100.0]], overrides=overrides)], 63, [62],
                     target=11)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 11)
    require(values["eligible"]["integer"] == 1, "F1 2b eligible")
    require(values["valid_observations"]["integer"] == 57, "F1 2b: 63 - 6 invalid = 57")
    require(values["adv63_usd"]["binary64"] == 200.0, "F1 2b: median of {0, 200 x 56} = 200")
    cases.append(case("f1_bar_validity_every_invalid_shape_is_missing", "F1", inputs, values,
                      "9.1 #2b: close NaN/+inf/0/-1 and volume -1/NaN each remove one "
                      "observation (6); volume 0 on session 7 stays valid with dv 0", refs))

    # 9.1 #3 -- all 63 bars volume 0 -> adv 0.0, eligible, ranks last among positive keys.
    inputs = fixture([2], [0], [name(1, [[0, 62, 2.0, 0.0]]), name(2, [[0, 62, 2.0, 100.0]])],
                     63, [62], target=1)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 1)
    require(values["eligible"]["integer"] == 1 and values["rank"]["integer"] == 2, "F1 zero vol")
    require(values["adv63_usd"]["binary64"] == 0.0, "F1 zero volume adv 0.0")
    cases.append(case("f1_zero_volume_adv_zero_eligible_ranks_last", "F1", inputs, values,
                      "9.1 #3: volume 0 is valid; adv == 0.0 passes min_adv_usd 0; rank 2 "
                      "behind the positive key", refs))

    # 9.1 #4 -- even count 58: (29 + 30) * 0.5 == 29.5; odd count 57 -> 29.
    # close 2.0, volume k/2 => dv == k exactly in binary64 for k = 1..58.
    bars58 = [[s, s, 2.0, (s - 4) / 2.0] for s in range(5, 63)]
    inputs = fixture([1], [0], [name(4, bars58)], 63, [62], target=4)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 4)
    require(values["valid_observations"]["integer"] == 58, "F1 even: 58 valid")
    require(values["adv63_usd"]["binary64"] == 29.5, "F1 even median 29.5")
    require(values["adv63_usd"]["hex"] == (29.5).hex(), "F1 even hex")
    cases.append(case("f1_median_even_58_is_half_sum", "F1", inputs, values,
                      "9.1 #4: 58 values 1..58 -> (29 + 30) * 0.5 == 29.5", refs))

    bars57 = [[s, s, 2.0, (s - 5) / 2.0] for s in range(6, 63)]
    inputs = fixture([1], [0], [name(4, bars57)], 63, [62], target=4)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 4)
    require(values["valid_observations"]["integer"] == 57, "F1 odd: 57 valid")
    require(values["adv63_usd"]["binary64"] == 29.0, "F1 odd median 29")
    cases.append(case("f1_median_odd_57_is_middle", "F1", inputs, values,
                      "9.1 #4: 57 values 1..57 -> v[28] == 29.0", refs))

    # 9.1 #5 -- identical dv, fed 30, 10, 20 -> ranks 1, 2, 3 by first-seen slot.
    inputs = fixture([3], [0], [name(30, [[0, 62, 2.0, 100.0]]), name(10, [[0, 62, 2.0, 100.0]]),
                                name(20, [[0, 62, 2.0, 100.0]])], 63, [62], target=10)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 10)
    require([w["integer"] for w in values["ranked_ids"]] == [30, 10, 20], "F1 tie order")
    require(values["rank"]["integer"] == 2, "F1 tie: id 10 is slot 1 -> rank 2")
    cases.append(case("f1_ties_break_by_first_seen_slot_ascending", "F1", inputs, values,
                      "9.1 #5: exact ties -> slot order; ranked_ids [30, 10, 20]; target 10 "
                      "has rank 2", refs))
    return cases


# --------------------------------------------------------------------------- #
# F2 -- price floor and rank-session bar (design 9.1 cases 6, 7)
# --------------------------------------------------------------------------- #

def f2_cases():
    refs = ("section 3.4 step 1", "section 6 Price floor", "section 9.1 #6-#7")
    cases = []
    inputs = fixture([1], [0], [name(6, [[0, 62, 1.0, 100.0]])], 63, [62], target=6)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 6)
    require(values["eligible"]["integer"] == 0, "F2 close 1.0 fails the strict floor")
    cases.append(case("f2_price_floor_close_exactly_one_ineligible", "F2", inputs, values,
                      "9.1 #6: raw close 1.0 on the rank session is NOT > 1.0", refs))

    above = 1.0000000000000002
    require(above > 1.0 and above == math.nextafter(1.0, 2.0), "nextafter(1.0, 2.0)")
    inputs = fixture([1], [0], [name(6, [[0, 62, above, 100.0]])], 63, [62], target=6)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 6)
    require(values["eligible"]["integer"] == 1 and values["rank"]["integer"] == 1, "F2 nextafter")
    cases.append(case("f2_price_floor_nextafter_one_eligible", "F2", inputs, values,
                      "9.1 #6: std::nextafter(1.0, 2.0) == 1.0000000000000002 passes", refs))

    inputs = fixture([1], [0], [name(6, [[0, 62, 2.0, 100.0]])], 64, [63], target=6)
    b = run_fixture(inputs)
    values = export_target(b, b.rebalances[0], 6)
    require(values["eligible"]["integer"] == 0, "F2 absent on rank session")
    cases.append(case("f2_absent_on_rank_session_ineligible", "F2", inputs, values,
                      "9.1 #7: 63 valid bars (0..62) then absent on the rank session 63 -> "
                      "ineligible even though 62 valid observations remain in the window",
                      refs))
    return cases


# --------------------------------------------------------------------------- #
# F3 -- cadence and effective session (design 9.1 cases 8, 28; DR15-8)
# --------------------------------------------------------------------------- #

def f3_cases():
    refs = ("section 3.1 select_monthly_rank_sessions", "section 6 Cadence/Effective",
            "section 9.1 #8 and #28", "DR15-8")
    cases = []

    def build(case_id, dates, start, end, expect_rank_dates, note):
        keys = [date_to_nanos(d) for d in dates]
        selected = select_monthly_rank_sessions(keys, date_to_nanos(start), date_to_nanos(end))
        effective = [keys[keys.index(k) + 1] for k in selected]
        require([nanos_to_date(k) for k in selected] == expect_rank_dates, f"F3 {case_id}")
        inputs = {"session_dates": list(dates), "start_date": start, "end_date": end}
        values = {"rank_session_keys": [whole(k) for k in selected],
                  "effective_session_keys": [whole(k) for k in effective],
                  "rank_session_count": whole(len(selected))}
        return case(case_id, "F3", inputs, values, note, refs)

    five = ["2013-01-02", "2013-01-15", "2013-02-03", "2013-02-27", "2013-03-01"]
    cases.append(build("f3_last_by_data_mid_month_never_last_attached", five,
                       "2013-01-01", "2013-03-31", ["2013-01-15", "2013-02-27"],
                       "9.1 #28: January's last session BY DATA is Jan 15; Mar 1 is the last "
                       "attached session and is never selected; effective = next session"))
    cases.append(build("f3_end_filter_excludes_feb27", five, "2013-01-01", "2013-02-26",
                       ["2013-01-15"], "9.1 #28: end = Feb 26 -> {Jan 15}"))
    cases.append(build("f3_end_filter_inclusive_on_jan15", five, "2013-01-01", "2013-01-15",
                       ["2013-01-15"], "9.1 #28: end = Jan 15 is inclusive -> {Jan 15}"))
    cases.append(build("f3_start_filter_excludes_jan15", five, "2013-01-16", "2013-03-31",
                       ["2013-02-27"], "start = Jan 16 -> {Feb 27}; the [start, end] filter "
                       "is applied AFTER month-boundary detection"))
    gap = ["2013-01-30", "2013-01-31", "2013-02-03", "2013-02-28", "2013-03-04"]
    cases.append(build("f3_effective_is_next_observed_session_weekend_gap", gap,
                       "2013-01-01", "2013-02-28", ["2013-01-31", "2013-02-28"],
                       "9.1 #8: rank Jan 31 -> effective Feb 3 (k + 3 days); rank Feb 28 -> "
                       "effective Mar 4 (k + 4 days); never interpolated"))
    return cases


# --------------------------------------------------------------------------- #
# Two-regime fixtures for F4 / F5 / F7 / F8 / F9: regime 0 = sessions 0..62 (rebalance at
# 62), regime 1 = sessions 63..125 (rebalance at 125), then session 126 so the second
# rebalance has an effective session (DR15-3). Every dv in the window at rebalance 1 comes
# from regime 1, so the two ranked orders are exactly the declared dv orders.
# --------------------------------------------------------------------------- #

R0_END, R1_END, TWO_REGIME_SESSIONS = 62, 125, 127


def two_regime(ident, dv0, dv1, overrides=(), absent=(), gics="nan"):
    """dv = close * volume with close 2.0, volume dv/2 (exact for the values used)."""
    bars = []
    if dv0 is not None:
        bars.append([0, R0_END, 2.0, dv0 / 2.0])
    if dv1 is not None:
        bars.append([R0_END + 1, R1_END + 1, 2.0, dv1 / 2.0])
    return name(ident, bars, overrides=overrides, absent=absent, gics=gics)


def export_rebalances(builder: Builder, cut: int = 0) -> dict:
    return {"rebalances": [export_cut(rb["cuts"][cut]) for rb in builder.rebalances]}


def ids_of(cut_export):
    return [w["integer"] for w in cut_export["members"]]


# --------------------------------------------------------------------------- #
# F4 -- banding threshold (design 9.1 case 9)
# --------------------------------------------------------------------------- #

def f4_cases():
    refs = ("section 3.4 step 3a-3b", "section 6 Band rule", "section 9.1 #9")
    cases = []

    def twelve(rank11_id_dv, rank12_id_dv, band):
        # regime 0: ids 1..12 with dv 1300 - 100 i -> ranks 1..12; members = ids 1..10.
        # regime 1: ids 1..9 unchanged; id 11 -> 250 (rank 10); id 10 / id 12 per argument.
        names = []
        for i in range(1, 13):
            dv1 = 1300.0 - 100.0 * i
            if i == 11:
                dv1 = 250.0
            elif i == 10:
                dv1 = rank11_id_dv if rank11_id_dv is not None else dv1
            elif i == 12:
                dv1 = rank12_id_dv
            names.append(two_regime(i, 1300.0 - 100.0 * i, dv1))
        return fixture([10], [band], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])

    # (a) K_band = 11: incumbent id 10 falls to rank 11 -> Keep; nobody added.
    inputs = twelve(200.0, 100.0, 1000)
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require(ids_of(r1) == [1, 2, 3, 4, 5, 6, 7, 8, 9, 10], "F4a members")
    require([w["integer"] for w in r1["ranks"]] == [1, 2, 3, 4, 5, 6, 7, 8, 9, 11], "F4a ranks")
    require(r1["churn"]["adds"]["integer"] == 0 and r1["churn"]["kept"]["integer"] == 10, "F4a")
    cases.append(case("f4_band1000_top10_incumbent_at_rank11_kept", "F4", inputs, values,
                      "9.1 #9: top_n 10, band_bp 1000 -> K_band 11; incumbent id 10 at rank 11 "
                      "is kept (status keep), the rank-10 entrant id 11 is not added", refs))

    # (b) incumbent id 10 falls to rank 12 -> dropped as Rank; id 11 (rank 10) added.
    inputs = twelve(50.0, 100.0, 1000)
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require(ids_of(r1) == [1, 2, 3, 4, 5, 6, 7, 8, 9, 11], "F4b members")
    require(r1["churn"]["drops_rank"]["integer"] == 1 and r1["churn"]["adds"]["integer"] == 1, "F4b")
    require([[d[0]["integer"], d[1]["integer"]] for d in r1["drops"]] == [[10, DROP_RANK]], "F4b drops")
    cases.append(case("f4_band1000_top10_incumbent_at_rank12_dropped", "F4", inputs, values,
                      "9.1 #9: incumbent id 10 at rank 12 > K_band 11 -> drops_rank 1; "
                      "id 11 at rank 10 added", refs))

    # (c) band 0: K_band = 10 -> the rank-11 incumbent is dropped.
    inputs = twelve(200.0, 100.0, 0)
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require(ids_of(r1) == [1, 2, 3, 4, 5, 6, 7, 8, 9, 11], "F4c members")
    require(r1["churn"]["drops_rank"]["integer"] == 1, "F4c drop")
    cases.append(case("f4_band0_top10_incumbent_at_rank11_dropped", "F4", inputs, values,
                      "9.1 #9: band_bp 0 -> K_band 10; the rank-11 incumbent is dropped", refs))

    # (d) top_n 2, band 1000 -> K_band = 2 + (2 * 1000) / 10000 = 2 (integer arithmetic).
    names = [two_regime(1, 300.0, 200.0), two_regime(2, 200.0, 100.0), two_regime(3, 100.0, 300.0)]
    inputs = fixture([2], [1000], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require(b.rebalances[1]["cuts"][0]["k_band"] == 2, "F4d K_band")
    require(ids_of(r1) == [3, 1], "F4d members rank order")
    require([w["integer"] for w in r1["status"]] == [STATUS_ADD, STATUS_KEEP], "F4d status")
    cases.append(case("f4_band1000_top2_kband_is_two", "F4", inputs, values,
                      "8.1 F4: top_n 2, band_bp 1000 -> K_band 2 (integer division); id 2 at "
                      "rank 3 dropped, id 3 at rank 1 added; output [3 add, 1 keep]", refs))
    return cases


# --------------------------------------------------------------------------- #
# F5 -- drop classification by formula only (design 9.1 case 11, DR15-10)
# --------------------------------------------------------------------------- #

def f5_cases():
    refs = ("section 3.4 step 3c", "section 6 Drop classification", "section 9.1 #11", "DR15-10")
    cases = []

    def shape(case_id, overrides, absent, expect_kind, note):
        names = [two_regime(1, 400.0, 400.0, overrides=overrides, absent=absent),
                 two_regime(2, 300.0, 300.0), two_regime(3, 200.0, 200.0),
                 two_regime(4, 100.0, 100.0)]
        inputs = fixture([3], [0], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
        b = run_fixture(inputs)
        values = export_rebalances(b)
        r1 = values["rebalances"][1]
        require(ids_of(r1) == [2, 3, 4], f"F5 {case_id} members")
        require([[d[0]["integer"], d[1]["integer"]] for d in r1["drops"]] == [[1, expect_kind]],
                f"F5 {case_id} drop kind")
        key = "drops_last_bar" if expect_kind == DROP_LAST_BAR else "drops_rank"
        require(r1["churn"][key]["integer"] == 1 and r1["churn"]["adds"]["integer"] == 1,
                f"F5 {case_id} churn")
        cases.append(case(case_id, "F5", inputs, values, note, refs))

    shape("f5_incumbent_absent_on_rank_session_drops_last_bar", [], [R1_END], DROP_LAST_BAR,
          "9.1 #11: incumbent id 1 absent on the rank session -> last_bar 124 < 125 -> LastBar")
    shape("f5_incumbent_close_nan_on_rank_session_drops_last_bar", [[R1_END, "nan", 200.0]], [],
          DROP_LAST_BAR, "9.1 #11: present with close NaN -> invalid bar -> last_bar 124 -> LastBar")
    shape("f5_incumbent_close_half_on_rank_session_drops_rank", [[R1_END, 0.5, 200.0]], [],
          DROP_RANK, "9.1 #11: present at close 0.5 (valid bar, floor fails) -> Rank (DR15-10)")

    # All three shapes in one rebalance; drops span slot ascending.
    names = [two_regime(1, 600.0, 600.0, absent=[R1_END]),
             two_regime(2, 500.0, 500.0, overrides=[[R1_END, "nan", 250.0]]),
             two_regime(3, 400.0, 400.0, overrides=[[R1_END, 0.5, 200.0]]),
             two_regime(4, 300.0, 300.0), two_regime(5, 200.0, 200.0), two_regime(6, 100.0, 100.0)]
    inputs = fixture([3], [0], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require(ids_of(r1) == [4, 5, 6], "F5 combined members")
    require([[d[0]["integer"], d[1]["integer"]] for d in r1["drops"]]
            == [[1, DROP_LAST_BAR], [2, DROP_LAST_BAR], [3, DROP_RANK]], "F5 combined drops")
    churn = {k: v["integer"] for k, v in r1["churn"].items()}
    require(churn == {"adds": 3, "drops_rank": 1, "drops_last_bar": 2, "kept": 0, "members": 3},
            "F5 combined churn")
    cases.append(case("f5_three_shapes_one_rebalance_drops_slot_ascending", "F5", inputs, values,
                      "9.1 #11 (M-3): absent / close NaN / close 0.5 incumbents in one rebalance; "
                      "drops [[1,1],[2,1],[3,0]] in slot order; churn adds 3, drops_rank 1, "
                      "drops_last_bar 2, kept 0", refs))
    return cases


# --------------------------------------------------------------------------- #
# F6 -- coverage / union / survivorship (design 9.1 cases 20, 20b, 21, 22, 25; DR15-15)
# --------------------------------------------------------------------------- #

F6_DATES = ["2013-01-02", "2013-01-03", "2013-06-28", "2013-12-31",
            "2014-01-02", "2014-01-03", "2014-06-30", "2014-12-31",
            "2015-01-02", "2015-01-05", "2015-06-30", "2015-12-31"]


def dvname(ident, dv, from_s, to_s, overrides=(), absent=(), gics="nan"):
    return name(ident, [[from_s, to_s, 10.0, dv / 10.0]], overrides=overrides, absent=absent,
                gics=gics)


def export_f6(builder: Builder) -> dict:
    coverage = []
    for row in builder.coverage_by_year():
        coverage.append({
            "year": whole(row["year"]), "sessions": whole(row["sessions"]),
            "ids_seen": whole(row["ids_seen"]),
            "ids_with_valid_bar_median_x2": whole(row["ids_with_valid_bar_median_x2"]),
            "rebalances": whole(row["rebalances"]),
            "eligible_median_x2": whole(row["eligible_median_x2"]),
            "cuts": [{"members_median_x2": whole(c["members_median_x2"]),
                      "gics_missing_members_median_x2": whole(c["gics_missing_members_median_x2"])}
                     for c in row["cuts"]],
        })
    union = [{"year": whole(row["year"]),
              "cuts": [{"distinct": whole(c["distinct"]), "cumulative": whole(c["cumulative"])}
                       for c in row["cuts"]]} for row in builder.union_by_year()]
    surv = [{"ever_members": whole(c["ever_members"]),
             "ended_before_window_end": whole(c["ended_before_window_end"]),
             "censored": whole(c["censored"]),
             "per_year": [{"year": whole(p["year"]),
                           "members_at_first_rebalance": whole(p["members_at_first_rebalance"]),
                           "exited_within_year": whole(p["exited_within_year"])}
                          for p in c["per_year"]]} for c in builder.survivorship()]
    rebs = [{"cuts": [{"valid_observations_total": whole(c["valid_observations_total"]),
                       "gics_missing_members": whole(c["gics_missing_members"]),
                       "members": whole(c["churn"]["members"])} for c in rb["cuts"]]}
            for rb in builder.rebalances]
    return {"coverage_by_year": coverage, "union_by_year": union,
            "survivorship": {"cuts": surv}, "rebalances": rebs}


def f6_cases():
    refs = ("sections 4.3-4.6", "section 9.1 #20, #20b, #21, #22, #25", "DR15-4", "DR15-15")
    cases = []
    # Three synthetic years, 4 sessions each, W = 3, min_valid 2, cuts top_n {3, 7}, band 0.
    # Ordinals: 0..3 = 2013, 4..7 = 2014, 8..11 = 2015. Rebalances at 3, 7, 10.
    names = [
        dvname(101, 100.0, 0, 11, gics=45.0),
        dvname(102, 90.0, 0, 11),
        dvname(103, 80.0, 0, 3, gics=20.0),                       # stops trading after 2013
        dvname(104, 95.0, 5, 11),                                 # enters 2014-01-03
        dvname(105, 70.0, 0, 11, overrides=[[7, 0.5, 140.0]], gics=35.0),   # floor fails at r1
        dvname(106, 60.0, 0, 11, overrides=[[10, 0.5, 120.0]], gics=35.0),  # floor fails at r2
        dvname(107, 40.0, 0, 9),                                  # last bar 9, absent at r2
        dvname(108, 50.0, 1, 11, overrides=[[1, "nan", 5.0]]),    # first_seen 1, first_bar 2
    ]
    inputs = {"config": config([3, 7], [0], window=3, min_valid=2), "session_dates": F6_DATES,
              "rank_sessions": [3, 7, 10], "names": names}
    b = run_fixture(inputs)
    values = export_f6(b)

    # Hand-derived checkpoints (see the report for the derivation).
    r = b.rebalances
    require([x["security_id"] for x in r[0]["ranked"]] == [101, 102, 103, 105, 106, 108, 107], "F6 r0")
    require([x["security_id"] for x in r[1]["ranked"]] == [101, 104, 102, 106, 108, 107], "F6 r1")
    require([x["security_id"] for x in r[2]["ranked"]] == [101, 104, 102, 105, 108], "F6 r2")
    cov = values["coverage_by_year"]
    require([c["ids_with_valid_bar_median_x2"]["integer"] for c in cov] == [13, 14, 13], "F6 valid x2")
    require([c["ids_seen"]["integer"] for c in cov] == [7, 7, 7], "F6 ids_seen")
    require([c["eligible_median_x2"]["integer"] for c in cov] == [14, 12, 10], "F6 eligible x2")
    require([c["cuts"][1]["members_median_x2"]["integer"] for c in cov] == [14, 12, 10], "F6 top7")
    require([c["cuts"][0]["gics_missing_members_median_x2"]["integer"] for c in cov] == [2, 4, 4],
            "F6 gics missing top3 x2")
    uni = values["union_by_year"]
    require([u["year"]["integer"] for u in uni] == [2014, 2015], "F6 union years by EFFECTIVE date")
    require([(u["cuts"][0]["distinct"]["integer"], u["cuts"][0]["cumulative"]["integer"]) for u in uni]
            == [(3, 3), (3, 4)], "F6 union top3")
    require([(u["cuts"][1]["distinct"]["integer"], u["cuts"][1]["cumulative"]["integer"]) for u in uni]
            == [(7, 7), (7, 8)], "F6 union top7")
    sv = values["survivorship"]["cuts"]
    require((sv[0]["ever_members"]["integer"], sv[0]["ended_before_window_end"]["integer"],
             sv[0]["censored"]["integer"]) == (4, 1, 3), "F6 survivorship top3")
    require((sv[1]["ever_members"]["integer"], sv[1]["ended_before_window_end"]["integer"],
             sv[1]["censored"]["integer"]) == (8, 2, 6), "F6 survivorship top7")
    require([(p["year"]["integer"], p["members_at_first_rebalance"]["integer"],
              p["exited_within_year"]["integer"]) for p in sv[1]["per_year"]]
            == [(2014, 7, 1), (2015, 6, 1)], "F6 per-year top7")
    require([c["cuts"][0]["valid_observations_total"]["integer"] for c in values["rebalances"]]
            == [9, 9, 9], "F6 valid totals top3")
    cases.append(case("f6_three_year_panel_coverage_union_survivorship", "F6", inputs, values,
                      "9.1 #20/#21/#22: 3 years x 4 sessions, W 3, min_valid 2, cuts top3/top7; "
                      "medians exported as _x2 integers (2013 and 2015 valid-bar medians are "
                      "6.5 -> 13); union years follow the EFFECTIVE session; 103 (last bar "
                      "ordinal 3) and 107 (ordinal 9) end before the window end, everything "
                      "else is censored", refs))

    # 9.1 #25 -- two IDs absent on one session thin that session only, they are not unseen.
    names = [dvname(1, 300.0, 0, 3, absent=[1]), dvname(2, 200.0, 0, 3, absent=[1]),
             dvname(3, 100.0, 0, 3)]
    inputs = {"config": config([3], [0], window=3, min_valid=2),
              "session_dates": F6_DATES[:4], "rank_sessions": [3], "names": names}
    b = run_fixture(inputs)
    values = export_f6(b)
    cov = values["coverage_by_year"][0]
    require(b.ids_with_valid_bar == [3, 1, 3, 3], "F6 #25 per-session valid")
    require(cov["ids_with_valid_bar_median_x2"]["integer"] == 6, "F6 #25 median (3+3)")
    require(cov["ids_seen"]["integer"] == 3 and cov["eligible_median_x2"]["integer"] == 6, "F6 #25")
    cases.append(case("f6_absent_ids_thin_one_session_not_unseen", "F6", inputs, values,
                      "9.1 #25: ids 1, 2 absent on session 1 -> ids_with_valid_bar [3,1,3,3], "
                      "median 3 (x2 = 6), ids_seen 3; all three eligible at the rebalance "
                      "(2 of 3 ring entries valid)", refs))

    # 9.1 #20b -- valid_observations_total and gics_missing_members are integers.
    names = [name(1, [[0, 62, 2.0, 100.0]], gics=10.0),
             name(2, [[3, 62, 2.0, 50.0]], gics=20.0),
             name(3, [[6, 62, 2.0, 25.0]])]
    inputs = fixture([3], [0], names, 63, [62])
    b = run_fixture(inputs)
    values = export_f6(b)
    cut = values["rebalances"][0]["cuts"][0]
    require(cut["valid_observations_total"]["integer"] == 63 + 60 + 57, "F6 #20b total 180")
    require(cut["gics_missing_members"]["integer"] == 1, "F6 #20b gics missing 1")
    cases.append(case("f6_valid_total_and_gics_missing_are_integers", "F6", inputs, values,
                      "9.1 #20b: members with 63 / 60 / 57 valid observations -> "
                      "valid_observations_total 180; one NaN gics -> gics_missing_members 1",
                      refs))
    return cases


# --------------------------------------------------------------------------- #
# F7 -- band retention set AND order (design 9.1 cases 9c, 10; DR15-2)
# --------------------------------------------------------------------------- #

def f7_cases():
    refs = ("section 3.4 step 3a-3f", "section 9.1 #9c and #10", "DR15-2")
    cases = []

    # 9.1 #10 (realisable reading, see report ambiguity A-2): top_n 2, band 1000 -> K_band 2.
    names = [two_regime(1, 300.0, 200.0), two_regime(2, 200.0, 100.0), two_regime(3, 100.0, 300.0)]
    inputs = fixture([2], [1000], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require(ids_of(r1) == [3, 1] and [w["integer"] for w in r1["ranks"]] == [1, 2], "F7 top2")
    require([[d[0]["integer"], d[1]["integer"]] for d in r1["drops"]] == [[2, DROP_RANK]], "F7 top2 drop")
    cases.append(case("f7_top2_band1000_outranked_incumbent_dropped_as_rank", "F7", inputs, values,
                      "9.1 #10: incumbents 1, 2 at ranks 2, 3 with K_band 2 -> the best-ranked "
                      "incumbent (rank 2) is kept, rank 3 is dropped as Rank; entrant 3 at "
                      "rank 1 is added; output order [3 add, 1 keep]", refs))

    # 9.1 #10 second half: top_n 3, band 1000 -> K_band 3; all incumbents kept, rank-4
    # entrant not added.
    names = [two_regime(1, 400.0, 200.0), two_regime(2, 300.0, 400.0), two_regime(3, 200.0, 300.0),
             two_regime(4, 100.0, 150.0)]
    inputs = fixture([3], [1000], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require(ids_of(r1) == [2, 3, 1], "F7 top3 members")
    require([w["integer"] for w in r1["status"]] == [STATUS_KEEP] * 3, "F7 top3 all keep")
    require(r1["churn"]["adds"]["integer"] == 0 and not r1["drops"], "F7 top3 no churn")
    cases.append(case("f7_top3_band1000_all_incumbents_kept_rank4_entrant_not_added", "F7",
                      inputs, values, "9.1 #10: K_band 3; incumbents at ranks 1, 2, 3 (ids "
                      "2, 3, 1) all kept, id 4 at rank 4 not added; output in rank order",
                      refs))

    # 9.1 #9c / C-2 shape: top_n 10, band 1000 (K_band 11), incumbents at ranks 5 and 11.
    dv0 = {i: 2000.0 - 100.0 * i for i in range(1, 11)}           # ids 1..10 ranks 1..10
    dv0.update({i: 1000.0 - 50.0 * (i - 10) for i in range(11, 21)})  # ids 11..20 ranks 11..20
    dv1 = {11: 2000.0, 12: 1900.0, 13: 1800.0, 14: 1700.0, 5: 1600.0,
           15: 1500.0, 16: 1400.0, 17: 1300.0, 18: 1200.0, 19: 1100.0, 10: 1000.0,
           1: 900.0, 2: 800.0, 3: 700.0, 4: 600.0, 6: 500.0, 7: 400.0, 8: 300.0, 9: 200.0,
           20: 100.0}
    names = [two_regime(i, dv0[i], dv1[i]) for i in range(1, 21)]
    inputs = fixture([10], [1000], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
    b = run_fixture(inputs)
    values = export_rebalances(b)
    r1 = values["rebalances"][1]
    require([w["integer"] for w in r1["ranks"]] == [1, 2, 3, 4, 5, 6, 7, 8, 9, 11], "F7 C-2 ranks")
    require(ids_of(r1) == [11, 12, 13, 14, 5, 15, 16, 17, 18, 10], "F7 C-2 members")
    require([w["integer"] for w in r1["status"]]
            == [0, 0, 0, 0, 1, 0, 0, 0, 0, 1], "F7 C-2 statuses add x4, keep, add x4, keep")
    churn = {k: v["integer"] for k, v in r1["churn"].items()}
    require(churn == {"adds": 8, "drops_rank": 8, "drops_last_bar": 0, "kept": 2, "members": 10},
            "F7 C-2 churn")
    require([d[0]["integer"] for d in r1["drops"]] == [1, 2, 3, 4, 6, 7, 8, 9], "F7 C-2 drops")
    cases.append(case("f7_c2_shape_members_rank_ascending_not_keep_then_fill", "F7", inputs,
                      values, "9.1 #9c (C-2, DR15-2): incumbents at ranks 5 (id 5) and 11 (id "
                      "10) kept, fill stops after rank 9 -> members ranks [1..9, 11], statuses "
                      "add x4, keep, add x4, keep; the rank-10 entrant id 19 is NOT added", refs))
    return cases


# --------------------------------------------------------------------------- #
# F8 -- membership.bin layout (design 4.7, 9.1 case 27, DR15-6)
# --------------------------------------------------------------------------- #

def export_f8(builder: Builder) -> dict:
    blob = encode_membership_bin(builder)
    image = decode_membership_bin(blob)                      # round-trip self-check
    require(len(image["rebalances"]) == len(builder.rebalances), "F8 round-trip count")
    for rb, decoded in zip(builder.rebalances, image["rebalances"]):
        require(decoded["rank_session_key"] == rb["rank_key"], "F8 rank key")
        require(decoded["effective_session_key"] == rb["effective_key"], "F8 effective key")
        for cut, dcut in zip(rb["cuts"], decoded["cuts"]):
            rows = sorted(cut["members"], key=lambda m: m[0])
            require(dcut["ids"] == [m[0] for m in rows], "F8 ids ascending")
            require(dcut["ranks"] == [m[1] for m in rows], "F8 parallel ranks")
    return {"sha256_hex": text(hashlib.sha256(blob).hexdigest()),
            "byte_length": whole(len(blob)),
            "fnv1a64_trailer": u64(image["fnv1a64"]),
            "rebalance_count": whole(len(builder.rebalances)),
            "header_bytes_hex": text(blob[:8 + 4 + 4 + 4 + 8].hex())}


def f8_cases():
    refs = ("section 4.7", "section 9.1 #27", "DR15-6")
    cases = []
    # 2 rebalances x 2 cuts (top_n {2, 3}, band {0}); ids deliberately out of order so the
    # id-ascending + parallel-rank layout is exercised: rank order at r0 is 9, 5, 7, 3.
    names = [two_regime(9, 400.0, 100.0), two_regime(5, 300.0, 400.0),
             two_regime(7, 200.0, 300.0), two_regime(3, 100.0, 200.0)]
    inputs = fixture([2, 3], [0], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
    b = run_fixture(inputs)
    values = export_f8(b)
    blob = encode_membership_bin(b)
    require(len(blob) == 8 + 4 + 4 + 4 + 8 + (4 + 2 * 4) + (4 + 4) + 4
            + 2 * (8 + 8 + (4 + 2 * 12) + (4 + 3 * 12)) + 8, "F8 byte length by hand")
    require(values["byte_length"]["integer"] == 228, "F8 byte length 228")
    require(b.rebalances[0]["effective_key"] > 0 and b.rebalances[1]["effective_key"] > 0, "F8 eff")
    image = decode_membership_bin(blob)
    require([c["ids"] for c in image["rebalances"][0]["cuts"]] == [[5, 9], [5, 7, 9]], "F8 r0 ids")
    require([c["ranks"] for c in image["rebalances"][0]["cuts"]] == [[2, 1], [2, 3, 1]], "F8 r0 ranks")
    require([c["ids"] for c in image["rebalances"][1]["cuts"]] == [[5, 7], [3, 5, 7]], "F8 r1 ids")
    require([c["ranks"] for c in image["rebalances"][1]["cuts"]] == [[1, 2], [3, 1, 2]], "F8 r1 ranks")
    cases.append(case("f8_membership_bin_two_rebalances_two_cuts", "F8", inputs, values,
                      "4.7 layout: 52 B through R (magic, version, W 63, min_valid 57, floor "
                      "1.0, T 2 [2,3], B 1 [0], R 2) + 2 x (16 + (4+24) + (4+36)) + 8-byte "
                      "FNV-1a-64 trailer = 228 bytes; r0 ranking 9,5,7,3 -> top2 ids [5,9] ranks "
                      "[2,1], top3 ids [5,7,9] ranks [2,3,1]; r1 ranking 5,7,3,9 -> top2 ids "
                      "[5,7] ranks [1,2], top3 ids [3,5,7] ranks [3,1,2]", refs))

    # Short lists: one eligible name only -> both cuts carry a single member.
    names = [two_regime(42, 500.0, 500.0), two_regime(41, None, 100.0)]  # 41 enters late: 63 valid at r1
    inputs = fixture([2, 3], [0], names, TWO_REGIME_SESSIONS, [R0_END, R1_END])
    b = run_fixture(inputs)
    values = export_f8(b)
    require([m[0] for m in b.rebalances[0]["cuts"][0]["members"]] == [42], "F8 short r0")
    require([m[0] for m in b.rebalances[1]["cuts"][0]["members"]] == [42, 41], "F8 short r1")
    cases.append(case("f8_membership_bin_short_lists_below_top_n", "F8", inputs, values,
                      "4.7 / 9.1 #9b: r0 has one eligible name so member_count 1 < top_n in "
                      "both cuts; r1 has two (id 41 enters at session 63 with exactly 63 "
                      "observations by session 125)", refs))
    return cases


# --------------------------------------------------------------------------- #
# F9 -- turnover and exit kind (design 4.2, 4.5, 9.1 cases 23, 23b; DR15-14)
# --------------------------------------------------------------------------- #

def f9_cases():
    refs = ("section 4.2", "section 4.5", "section 9.1 #23 and #23b", "DR15-14")
    cases = []
    for adds, drops_rank, drops_last_bar, top_n in ((8, 8, 0, 10), (1, 0, 1, 3), (3, 1, 2, 3),
                                                     (0, 0, 0, 1000), (1, 1, 0, 2), (2, 1, 0, 7)):
        numerator = adds + drops_rank + drops_last_bar
        exact = Q(numerator, 2 * top_n)
        value = float(exact)
        # binary64 division of two exactly-representable integers is correctly rounded, so it
        # equals float(Fraction).
        require(value == numerator / (2 * top_n), "F9 turnover equals binary64 division")
        inputs = {"adds": adds, "drops_rank": drops_rank, "drops_last_bar": drops_last_bar,
                  "top_n": top_n}
        cases.append(case(f"f9_turnover_{adds}_{drops_rank}_{drops_last_bar}_over_2x{top_n}", "F9",
                          inputs, {"one_way_turnover": real(value)},
                          f"4.2: ({adds} + {drops_rank} + {drops_last_bar}) / (2 * {top_n}) = "
                          f"{exact} -> float", refs))

    # 9.1 #23 / #23b -- exits on a small calendar fixture, cut 0 (top_n 2, band 0, W 3, min 2).
    names = [
        dvname(300, 100.0, 0, 11),                                  # member throughout -> window_end
        dvname(200, 90.0, 0, 11, overrides=[[7, 0.5, 180.0]]),      # rank-drop at r1, keeps trading
        dvname(400, 80.0, 0, 5),                                    # member at r0, last bar 5 -> LastBar at r1
        dvname(100, 70.0, 1, 11, overrides=[[1, "nan", 7.0]]),      # first_seen 1, first_bar 2
    ]
    inputs = {"config": config([2], [0], window=3, min_valid=2), "session_dates": F6_DATES,
              "rank_sessions": [3, 7, 10], "names": names}
    b = run_fixture(inputs)
    exits = b.exits(0)
    values = {"exits": [{k: whole(v) for k, v in row.items()} for row in exits]}
    by_id = {row["security_id"]: row for row in exits}
    # r0 members 300, 200; r1: 200 floor-fails (Rank), 100 fills; r2: 200 re-admitted at
    # rank 2, 100 (rank 3 > K_band 2) dropped as Rank. 400 never enters top2.
    require([row["security_id"] for row in exits] == [100, 200, 300], "F9 top2 exits order")
    require(by_id[100]["exit_kind"] == EXIT_RANK_DROP and by_id[100]["last_bar"] == 11, "F9 100")
    require(by_id[100]["first_member_rebalance"] == 1 and by_id[100]["last_member_rebalance"] == 1, "F9 100 r")
    require(by_id[200]["exit_kind"] == EXIT_WINDOW_END and by_id[200]["last_member_rebalance"] == 2, "F9 200")
    require(by_id[300]["exit_kind"] == EXIT_WINDOW_END, "F9 300")
    cases.append(case("f9_exit_kind_readmitted_member_is_window_end", "F9", inputs, values,
                      "9.1 #23 (top_n 2): id 200 is dropped at r1 as Rank (close 0.5 on the "
                      "rank session) and re-admitted at r2, so it is a member at the final "
                      "rebalance -> window_end (2); id 100 (member only at r1, dropped at r2 as "
                      "Rank, still trading with last_bar 11) -> rank_drop (0); id 300 -> 2; "
                      "id 400 never enters top2; rows security_id ascending", refs))

    # A cut wide enough (top_n 3) for the rank-drop-then-keeps-trading and last-bar shapes.
    names = [
        dvname(300, 100.0, 0, 11, gics=1.0),
        dvname(200, 90.0, 0, 11, overrides=[[10, 0.5, 180.0]]),    # rank-drop at the FINAL rebalance
        dvname(400, 80.0, 0, 5),                                    # last bar 5 -> LastBar at r1
        dvname(100, 70.0, 1, 11, overrides=[[1, "nan", 7.0]]),      # first_seen 1 < first_bar 2
        dvname(500, 60.0, 0, 11),
    ]
    inputs = {"config": config([3], [0], window=3, min_valid=2), "session_dates": F6_DATES,
              "rank_sessions": [3, 7, 10], "names": names}
    b = run_fixture(inputs)
    exits = b.exits(0)
    values = {"exits": [{k: whole(v) for k, v in row.items()} for row in exits]}
    by_id = {row["security_id"]: row for row in exits}
    # r0: 300, 200, 400; r1: 400 absent (LastBar), 100 fills; r2: 200 floor-fails (Rank),
    # 500 fills. Final members 300, 100, 500.
    require([row["security_id"] for row in exits] == [100, 200, 300, 400, 500], "F9 top3 exits order")
    require(by_id[200]["exit_kind"] == EXIT_RANK_DROP and by_id[200]["last_bar"] == 11, "F9 200 rank_drop")
    require(by_id[400]["exit_kind"] == EXIT_LAST_BAR_WITHIN_WINDOW and by_id[400]["last_bar"] == 5, "F9 400")
    require(by_id[300]["exit_kind"] == EXIT_WINDOW_END, "F9 300 window_end")
    require(by_id[500]["exit_kind"] == EXIT_WINDOW_END and by_id[500]["first_member_rebalance"] == 2, "F9 500")
    require(by_id[100]["first_seen"] == 1 and by_id[100]["first_bar"] == 2, "F9 100 first_seen<first_bar")
    require(by_id[100]["first_member_rebalance"] == 1 and by_id[100]["last_member_rebalance"] == 2, "F9 100 r")
    require(by_id[400]["first_member_rebalance"] == 0 and by_id[400]["last_member_rebalance"] == 0, "F9 400 r")
    cases.append(case("f9_exit_kinds_all_three_with_first_seen_before_first_bar", "F9", inputs, values,
                      "9.1 #23/#23b (top_n 3): 200 rank-dropped at the final rebalance while "
                      "still trading (last_bar 11) -> 0; 400 last bar 5, dropped at r1 -> 1; "
                      "300, 100 and 500 members at the final rebalance -> 2; 100 first_seen 1 < "
                      "first_bar 2; rows security_id ascending", refs))
    return cases


# --------------------------------------------------------------------------- #
# Self-checks that do not belong to a single family
# --------------------------------------------------------------------------- #

def structural_selfcheck(cases) -> dict:
    ids = [c["case_id"] for c in cases]
    require(len(ids) == len(set(ids)), "duplicate oracle case id")
    # Calendar: the frozen boundaries and a leap-day round trip.
    require(date_to_nanos("2020-01-01") == K_SESSION_KEY_END_EXCLUSIVE, "2020-01-01 key")
    require(date_to_nanos("1970-01-01") == 0, "epoch")
    for iso in ("2012-03-26", "2012-12-31", "2016-02-29", "2019-11-29", "2019-12-31", "2100-03-01"):
        require(nanos_to_date(date_to_nanos(iso)) == iso, f"calendar round trip {iso}")
    require(year_of(date_to_nanos("2019-12-31")) == 2019 and year_of(date_to_nanos("2020-01-01")) == 2020,
            "year_of")
    # Real-run cadence facts from section 6 restated on the calendar alone: 2012-12-31 and
    # 2019-11-29 are month-boundary dates (their next trading session is in another month).
    require(month_of(date_to_nanos("2012-12-31")) != month_of(date_to_nanos("2013-01-02")), "cadence")
    require(month_of(date_to_nanos("2019-11-29")) != month_of(date_to_nanos("2019-12-02")), "cadence")
    # Median rule on the design's own examples.
    require(median_binary64([float(k) for k in range(1, 59)]) == 29.5, "median even")
    require(median_binary64([float(k) for k in range(1, 58)]) == 29.0, "median odd")
    require(median_x2_of_integers([6, 6, 7, 7]) == 13 and median_x2_of_integers([7]) == 14, "x2")
    # K_band table of section 6.
    require([n + (n * bp) // 10000 for n in K_TOP_N for bp in K_BAND_BP]
            == [1000, 1100, 2000, 2200, 3000, 3300], "K_band table")
    # FNV-1a-64 known answers (offset basis for empty input; "a" -> 0xaf63dc4c8601ec8c).
    require(fnv1a64(b"") == FNV_OFFSET and fnv1a64(b"a") == 0xAF63DC4C8601EC8C, "fnv1a64")
    # Decoder refusals on a real image.
    good = encode_membership_bin(run_fixture(cases_by_id(cases)["f8_membership_bin_two_rebalances_two_cuts"]["inputs"]))
    for label, blob in (("bad magic", b"X" + good[1:]),
                        ("bad version", good[:8] + struct.pack("<I", 2) + good[12:]),
                        ("short buffer", good[:20]),
                        ("trailer flip", good[:-1] + bytes([good[-1] ^ 1]))):
        try:
            decode_membership_bin(blob)
        except OracleError:
            continue
        raise OracleError(f"decoder accepted a corrupt image: {label}")
    per_family = {}
    for c in cases:
        per_family.setdefault(c["family"], []).append(c["case_id"])
    require(sorted(per_family) == [f"F{i}" for i in range(1, 10)], "all nine families present")
    return {"checks": "calendar, cadence, median, K_band, FNV-1a-64, codec refusals, unique ids, "
                      "nine families present, every family's hand-derived checkpoints",
            "cases_by_family": {k: len(v) for k, v in per_family.items()},
            "case_count": len(cases)}


def cases_by_id(cases):
    return {c["case_id"]: c for c in cases}


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
    design = file_evidence(DESIGN)
    require(design["exists"], f"design note missing: {design['path']}")
    require(design["sha256"] == DESIGN_SHA256,
            f"design note is not the frozen Revision 3: {design['sha256']} != {DESIGN_SHA256}")
    cases = (f1_cases() + f2_cases() + f3_cases() + f4_cases() + f5_cases() + f6_cases()
             + f7_cases() + f8_cases() + f9_cases())
    selfcheck = structural_selfcheck(cases)
    return {
        "schema": SCHEMA,
        "status": "exact-synthetic-math-verified",
        "design_note_sha256": DESIGN_SHA256,
        "design_note": design,
        "measurement_schema_expected_from_native": MEASUREMENT_SCHEMA,
        "measurement_marker_expected_from_native": MARKER,
        "marker_top_level_keys_closed": ["schema", "case_id", "family", "inputs", "values"],
        "qualification": QUALIFICATION,
        "independence": {
            "native_cpp_source_read": False,
            "point_in_time_universe_cpp_read": False,
            "stage_equity_universe_cpp_read": False,
            "stdlib_only": True,
            "build_or_test_executed_by_this_script": False,
            "real_panel_or_archive_admitted": False,
            "reals_reproduce_binary64_arithmetic_as_pinned": True,
        },
        "frozen_parameters": {
            "adv_window": K_ADV_WINDOW, "min_valid_observations": K_MIN_VALID,
            "min_raw_price_exclusive": K_MIN_RAW_PRICE_EXCLUSIVE, "min_adv_usd": K_MIN_ADV_USD,
            "top_n": list(K_TOP_N), "band_bp": list(K_BAND_BP),
            "k_band": [n + (n * bp) // 10000 for n in K_TOP_N for bp in K_BAND_BP],
            "session_key_end_exclusive": K_SESSION_KEY_END_EXCLUSIVE,
            "rank_start": "2012-12-31", "rank_end": "2019-11-29", "rebalances": 84,
            "median_rule": "sort ascending; odd -> v[m/2]; even -> (v[m/2-1] + v[m/2]) * 0.5 "
                           "in binary64 (section 3.5)",
            "dollar_volume": "close * volume, one binary64 product stored before use (DR15-5)",
            "tie_break": "adv descending, then first-seen slot ascending",
            "band_rule": "incumbent kept iff rank <= K_band = n + (n * band_bp) / 10000; keep "
                         "then fill; output order rank ascending (DR15-2)",
            "drop_kind": "LastBar iff last_bar < rank ordinal, else Rank (DR15-10)",
            "enums": {"PitMemberStatus": {"Add": STATUS_ADD, "Keep": STATUS_KEEP},
                      "PitDropKind": {"Rank": DROP_RANK, "LastBar": DROP_LAST_BAR},
                      "PitExitKind": {"RankDrop": EXIT_RANK_DROP,
                                      "LastBarWithinWindow": EXIT_LAST_BAR_WITHIN_WINDOW,
                                      "WindowEnd": EXIT_WINDOW_END}},
            "not_ranked_rank": NOT_RANKED,
        },
        "value_packs": {
            "exact_integer": "{kind, integer}: compared as integers, no tolerance",
            "exact_u64": "{kind, integer, decimal_string}: a u64 that may exceed 2**53",
            "binary64": "{kind, binary64, hex, exact}: the native JSON number is parsed with "
                        "float() and compared as Fraction(native) == Fraction(binary64); hex is "
                        "float.hex() of the same value; nothing is compared as text (DR15-5)",
            "string": "{kind, string}: digests only (F8 sha256_hex, header_bytes_hex)",
            "lists_and_objects": "compared element-wise / key-wise; a missing expected key is a "
                                 "failure; extra native keys are recorded",
        },
        "input_encoding": INPUT_ENCODING,
        "families": {
            "F1": "ADV window, gaps, ties, median (9.1 #1-#5)",
            "F2": "price floor and rank-session bar (9.1 #6-#7)",
            "F3": "monthly cadence and effective session (9.1 #8, #28; DR15-8)",
            "F4": "banding threshold K_band (9.1 #9)",
            "F5": "drop classification by formula (9.1 #11; DR15-10)",
            "F6": "coverage / union / survivorship exact integers, medians as _x2 (9.1 #20-#22, #25)",
            "F7": "band retention set AND rank-ascending output order (9.1 #9c, #10; DR15-2)",
            "F8": "membership.bin byte layout with FNV-1a-64 trailer (4.7, 9.1 #27; DR15-6)",
            "F9": "one_way_turnover as float(Fraction) and exit_kind integers (4.2, 4.5, 9.1 #23)",
        },
        "native_export_policy": "every oracle case is REQUIRED natively (design 9.1 #26 prints "
                                "the marker for every F1-F9 oracle case id); a case with no "
                                "native line is recorded as not_measured_natively and fails "
                                "the comparator (section 8.3)",
        "selfcheck": selfcheck,
        "case_count": len(cases),
        "cases_by_family": {k: v for k, v in
                            ((f, [c["case_id"] for c in cases if c["family"] == f])
                             for f in sorted({c["family"] for c in cases}))},
        "cases": cases,
        "evidence": {"oracle_script": file_evidence(SELF), "design_note": design},
    }


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    selftest_only = "--selftest" in argv
    for arg in argv:
        if arg not in ("--selftest",):
            print(f"unknown argument: {arg}", file=sys.stderr)
            return 2
    document = build_document()
    summary = {"schema": SCHEMA, "case_count": document["case_count"],
               "cases_by_family": {k: len(v) for k, v in document["cases_by_family"].items()},
               "selfcheck": document["selfcheck"]["checks"],
               "design_note_sha256": DESIGN_SHA256}
    if selftest_only:
        print(json.dumps({**summary, "status": "selftest-only"}, indent=2))
        return 0
    if OUTPUT.exists():
        print(json.dumps({"schema": SCHEMA, "status": "refused",
                          "reason": "output already exists; refusing to overwrite",
                          "out": str(OUTPUT).replace("\\", "/")}, indent=2))
        return 1
    with OUTPUT.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({**summary, "status": document["status"],
                      "out": str(OUTPUT).replace("\\", "/"),
                      "oracle_sha256": hashlib.sha256(OUTPUT.read_bytes()).hexdigest(),
                      "qualification": QUALIFICATION}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
