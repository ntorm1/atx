"""Checkpoint 15 / T4: the one bounded native `equity-universe` real-data run.

Parent-executed. This script does NOT build, configure or test anything: the wrapper-driven
configure / check / build / ctest sequence is run by hand before this. Here the already-built
`atx-impl.exe` is invoked once, under a working-set monitor, on the seven ingested
2012-2019 segment directories with EXACTLY the allow-listed flags and the pinned real-run
values of design sections 5.2 and 6 (Revision 3, frozen).

Contract, from the checkpoint-14 runner precedent (`iteration14_run_equity_ic.py`) and
design section 10 T3:
  * `--attempt N` versions every audit file AND the data output directory, so a failed
    earlier attempt's evidence is kept, never overwritten and never in the way; only THIS
    attempt's own paths are refused when they already exist;
  * preflight refuses the run unless the on-disk design note hashes to the frozen digest,
    the stage source embeds the SAME digest as `kEquityUniverseDesignNoteSha256`, the
    executable carries the `equity-universe` subcommand string, no attached directory holds
    a segment dated >= 2020-01-01 (R15-13), the directories are disjoint and in date order,
    `--rank-end` is 2019-11-29 (DR15-3), every preparation manifest hashes to the value its
    ingestion manifest binds, the trial ledger verifies and carries NO stale `.lock`, and the
    output root does not exist;
  * inputs, sources and the executable are pinned by SHA-256 + byte count before and after,
    and compared BY DIGEST (never by path string -- the cp14 attempt-1 pin bug);
  * the trial ledger is verified independently here, in Python, before and after: the stage
    appends its own pre-registration and terminal lines (section 5.6), so this script asserts
    the chain gained exactly two links, both `checkpoint 15`, `trial_count_declared 0`,
    purpose `point-in-time-universe-construction`;
  * the run is accepted only if `membership.bin` decodes to exactly 84 rebalances whose first
    rank key is 2012-12-31, last rank key 2019-11-29 and last effective key 2019-12-02, and
    `request.json` agrees (section 6 "Real-run flags");
  * the child's peak working set is sampled every 100 ms (design section 7 / M-4) and a
    breach of `--max-working-bytes` terminates the child;
  * the oracle and comparator are recorded, NOT executed against this run: they bind the
    ENGINE TEST log's POINT_IN_TIME_UNIVERSE_MEASUREMENT lines, not real-data numbers.

Nothing in this file claims investment quality: the output is membership lists and counts.

  python build-equity/audits/iteration15_run_equity_universe.py --selftest
  python build-equity/audits/iteration15_run_equity_universe.py --dry-run [--pin-segments]
  python build-equity/audits/iteration15_run_equity_universe.py [--attempt N]
"""
from __future__ import annotations

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path("C:/atx/.worktrees/equity-platform")
SHARED = Path("C:/atx")
AUDITS = ROOT / "build-equity/audits"
EXE = ROOT / "build-equity/bin/atx-impl.exe"
SUBCOMMAND = "equity-universe"

# Design section 2.1: the real-run --segments-dirs list IN DATE ORDER and the positionally
# paired preparation manifests.
SEGMENT_DIRS = (
    SHARED / "data/tickerhistory_training_native_20260919/segments",
    SHARED / "data/tickerhistory_training_native_2014_20260920/segments",
    SHARED / "data/tickerhistory_training_native_2015_20260920/segments",
    SHARED / "data/tickerhistory_training_native_2016_20260920/segments",
    SHARED / "data/tickerhistory_training_native_2017_20260920/segments",
    SHARED / "data/tickerhistory_training_native_2018_20260920/segments",
    SHARED / "data/tickerhistory_training_native_2019_20260920/segments",
)
PREPARATION_MANIFESTS = (
    SHARED / "data/tickerhistory_training_20120326_20131231_20260919/manifest.json",
    SHARED / "data/tickerhistory_training_20140101_20141231_20260920/manifest.json",
    SHARED / "data/tickerhistory_training_20150101_20151231_20260920/manifest.json",
    SHARED / "data/tickerhistory_training_20160101_20161231_20260920/manifest.json",
    SHARED / "data/tickerhistory_training_20170101_20171231_20260920/manifest.json",
    SHARED / "data/tickerhistory_training_20180101_20181231_20260920/manifest.json",
    SHARED / "data/tickerhistory_training_20190101_20191231_20260920/manifest.json",
)
EXPECTED_DATES_PER_DIR = (445, 252, 252, 252, 251, 251, 252)     # section 2.1 dates_written
EXPECTED_SESSIONS = 1955                                          # section 2.1
EXPECTED_SESSIONS_AT_OR_BEFORE_RANK_START = 193                   # section 2.1 (>= 63 warmup)
INGESTION_MANIFEST = "_ingestion.manifest.json"
INGESTION_SCHEMA = "atx-ingestion-v1"

OUTPUT_STEM = "equity_universe_pit_2013_2019_20260920"
LEDGER = ROOT / "atx-engine/reviews/trial-ledger.jsonl"
LEDGER_SIDECAR = ROOT / "atx-engine/reviews/trial-ledger.manifest.json"
LEDGER_LOCK = Path(str(LEDGER) + ".lock")        # trial_ledger.hpp: `<ledger_path>.lock`
DESIGN_NOTE = ROOT / "atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md"
# Design section 5.8 / R15-16: the stage embeds this digest as
# kEquityUniverseDesignNoteSha256 and never reads the note. This preflight is the other half
# of that binding: the on-disk note must hash to it AND the stage source must embed the same
# value. A later edit to the design breaks this check BY DESIGN.
# Re-pinned by the parent after the design's section 15 amendment (Revision 3 + section 15,
# 1,060 lines). The previous value be4b3b670eaced4f0b6f5cee8a7d5e01dc8b201ae1ecd4ef8b3377bcdb41020f
# is the pre-section-15 digest that the oracle v1 still records as its source (section 15.5).
DESIGN_NOTE_SHA256 = "4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8"
# Digests an oracle may cite as its design source: the frozen digest and its documented
# pre-section-15 predecessor (section 15.5).
ORACLE_DESIGN_LINEAGE = (
    DESIGN_NOTE_SHA256,
    "be4b3b670eaced4f0b6f5cee8a7d5e01dc8b201ae1ecd4ef8b3377bcdb41020f",
)
STAGE_SOURCE = ROOT / "atx-impl/src/stage_equity_universe.cpp"
STAGE_CONSTANT = "kEquityUniverseDesignNoteSha256"
ORACLE = AUDITS / "iteration15-universe-oracle-v1.json"
ORACLE_SCHEMA = "atx-iteration15-universe-oracle-v1"
COMPARATOR = AUDITS / "iteration15_native_comparator.py"

