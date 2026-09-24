"""Independent comparator: native point-in-time-universe measurements vs the exact oracle.

Mirrors the checkpoint-14 protocol of ``iteration14_native_comparator.py`` (design section 8.3).

Inputs
------
  * ``--oracle``  the exact oracle document ``iteration15-universe-oracle-v1.json`` produced
                  by ``iteration15_universe_oracle.py`` (schema
                  ``atx-iteration15-universe-oracle-v1``).
  * ``--logs``    one or more ``ctest -VV`` logs (the design expects ONE:
                  ``atx-engine-data-tests``, DR15-6), scanned for lines carrying the marker
                  ``POINT_IN_TIME_UNIVERSE_MEASUREMENT `` followed by a JSON object of schema
                  ``atx-point-in-time-universe-measurement-v1``. CTest verbose output prepends
                  a test number, so the marker is LOCATED inside the line, never assumed at
                  column zero.
  * ``--out``     the comparison document to create. An existing file is a hard refusal.

What it does
------------
  1. Re-derives, a SECOND time and independently of the oracle script, the F3 cadence rule,
     the F4/F5/F7 keep-then-fill band rule, the F9 turnover formula and the F8 byte image from
     each oracle case's declared *inputs*, and asserts exact agreement with the oracle's
     *expected* values before any native data is touched (design section 8.3).
  2. Decodes every marker line, de-duplicating byte-identical repeats and failing on
     conflicting duplicates of one ``case_id``; refuses any marker whose top-level key set is
     not exactly {schema, case_id, family, inputs, values} (section 8.2, closed list).
  3. Requires the native ``inputs`` to equal the oracle case's inputs key-for-key (the fixture
     must be the same for the comparison to mean anything).
  4. Compares every expected value exactly: integers as integers; reals as
     ``Fraction(float(native)) == Fraction(float(oracle))`` (DR15-5) -- never text; every
     family is exact, no tolerance anywhere.
  5. Records every oracle case with no native line as ``not_measured_natively`` -- never as
     passing -- and exits nonzero.

Discipline
----------
stdlib only; no native C++ source is read; nothing is built, run or measured by this
script; all text I/O passes ``encoding="utf-8"`` explicitly.

  python build-equity/audits/iteration15_native_comparator.py --selftest
  python build-equity/audits/iteration15_native_comparator.py --logs <ctest -VV log> --out <new json>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from fractions import Fraction as Q
from pathlib import Path

SCHEMA = "atx-iteration15-universe-comparison-v1"
ACCEPTED_ORACLE_SCHEMAS = ("atx-iteration15-universe-oracle-v1",)
MEASUREMENT_SCHEMA = "atx-point-in-time-universe-measurement-v1"
MARKER = "POINT_IN_TIME_UNIVERSE_MEASUREMENT "
CLOSED_TOP_LEVEL_KEYS = ("schema", "case_id", "family", "inputs", "values")

SELF = Path(__file__).resolve()
ROOT = SELF.parents[2]
DEFAULT_ORACLE = ROOT / "build-equity" / "audits" / "iteration15-universe-oracle-v1.json"
DEFAULT_OUT = ROOT / "build-equity" / "audits" / "iteration15-native-comparison.json"

NS_PER_DAY = 86_400_000_000_000
M64 = (1 << 64) - 1
STATUS_ADD, STATUS_KEEP = 0, 1
DROP_RANK, DROP_LAST_BAR = 0, 1

QUALIFICATION = (
    "Synthetic-fixture arithmetic comparison against an exact oracle. It is not evidence of a "
    "real-data run, of survivorship completeness, of instrument-type correctness, or of any "
    "investment performance. A passing verdict means the native arithmetic agrees with the "
    "frozen design's universe rules on the fixtures listed, and nothing more."
)


# --------------------------------------------------------------------------- #
# Small helpers
# --------------------------------------------------------------------------- #

def sha256_and_len(path: Path):
    data = path.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def posix(path) -> str:
    return str(path).replace("\\", "/")


def native_integer(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        try:
            return int(value.strip())
        except ValueError:
            return None
    return None


def native_fraction(value):
    """Exact rational of the binary64 the native side printed (shortest round-trip text
    parsed by float() recovers the same double, so Fraction(float) is exact)."""
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        if isinstance(value, float) and not math.isfinite(value):
            return None
        return Q(float(value))
    if isinstance(value, str):
        try:
            parsed = float(value.strip())
        except ValueError:
            return None
        return Q(parsed) if math.isfinite(parsed) else None
    return None


def oracle_real(pack: dict) -> Q:
    value = float(pack["binary64"])
    hexed = float.fromhex(pack["hex"])
    if value != hexed:
        raise ValueError(f"oracle binary64 {value!r} disagrees with its hex {pack['hex']!r}")
    return Q(value)


# --------------------------------------------------------------------------- #
# Independent re-derivation (second reading of design sections 3.1, 3.3-3.5, 4.2, 4.7)
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


def key_of(iso: str) -> int:
    y, m, d = (int(p) for p in iso.split("-"))
    return days_from_civil(y, m, d) * NS_PER_DAY


def ym(key: int):
    y, m, _ = civil_from_days(key // NS_PER_DAY)
    return (y, m)


def rederive_f3(inputs: dict):
    keys = [key_of(d) for d in inputs["session_dates"]]
    lo, hi = key_of(inputs["start_date"]), key_of(inputs["end_date"])
    ranks, effective = [], []
    for i in range(len(keys) - 1):                  # the last attached session never qualifies
        if ym(keys[i]) != ym(keys[i + 1]) and lo <= keys[i] <= hi:
            ranks.append(keys[i])
            effective.append(keys[i + 1])
    return ranks, effective


def as_float(value):
    if isinstance(value, str):
        return {"nan": math.nan, "inf": math.inf, "-inf": -math.inf}[value]
    return float(value)


def bars_of(spec: dict):
    bars = {}
    for a, b, close, volume in spec.get("bars", []):
        for s in range(int(a), int(b) + 1):
            bars[s] = (as_float(close), as_float(volume))
    for s, close, volume in spec.get("overrides", []):
        bars[int(s)] = (as_float(close), as_float(volume))
    for s in spec.get("absent", []):
        bars.pop(int(s), None)
    return bars


def valid_bar(close: float, volume: float) -> bool:
    return math.isfinite(close) and close > 0.0 and math.isfinite(volume) and volume >= 0.0


def median_double(values):
    v = sorted(values)
    m = len(v)
    if m % 2:
        return v[m // 2]
    return (v[m // 2 - 1] + v[m // 2]) * 0.5


def rederive_membership(inputs: dict):
    """A second, differently shaped simulation of sections 3.3-3.4: per-name session maps
    instead of a ring; the result is the per-rebalance, per-cut member/drop/churn tables and
    the session keys, enough for F4, F5, F7, F8 and the F9 exits."""
    cfg = inputs["config"]
    W, min_valid = int(cfg["adv_window"]), int(cfg["min_valid_observations"])
    floor, min_adv = float(cfg["min_raw_price_exclusive"]), float(cfg["min_adv_usd"])
    if "session_dates" in inputs:
        keys = [key_of(d) for d in inputs["session_dates"]]
    else:
        base = key_of(inputs["first_session_date"])
        keys = [base + s * NS_PER_DAY for s in range(int(inputs["session_count"]))]
    names = [(spec["id"], bars_of(spec), as_float(spec.get("gics", "nan"))) for spec in inputs["names"]]
    # slot = first appearance (present on a session, valid or not), feed order within a session
    slot_of, order = {}, []
    for s in range(len(keys)):
        for ident, bars, _g in names:
            if s in bars and ident not in slot_of:
                slot_of[ident] = len(order)
                order.append(ident)
    dv = {ident: {} for ident, _b, _g in names}          # valid bars only: ordinal -> dv
    close_at = {ident: {} for ident, _b, _g in names}
    gics = {ident: g for ident, _b, g in names}
    for ident, bars, _g in names:
        for s, (c, v) in bars.items():
            if valid_bar(c, v):
                product = c * v
                dv[ident][s] = product
                close_at[ident][s] = c
    cuts = [(int(n), int(b)) for n in cfg["top_n"] for b in cfg["band_bp"]]
    incumbents = [set() for _ in cuts]
    rebalances = []
    for R in (int(x) for x in inputs.get("rank_sessions", [])):
        rows = []
        for ident in order:
            window = [dv[ident][s] for s in range(max(0, R - W + 1), R + 1) if s in dv[ident]]
            if R not in dv[ident] or len(window) < min_valid:
                continue
            adv = median_double(window)
            if close_at[ident][R] > floor and adv >= min_adv:
                rows.append((ident, adv, len(window)))
        rows.sort(key=lambda t: (-t[1], slot_of[t[0]]))
        rank = {ident: i + 1 for i, (ident, _a, _n) in enumerate(rows)}
        valid_n = {ident: n for ident, _a, n in rows}
        entry = {"rank_key": keys[R], "effective_key": keys[R + 1] if R + 1 < len(keys) else 0,
                 "cuts": []}
        for c, (n, band) in enumerate(cuts):
            k_band = n + (n * band) // 10000
            picked = [ident for ident, _a, _n in rows
                      if ident in incumbents[c] and rank[ident] <= k_band][:n]
            for ident, _a, _n in rows:
                if len(picked) == n:
                    break
                if ident not in picked:
                    picked.append(ident)
            status = {ident: (STATUS_KEEP if ident in incumbents[c] else STATUS_ADD) for ident in picked}
            drops = []
            for ident in sorted(incumbents[c], key=lambda i: slot_of[i]):
                if ident in picked:
                    continue
                # Only bars observed at or before the rank ordinal exist at rebalance time
                # (design section 3.6 truncation invariance).
                last_bar = max((s for s in dv[ident] if s <= R), default=-1)
                drops.append((ident, DROP_LAST_BAR if last_bar < R else DROP_RANK))
            picked.sort(key=lambda i: rank[i])
            entry["cuts"].append({
                "members": picked, "ranks": [rank[i] for i in picked],
                "status": [status[i] for i in picked], "drops": drops,
                "churn": {"adds": sum(1 for i in picked if status[i] == STATUS_ADD),
                          "drops_rank": sum(1 for _i, k in drops if k == DROP_RANK),
                          "drops_last_bar": sum(1 for _i, k in drops if k == DROP_LAST_BAR),
                          "kept": sum(1 for i in picked if status[i] == STATUS_KEEP),
                          "members": len(picked)},
                "valid_total": sum(valid_n[i] for i in picked),
                "gics_missing": sum(1 for i in picked if math.isnan(gics[i])),
            })
            incumbents[c] = set(picked)
        rebalances.append(entry)
    return {"config": cfg, "rebalances": rebalances}


def fnv1a64(data: bytes) -> int:
    h = 14695981039346656037
    for byte in data:
        h = ((h ^ byte) * 1099511628211) & M64
    return h


def rederive_f8_bytes(sim: dict) -> bytes:
    cfg = sim["config"]
    parts = [b"ATXPITU1", struct.pack("<IIId", 1, int(cfg["adv_window"]),
                                      int(cfg["min_valid_observations"]),
                                      float(cfg["min_raw_price_exclusive"]))]
    parts.append(struct.pack("<I", len(cfg["top_n"])) + b"".join(struct.pack("<I", int(n)) for n in cfg["top_n"]))
    parts.append(struct.pack("<I", len(cfg["band_bp"])) + b"".join(struct.pack("<I", int(b)) for b in cfg["band_bp"]))
    parts.append(struct.pack("<I", len(sim["rebalances"])))
    for rb in sim["rebalances"]:
        if rb["effective_key"] <= 0:
            raise ValueError("pending rebalance cannot be encoded")
        parts.append(struct.pack("<qq", rb["rank_key"], rb["effective_key"]))
        for cut in rb["cuts"]:
            pairs = sorted(zip(cut["members"], cut["ranks"]))
            parts.append(struct.pack("<I", len(pairs)))
            parts.append(b"".join(struct.pack("<q", i) for i, _r in pairs))
            parts.append(b"".join(struct.pack("<I", r) for _i, r in pairs))
    body = b"".join(parts)
    return body + struct.pack("<Q", fnv1a64(body))


def unpack_ints(packs):
    return [int(p["integer"]) for p in packs]


def cross_check(oracle: dict):
    """Re-derive from declared inputs; compare exactly with the oracle's expectations."""
    rows = []
    for case in oracle["cases"]:
        cid, family, inputs, expected = case["case_id"], case["family"], case["inputs"], case["expected"]
        failures, checked = [], []
        try:
            if family == "F3":
                ranks, effective = rederive_f3(inputs)
                checked = ["rank_session_keys", "effective_session_keys", "rank_session_count"]
                if ranks != unpack_ints(expected["rank_session_keys"]):
                    failures.append(f"{cid}: rank_session_keys re-derived {ranks} != oracle")
                if effective != unpack_ints(expected["effective_session_keys"]):
                    failures.append(f"{cid}: effective_session_keys re-derived {effective} != oracle")
                if len(ranks) != int(expected["rank_session_count"]["integer"]):
                    failures.append(f"{cid}: rank_session_count")
            elif family in ("F4", "F5", "F7"):
                sim = rederive_membership(inputs)
                checked = ["rebalances[].members/ranks/status/drops/churn"]
                if len(sim["rebalances"]) != len(expected["rebalances"]):
                    failures.append(f"{cid}: rebalance count")
                for r, (mine, theirs) in enumerate(zip(sim["rebalances"], expected["rebalances"])):
                    cut = mine["cuts"][0]
                    if cut["members"] != unpack_ints(theirs["members"]):
                        failures.append(f"{cid}: r{r} members {cut['members']} != oracle")
                    if cut["ranks"] != unpack_ints(theirs["ranks"]):
                        failures.append(f"{cid}: r{r} ranks")
                    if cut["status"] != unpack_ints(theirs["status"]):
                        failures.append(f"{cid}: r{r} status")
                    if [list(d) for d in cut["drops"]] != [unpack_ints(d) for d in theirs["drops"]]:
                        failures.append(f"{cid}: r{r} drops")
                    if cut["churn"] != {k: int(v["integer"]) for k, v in theirs["churn"].items()}:
                        failures.append(f"{cid}: r{r} churn {cut['churn']} != oracle")
            elif family == "F8":
                blob = rederive_f8_bytes(rederive_membership(inputs))
                checked = ["sha256_hex", "byte_length", "fnv1a64_trailer", "rebalance_count",
                           "header_bytes_hex"]
                if hashlib.sha256(blob).hexdigest() != expected["sha256_hex"]["string"]:
                    failures.append(f"{cid}: sha256 of the re-derived image differs")
                if len(blob) != int(expected["byte_length"]["integer"]):
                    failures.append(f"{cid}: byte_length {len(blob)} != oracle")
                if struct.unpack("<Q", blob[-8:])[0] != int(expected["fnv1a64_trailer"]["integer"]):
                    failures.append(f"{cid}: fnv1a64 trailer")
                if blob[:28].hex() != expected["header_bytes_hex"]["string"]:
                    failures.append(f"{cid}: header bytes")
            elif family == "F9" and "one_way_turnover" in expected:
                numerator = int(inputs["adds"]) + int(inputs["drops_rank"]) + int(inputs["drops_last_bar"])
                exact = Q(numerator, 2 * int(inputs["top_n"]))
                checked = ["one_way_turnover"]
                if Q(float(exact)) != oracle_real(expected["one_way_turnover"]):
                    failures.append(f"{cid}: one_way_turnover")
                if Q(numerator / (2 * int(inputs["top_n"]))) != Q(float(exact)):
                    failures.append(f"{cid}: float division and float(Fraction) disagree")
            elif family == "F6" and "rebalances" in expected:
                sim = rederive_membership(inputs)
                checked = ["rebalances[].cuts[].valid_observations_total/gics_missing_members/members"]
                for r, (mine, theirs) in enumerate(zip(sim["rebalances"], expected["rebalances"])):
                    for c, (mc, tc) in enumerate(zip(mine["cuts"], theirs["cuts"])):
                        if mc["valid_total"] != int(tc["valid_observations_total"]["integer"]):
                            failures.append(f"{cid}: r{r} cut{c} valid_observations_total")
                        if mc["gics_missing"] != int(tc["gics_missing_members"]["integer"]):
                            failures.append(f"{cid}: r{r} cut{c} gics_missing_members")
                        if mc["churn"]["members"] != int(tc["members"]["integer"]):
                            failures.append(f"{cid}: r{r} cut{c} members")
            else:
                continue   # F1/F2 reals and the F6 aggregates are the oracle's own domain
        except Exception as exc:  # noqa: BLE001 -- any re-derivation error is a finding
            failures.append(f"{cid}: re-derivation raised {type(exc).__name__}: {exc}")
        rows.append({"case_id": cid, "family": family, "checked": checked,
                     "exact_agreement": not failures, "failures": failures})
    return rows


