"""Independent comparator: native cross-section-IC measurements vs the exact oracle.

Mirrors the checkpoint-12 pattern of ``iteration12_native_comparator.py``.

Inputs
------
  * ``--oracle``  the exact oracle document. Defaults to the **v2** document
                  (``iteration14-cross-section-oracle-v2.json``, produced by
                  ``iteration14_cross_section_oracle_v2.py``), whose four re-cut F3
                  fixtures and new case 8c carry ruling A-6. The v1 document is
                  still accepted when passed explicitly, so an earlier comparison
                  can be reproduced.
  * ``--logs``    one or more ``ctest -VV`` logs, scanned for lines carrying the
                  marker ``CROSS_SECTION_IC_MEASUREMENT `` followed by a JSON
                  object of schema ``atx-cross-section-ic-measurement-v1``.
                  CTest verbose output prepends a test number, so the marker is
                  LOCATED inside the line rather than assumed at column zero.
  * ``--out``     the comparison document to create. An existing file is a hard
                  refusal, never an overwrite.

What it does
------------
  1. Re-derives the design's arithmetic a SECOND time, here, from the oracle's
     own declared *inputs*, and asserts exact agreement with the oracle's
     *expected* values before any native data is touched. A disagreement fails
     the run: it means the oracle and this file read the design differently.
  2. Decodes every marker line, de-duplicating byte-identical repeats and
     failing on conflicting duplicates of one case id.
  3. Compares each native value against the oracle's expectation under the
     family bound declared by design section 9.2 (F4 and F6 are exact-integer
     and take no tolerance at all).
  4. Applies the design section 9.2 cross-checks that do not need the oracle:
     every interval brackets its point estimate, ``reportable`` and
     ``unreportable_reason`` match the frozen rule, ``modulo_fallbacks == 0``.
  5. Records every oracle case with no native measurement as
     ``not_measured_natively`` -- never as passing -- and exits nonzero.

Discipline
----------
stdlib only; no native C++ source is read; nothing is built, run or measured by
this script; all text I/O passes ``encoding="utf-8"`` explicitly.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from decimal import Decimal
from fractions import Fraction as Q
from pathlib import Path

SCHEMA = "atx-iteration14-cross-section-comparison-v1"
# v2 is the default oracle (ruling A-6 re-cut the four F3 fixtures); v1 stays loadable
# via --oracle so an earlier comparison can still be reproduced.
ACCEPTED_ORACLE_SCHEMAS = (
    "atx-iteration14-cross-section-oracle-v2",
    "atx-iteration14-cross-section-oracle-v1",
)
MEASUREMENT_SCHEMA = "atx-cross-section-ic-measurement-v1"
MARKER = "CROSS_SECTION_IC_MEASUREMENT "

SELF = Path(__file__).resolve()
ROOT = SELF.parents[2]
DEFAULT_ORACLE = ROOT / "build-equity" / "audits" / "iteration14-cross-section-oracle-v2.json"
DEFAULT_OUT = ROOT / "build-equity" / "audits" / "iteration14-native-comparison.json"

DBL_EPSILON = 2.0 ** -52
ULP_MULTIPLIER = 64
EXACT_FAMILIES = ("F4", "F6")

# Frozen constants this comparator re-derives independently of the oracle.
K_BOOTSTRAP_SEED = 20260920
BLOCK_LEN_FLOOR = 5
NS_PER_DAY = 86_400_000_000_000
M64 = (1 << 64) - 1

# Only these two unreportable_reason integer codes are pinned by the design
# (section 3.13 and section 9.1 case 14). Any other code is RECORDED, not failed.
PINNED_REASON_CODES = {
    2: "common-sample-prefix-incomplete",
    3: "series-too-short-for-block-length",
}

QUALIFICATION = (
    "Synthetic-fixture arithmetic comparison against an exact oracle. It is not "
    "evidence of a real-data run, of survivorship correction, of terminal-event "
    "classification, or of any investment performance. A passing verdict means "
    "the native arithmetic agrees with the frozen design's formulas on the "
    "fixtures listed, and nothing more."
)


# --------------------------------------------------------------------------- #
# Small helpers
# --------------------------------------------------------------------------- #

def sha256_and_len(path: Path):
    data = path.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def posix(path) -> str:
    return str(path).replace("\\", "/")


def as_fraction(pack: dict) -> Q:
    """Exact value of an oracle expectation pack."""
    kind = pack.get("kind")
    if kind == "exact_rational":
        num, den = pack["exact"].split("/")
        return Q(int(num), int(den))
    if kind == "decimal_50":
        return Q(Decimal(pack["decimal"]))
    if kind in ("exact_integer", "exact_u64"):
        return Q(int(pack["integer"]))
    raise ValueError(f"cannot take the exact value of kind {kind!r}")


def native_integer(value):
    """Accept a JSON integer, or a decimal string for values above 2**53."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        try:
            return int(text)
        except ValueError:
            return None
    if isinstance(value, float) and value.is_integer():
        return int(value)
    return None


def native_fraction(value):
    """Exact rational value of a native number as it was actually printed.

    A native float printed with max_digits10 in the classic locale round-trips
    exactly, so Fraction(float) recovers the binary64 the native code held.
    """
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        if isinstance(value, float) and not math.isfinite(value):
            return None
        return Q(value)
    if isinstance(value, str):
        try:
            return Q(Decimal(value.strip()))
        except (ValueError, ArithmeticError):
            return None
    return None