RANK_START = "2012-12-31"
RANK_END = "2019-11-29"                # DR15-3, pinned
EXPECTED_LAST_EFFECTIVE = "2019-12-02"
VALIDATION_BEGIN = "2020-01-01"        # R15-13: refuse, never skip
LIMIT = 3_000_000_000
SAMPLE_SECONDS = 0.1
GENESIS = "0" * 64
EXPECTED_REBALANCES = 84
PURPOSE = "point-in-time-universe-construction"
CHECKPOINT = 15
NS_PER_DAY = 86_400_000_000_000
BIN_MAGIC = b"ATXPITU1"
EXPECTED_BIN_CONFIG = {"adv_window": 63, "min_valid_observations": 57,
                       "min_raw_price_exclusive": 1.0, "top_n": [1000, 2000, 3000],
                       "band_bp": [0, 1000]}

SOURCE_NAMES = (
    "atx-impl/src/stage_equity_universe.hpp",
    "atx-impl/src/stage_equity_universe.cpp",
    "atx-impl/src/trial_ledger.hpp",
    "atx-impl/src/trial_ledger.cpp",
    "atx-impl/src/config.hpp",
    "atx-impl/src/config.cpp",
    "atx-impl/src/dispatch.cpp",
    "atx-impl/CMakeLists.txt",
    "atx-engine/include/atx/engine/data/point_in_time_universe.hpp",
    "atx-engine/src/data/point_in_time_universe.cpp",
    "atx-engine/CMakeLists.txt",
    "atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md",
)

PUBLISHED = ("request.json", "seal.json", "membership.csv", "churn.csv", "coverage_by_year.csv",
             "union_by_year.csv", "delisting.csv", "survivorship.json", "membership.bin",
             "manifest.json")

SEG_NAME = re.compile(r"^(\d{4})-(\d{2})-(\d{2})\.seg$")
CONSTANT_RE = re.compile(STAGE_CONSTANT + r"\s*=\s*\"([0-9a-fA-F]{64})\"")


# ---------------------------------------------------------------------------
#  Pure helpers (all covered by --selftest)
# ---------------------------------------------------------------------------
def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> tuple:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def pin(path: Path) -> dict:
    sha, size = sha256_file(path)
    return {"path": str(path), "sha256": sha, "bytes": size}


def is_pin(entry) -> bool:
    """A pin is exactly {path, sha256, bytes}; notes and facts never qualify."""
    return (isinstance(entry, dict) and isinstance(entry.get("path"), str)
            and isinstance(entry.get("sha256"), str) and isinstance(entry.get("bytes"), int))


def pins_differ(before: dict, after: dict) -> list:
    """Compare BY DIGEST AND SIZE, never by path text (the cp14 attempt-1 pin bug). Entries
    that are not pins (a stray note, cp15 attempt-1 crash) are ignored on both sides."""
    changed = []
    keys = {k for k, v in before.items() if is_pin(v)} | {k for k, v in after.items() if is_pin(v)}
    for key in sorted(keys):
        a, b = before.get(key), after.get(key)
        if not is_pin(a) or not is_pin(b) or a["sha256"] != b["sha256"] or a["bytes"] != b["bytes"]:
            changed.append(key)
    return changed


def suffixed(name: str, attempt: int) -> str:
    """Every audit file of this runner carries its attempt ordinal (the brief pins
    `iteration15-equity-universe-measurement-attemptN.json`), attempt 1 included."""
    stem, dot, extension = name.partition(".")
    return f"{stem}-attempt{attempt}{dot}{extension}"


def output_root(attempt: int) -> Path:
    """The DATA output directory under C:/atx/data, versioned per attempt (cp14 rule: attempt 1
    keeps the plain stamped name; a retry gets `_attemptN`)."""
    if attempt <= 1:
        return SHARED / "data" / OUTPUT_STEM
    return SHARED / "data" / f"{OUTPUT_STEM}_attempt{attempt}"


def days_from_civil(y: int, m: int, d: int) -> int:
    y -= 1 if m <= 2 else 0
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def civil_from_days(z: int) -> tuple:
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
    y, m, d = (int(p) for p in iso.split("-"))
    return days_from_civil(y, m, d) * NS_PER_DAY


def nanos_to_date(key: int) -> str:
    y, m, d = civil_from_days(key // NS_PER_DAY)
    return f"{y:04d}-{m:02d}-{d:02d}"


def segment_dates(names) -> dict:
    """Filename -> ISO date for `*.seg` names; a non-date `.seg` name is an error (section 5.2)."""
    out = {}
    for name in sorted(names):
        if not name.endswith(".seg"):
            continue
        match = SEG_NAME.match(name)
        if not match:
            raise ValueError(f"segment name is not YYYY-MM-DD.seg: {name}")
        out[name] = name[:-4]
    return out


def seal_violations(dates, boundary: str = VALIDATION_BEGIN) -> list:
    """R15-13: every segment dated >= the validation boundary (refuse, never skip)."""
    limit = date_to_nanos(boundary)
    return sorted(d for d in dates if date_to_nanos(d) >= limit)


def ranges_disjoint_and_ordered(per_dir_dates) -> list:
    """per_dir_dates: list of sorted ISO date lists, one per dir, in the order given."""
    problems = []
    for i in range(len(per_dir_dates) - 1):
        left, right = per_dir_dates[i], per_dir_dates[i + 1]
        if not left or not right:
            problems.append(f"dir {i} or {i + 1} has no segments")
            continue
        if left[-1] >= right[0]:
            problems.append(f"dir {i} (..{left[-1]}) is not strictly before dir {i + 1} ({right[0]}..)")
    seen = {}
    for i, dates in enumerate(per_dir_dates):
        for d in dates:
            if d in seen:
                problems.append(f"date {d} appears in dirs {seen[d]} and {i}")
            seen[d] = i
    return problems


def embedded_constant(source_text: str) -> str:
    matches = CONSTANT_RE.findall(source_text)
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {STAGE_CONSTANT} = \"<64 hex>\" in the stage "
                         f"source, found {len(matches)}")
    return matches[0].lower()