# --------------------------------------------------------------------------- #
# Marker-line decoding
# --------------------------------------------------------------------------- #

def scan_text(text: str, source_label: str, records: dict, errors: list) -> int:
    decoder = json.JSONDecoder()
    matched = 0
    for lineno, raw in enumerate(text.splitlines(), start=1):
        idx = raw.find(MARKER)
        if idx < 0:
            continue
        matched += 1
        where = f"{source_label}:{lineno}"
        tail = raw[idx + len(MARKER):].lstrip()
        try:
            obj, _end = decoder.raw_decode(tail)
        except ValueError as exc:
            errors.append(f"{where}: undecodable measurement JSON: {exc}")
            continue
        if not isinstance(obj, dict):
            errors.append(f"{where}: measurement payload is not an object")
            continue
        if obj.get("schema") != MEASUREMENT_SCHEMA:
            errors.append(f"{where}: unexpected measurement schema {obj.get('schema')!r} "
                          f"(want {MEASUREMENT_SCHEMA!r})")
            continue
        extra = sorted(set(obj) - set(CLOSED_TOP_LEVEL_KEYS))
        missing = sorted(set(CLOSED_TOP_LEVEL_KEYS) - set(obj))
        if extra or missing:
            errors.append(f"{where}: marker key list is closed to {list(CLOSED_TOP_LEVEL_KEYS)}; "
                          f"extra {extra}, missing {missing}")
            continue
        case_id = obj.get("case_id")
        if not isinstance(case_id, str) or not case_id:
            errors.append(f"{where}: measurement has no usable case_id")
            continue
        canonical = json.dumps(obj, sort_keys=True, separators=(",", ":"))
        if case_id in records:
            prior = records[case_id]
            if prior["canonical"] != canonical:
                errors.append(f"conflicting duplicate measurement for case_id {case_id!r}: "
                              f"{prior['sources'][0]} vs {where}")
                prior["conflicting"] = True
            else:
                prior["sources"].append(where)
            continue
        records[case_id] = {"object": obj, "canonical": canonical,
                            "line_sha256": hashlib.sha256(raw.encode("utf-8")).hexdigest(),
                            "sources": [where], "conflicting": False}
    return matched