# --------------------------------------------------------------------------- #
# Independent re-derivation of the design's arithmetic (cross-check pass)
# --------------------------------------------------------------------------- #

def xs_rank(values):
    """core::stats::rank: averaged positions over runs of bit-equal values,
    normalized by n-1; n == 1 yields 0."""
    n = len(values)
    if n == 0:
        return []
    if n == 1:
        return [Q(0)]
    order = sorted(range(n), key=lambda i: (values[i], i))
    out = [Q(0)] * n
    lo = 0
    while lo < n:
        hi = lo + 1
        while hi < n and values[order[hi]] == values[order[lo]]:
            hi += 1
        norm = ((Q(lo) + Q(hi - 1)) / 2) / Q(n - 1)
        for p in range(lo, hi):
            out[order[p]] = norm
        lo = hi
    return out


def xs_pearson_parts(xs, rs):
    """Returns (cov, va, vb) exactly; the caller takes the square root."""
    n = len(xs)
    if n < 2:
        return Q(0), Q(0), Q(0)
    ma = sum(xs, Q(0)) / n
    mb = sum(rs, Q(0)) / n
    cov = sum(((x - ma) * (r - mb) for x, r in zip(xs, rs)), Q(0))
    va = sum(((x - ma) ** 2 for x in xs), Q(0))
    vb = sum(((r - mb) ** 2 for r in rs), Q(0))
    return cov, va, vb