def fnv1a64(data: bytes) -> int:
    h = 14695981039346656037
    for byte in data:
        h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return h


def decode_membership_bin_header(blob: bytes) -> dict:
    """Design section 4.7: header + per-rebalance keys; member payloads are skipped by size."""
    if len(blob) < 8 + 4 + 4 + 4 + 8 + 4 + 4 + 4 + 8:
        raise ValueError("membership.bin: short buffer")
    if blob[:8] != BIN_MAGIC:
        raise ValueError("membership.bin: bad magic")
    body, trailer = blob[:-8], struct.unpack("<Q", blob[-8:])[0]
    if fnv1a64(body) != trailer:
        raise ValueError("membership.bin: FNV-1a-64 trailer mismatch")
    pos = 8

    def take(fmt):
        nonlocal pos
        size = struct.calcsize(fmt)
        if pos + size > len(body):
            raise ValueError("membership.bin: short buffer")
        value = struct.unpack(fmt, body[pos:pos + size])
        pos += size
        return value[0]

    version = take("<I")
    if version != 1:
        raise ValueError(f"membership.bin: bad version {version}")
    image = {"adv_window": take("<I"), "min_valid_observations": take("<I"),
             "min_raw_price_exclusive": take("<d")}
    t = take("<I")
    image["top_n"] = [take("<I") for _ in range(t)]
    b = take("<I")
    image["band_bp"] = [take("<I") for _ in range(b)]
    r = take("<I")
    image["rebalance_count"] = r
    image["rebalances"] = []
    for _ in range(r):
        rank_key, effective_key = take("<q"), take("<q")
        counts = []
        for _c in range(t * b):
            n = take("<I")
            pos += n * 8 + n * 4
            if pos > len(body):
                raise ValueError("membership.bin: short buffer")
            counts.append(n)
        image["rebalances"].append({"rank_session_key": rank_key,
                                    "effective_session_key": effective_key,
                                    "member_counts": counts})
    if pos != len(body):
        raise ValueError("membership.bin: trailing bytes before the trailer")
    image["fnv1a64_trailer"] = trailer
    return image


def find_named_int(node, names) -> list:
    """Every integer found under a key in `names`, anywhere in a JSON document."""
    found = []
    if isinstance(node, dict):
        for key, value in node.items():
            if key in names and isinstance(value, int) and not isinstance(value, bool):
                found.append(value)
            found.extend(find_named_int(value, names))
    elif isinstance(node, list):
        for value in node:
            found.extend(find_named_int(value, names))
    return found


def manifest_digests(node) -> dict:
    """basename -> sha256 for every object carrying `sha256` and a file name key."""
    out = {}
    if isinstance(node, dict):
        name = None
        for key in ("filename", "file", "name", "path"):
            if isinstance(node.get(key), str):
                name = Path(node[key]).name
                break
        if name and isinstance(node.get("sha256"), str):
            out[name] = node["sha256"].lower()
        for value in node.values():
            out.update(manifest_digests(value))
    elif isinstance(node, list):
        for value in node:
            out.update(manifest_digests(value))
    return out


def verify_ledger(ledger: Path, sidecar: Path) -> dict:
    """Independent Python walk of the append-only chain (cp14 design section 4.5)."""
    if not ledger.exists():
        return {"lines": 0, "head_sha256": GENESIS, "anchored": False, "present": False, "trials": []}
    payload = ledger.read_bytes()
    if payload and not payload.endswith(b"\n"):
        raise ValueError("trial ledger has a torn final line")
    lines = payload.split(b"\n")[:-1] if payload else []
    previous = GENESIS
    trials = []
    for index, raw in enumerate(lines):
        if not raw:
            raise ValueError(f"trial ledger line {index} is empty")
        if b"\r" in raw:
            raise ValueError(f"trial ledger line {index} carries a CR")
        entry = json.loads(raw.decode("utf-8"))
        if entry.get("prev_sha256") != previous:
            raise ValueError(f"trial ledger chain breaks at line {index}")
        trials.append({"trial_id": entry.get("trial_id"), "status": entry.get("status"),
                       "checkpoint": entry.get("checkpoint"), "purpose": entry.get("purpose"),
                       "trial_count_declared": entry.get("trial_count_declared"),
                       "outcome": (entry.get("result") or {}).get("outcome"),
                       "line_sha256": sha256_bytes(raw + b"\n")})
        previous = sha256_bytes(raw + b"\n")
    anchored = False
    if sidecar.exists():
        recorded = json.loads(sidecar.read_text(encoding="utf-8"))
        if recorded.get("lines") != len(lines) or recorded.get("head_sha256") != previous:
            raise ValueError("trial ledger sidecar disagrees with the walked chain")
        anchored = True
    elif lines:
        raise ValueError("trial ledger sidecar is missing beside a non-empty ledger")
    return {"lines": len(lines), "head_sha256": previous, "anchored": anchored,
            "present": True, "trials": trials}


def command(output: Path) -> list:
    """EXACTLY the allow-listed flags of design section 5.2 with the section 6 values."""
    return [str(EXE), SUBCOMMAND,
            "--segments-dirs", ";".join(str(d).replace("\\", "/") for d in SEGMENT_DIRS),
            "--preparation-manifests", ";".join(str(m).replace("\\", "/") for m in PREPARATION_MANIFESTS),
            "--out", str(output).replace("\\", "/"),
            "--rank-start", RANK_START,
            "--rank-end", RANK_END,
            "--max-working-bytes", str(LIMIT),
            "--trial-ledger", str(LEDGER).replace("\\", "/")]