def scan_logs(log_paths):
    records, evidence, errors = {}, [], []
    for path in log_paths:
        entry = {"path": posix(path), "exists": path.exists()}
        if not path.exists():
            entry["status"] = "missing"
            evidence.append(entry)
            errors.append(f"log file not found: {entry['path']}")
            continue
        sha, size = sha256_and_len(path)
        entry.update({"status": "scanned", "sha256": sha, "bytes": size})
        text = path.read_text(encoding="utf-8", errors="replace")
        entry["marker_lines"] = scan_text(text, entry["path"], records, errors)
        evidence.append(entry)
    return records, evidence, errors


# --------------------------------------------------------------------------- #
# Value comparison (exact everywhere)
# --------------------------------------------------------------------------- #

def compare_leaf(path, native, pack, failures, rows):
    kind = pack.get("kind")
    if kind in ("exact_integer", "exact_u64"):
        got = native_integer(native)
        want = int(pack["integer"])
        ok = got is not None and got == want
        if not ok:
            failures.append(f"{path}: exact-integer mismatch -- native {native!r}, expected {want}")
        rows.append({"path": path, "kind": kind, "native": native, "expected": want, "passed": ok})
        return
    if kind == "binary64":
        got = native_fraction(native)
        try:
            want = oracle_real(pack)
        except ValueError as exc:
            failures.append(f"{path}: {exc}")
            rows.append({"path": path, "kind": kind, "passed": False})
            return
        ok = got is not None and got == want
        if not ok:
            failures.append(f"{path}: binary64 mismatch -- native {native!r}, expected "
                            f"{pack['binary64']!r} ({pack['hex']})")
        rows.append({"path": path, "kind": kind, "native": native, "expected_binary64": pack["binary64"],
                     "expected_hex": pack["hex"], "passed": ok})
        return
    if kind == "string":
        ok = isinstance(native, str) and native == pack["string"]
        if not ok:
            failures.append(f"{path}: string mismatch -- native {native!r}, expected {pack['string']!r}")
        rows.append({"path": path, "kind": kind, "native": native, "expected": pack["string"], "passed": ok})
        return
    failures.append(f"{path}: unknown oracle expectation kind {kind!r}")
    rows.append({"path": path, "kind": kind, "passed": False})