def xs_block_len(h: int) -> int:
    return max(BLOCK_LEN_FLOOR, (h + 1) // 2)


def xs_reportable(draws: int, n: int, h: int):
    length = xs_block_len(h)
    if draws < 1:
        return 0, "bootstrap-draws-zero", length
    if n < 20:
        return 0, "series-shorter-than-twenty", length
    if n // length < 10:
        return 0, "series-too-short-for-block-length", length
    return 1, None, length


def xs_stream_key(stat, hidx, sig, var, restr, sample) -> int:
    return (K_BOOTSTRAP_SEED ^ (stat << 8) ^ (hidx << 16) ^ (sig << 24)
            ^ (var << 32) ^ (restr << 40) ^ (sample << 48)) & M64


def xs_splitmix(state: int):
    state = (state + 0x9E3779B97F4A7C15) & M64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return (z ^ (z >> 31)) & M64, state


def xs_draw_starts(key: int, n: int, count: int):
    """A second, deliberately independent transcription of design section 3.10.

    The generator state is kept as a plain 4-element list and the rotate is
    written out inline, so a transcription error shared with the oracle is
    unlikely to survive the cross-check.
    """
    seed, _ = xs_splitmix(key)
    s = []
    cur = seed
    for _ in range(4):
        value, cur = xs_splitmix(cur)
        s.append(value)

    def nxt():
        r = (s[0] + s[3]) & M64
        result = ((((r << 23) | (r >> 41)) & M64) + s[0]) & M64
        t = (s[1] << 17) & M64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = ((s[3] << 45) | (s[3] >> 19)) & M64
        return result

    thresh = ((1 << 64) - n) % n
    starts = []
    fallbacks = 0
    for _ in range(count):
        x = nxt()
        prod = x * n
        hi, lo = (prod >> 64) & M64, prod & M64
        if lo < n:
            tries = 0
            while lo < thresh:
                if tries == 64:
                    fallbacks += 1
                    hi = x % n
                    break
                x = nxt()
                prod = x * n
                hi, lo = (prod >> 64) & M64, prod & M64
                tries += 1
        starts.append(hi)
    return seed, starts, fallbacks


def cross_check(oracle) -> list:
    """Re-derive what can be re-derived from each case's declared inputs and
    assert exact agreement with the oracle's expected values."""
    rows = []
    for case in oracle["cases"]:
        cid, family = case["id"], case["family"]
        inputs, expected = case.get("inputs", {}), case.get("expected", {})
        checks, failures = [], []

        def cmp_exact(name, derived, pack):
            if pack is None:
                return
            got = as_fraction(pack)
            checks.append(name)
            if derived != got:
                failures.append(f"{cid}: re-derived {name} {derived} != oracle {got}")

        if family == "F1" and "signal" in inputs and "forward_return" in inputs:
            xs = [as_fraction(p) for p in inputs["signal"]]
            rs = [as_fraction(p) for p in inputs["forward_return"]]
            cov, va, vb = xs_pearson_parts(xs, rs)
            cmp_exact("cov", cov, expected.get("cov"))
            cmp_exact("va", va, expected.get("va"))
            cmp_exact("vb", vb, expected.get("vb"))
            if "signal_ranks" in expected:
                derived = xs_rank(xs)
                checks.append("signal_ranks")
                if derived != [as_fraction(p) for p in expected["signal_ranks"]]:
                    failures.append(f"{cid}: re-derived signal_ranks disagree")
            if "return_ranks" in expected:
                derived = xs_rank(rs)
                checks.append("return_ranks")
                if derived != [as_fraction(p) for p in expected["return_ranks"]]:
                    failures.append(f"{cid}: re-derived return_ranks disagree")
        if family == "F1" and "ranks" in expected and "signal" in inputs:
            derived = xs_rank([as_fraction(p) for p in inputs["signal"]])
            checks.append("ranks")
            if derived != [as_fraction(p) for p in expected["ranks"]]:
                failures.append(f"{cid}: re-derived ranks disagree")

        if family == "F2" and expected.get("spread_emitted", {}).get("integer") == 1:
            quantiles = inputs["quantile_count"]["integer"]
            idx = [p["integer"] for p in inputs["instrument_indices"]]
            sig = dict(zip(idx, (as_fraction(p) for p in inputs["signal"])))
            ret = dict(zip(idx, (as_fraction(p) for p in inputs["forward_return"])))
            ordered = sorted(sig.items(), key=lambda kv: (-kv[1], kv[0]))
            n = len(ordered)
            buckets = {q: [] for q in range(quantiles)}
            for p, (i, _v) in enumerate(ordered):
                buckets[(p * quantiles) // n].append(i)
            means = {q: sum((ret[i] for i in m), Q(0)) / len(m) for q, m in buckets.items()}
            cmp_exact("spread", means[0] - means[quantiles - 1], expected.get("spread"))
            cmp_exact("mean_top_decile", means[0], expected.get("mean_top_decile"))
            cmp_exact("mean_bottom_decile", means[quantiles - 1],
                      expected.get("mean_bottom_decile"))

        if family == "F3" and "terminal_leg" in expected:
            marks = inputs["forward_marks"]
            valid = {int(u): m for u, m in marks.items()
                     if m is not None and as_fraction(m["price"]) > 0}
            u_last = max(valid)
            raw_last = as_fraction(valid[u_last]["raw_close"])
            price_last = as_fraction(valid[u_last]["price"])
            price_t = as_fraction(inputs["price_t"])
            consideration = as_fraction(inputs["consideration"])
            special = as_fraction(inputs["special_dividend"])
            record = inputs.get("record_date_session_key")
            obs = int(inputs["session_key_t"]["integer"])
            applied = special if (record is not None
                                  and obs <= int(record["integer"])) else Q(0)
            leg = (consideration + applied) / raw_last - 1
            cmp_exact("terminal_leg", leg, expected.get("terminal_leg"))
            cmp_exact("forward_return", (price_last / price_t) * (1 + leg) - 1,
                      expected.get("forward_return"))
            cmp_exact("special_dividend_applied", applied,
                      expected.get("special_dividend_applied"))

        if family == "F4" and "n_eligible" in expected:
            # Partial re-derivation: only the counters that the case's declared
            # inputs fully determine. n_terminal_applied needs the prices at every
            # offset inside (t, t+h], which the inputs carry only when h == 1, so
            # it is re-derived there and left to the oracle's hand checkpoints
            # otherwise. The counter IDENTITIES are checked unconditionally.
            ids = [p["integer"] for p in inputs["instrument_ids"]]
            ex34 = {p["integer"] for p in inputs["ex34_ids"]}
            restriction = inputs["restriction"]
            mask = [p["integer"] for p in inputs["mask"]]
            sig = inputs["signal"]
            p_t = inputs["price_t"]
            p_th = inputs["price_t_plus_h"]
            horizon = inputs["horizon"]["integer"]
            names = [i for i in range(len(ids))
                     if restriction == "full" or ids[i] not in ex34]
            eligible = [i for i in names if mask[i] == 1]
            finite = [i for i in eligible if sig[i].get("kind") != "nonfinite"]
            excluded = [i for i in eligible if ids[i] in ex34]
            cmp_exact("n_eligible", Q(len(eligible)), expected.get("n_eligible"))
            cmp_exact("n_signal_finite", Q(len(finite)), expected.get("n_signal_finite"))
            cmp_exact("n_excluded_audited", Q(len(excluded)),
                      expected.get("n_excluded_audited"))
            if horizon == 1:
                fwd = [i for i in finite
                       if p_t[i] is not None and p_th[i] is not None
                       and as_fraction(p_t[i]) > 0 and as_fraction(p_th[i]) > 0]
                cmp_exact("n_with_forward", Q(len(fwd)), expected.get("n_with_forward"))
                term = [p["integer"] for p in inputs["terminal"]]
                evid = [p["integer"] for p in inputs["terminal_evidenced"]]
                applied = [i for i in finite if i not in fwd and term[i] == 1 and evid[i] == 1
                           and p_th[i] is not None and as_fraction(p_th[i]) > 0
                           and p_t[i] is not None and as_fraction(p_t[i]) > 0]
                unevid = [i for i in finite if i not in fwd and term[i] == 1 and evid[i] == 0]
                cmp_exact("n_terminal_applied", Q(len(applied)),
                          expected.get("n_terminal_applied"))
                cmp_exact("n_terminal_unevidenced", Q(len(unevid)),
                          expected.get("n_terminal_unevidenced"))
            counters = {k: expected[k]["integer"] for k in expected
                        if isinstance(expected[k], dict) and "integer" in expected[k]}
            checks.append("counter_identities")
            if counters["n_used"] != counters["n_with_forward"] + counters["n_terminal_applied"]:
                failures.append(f"{cid}: n_used != n_with_forward + n_terminal_applied")
            if counters["n_dropped_missing_forward"] != (counters["n_signal_finite"]
                                                         - counters["n_with_forward"]):
                failures.append(f"{cid}: n_dropped_missing_forward != "
                                "n_signal_finite - n_with_forward")
            if not (counters["n_used"] <= counters["n_signal_finite"] <= counters["n_eligible"]):
                failures.append(f"{cid}: counter ordering n_used <= n_signal_finite "
                                "<= n_eligible violated")

        if family == "F3" and expected.get("terminal_leg_applied", {}).get("integer") == 0                 and "forward_return" in expected:
            # Ruling A-6: a live finite positive mark at offset == horizon means the ORDINARY
            # return, with no leg. Re-derived here from the declared inputs.
            h = str(inputs["horizon"]["integer"])
            mark = inputs["forward_marks"][h]
            ordinary = as_fraction(mark["price"]) / as_fraction(inputs["price_t"]) - 1
            cmp_exact("forward_return_a6_ordinary", ordinary, expected.get("forward_return"))
            checks.append("a6_no_leg_and_no_terminal_application")
            if expected.get("n_terminal_applied_increment", {}).get("integer") != 0:
                failures.append(f"{cid}: ruling A-6 forbids incrementing n_terminal_applied "
                                "when a live P(t+h) exists")

        if family == "F5" and "days_forward" in expected and "session_key_t" in inputs:
            start = int(inputs["session_key_t"]["integer"])
            end = int(inputs["session_key_t_plus_h"]["integer"])
            days = (end - start) // NS_PER_DAY
            checks.append("days_forward")
            if days != expected["days_forward"]["integer"]:
                failures.append(f"{cid}: re-derived days_forward {days} != oracle "
                                f"{expected['days_forward']['integer']}")
            borrow = (as_fraction(inputs["annual_borrow_bps"]) / 10000) * (
                Q(days) / 365) * as_fraction(inputs["short_leg_gross"])
            cmp_exact("borrow_drag", borrow, expected.get("borrow_drag"))

        if family == "F5" and "trade_drag" in expected and "oneway_turnover" in expected:
            oneway = as_fraction(expected["oneway_turnover"])
            derived = (as_fraction(inputs["trade_bps"]) / 10000) * 2 * oneway
            cmp_exact("trade_drag", derived, expected.get("trade_drag"))

        if family == "F6" and "stream_key" in expected:
            key = xs_stream_key(
                inputs["statistic_id"]["integer"], inputs["horizon_index"]["integer"],
                inputs["signal_index"]["integer"], inputs["variant_id"]["integer"],
                inputs["restriction_id"]["integer"], inputs["sample_id"]["integer"])
            checks.append("stream_key")
            if key != int(expected["stream_key"]["integer"]):
                failures.append(f"{cid}: re-derived stream_key {key} != oracle "
                                f"{expected['stream_key']['integer']}")
            n = inputs["n"]["integer"]
            count = len(expected["draw_starts"])
            seed, starts, fallbacks = xs_draw_starts(key, n, count)
            checks.extend(["seed_x", "draw_starts", "modulo_fallbacks"])
            if seed != int(expected["seed_x"]["integer"]):
                failures.append(f"{cid}: re-derived seed_x {seed} != oracle "
                                f"{expected['seed_x']['integer']}")
            if starts != [p["integer"] for p in expected["draw_starts"]]:
                failures.append(f"{cid}: re-derived draw_starts {starts} != oracle "
                                f"{[p['integer'] for p in expected['draw_starts']]}")
            if fallbacks != 0:
                failures.append(f"{cid}: re-derived modulo_fallbacks {fallbacks} != 0")
            for aux in case.get("auxiliary_draw_starts_by_n", []):
                aux_n = aux["n"]["integer"]
                _s, aux_starts, _f = xs_draw_starts(key, aux_n, len(aux["draw_starts"]))
                checks.append(f"auxiliary_draw_starts_n{aux_n}")
                if aux_starts != [p["integer"] for p in aux["draw_starts"]]:
                    failures.append(f"{cid}: re-derived auxiliary draw starts for n={aux_n} disagree")

        if family == "F6" and "grid" in expected:
            for row in expected["grid"]:
                rep, reason, length = xs_reportable(
                    oracle["frozen_parameters"]["bootstrap_draws"],
                    row["n"]["integer"], row["horizon"]["integer"])
                checks.append(f"grid_h{row['horizon']['integer']}_{row['sample']}")
                if rep != row["reportable"]["integer"] or reason != row["unreportable_reason_text"] \
                        or length != row["block_len"]["integer"]:
                    failures.append(f"{cid}: re-derived reportability row disagrees "
                                    f"for h={row['horizon']['integer']} "
                                    f"sample={row['sample']}")

        if checks:
            rows.append({"case_id": cid, "family": family, "checked": checks,
                         "exact_agreement": not failures, "failures": failures})
    return rows


# --------------------------------------------------------------------------- #
# Marker-line decoding
# --------------------------------------------------------------------------- #

def scan_logs(log_paths):
    decoder = json.JSONDecoder()
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
        matched = 0
        for lineno, raw in enumerate(text.splitlines(), start=1):
            idx = raw.find(MARKER)
            if idx < 0:
                continue
            matched += 1
            tail = raw[idx + len(MARKER):].lstrip()
            try:
                obj, _end = decoder.raw_decode(tail)
            except ValueError as exc:
                errors.append(f"{entry['path']}:{lineno}: undecodable measurement JSON: {exc}")
                continue
            if not isinstance(obj, dict):
                errors.append(f"{entry['path']}:{lineno}: measurement payload is not an object")
                continue
            if obj.get("schema") != MEASUREMENT_SCHEMA:
                errors.append(f"{entry['path']}:{lineno}: unexpected measurement schema "
                              f"{obj.get('schema')!r} (want {MEASUREMENT_SCHEMA!r})")
                continue
            case_id = obj.get("case_id")
            if not isinstance(case_id, str) or not case_id:
                errors.append(f"{entry['path']}:{lineno}: measurement has no usable case_id")
                continue
            canonical = json.dumps(obj, sort_keys=True, separators=(",", ":"))
            source = f"{entry['path']}:{lineno}"
            if case_id in records:
                prior = records[case_id]
                if prior["canonical"] != canonical:
                    errors.append(f"conflicting duplicate measurement for case_id "
                                  f"{case_id!r}: {prior['sources'][0]} vs {source}")
                    prior["conflicting"] = True
                else:
                    prior["sources"].append(source)
                continue
            records[case_id] = {
                "object": obj,
                "canonical": canonical,
                "line_sha256": hashlib.sha256(raw.encode("utf-8")).hexdigest(),
                "sources": [source],
                "conflicting": False,
            }
        entry["marker_lines"] = matched
        evidence.append(entry)
    return records, evidence, errors


# --------------------------------------------------------------------------- #
# Value comparison
# --------------------------------------------------------------------------- #

def bound_for(family: str, leg: float, actual: Q, expected: Q) -> Q:
    """Design section 9.2: 64 * DBL_EPSILON * max(|actual|, |expected|, leg)."""
    scale = max(abs(actual), abs(expected), Q(abs(float(leg))))
    return Q(ULP_MULTIPLIER) * Q(DBL_EPSILON) * scale


def compare_leaf(path, native, pack, family, leg, failures, rows):
    kind = pack.get("kind")
    if kind == "nonfinite":
        ok = isinstance(native, float) and math.isnan(native) or native is None \
            or (isinstance(native, str) and native.strip().lower() in ("nan", "-nan"))
        if not ok:
            failures.append(f"{path}: expected a non-finite marker, native carried {native!r}")
        rows.append({"path": path, "kind": kind, "passed": bool(ok)})
        return
    if kind in ("exact_integer", "exact_u64"):
        got = native_integer(native)
        want = int(pack["integer"])
        ok = got is not None and got == want
        if not ok:
            failures.append(f"{path}: exact-integer mismatch -- native {native!r}, expected {want}")
        rows.append({"path": path, "kind": kind, "native": native, "expected": want,
                     "passed": bool(ok)})
        return
    if kind in ("exact_rational", "decimal_50"):
        got = native_fraction(native)
        want = as_fraction(pack)
        if got is None:
            failures.append(f"{path}: native value {native!r} is not a finite number")
            rows.append({"path": path, "kind": kind, "native": native, "passed": False})
            return
        residual = abs(got - want)
        if family in EXACT_FAMILIES:
            ok = residual == 0
            bound = Q(0)
        else:
            bound = bound_for(family, leg, got, want)
            ok = residual <= bound
        if not ok:
            failures.append(
                f"{path}: residual {float(residual):.6e} exceeds bound {float(bound):.6e} "
                f"(native {native!r}, expected {pack.get('exact') or pack.get('decimal')})")
        rows.append({
            "path": path, "kind": kind, "native": native,
            "expected_binary64": pack["binary64"],
            "residual": float(residual), "bound": float(bound),
            "relative_only_residual_diagnostic": (
                float(residual / abs(want)) if want != 0 else None),
            "passed": bool(ok),
        })
        return
    failures.append(f"{path}: unknown oracle expectation kind {kind!r}")
    rows.append({"path": path, "kind": kind, "passed": False})


def compare_node(path, native, expected, family, legs, failures, rows):
    if expected is None:
        if native is not None:
            failures.append(f"{path}: expected null, native carried {native!r}")
        return
    if isinstance(expected, str):
        if native != expected:
            failures.append(f"{path}: string mismatch -- native {native!r}, expected {expected!r}")
        rows.append({"path": path, "kind": "string", "native": native,
                     "expected": expected, "passed": native == expected})
        return
    if isinstance(expected, dict) and "kind" in expected:
        leg = legs.get(path.split(".")[-1].split("[")[0], 0.0)
        compare_leaf(path, native, expected, family, leg, failures, rows)
        return
    if isinstance(expected, dict):
        if not isinstance(native, dict):
            failures.append(f"{path}: expected an object, native carried {type(native).__name__}")
            return
        for key, sub in expected.items():
            if key not in native:
                continue  # unexported sub-keys are handled by the coverage report
            compare_node(f"{path}.{key}", native[key], sub, family, legs, failures, rows)
        return
    if isinstance(expected, list):
        if not isinstance(native, list):
            failures.append(f"{path}: expected a list, native carried {type(native).__name__}")
            return
        if len(native) != len(expected):
            failures.append(f"{path}: list length {len(native)} != expected {len(expected)}")
            return
        for i, sub in enumerate(expected):
            compare_node(f"{path}[{i}]", native[i], sub, family, legs, failures, rows)
        return
    if native != expected:
        failures.append(f"{path}: mismatch -- native {native!r}, expected {expected!r}")


def bootstrap_cross_checks(case_id, values, inputs, draws_default):
    """Design section 9.2: interval brackets its point estimate; reportable and
    unreportable_reason match the frozen rule; modulo_fallbacks == 0."""
    failures, notes = [], []
    if "modulo_fallbacks" in values:
        got = native_integer(values["modulo_fallbacks"])
        if got != 0:
            failures.append(f"{case_id}: modulo_fallbacks == {got}, must be 0 "
                            "(the section 3.10 bounded-loop counter)")
        else:
            notes.append("modulo_fallbacks == 0")

    lo, hi = values.get("ci_lo"), values.get("ci_hi")
    point = values.get("point_estimate")
    if lo is not None and hi is not None and point is not None:
        flo, fhi, fp = (native_fraction(lo), native_fraction(hi), native_fraction(point))
        if None in (flo, fhi, fp):
            failures.append(f"{case_id}: non-numeric interval or point estimate")
        else:
            if flo > fhi:
                failures.append(f"{case_id}: interval is inverted ({lo} > {hi})")
            if not (flo <= fp <= fhi):
                failures.append(f"{case_id}: interval [{lo}, {hi}] does not bracket its "
                                f"point estimate {point}")
            else:
                notes.append("interval brackets its point estimate")

    n = native_integer(values.get("n", inputs.get("n")))
    h = native_integer(values.get("horizon", inputs.get("horizon")))
    draws = native_integer(values.get("bootstrap_draws", inputs.get("bootstrap_draws")))
    if draws is None:
        draws = draws_default
    if n is not None and h is not None and "reportable" in values:
        want, reason, length = xs_reportable(draws, n, h)
        got = native_integer(values["reportable"])
        if got != want:
            failures.append(f"{case_id}: reportable == {got}; the frozen rule "
                            f"(draws={draws}, n={n}, L_{h}={length}) gives {want}")
        else:
            notes.append(f"reportable matches the frozen rule (L_{h} = {length})")
        if "block_len" in values:
            got_len = native_integer(values["block_len"])
            if got_len != length:
                failures.append(f"{case_id}: block_len == {got_len}, frozen rule gives {length}")
        if "unreportable_reason" in values:
            code = native_integer(values["unreportable_reason"])
            if want == 1:
                notes.append(f"unreportable_reason == {code} on a reportable series (not pinned)")
            elif reason == "series-too-short-for-block-length":
                if code != 3:
                    failures.append(f"{case_id}: unreportable_reason == {code}; the design pins "
                                    "3 for series-too-short-for-block-length")
                else:
                    notes.append("unreportable_reason == 3 as pinned")
            else:
                notes.append(f"unreportable_reason == {code} for {reason!r}; the design pins "
                             "integer codes only for 2 (common-sample-prefix-incomplete) and "
                             "3 (series-too-short-for-block-length), so this code is RECORDED, "
                             "not asserted")
    if "unreportable_reason" in values and "common_prefix_gaps" in values:
        gaps = native_integer(values["common_prefix_gaps"])
        code = native_integer(values["unreportable_reason"])
        n_block = native_integer(values.get("n", inputs.get("n")))
        # Parent ruling I-7 (T2T3 review): design section 3.10's LOWEST-NONZERO-WINS
        # precedence is the general rule, and section 3.13's blanket "gaps => 2" is
        # written for a block that is otherwise reportable. A gapped block whose
        # series is ALSO shorter than two points carries code 1, which beats 2.
        # Code 2 is therefore required only when nothing lower can hold.
        if gaps is not None and gaps > 0 and code not in (1, 2):
            failures.append(f"{case_id}: common_prefix_gaps == {gaps} must void the whole "
                            f"common block with unreportable_reason in (1, 2) — 2 normally, "
                            f"1 when the block is also short (lowest nonzero wins, I-7); "
                            f"got {code}")
        elif gaps is not None and gaps > 0 and code == 1 and (n_block is None or n_block >= 20):
            failures.append(f"{case_id}: common_prefix_gaps == {gaps} with n == {n_block} "
                            "must carry unreportable_reason == 2; code 1 is reserved for a "
                            "block that is genuinely short (I-7)")
    return failures, notes


def compare_case(case, record, draws_default):
    values = record["object"].get("values")
    inputs = record["object"].get("inputs", {})
    failures, rows = [], []
    if not isinstance(values, dict):
        return {"case_id": case["id"], "family": case["family"], "passed": False,
                "failures": [f"{case['id']}: measurement has no 'values' object"],
                "comparisons": [], "sources": record["sources"]}
    if record["conflicting"]:
        failures.append(f"{case['id']}: conflicting duplicate measurements in the logs")

    expected = case.get("expected", {})
    legs = case.get("bound_legs", {}) or {}
    compared, missing = [], []
    for key, sub in expected.items():
        if key not in values:
            missing.append(key)
            continue
        compared.append(key)
        compare_node(key, values[key], sub, case["family"], legs, failures, rows)

    bs_failures, bs_notes = bootstrap_cross_checks(case["id"], values, inputs, draws_default)
    failures.extend(bs_failures)

    unchecked = sorted(k for k in values if k not in expected)
    return {
        "case_id": case["id"],
        "family": case["family"],
        "native_export": case.get("native_export"),
        "bound_family": case["family"],
        "compared_keys": compared,
        "oracle_keys_not_exported_natively": missing,
        "native_keys_with_no_oracle_expectation": unchecked,
        "bootstrap_cross_checks": bs_notes,
        "comparisons": rows,
        "failures": failures,
        "passed": not failures,
        "sources": record["sources"],
        "line_sha256": record["line_sha256"],
    }


# --------------------------------------------------------------------------- #
# Self-test
# --------------------------------------------------------------------------- #

def fabricate(case):
    """Build a perfect in-memory measurement object from an oracle case."""
    def render(node):
        if isinstance(node, dict) and "kind" in node:
            kind = node["kind"]
            if kind in ("exact_integer",):
                return node["integer"]
            if kind == "exact_u64":
                return node["decimal_string"]
            if kind == "nonfinite":
                return "nan"
            return node["binary64"]
        if isinstance(node, dict):
            return {k: render(v) for k, v in node.items()}
        if isinstance(node, list):
            return [render(v) for v in node]
        return node

    return {
        "schema": MEASUREMENT_SCHEMA,
        "case_id": case["id"],
        "inputs": {"n": case.get("inputs", {}).get("n", {}).get("integer"),
                   "horizon": case.get("inputs", {}).get("horizon", {}).get("integer")},
        "values": render(case.get("expected", {})),
    }


def selftest(oracle) -> dict:
    """Unit-style check of the comparator itself against fabricated data.

    No file is written and no native log is read. A perfect fabricated sample
    must pass; a sample perturbed well beyond the family bound must fail; an
    exact-integer family must reject a one-off perturbation.
    """
    results = []
    draws = oracle["frozen_parameters"]["bootstrap_draws"]
    for case in oracle["cases"]:
        obj = fabricate(case)
        record = {"object": obj, "canonical": "", "line_sha256": "", "sources": ["selftest"],
                  "conflicting": False}
        good = compare_case(case, record, draws)
        results.append({"case_id": case["id"], "perfect_sample_passed": good["passed"],
                        "failures": good["failures"]})

    perturbed = []
    for case in oracle["cases"]:
        obj = fabricate(case)
        values = obj["values"]
        target = None
        for key, sub in case.get("expected", {}).items():
            if isinstance(sub, dict) and sub.get("kind") in (
                    "exact_rational", "decimal_50", "exact_integer", "exact_u64"):
                target = (key, sub)
                break
        if target is None:
            continue
        key, sub = target
        if sub["kind"] in ("exact_integer", "exact_u64"):
            values[key] = int(sub["integer"]) + 1
        else:
            base = sub["binary64"]
            values[key] = base + (abs(base) * 1e-6 if base != 0 else 1e-6)
        record = {"object": obj, "canonical": "", "line_sha256": "", "sources": ["selftest"],
                  "conflicting": False}
        bad = compare_case(case, record, draws)
        perturbed.append({"case_id": case["id"], "perturbed_key": key,
                          "perturbed_sample_rejected": not bad["passed"]})

    return {
        "perfect_samples_total": len(results),
        "perfect_samples_passed": sum(1 for r in results if r["perfect_sample_passed"]),
        "perfect_sample_failures": [r for r in results if not r["perfect_sample_passed"]],
        "perturbed_samples_total": len(perturbed),
        "perturbed_samples_rejected": sum(1 for r in perturbed
                                          if r["perturbed_sample_rejected"]),
        "perturbed_samples_not_rejected": [r for r in perturbed
                                           if not r["perturbed_sample_rejected"]],
    }


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Independent comparator: native cross-section-IC measurements vs the "
                    "exact iteration-14 oracle.")
    parser.add_argument("--oracle", default=str(DEFAULT_ORACLE),
                        help="exact oracle JSON; defaults to the v2 document produced by "
                             "iteration14_cross_section_oracle_v2.py (ruling A-6). The v1 "
                             "document remains loadable by passing it explicitly.")
    parser.add_argument("--logs", nargs="+", default=[],
                        help="ctest -VV log files to scan for CROSS_SECTION_IC_MEASUREMENT lines")
    parser.add_argument("--out", default=str(DEFAULT_OUT),
                        help="comparison JSON to create (must not exist)")
    parser.add_argument("--optional-case", action="append", default=[], metavar="CASE_ID",
                        help="downgrade one 'expected' oracle case to not_measured_natively "
                             "without failing the run; recorded in the output. Cases the "
                             "design NAMES as natively exported cannot be downgraded.")
    parser.add_argument("--selftest", action="store_true",
                        help="run the comparator against fabricated in-memory samples and "
                             "exit; writes no file and reads no log")
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
        ok = (report["perfect_samples_passed"] == report["perfect_samples_total"]
              and report["perturbed_samples_rejected"] == report["perturbed_samples_total"])
        report.update({"schema": SCHEMA, "status": "selftest-pass" if ok else "selftest-fail",
                       "oracle_sha256": oracle_sha, "file_written": False, "logs_read": False})
        print(json.dumps(report, indent=2))
        return 0 if ok else 1

    out_path = Path(args.out)
    if out_path.exists():
        print(json.dumps({"schema": SCHEMA, "status": "refused",
                          "reason": "output already exists; refusing to overwrite",
                          "out": posix(out_path)}, separators=(",", ":")))
        return 1

    self_sha, self_len = sha256_and_len(SELF)
    draws_default = oracle["frozen_parameters"]["bootstrap_draws"]
    cases_by_id = {c["id"]: c for c in oracle["cases"]}

    design_named = [c["id"] for c in oracle["cases"] if c.get("native_export") == "required"]
    expected_natively = [c["id"] for c in oracle["cases"]
                         if c.get("native_export") == "expected"]
    optional = []
    global_failures = []
    for cid in args.optional_case:
        if cid in design_named:
            global_failures.append(
                f"--optional-case {cid!r} is refused: the design NAMES this case as natively "
                "exported (section 9.1 case 13b / section 9.2 F6)")
        elif cid not in cases_by_id:
            global_failures.append(f"--optional-case {cid!r} is not an oracle case id")
        else:
            optional.append(cid)

    required = [cid for cid in design_named + expected_natively if cid not in optional]

    cross_rows = cross_check(oracle)
    for row in cross_rows:
        global_failures.extend(row["failures"])

    log_paths = [Path(p) for p in args.logs]
    if not log_paths:
        global_failures.append("no --logs given: there is nothing to compare against")
    records, log_evidence, scan_errors = scan_logs(log_paths)
    global_failures.extend(scan_errors)

    case_results, not_measured = [], []
    for cid in required:
        if cid in records:
            case_results.append(compare_case(cases_by_id[cid], records[cid], draws_default))
        else:
            not_measured.append({
                "case_id": cid, "family": cases_by_id[cid]["family"],
                "status": "not_measured_natively",
                "required": True,
                "design_named": cid in design_named,
            })
            global_failures.append(
                f"required case {cid!r} has no native measurement in the scanned logs "
                "(recorded as not_measured_natively, never as passing)")
    for cid in optional:
        not_measured.append({
            "case_id": cid, "family": cases_by_id[cid]["family"],
            "status": "not_measured_natively", "required": False,
            "downgraded_by": "--optional-case",
        })

    unexpected = sorted(cid for cid in records if cid not in cases_by_id)
    for cid in unexpected:
        global_failures.append(f"native measurement {cid!r} matches no oracle case id")

    passed = (not global_failures) and all(c["passed"] for c in case_results) \
        and len(case_results) == len(required)

    document = {
        "schema": SCHEMA,
        "status": "pass" if passed else "fail",
        "qualification": QUALIFICATION,
        "independence": {
            "native_cpp_source_read": False,
            "build_or_test_executed_by_this_script": False,
            "expected_values_rederived_independently_of_the_oracle": True,
            "stdlib_only": True,
        },
        "bound_policy": {
            "eps": DBL_EPSILON,
            "ulp_multiplier": ULP_MULTIPLIER,
            "formula": "64 * DBL_EPSILON * max(|actual|, |expected|, declared_leg)",
            "exact_families": list(EXACT_FAMILIES),
            "per_family": oracle["bound_policy"]["families"],
            "rationale": (
                "Bounds come from the contributing legs the design declares per family, "
                "never from an absolute floor and never as a relative test against zero. "
                "F4 (coverage counters) and F6 (bootstrap stream reproduction) are "
                "exact-integer and take no tolerance at all. A non-binding "
                "relative-only residual is recorded beside every bounded comparison."
            ),
            "pinned_unreportable_reason_codes": {str(k): v for k, v in PINNED_REASON_CODES.items()},
        },
        "evidence": {
            "comparator_script": {"path": posix(SELF), "sha256": self_sha, "bytes": self_len},
            "oracle": {"path": posix(oracle_path), "sha256": oracle_sha, "bytes": oracle_len,
                       "schema": oracle.get("schema"),
                       "case_count": oracle.get("case_count")},
            "logs": log_evidence,
        },
        "convention_cross_check": {
            "description": "the design's arithmetic re-derived in this file, from each oracle "
                           "case's declared INPUTS, and compared exactly against the oracle's "
                           "expected values before any native data was read",
            "rows": cross_rows,
            "all_exact": all(r["exact_agreement"] for r in cross_rows),
        },
        "design_named_required_case_ids": design_named,
        "expected_natively_case_ids": expected_natively,
        "optional_case_ids": optional,
        "required_case_ids": required,
        "native_case_ids_found": sorted(records),
        "native_case_ids_with_no_oracle_case": unexpected,
        "global_failures": global_failures,
        "cases": case_results,
        "not_measured_natively": not_measured,
        "counts": {
            "oracle_cases": len(oracle["cases"]),
            "required": len(required),
            "compared": len(case_results),
            "passed": sum(1 for c in case_results if c["passed"]),
            "failed": sum(1 for c in case_results if not c["passed"]),
            "not_measured_natively": len(not_measured),
            "global_failures": len(global_failures),
        },
    }

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, indent=2, allow_nan=False)
        stream.write("\n")

    print(json.dumps({
        "schema": SCHEMA,
        "status": document["status"],
        "counts": document["counts"],
        "comparator_sha256": self_sha,
        "oracle_sha256": oracle_sha,
        "out": posix(out_path),
        "qualification": QUALIFICATION,
    }, separators=(",", ":")))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