# ---------------------------------------------------------------------------
#  Working-set monitor (PROCESS_MEMORY_COUNTERS_EX via psapi), as cp14
# ---------------------------------------------------------------------------
if os.name == "nt":
    from ctypes import wintypes

    class _Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]

    def read_memory(process) -> dict:
        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, process.pid)
        if not handle:
            return {}
        try:
            counters = _Counters()
            counters.cb = ctypes.sizeof(_Counters)
            if not ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return {}
            names = [field[0] for field in _Counters._fields_]
            return {name: int(getattr(counters, name)) for name in names
                    if name not in ("cb", "PageFaultCount")}
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
else:  # pragma: no cover -- --selftest / --help only off Windows
    def read_memory(process) -> dict:
        return {}


# ---------------------------------------------------------------------------
#  Preflight: every check appends to `problems`; nothing is launched while any remain
# ---------------------------------------------------------------------------
def preflight(output: Path, pin_segments: bool) -> dict:
    problems, checks, facts, notes = [], {}, {}, []

    def must_exist(label, path):
        if not path.exists():
            problems.append(f"missing {label}: {path}")
            return False
        return True

    # Executable, stage source, design note, oracle, comparator.
    if must_exist("executable", EXE):
        checks["executable"] = pin(EXE)
        if SUBCOMMAND.encode("ascii") not in EXE.read_bytes():
            problems.append(f"executable does not carry the {SUBCOMMAND!r} subcommand string; stale build")
    if must_exist("design_note", DESIGN_NOTE):
        checks["design_note"] = pin(DESIGN_NOTE)
        if checks["design_note"]["sha256"] != DESIGN_NOTE_SHA256:
            problems.append("design note has changed since the freeze: on disk "
                            f"{checks['design_note']['sha256']}, frozen {DESIGN_NOTE_SHA256}; a changed "
                            "design is a new pre-registration")
    if must_exist("stage_source", STAGE_SOURCE):
        checks["stage_source"] = pin(STAGE_SOURCE)
        try:
            embedded = embedded_constant(STAGE_SOURCE.read_text(encoding="utf-8"))
            facts["stage_embedded_design_sha256"] = embedded
            if embedded != DESIGN_NOTE_SHA256:
                problems.append(f"stage embeds {STAGE_CONSTANT} = {embedded}, runner pins "
                                f"{DESIGN_NOTE_SHA256}; re-embed deliberately, rebuild, re-run")
        except ValueError as error:
            problems.append(str(error))
    if must_exist("oracle", ORACLE):
        checks["oracle"] = pin(ORACLE)
        oracle = json.loads(ORACLE.read_text(encoding="utf-8"))
        if oracle.get("schema") != ORACLE_SCHEMA:
            problems.append(f"oracle schema {oracle.get('schema')!r} != {ORACLE_SCHEMA!r}")
        if oracle.get("design_note_sha256") not in ORACLE_DESIGN_LINEAGE:
            problems.append("oracle was derived from a different design revision")
        elif oracle.get("design_note_sha256") != DESIGN_NOTE_SHA256:
            # A NOTE, never a pin: `checks` holds only {path, sha256, bytes} entries because it
            # becomes `source_and_input_pins_before` and is re-pinned after the run.
            facts["oracle_design_lineage"] = (
                "oracle v1 was cut against the pre-section-15 digest "
                f"{oracle.get('design_note_sha256')}; sections 1-14 are unchanged and "
                "section 15.5 records this lineage (parent ruling)")
            notes.append(facts["oracle_design_lineage"])
    if must_exist("comparator", COMPARATOR):
        checks["comparator"] = pin(COMPARATOR)

    # Segment directories: manifests, names, seal, order, counts, positional pairing.
    if len(SEGMENT_DIRS) != len(PREPARATION_MANIFESTS):
        problems.append("segments-dirs and preparation-manifests counts differ")
    per_dir, per_dir_dates = [], []
    for index, (directory, prep) in enumerate(zip(SEGMENT_DIRS, PREPARATION_MANIFESTS)):
        entry = {"segments_dir": str(directory), "preparation_manifest": str(prep)}
        per_dir.append(entry)
        if not directory.is_dir():
            problems.append(f"missing segments dir {index}: {directory}")
            per_dir_dates.append([])
            continue
        try:
            dates = segment_dates(p.name for p in directory.iterdir())
        except ValueError as error:
            problems.append(f"dir {index}: {error}")
            dates = {}
        ordered = sorted(dates.values())
        per_dir_dates.append(ordered)
        entry.update({"segments": len(ordered), "first": ordered[0] if ordered else None,
                      "last": ordered[-1] if ordered else None,
                      "expected_segments": EXPECTED_DATES_PER_DIR[index]})
        if len(ordered) != EXPECTED_DATES_PER_DIR[index]:
            problems.append(f"dir {index} has {len(ordered)} segments, section 2.1 pins "
                            f"{EXPECTED_DATES_PER_DIR[index]}")
        sealed = seal_violations(ordered)
        if sealed:
            problems.append(f"dir {index} holds {len(sealed)} segment(s) at/after {VALIDATION_BEGIN}: "
                            f"{sealed[:3]} (R15-13: refuse, never skip)")
        ingestion = directory / INGESTION_MANIFEST
        if not ingestion.exists():
            problems.append(f"dir {index}: {INGESTION_MANIFEST} missing")
            continue
        entry["ingestion_manifest"] = pin(ingestion)
        manifest = json.loads(ingestion.read_text(encoding="utf-8"))
        if manifest.get("schema") != INGESTION_SCHEMA:
            problems.append(f"dir {index}: ingestion manifest schema {manifest.get('schema')!r}")
        bound = ((manifest.get("preparation") or {}).get("manifest_sha256") or "").lower()
        recipe = manifest.get("recipe")
        if isinstance(recipe, str):          # atx-ingestion-v1 stores the recipe as a JSON string
            try:
                recipe = json.loads(recipe)
            except ValueError:
                recipe = None
        entry["ingestion_recipe_executable_sha256"] = (
            recipe.get("executable_sha256") if isinstance(recipe, dict) else None)
        if not prep.exists():
            problems.append(f"missing preparation manifest {index}: {prep}")
        else:
            entry["preparation_manifest_pin"] = pin(prep)
            if entry["preparation_manifest_pin"]["sha256"] != bound:
                problems.append(f"dir {index}: preparation manifest hashes to "
                                f"{entry['preparation_manifest_pin']['sha256']}, ingestion manifest "
                                f"binds {bound or '<absent>'} (positional pairing, section 5.2)")
        listed = manifest_digests(manifest.get("segments", []))
        entry["manifest_listed_segments"] = len(listed)
        absent_from_manifest = sorted(n for n in dates if n not in listed)
        if absent_from_manifest:
            problems.append(f"dir {index}: {len(absent_from_manifest)} segment file(s) absent from the "
                            f"ingestion manifest, e.g. {absent_from_manifest[:3]}")
        if pin_segments:
            mismatched, pinned = [], {}
            for name in sorted(dates):
                sha, size = sha256_file(directory / name)
                pinned[name] = {"sha256": sha, "bytes": size}
                if listed.get(name) != sha:
                    mismatched.append(name)
            entry["segment_pins"] = pinned
            entry["segment_bytes_total"] = sum(p["bytes"] for p in pinned.values())
            if mismatched:
                problems.append(f"dir {index}: {len(mismatched)} segment digest(s) differ from the "
                                f"ingestion manifest, e.g. {mismatched[:3]} (I-12)")
    problems.extend(ranges_disjoint_and_ordered(per_dir_dates))
    all_dates = sorted(d for dates in per_dir_dates for d in dates)
    facts["sessions_attached"] = len(all_dates)
    if all_dates and len(all_dates) != EXPECTED_SESSIONS:
        problems.append(f"{len(all_dates)} sessions attached, section 2.1 pins {EXPECTED_SESSIONS}")
    warm = sum(1 for d in all_dates if d <= RANK_START)
    facts["sessions_at_or_before_rank_start"] = warm
    if all_dates and warm < 63:
        problems.append(f"only {warm} sessions at or before {RANK_START}; 63 required (R15-8)")
    if all_dates and warm != EXPECTED_SESSIONS_AT_OR_BEFORE_RANK_START:
        problems.append(f"{warm} sessions at or before {RANK_START}, section 2.1 pins "
                        f"{EXPECTED_SESSIONS_AT_OR_BEFORE_RANK_START}")
    # Rank window (DR15-3, R15-13, C-6).
    if RANK_END != "2019-11-29":
        problems.append("--rank-end must be 2019-11-29 (DR15-3)")
    if date_to_nanos(RANK_END) >= date_to_nanos(VALIDATION_BEGIN):
        problems.append("--rank-end must be <= 2019-12-31")
    if all_dates and RANK_END >= all_dates[-1]:
        problems.append(f"--rank-end {RANK_END} must be < the last attached session {all_dates[-1]}")
    if all_dates and RANK_START not in all_dates:
        problems.append(f"--rank-start {RANK_START} is not an attached session")
    if all_dates and RANK_END not in all_dates:
        problems.append(f"--rank-end {RANK_END} is not an attached session")

    # Ledger: no stale lock, chain verifies.
    if LEDGER_LOCK.exists():
        problems.append(f"stale trial-ledger lock present: {LEDGER_LOCK}; the stage would refuse "
                        "with Unavailable. Investigate the owner; this runner never removes a lock")
    try:
        ledger_before = verify_ledger(LEDGER, LEDGER_SIDECAR)
    except ValueError as error:
        problems.append(f"trial ledger does not verify: {error}")
        ledger_before = None
    if output.exists():
        problems.append(f"output root already exists: {output}")

    return {"problems": problems, "files": checks, "facts": facts, "notes": notes,
            "segment_dirs": per_dir,
            "ledger_before": ledger_before, "output_root": str(output),
            "design_note_sha256": DESIGN_NOTE_SHA256,
            "design_note_matches_frozen_digest": checks.get("design_note", {}).get("sha256") == DESIGN_NOTE_SHA256,
            "stage_embeds_frozen_digest": facts.get("stage_embedded_design_sha256") == DESIGN_NOTE_SHA256,
            "segments_pinned": pin_segments,
            "oracle_scope": "the oracle and comparator bind the ENGINE TEST log, not this real-data "
                            "run; neither is executed here",
            "rank_start": RANK_START, "rank_end": RANK_END,
            "expected_rebalances": EXPECTED_REBALANCES,
            "working_memory_limit_bytes": LIMIT}