def compare_node(path, native, expected, failures, rows, extras):
    if isinstance(expected, dict) and "kind" in expected:
        compare_leaf(path, native, expected, failures, rows)
        return
    if isinstance(expected, dict):
        if not isinstance(native, dict):
            failures.append(f"{path}: expected an object, native carried {type(native).__name__}")
            return
        for key, sub in expected.items():
            if key not in native:
                failures.append(f"{path}.{key}: expected key not exported natively")
                continue
            compare_node(f"{path}.{key}", native[key], sub, failures, rows, extras)
        for key in native:
            if key not in expected:
                extras.append(f"{path}.{key}")
        return
    if isinstance(expected, list):
        if not isinstance(native, list):
            failures.append(f"{path}: expected a list, native carried {type(native).__name__}")
            return
        if len(native) != len(expected):
            failures.append(f"{path}: list length {len(native)} != expected {len(expected)}")
            return
        for i, sub in enumerate(expected):
            compare_node(f"{path}[{i}]", native[i], sub, failures, rows, extras)
        return
    if native != expected:
        failures.append(f"{path}: mismatch -- native {native!r}, expected {expected!r}")


def inputs_equal(path, native, expected, failures):
    """Oracle inputs must be reproduced natively: numbers as Fraction(float), strings exact."""
    if isinstance(expected, dict):
        if not isinstance(native, dict):
            failures.append(f"{path}: input object expected, native carried {type(native).__name__}")
            return
        for key, sub in expected.items():
            if key not in native:
                failures.append(f"{path}.{key}: input key missing natively")
                continue
            inputs_equal(f"{path}.{key}", native[key], sub, failures)
        return
    if isinstance(expected, list):
        if not isinstance(native, list) or len(native) != len(expected):
            failures.append(f"{path}: input list mismatch")
            return
        for i, sub in enumerate(expected):
            inputs_equal(f"{path}[{i}]", native[i], sub, failures)
        return
    if isinstance(expected, str):
        if native != expected:
            failures.append(f"{path}: input string {native!r} != {expected!r}")
        return
    if isinstance(expected, bool) or expected is None:
        if native != expected:
            failures.append(f"{path}: input {native!r} != {expected!r}")
        return
    got, want = native_fraction(native), Q(float(expected)) if isinstance(expected, float) else Q(expected)
    if got is None or got != want:
        failures.append(f"{path}: input number {native!r} != {expected!r}")


def compare_case(case: dict, record: dict) -> dict:
    obj = record["object"]
    values, inputs = obj.get("values"), obj.get("inputs")
    failures, rows, extras = [], [], []
    if record["conflicting"]:
        failures.append(f"{case['case_id']}: conflicting duplicate measurements in the logs")
    if obj.get("family") != case["family"]:
        failures.append(f"{case['case_id']}: family {obj.get('family')!r} != oracle {case['family']!r}")
    if not isinstance(inputs, dict):
        failures.append(f"{case['case_id']}: measurement has no 'inputs' object")
    else:
        inputs_equal("inputs", inputs, case["inputs"], failures)
    if not isinstance(values, dict):
        failures.append(f"{case['case_id']}: measurement has no 'values' object")
    else:
        compare_node("values", values, case["expected"], failures, rows, extras)
    return {"case_id": case["case_id"], "family": case["family"],
            "comparisons": rows, "native_keys_with_no_oracle_expectation": extras,
            "failures": failures, "passed": not failures,
            "sources": record["sources"], "line_sha256": record["line_sha256"]}


# --------------------------------------------------------------------------- #
# Self-test: synthetic marker lines, accept-all and reject-all
# --------------------------------------------------------------------------- #

def render(node):
    if isinstance(node, dict) and "kind" in node:
        kind = node["kind"]
        if kind == "exact_integer":
            return node["integer"]
        if kind == "exact_u64":
            return node["decimal_string"]
        if kind == "binary64":
            return node["binary64"]
        if kind == "string":
            return node["string"]
    if isinstance(node, dict):
        return {k: render(v) for k, v in node.items()}
    if isinstance(node, list):
        return [render(v) for v in node]
    return node