def run_native(args, stdout_log: Path, stderr_log: Path, samples_log: Path) -> dict:
    maxima: dict = {}
    count = 0
    started = time.perf_counter()
    with stdout_log.open("xb") as out, stderr_log.open("xb") as err, \
            samples_log.open("x", encoding="utf-8", newline="\n") as samples:
        process = subprocess.Popen(args, cwd=str(ROOT), stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        breached = None
        while True:
            sample = read_memory(process)
            count += 1
            for key, value in sample.items():
                maxima[key] = max(maxima.get(key, 0), value)
            sample["elapsed_seconds"] = time.perf_counter() - started
            samples.write(json.dumps(sample, sort_keys=True) + "\n")
            samples.flush()
            code = process.poll()
            breached = {k: v for k, v in maxima.items()
                        if k in ("WorkingSetSize", "PeakWorkingSetSize", "PrivateUsage",
                                 "PagefileUsage", "PeakPagefileUsage") and v > LIMIT}
            if breached:
                if code is None:
                    process.terminate()  # only this Popen-owned child, never another process
                    code = process.wait(timeout=30)
                break
            if code is not None:
                break
            time.sleep(SAMPLE_SECONDS)
    return {"exit_code": code, "wall_seconds": time.perf_counter() - started,
            "sample_count": count, "sampled_os_maxima_bytes": maxima,
            "budget_breach": breached or None}


def output_evidence(output: Path) -> dict:
    files = {name: pin(output / name) for name in PUBLISHED if (output / name).exists()}
    missing = [name for name in PUBLISHED if name not in files]
    report = {"files": files, "missing": missing,
              "pending_marker_present": (output / ".pending").exists(),
              "failure_present": (output / "failure.json").exists()}
    if "manifest.json" in files:
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        recorded = manifest_digests(manifest)
        report["manifest_files_match"] = all(recorded.get(n) == files[n]["sha256"]
                                             for n in files if n in recorded)
        report["manifest_files_recorded"] = sorted(n for n in recorded if n in files)
        report["runtime_seconds"] = manifest.get("runtime_seconds")
    if "request.json" in files:
        request = json.loads((output / "request.json").read_text(encoding="utf-8"))
        found = find_named_int(request, ("rebalances", "rebalance_count", "observations"))
        report["request_rebalances_found"] = found
        report["request_rebalances_match"] = EXPECTED_REBALANCES in found
    if "membership.bin" in files:
        try:
            image = decode_membership_bin_header((output / "membership.bin").read_bytes())
            rebs = image["rebalances"]
            header_ok = all(image[k] == v for k, v in EXPECTED_BIN_CONFIG.items())
            report["membership_bin"] = {
                "rebalance_count": image["rebalance_count"], "header_matches_frozen": header_ok,
                "first_rank_date": nanos_to_date(rebs[0]["rank_session_key"]) if rebs else None,
                "last_rank_date": nanos_to_date(rebs[-1]["rank_session_key"]) if rebs else None,
                "last_effective_date": nanos_to_date(rebs[-1]["effective_session_key"]) if rebs else None,
                "all_effective_positive": all(r["effective_session_key"] > 0 for r in rebs),
                "fnv1a64_trailer": str(image["fnv1a64_trailer"]),
                "member_counts_final": rebs[-1]["member_counts"] if rebs else None,
            }
            report["membership_bin_ok"] = (
                header_ok and image["rebalance_count"] == EXPECTED_REBALANCES
                and report["membership_bin"]["first_rank_date"] == RANK_START
                and report["membership_bin"]["last_rank_date"] == RANK_END
                and report["membership_bin"]["last_effective_date"] == EXPECTED_LAST_EFFECTIVE
                and report["membership_bin"]["all_effective_positive"])
        except ValueError as error:
            report["membership_bin"] = {"error": str(error)}
            report["membership_bin_ok"] = False
    return report


def ledger_delta_ok(before: dict, after: dict) -> tuple:
    gained = after["lines"] - before["lines"]
    new = after["trials"][before["lines"]:]
    ok = (gained == 2 and len(new) == 2
          and all(t["checkpoint"] == CHECKPOINT and t["purpose"] == PURPOSE
                  and t["trial_count_declared"] == 0 for t in new)
          and new[0]["status"] == "pre-registered" and new[1]["status"] == "completed"
          and new[0]["trial_id"] == new[1]["trial_id"])
    return gained, new, ok


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--attempt", type=int, default=1,
                        help="retry ordinal; every output carries it, nothing is overwritten")
    parser.add_argument("--dry-run", action="store_true",
                        help="run every precondition, print the exact command; launch nothing")
    parser.add_argument("--pin-segments", action="store_true",
                        help="dry-run only: also SHA-256 every .seg against the ingestion manifest "
                             "(always done for a real run)")
    parser.add_argument("--selftest", action="store_true", help="exercise the pure helpers")
    options = parser.parse_args(argv)
    if options.selftest:
        return selftest()
    if options.attempt < 1:
        raise SystemExit("--attempt must be >= 1")

    output = output_root(options.attempt)
    destination = AUDITS / suffixed("iteration15-equity-universe-measurement.json", options.attempt)
    stdout_log = AUDITS / suffixed("iteration15-universe-run.stdout.log", options.attempt)
    stderr_log = AUDITS / suffixed("iteration15-universe-run.stderr.log", options.attempt)
    samples_log = AUDITS / suffixed("iteration15-universe-memory-samples.jsonl", options.attempt)
    for path in (destination, stdout_log, stderr_log, samples_log):
        if path.exists():
            raise FileExistsError(f"immutable attempt-{options.attempt} output exists: {path}")

    plan = preflight(output, pin_segments=(options.pin_segments or not options.dry_run))
    if options.dry_run:
        print(json.dumps({"dry_run": True, "launchable": not plan["problems"],
                          "problems": plan["problems"],
                          "command": command(output),
                          "command_line": subprocess.list2cmdline(command(output)),
                          "preflight": plan}, indent=2))
        return 0 if not plan["problems"] else 1
    if plan["problems"]:
        print(json.dumps({"refused": True, "problems": plan["problems"]}, indent=2))
        return 1

    sources = {name: pin(ROOT / name) for name in SOURCE_NAMES if (ROOT / name).exists()}
    sources[str(Path(__file__).resolve())] = pin(Path(__file__).resolve())
    before = {k: v for k, v in {**plan["files"], **sources}.items() if is_pin(v)}
    non_pins = [k for k, v in plan["files"].items() if not is_pin(v)]
    for index, entry in enumerate(plan["segment_dirs"]):
        for name, seg in entry.get("segment_pins", {}).items():
            before[f"segments[{index}]/{name}"] = {"path": str(SEGMENT_DIRS[index] / name), **seg}
        for key in ("ingestion_manifest", "preparation_manifest_pin"):
            if key in entry:
                before[f"{key}[{index}]"] = entry[key]

    report = {"schema": "atx-equity-universe-native-measurement-v1", "attempt": options.attempt,
              "started_utc": datetime.now(timezone.utc).isoformat(),
              "command": command(output), "command_line": subprocess.list2cmdline(command(output)),
              "output_root": str(output), "cwd": str(ROOT), "preflight": plan,
              "oracle": pin(ORACLE), "comparator": pin(COMPARATOR),
              "oracle_executed_against_this_run": False,
              "monitor": {"api": "GetProcessMemoryInfo/PROCESS_MEMORY_COUNTERS_EX",
                          "sampling_interval_seconds": SAMPLE_SECONDS, "limit_bytes": LIMIT,
                          "sampling_limit": "polling and termination are not an instantaneous "
                                            "hard memory cap"},
              "source_and_input_pins_before": before}
    try:
        report.update(run_native(command(output), stdout_log, stderr_log, samples_log))
    except BaseException as error:  # the evidence is written either way
        report["error"] = f"{type(error).__name__}: {error}"
    after = {key: pin(Path(entry["path"])) for key, entry in before.items()
             if is_pin(entry) and Path(entry["path"]).exists()}
    report["source_and_input_pins_after"] = after
    report["notes"] = list(plan.get("notes", [])) + (
        [f"non-pin entries ignored in the pin map: {non_pins}"] if non_pins else [])
    report["pins_changed"] = pins_differ(before, after)
    report["pins_unchanged"] = not report["pins_changed"]
    try:
        report["ledger_after"] = verify_ledger(LEDGER, LEDGER_SIDECAR)
        gained, new, ok = ledger_delta_ok(plan["ledger_before"], report["ledger_after"])
    except ValueError as error:
        report["ledger_after"] = {"error": str(error)}
        gained, new, ok = None, [], False
    report["ledger_lines_gained"] = gained
    report["ledger_new_lines"] = new
    report["ledger_two_cp15_lines"] = ok
    report["output_evidence"] = output_evidence(output)
    report["logs"] = {"stdout": str(stdout_log), "stderr": str(stderr_log), "samples": str(samples_log)}
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    evidence = report["output_evidence"]
    report["accepted"] = bool(report.get("exit_code") == 0 and report["pins_unchanged"] and ok
                              and not evidence["missing"] and report.get("budget_breach") is None
                              and evidence.get("membership_bin_ok") is True
                              and evidence.get("request_rebalances_match") is True
                              and evidence.get("manifest_files_match") is True)
    report["qualification"] = ("Bounded software measurement of a pre-registered point-in-time "
                               "universe construction (trial_count_declared 0). Membership lists "
                               "and counts only: no alpha, no forecast, no Sharpe, no capacity; "
                               "survivorship figures are lower bounds.")
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"accepted": report["accepted"], "exit_code": report.get("exit_code"),
                      "wall_seconds": report.get("wall_seconds"), "ledger_lines_gained": gained,
                      "rebalances": (evidence.get("membership_bin") or {}).get("rebalance_count"),
                      "pins_changed": report["pins_changed"], "measurement": str(destination)}, indent=2))
    return 0 if report["accepted"] else 1