def fabricate(case: dict) -> dict:
    return {"schema": MEASUREMENT_SCHEMA, "case_id": case["case_id"], "family": case["family"],
            "inputs": json.loads(json.dumps(case["inputs"])), "values": render(case["expected"])}


def perturb(obj: dict, case: dict):
    """Change exactly one leaf of the values: the first leaf found, depth-first."""
    def walk(native, expected, path):
        if isinstance(expected, dict) and "kind" in expected:
            return path
        if isinstance(expected, dict):
            for key, sub in expected.items():
                found = walk(native[key], sub, path + [key])
                if found:
                    return found
        if isinstance(expected, list):
            for i, sub in enumerate(expected):
                found = walk(native[i], sub, path + [i])
                if found:
                    return found
        return None

    path = walk(obj["values"], case["expected"], [])
    if not path:
        return None
    node = obj["values"]
    for step in path[:-1]:
        node = node[step]
    leaf = node[path[-1]]
    if isinstance(leaf, bool):
        node[path[-1]] = not leaf
    elif isinstance(leaf, int):
        node[path[-1]] = leaf + 1
    elif isinstance(leaf, float):
        node[path[-1]] = math.nextafter(leaf, math.inf)
    elif isinstance(leaf, str):
        node[path[-1]] = leaf + "0"
    return ".".join(str(p) for p in path)


def marker_line(obj: dict, test_number: int) -> str:
    return f"{test_number}: {MARKER}{json.dumps(obj, separators=(',', ':'))}"