# ---------------------------------------------------------------------------
#  --selftest: pure helpers only. Nothing native, nothing written outside a temporary
#  directory, no repository or data path touched.
# ---------------------------------------------------------------------------
def selftest() -> int:
    checks = 0

    def ok(condition, label):
        nonlocal checks
        if not condition:
            raise AssertionError(label)
        checks += 1

    ok(suffixed("a-run.json", 1) == "a-run-attempt1.json", "suffixed attempt 1")
    ok(suffixed("iteration15-universe-run.stdout.log", 3) == "iteration15-universe-run-attempt3.stdout.log", "suffixed 3")
    ok(output_root(1).name == OUTPUT_STEM and output_root(2).name == OUTPUT_STEM + "_attempt2", "output_root")
    ok(output_root(1).parent == SHARED / "data", "output under C:/atx/data")
    ok(sha256_bytes(b"") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256")
    ok(date_to_nanos("2020-01-01") == 1_577_836_800_000_000_000, "validation boundary key")
    ok(nanos_to_date(date_to_nanos("2019-11-29")) == "2019-11-29", "calendar round trip")
    ok(nanos_to_date(date_to_nanos("2016-02-29")) == "2016-02-29", "leap day")
    dates = segment_dates(["2019-12-31.seg", "2020-01-02.seg", "2013-01-02.seg", "notes.txt"])
    ok(list(dates) == ["2013-01-02.seg", "2019-12-31.seg", "2020-01-02.seg"], "segment_dates")
    ok(seal_violations(dates.values()) == ["2020-01-02"], "seal refuses >= 2020-01-01")
    ok(seal_violations(["2019-12-31"]) == [], "seal accepts 2019-12-31")
    try:
        segment_dates(["foo.seg"])
        raise AssertionError("non-date .seg must be refused")
    except ValueError:
        checks += 1
    ok(ranges_disjoint_and_ordered([["2013-01-02", "2013-12-31"], ["2014-01-02", "2014-12-31"]]) == [], "disjoint")
    ok(ranges_disjoint_and_ordered([["2014-01-02"], ["2013-12-31"]]) != [], "order violation")
    ok(ranges_disjoint_and_ordered([["2013-12-31"], ["2013-12-31"]]) != [], "duplicate date")
    ok(embedded_constant(f'constexpr std::string_view {STAGE_CONSTANT} =\n    "{DESIGN_NOTE_SHA256}";')
       == DESIGN_NOTE_SHA256, "embedded constant parse")
    try:
        embedded_constant("nothing here")
        raise AssertionError("absent constant must be refused")
    except ValueError:
        checks += 1
    # Pins compare by digest and size, never by path text.
    a = {"x": {"path": "C:/a", "sha256": "1", "bytes": 1}}
    b = {"x": {"path": "C:\\a", "sha256": "1", "bytes": 1}}
    ok(pins_differ(a, b) == [], "pins by digest")
    ok(pins_differ(a, {"x": {"path": "C:/a", "sha256": "2", "bytes": 1}}) == ["x"], "pins detect change")
    ok(pins_differ(a, {}) == ["x"], "pins detect loss")
    ok(pins_differ({**a, "note": "text"}, {**a, "note": "other"}) == [], "non-pin entries ignored")
    ok(not is_pin("text") and not is_pin({"sha256": "1"}) and is_pin(a["x"]), "is_pin")
    # membership.bin header decode on a synthetic image (design section 4.7).
    body = BIN_MAGIC + struct.pack("<IIId", 1, 63, 57, 1.0)
    body += struct.pack("<IIII", 3, 1000, 2000, 3000) + struct.pack("<III", 2, 0, 1000)
    body += struct.pack("<I", 1) + struct.pack("<qq", date_to_nanos("2012-12-31"), date_to_nanos("2013-01-02"))
    for _ in range(6):
        body += struct.pack("<I", 2) + struct.pack("<qq", 5, 9) + struct.pack("<II", 2, 1)
    blob = body + struct.pack("<Q", fnv1a64(body))
    image = decode_membership_bin_header(blob)
    ok(image["rebalance_count"] == 1 and image["top_n"] == [1000, 2000, 3000] and image["band_bp"] == [0, 1000], "bin header")
    ok(nanos_to_date(image["rebalances"][0]["effective_session_key"]) == "2013-01-02", "bin effective")
    ok(image["rebalances"][0]["member_counts"] == [2] * 6, "bin member counts")
    for label, bad in (("magic", b"X" + blob[1:]), ("trailer", blob[:-1] + bytes([blob[-1] ^ 1])),
                       ("short", blob[:30])):
        try:
            decode_membership_bin_header(bad)
            raise AssertionError(f"corrupt image accepted: {label}")
        except ValueError:
            checks += 1
    ok(find_named_int({"window": {"observations": 84}, "x": [{"rebalances": 84}]},
                      ("rebalances", "observations")) == [84, 84], "find_named_int")
    ok(manifest_digests({"files": [{"filename": "a.csv", "sha256": "AB"}], "m": {"path": "d/b.bin", "sha256": "cd"}})
       == {"a.csv": "ab", "b.bin": "cd"}, "manifest_digests")
    # Command: exactly the allow-listed flags, pinned values, dirs in date order.
    cmd = command(output_root(2))
    flags = [c for c in cmd if c.startswith("--")]
    ok(flags == ["--segments-dirs", "--preparation-manifests", "--out", "--rank-start", "--rank-end",
                 "--max-working-bytes", "--trial-ledger"], "allow-listed flags only")
    ok(cmd[1] == SUBCOMMAND and cmd[cmd.index("--rank-end") + 1] == "2019-11-29", "pinned rank-end")
    ok(cmd[cmd.index("--rank-start") + 1] == "2012-12-31", "pinned rank-start")
    ok(cmd[cmd.index("--segments-dirs") + 1].count(";") == 6, "seven dirs")
    ok("--top-n" not in cmd and "--band" not in cmd and "--config" not in cmd, "no frozen-recipe override")
    ok(cmd[cmd.index("--out") + 1] == str(output_root(2)).replace("\\", "/"), "out per attempt")
    ok(all(";" not in str(p) for p in (*SEGMENT_DIRS, *PREPARATION_MANIFESTS)), "no ';' in paths")
    ok(len(DESIGN_NOTE_SHA256) == 64 and all(c in "0123456789abcdef" for c in DESIGN_NOTE_SHA256), "64 hex")
    ok(sum(EXPECTED_DATES_PER_DIR) == EXPECTED_SESSIONS, "section 2.1 session count")
    # Ledger walk (as cp14) plus the cp15 two-line delta rule.
    with tempfile.TemporaryDirectory() as temporary:
        room = Path(temporary)
        ledger, sidecar = room / "t.jsonl", room / "t.manifest.json"
        ok(verify_ledger(ledger, sidecar)["head_sha256"] == GENESIS, "empty ledger")
        first = json.dumps({"prev_sha256": GENESIS, "trial_id": "iteration15-point-in-time-universe-0001",
                            "status": "pre-registered", "checkpoint": 15, "purpose": PURPOSE,
                            "trial_count_declared": 0, "result": {"outcome": "pending"}},
                           sort_keys=True).encode("utf-8")
        head = sha256_bytes(first + b"\n")
        second = json.dumps({"prev_sha256": head, "trial_id": "iteration15-point-in-time-universe-0001",
                             "status": "completed", "checkpoint": 15, "purpose": PURPOSE,
                             "trial_count_declared": 0, "result": {"outcome": "completed"}},
                            sort_keys=True).encode("utf-8")
        ledger.write_bytes(first + b"\n" + second + b"\n")
        sidecar.write_text(json.dumps({"lines": 2, "head_sha256": sha256_bytes(second + b"\n")}),
                           encoding="utf-8")
        walked = verify_ledger(ledger, sidecar)
        ok(walked["lines"] == 2 and walked["anchored"], "walk two lines")
        gained, new, delta_ok = ledger_delta_ok({"lines": 0, "trials": []}, walked)
        ok(gained == 2 and delta_ok, "two cp15 lines accepted")
        ok(not ledger_delta_ok({"lines": 1, "trials": walked["trials"][:1]}, walked)[2], "one line rejected")
        ledger.write_bytes(first + b"\n" + first + b"\n")
        try:
            verify_ledger(ledger, sidecar)
            raise AssertionError("broken chain verified")
        except ValueError as error:
            ok("chain breaks at line 1" in str(error), "chain break")
        ledger.write_bytes(first)
        try:
            verify_ledger(ledger, sidecar)
            raise AssertionError("torn line verified")
        except ValueError as error:
            ok("torn final line" in str(error), "torn line")
        # A lock beside the ledger is a stop sign, never removed.
        lock = Path(str(ledger) + ".lock")
        lock.write_bytes(b"")
        ok(lock.exists(), "lock detection path shape")
    print(json.dumps({"selftest": "passed", "checks": checks}))
    return 0


if __name__ == "__main__":
    if os.name != "nt" and "--selftest" not in sys.argv and "--help" not in sys.argv:
        raise SystemExit("this runner drives a Windows build of atx-impl.exe")
    raise SystemExit(main())