def selftest(oracle: dict) -> dict:
    cases = oracle["cases"]
    noise = ["1: Test command: C:/x/atx-engine-data-tests.exe",
             "1: [ RUN      ] DataPointInTimeUniverse.OracleMarkers_EmitAllFamilies",
             "1: POINT_IN_TIME_UNIVERSE_MEASUREMENT_NOT_REALLY {\"schema\":\"x\"}"]
    # Accept-all log: every case once, the first case repeated byte-identically (deduped).
    accept_lines = list(noise)
    for i, case in enumerate(cases):
        accept_lines.append(marker_line(fabricate(case), 1))
        if i == 0:
            accept_lines.append(marker_line(fabricate(case), 1))
    records, errors = {}, []
    matched = scan_text("\n".join(accept_lines), "selftest-accept", records, errors)
    accept_results = [compare_case(c, records[c["case_id"]]) for c in cases]
    # Reject-all log: every case with exactly one perturbed leaf.
    reject_lines, perturbed = list(noise), []
    for case in cases:
        obj = fabricate(case)
        where = perturb(obj, case)
        perturbed.append({"case_id": case["case_id"], "perturbed_leaf": where})
        reject_lines.append(marker_line(obj, 1))
    records_bad, errors_bad = {}, []
    scan_text("\n".join(reject_lines), "selftest-reject", records_bad, errors_bad)
    reject_results = [compare_case(c, records_bad[c["case_id"]]) for c in cases]
    # Conflicting duplicate, closed key list, wrong schema, undecodable payload.
    conflict = [marker_line(fabricate(cases[0]), 1)]
    twisted = fabricate(cases[0])
    perturb(twisted, cases[0])
    conflict.append(marker_line(twisted, 1))
    extra_key = fabricate(cases[1])
    extra_key["extra"] = 1
    conflict.append(marker_line(extra_key, 1))
    wrong_schema = fabricate(cases[2])
    wrong_schema["schema"] = "atx-something-else"
    conflict.append(marker_line(wrong_schema, 1))
    conflict.append(f"1: {MARKER}{{not json")
    recs_c, errs_c = {}, []
    scan_text("\n".join(conflict), "selftest-conflict", recs_c, errs_c)
    inputs_drift = fabricate(cases[0])
    inputs_drift["inputs"] = {"changed": True}
    drift = compare_case(cases[0], {"object": inputs_drift, "canonical": "", "line_sha256": "",
                                    "sources": ["selftest"], "conflicting": False})
    cross = cross_check(oracle)
    report = {
        "oracle_cases": len(cases),
        "accept_all": {"marker_lines_seen": matched, "records": len(records), "scan_errors": errors,
                       "passed": sum(1 for r in accept_results if r["passed"]),
                       "failed": [r for r in accept_results if not r["passed"]],
                       "first_case_deduplicated_sources": records[cases[0]["case_id"]]["sources"]},
        "reject_all": {"records": len(records_bad), "scan_errors": errors_bad,
                       "rejected": sum(1 for r in reject_results if not r["passed"]),
                       "not_rejected": [r["case_id"] for r in reject_results if r["passed"]],
                       "perturbations": perturbed},
        "scan_refusals": {"errors": errs_c, "records_kept": len(recs_c),
                          "conflicting_flagged": recs_c[cases[0]["case_id"]]["conflicting"]},
        "inputs_drift_rejected": not drift["passed"],
        "cross_check": {"rows": len(cross), "all_exact": all(r["exact_agreement"] for r in cross),
                        "failures": [r for r in cross if not r["exact_agreement"]]},
    }
    report["ok"] = (
        not errors and report["accept_all"]["passed"] == len(cases)
        and report["reject_all"]["rejected"] == len(cases) and not errors_bad
        and report["scan_refusals"]["conflicting_flagged"]
        and len(errs_c) == 4 and report["inputs_drift_rejected"]
        and report["cross_check"]["all_exact"]
        and len(report["accept_all"]["first_case_deduplicated_sources"]) == 2
    )
    return report


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Independent comparator: native point-in-time-universe measurements vs "
                    "the exact iteration-15 oracle.")
    parser.add_argument("--oracle", default=str(DEFAULT_ORACLE), help="exact oracle JSON")
    parser.add_argument("--logs", nargs="+", default=[],
                        help="ctest -VV log files to scan for POINT_IN_TIME_UNIVERSE_MEASUREMENT lines")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="comparison JSON to create (must not exist)")
    parser.add_argument("--selftest", action="store_true",
                        help="feed synthetic accept-all and reject-all marker lines through the "
                             "scanner and comparator, print counts, write nothing, read no log")
    args = parser.parse_args(argv)

    oracle_path = Path(args.oracle)
    if not oracle_path.exists():
        print(json.dumps({"schema": SCHEMA, "status": "refused", "reason": "oracle not found",
                          "oracle": posix(oracle_path)}, separators=(",", ":")))
        return 1
    oracle_sha, oracle_len = sha256_and_len(oracle_path)
    oracle = json.loads(oracle_path.read_text(encoding="utf-8"))
    if oracle.get("schema") not in ACCEPTED_ORACLE_SCHEMAS:
        print(json.dumps({"schema": SCHEMA, "status": "refused",
                          "reason": f"oracle schema {oracle.get('schema')!r} is not one of "
                                    f"{list(ACCEPTED_ORACLE_SCHEMAS)}"}, separators=(",", ":")))
        return 1

    if args.selftest:
        report = selftest(oracle)
        report.update({"schema": SCHEMA, "status": "selftest-pass" if report["ok"] else "selftest-fail",
                       "oracle_sha256": oracle_sha, "file_written": False, "logs_read": False})
        print(json.dumps(report, indent=2))
        return 0 if report["ok"] else 1

    out_path = Path(args.out)
    if out_path.exists():
        print(json.dumps({"schema": SCHEMA, "status": "refused",
                          "reason": "output already exists; refusing to overwrite",
                          "out": posix(out_path)}, separators=(",", ":")))
        return 1

    self_sha, self_len = sha256_and_len(SELF)
    cases_by_id = {c["case_id"]: c for c in oracle["cases"]}
    global_failures = []
    cross_rows = cross_check(oracle)
    for row in cross_rows:
        global_failures.extend(row["failures"])

    log_paths = [Path(p) for p in args.logs]
    if not log_paths:
        global_failures.append("no --logs given: there is nothing to compare against")
    records, log_evidence, scan_errors = scan_logs(log_paths)
    global_failures.extend(scan_errors)

    case_results, not_measured = [], []
    for cid, case in cases_by_id.items():
        if cid in records:
            case_results.append(compare_case(case, records[cid]))
        else:
            not_measured.append({"case_id": cid, "family": case["family"],
                                 "status": "not_measured_natively", "required": True})
            global_failures.append(f"required case {cid!r} has no native measurement in the "
                                   "scanned logs (recorded as not_measured_natively, never as passing)")
    unexpected = sorted(cid for cid in records if cid not in cases_by_id)
    for cid in unexpected:
        global_failures.append(f"native measurement {cid!r} matches no oracle case id")

    passed = (not global_failures and all(c["passed"] for c in case_results)
              and len(case_results) == len(cases_by_id))
    document = {
        "schema": SCHEMA,
        "status": "pass" if passed else "fail",
        "qualification": QUALIFICATION,
        "independence": {"native_cpp_source_read": False,
                         "build_or_test_executed_by_this_script": False,
                         "expected_values_rederived_independently_of_the_oracle":
                             "F3, F4, F5, F7, F8, F9 turnover and the F6 per-rebalance integers",
                         "stdlib_only": True},
        "bound_policy": {"formula": "exact equality for every family: integers as integers, reals as "
                                    "Fraction(float(native)) == Fraction(float(oracle)) (DR15-5), "
                                    "digests as strings; no tolerance anywhere",
                         "marker_top_level_keys_closed": list(CLOSED_TOP_LEVEL_KEYS),
                         "inputs_must_match_oracle": True},
        "evidence": {"comparator_script": {"path": posix(SELF), "sha256": self_sha, "bytes": self_len},
                     "oracle": {"path": posix(oracle_path), "sha256": oracle_sha, "bytes": oracle_len,
                                "schema": oracle.get("schema"), "case_count": oracle.get("case_count"),
                                "design_note_sha256": oracle.get("design_note_sha256")},
                     "logs": log_evidence},
        "convention_cross_check": {"description": "the design's rules re-derived in this file from "
                                                  "each oracle case's declared INPUTS and compared "
                                                  "exactly against the oracle's expected values "
                                                  "before any native data was read",
                                   "rows": cross_rows,
                                   "all_exact": all(r["exact_agreement"] for r in cross_rows)},
        "required_case_ids": list(cases_by_id),
        "native_case_ids_found": sorted(records),
        "native_case_ids_with_no_oracle_case": unexpected,
        "global_failures": global_failures,
        "cases": case_results,
        "not_measured_natively": not_measured,
        "counts": {"oracle_cases": len(cases_by_id), "required": len(cases_by_id),
                   "compared": len(case_results),
                   "passed": sum(1 for c in case_results if c["passed"]),
                   "failed": sum(1 for c in case_results if not c["passed"]),
                   "not_measured_natively": len(not_measured),
                   "global_failures": len(global_failures)},
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"schema": SCHEMA, "status": document["status"], "counts": document["counts"],
                      "comparator_sha256": self_sha, "oracle_sha256": oracle_sha,
                      "out": posix(out_path), "qualification": QUALIFICATION}, separators=(",", ":")))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
