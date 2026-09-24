"""Write the immutable checkpoint-15 validation receipt (point-in-time universe construction).

Stage 1 universe construction over the 2013-2019 archive, seven segment directories, 84
monthly rebalances, six top_n x band cuts reported side by side, trial_count_declared 0.
Binds (sha256 + bytes, digests compared, never path strings) the frozen design (SHA
4810fda2... - a different digest FAILS the receipt), engine and stage sources, the exact
oracle and comparator with their JSON outputs, every native log incl. the failed T1 build
and test attempts, the junit files, the three binaries, the runner and its measurement JSON,
the real-data output directory, the trial ledger (chain re-verified here in Python; the two
cp14 lines byte-unchanged against the cp14 receipt's pin; N14 == 30; declared(15) == 0),
the seven ingestion manifests (digests only - the stage manifest pins the segments), the
ingestion receipts and the review records.

Every headline number in `summary` is recomputed from the CSV/JSON outputs by this script;
nothing is copied from prose. `status` is a fixed string; `accepted` is true only when every
check below holds. Run once; refuses to overwrite (`--out` another name for a failed
attempt). `--dry-run` verifies what exists, lists what is missing and writes nothing.
Historical receipt writers are never rerun. Stdlib only.

Usage:
  python build-equity/audits/iteration15_write_validation_receipt.py --dry-run
  python build-equity/audits/iteration15_write_validation_receipt.py \\
      --measurement build-equity/audits/iteration15-equity-universe-measurement-attempt1.json \\
      --comparison build-equity/audits/iteration15-native-comparison-t1fix1.json \\
      --log build-equity/audits/iteration15-build-t2.log [--log ...] [--end-review PATH]
"""
from fractions import Fraction
from pathlib import Path
import argparse
import csv
import hashlib
import json
import re
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
AUDITS = 'build-equity/audits/'
SDD = '.superpowers/sdd/equity-platform-parent-goal/'
DEFAULT_OUT = 'atx-engine/reviews/2026-09-20-point-in-time-universe-validation.json'
DEFAULT_MEASUREMENT = AUDITS + 'iteration15-equity-universe-measurement-attempt1.json'
DEFAULT_COMPARISONS = [AUDITS + 'iteration15-native-comparison-t1.json',
                       AUDITS + 'iteration15-native-comparison-t1fix1.json']
DEFAULT_LOGS = [AUDITS + 'iteration15-configure-t1.log',
                AUDITS + 'iteration15-check-t1.log',
                AUDITS + 'iteration15-build-t1.log',
                AUDITS + 'iteration15-data-t1-tests.log',
                AUDITS + 'iteration15-build-t1fix1.log',
                AUDITS + 'iteration15-data-t1fix1-tests.log',
                AUDITS + 'iteration15-data-group-tests.log',
                AUDITS + 'iteration15-configure-t2.log',
                AUDITS + 'iteration15-check-t2.log',
                AUDITS + 'iteration15-build-t2.log',
                AUDITS + 'iteration15-build-t2fix1.log',
                AUDITS + 'iteration15-impl-t2-tests.log',
                AUDITS + 'iteration15-build-atx-impl.log',
                AUDITS + 'iteration15-equity-universe-run.err.log']
DEFAULT_FINDINGS = [SDD + 'cp15-idgap-investigation.md']
DEFAULT_END_REVIEW = SDD + 'cp15-end-review.md'
FINDING_EXCERPT_LINES = 15
# manifest.json.producer_executable_sha256 of the attempt-1 run; must equal the CURRENT
# build-equity/bin/atx-impl.exe (end review I-2). The launcher's own digest was never recorded
# because the runner crashed post-run and was fixed in place afterwards.
EXPECTED_PRODUCER_EXE_SHA256 = '8425fc03c136169a2f7522c0ade37bcf855179de431b302b4f0aae6fe49f950b'

# End review I-1, restated; a documented deviation, not corrected.
COVERAGE_YEAR_ATTRIBUTION = [
    'coverage_by_year.csv attributes each rebalance (rebalances, eligible_median, members_median, nonmissing_fraction_median, gics_missing_members_median) to the calendar year of its RANK session: the 2012 row carries the 2012-12-31 rebalance and 2019 carries eleven.',
    'This deviates from the wording of design section 15.2 pins 5/6 (effective-session year); section 4.3 states the rank-session rule.',
    'Engine, oracle fixtures and tests agree on the measured convention (comparator 42/42, F6), so the number is exact under that convention.',
    'Recorded as a documented deviation in the cp15 addendum; not corrected in the outputs, the receipt or the frozen design.',
]

# Ruling R15-17 (progress.md, 2026-09-20), restated; the run stands as measured.
DATA_HOLE_2016_12_30 = [
    'Vendor archive carries corrupted OHLC on the session before every NYSE holiday 2016-01-15..2018-02-16 (19 sessions: 9/8/2 in 2016/2017/2018): open == prior close, high/low truncated.',
    'tickerhistory-qa-v1 rejects those rows as ohlc_order_violation, about half the rows on each such session (2016-12-30: 3,965 violations, 3,957 accepted of 8,102 raw); close/volume spot-check correct.',
    '2016-12-30 is the only such hole that is also a month-end rank date (bar on rank session required), so 4,034 of 7,869 ids lose eligibility for one rebalance and 99.2 % return under the SAME id within 5 sessions; not an id remap.',
    'Contaminated outputs: churn.csv at rank 2016-12-30 and 2017-01-31 for all six cuts; 2016 drops_last_bar totals; union_by_year.csv 2017 distinct and every cumulative column from 2017 on; nonmissing_fraction medians 2016 and 2017.',
    'Unaffected: survivorship.json per-year exits and delisting.csv (no last_bar wave in 2016-12).',
    'The run STANDS AS MEASURED: pre-registered, no re-tune, no silent correction; the frozen design 4810fda2... is not edited.',
    'Remediation is a NEW pre-registration (cp16 or cp15b): preparation policy tickerhistory-qa-v2 (accept rows with valid close/volume, flag missing OHL) and/or a hole-aware rank rule (rank on the last full session <= rank date).',
    'Source: cp15-idgap-investigation.md (pinned under data_quality_findings) and ruling R15-17.',
]

SCHEMA = 'atx-point-in-time-universe-validation-v1'
CHECKPOINT = 15
STATUS = 'stage1_universe_construction_measured'
PURPOSE = 'point-in-time-universe-construction'
FROZEN_DESIGN_SHA256 = '4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8'
# Design section 14 SHA of the 1,000-line pre-T1 file, superseded by section 15.5. The oracle
# v1 document was cut against it and the design records that supersession explicitly.
SUPERSEDED_DESIGN_SHA256 = 'be4b3b670eaced4f0b6f5cee8a7d5e01dc8b201ae1ecd4ef8b3377bcdb41020f'
CP14_RECEIPT = 'atx-engine/reviews/2026-09-20-cross-section-ic-validation.json'
CP14_ADDENDUM = 'atx-engine/reviews/2026-09-20-cross-section-ic-validation-addendum.json'
LEDGER = 'atx-engine/reviews/trial-ledger.jsonl'
LEDGER_SIDECAR = 'atx-engine/reviews/trial-ledger.manifest.json'
# trial-ledger.manifest.json head_sha256 as it stood after the two cp14 lines (read
# 2026-09-20 before any cp15 append): the digest of cp14 ledger line 2.
CP14_LEDGER_HEAD_SHA256 = '421253a7497522a7017e257963393f0a5d3284201db954b332d5bd3be4b3028d'
GENESIS = '0' * 64
EXPECTED_REBALANCES = 84
EXPECTED_SESSIONS = 1955
EXPECTED_SEGMENTS_PER_DIR = (445, 252, 252, 252, 251, 251, 252)
EXPECTED_QUARANTINED_DATES = {2012: 2, 2013: 2, 2014: 7, 2015: 3, 2016: 10, 2017: 0, 2018: 83, 2019: 0}
EXPECTED_CUTS = [(1000, 0), (1000, 1000), (2000, 0), (2000, 1000), (3000, 0), (3000, 1000)]
IC_INSTRUMENT_CAP = 4096
LOADER_2012_2013 = '9415a6ab798f0e5fd0eb4fec31a7d2b3d09a47dd7eaf48e668deb844f0fe4daa'
LOADER_2014_2019 = 'ac3ab17f1202b0802ba7889ad220d065ee18f4fbea9493b6e372120d492b7eff'
BIN_MAGIC = b'ATXPITU1'
STAGE_CONSTANT_RE = re.compile(r'kEquityUniverseDesignNoteSha256\s*=\s*"([0-9a-fA-F]{64})"')
RUNNER_CONSTANT_RE = re.compile(r'DESIGN_NOTE_SHA256\s*=\s*"([0-9a-fA-F]{64})"')
CTEST_RE = re.compile(r'(\d+)% tests passed, (\d+) tests failed out of (\d+)')

SEGMENT_DIRS = ['C:/atx/data/tickerhistory_training_native_20260919/segments',
                'C:/atx/data/tickerhistory_training_native_2014_20260920/segments',
                'C:/atx/data/tickerhistory_training_native_2015_20260920/segments',
                'C:/atx/data/tickerhistory_training_native_2016_20260920/segments',
                'C:/atx/data/tickerhistory_training_native_2017_20260920/segments',
                'C:/atx/data/tickerhistory_training_native_2018_20260920/segments',
                'C:/atx/data/tickerhistory_training_native_2019_20260920/segments']
SEGMENT_ROLES = ['segments-2012-2013', 'segments-2014', 'segments-2015', 'segments-2016',
                 'segments-2017', 'segments-2018', 'segments-2019']
OUTPUT_FILES = ['manifest.json', 'request.json', 'seal.json', 'survivorship.json',
                'coverage_by_year.csv', 'churn.csv', 'union_by_year.csv', 'delisting.csv',
                'membership.bin', 'membership.csv']

PINNED = {
    'design': 'atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md',
    'research': SDD + 'research-cp15-universe.md',
    'predecessor_receipt_cp14': CP14_RECEIPT,
    'predecessor_receipt_cp14_addendum': CP14_ADDENDUM,
    'engine_header': 'atx-engine/include/atx/engine/data/point_in_time_universe.hpp',
    'engine_source': 'atx-engine/src/data/point_in_time_universe.cpp',
    'engine_test': 'atx-engine/tests/data/data_point_in_time_universe_test.cpp',
    'engine_cmake': 'atx-engine/CMakeLists.txt',
    'stage_header': 'atx-impl/src/stage_equity_universe.hpp',
    'stage_source': 'atx-impl/src/stage_equity_universe.cpp',
    'stage_test': 'atx-impl/tests/stage_equity_universe_test.cpp',
    'ledger_header': 'atx-impl/src/trial_ledger.hpp',
    'ledger_source': 'atx-impl/src/trial_ledger.cpp',
    'ledger_test': 'atx-impl/tests/trial_ledger_test.cpp',
    'impl_config_hpp': 'atx-impl/src/config.hpp',
    'impl_config_cpp': 'atx-impl/src/config.cpp',
    'impl_dispatch': 'atx-impl/src/dispatch.cpp',
    'impl_cmake': 'atx-impl/CMakeLists.txt',
    'oracle_script': AUDITS + 'iteration15_universe_oracle.py',
    'oracle_v1': AUDITS + 'iteration15-universe-oracle-v1.json',
    'comparator': AUDITS + 'iteration15_native_comparator.py',
    'runner': AUDITS + 'iteration15_run_equity_universe.py',
    'ingest_script': AUDITS + 'iteration15_ingest_tickerhistory.py',
    'ingest_receipt_attempt1_killed': AUDITS + 'iteration15-ingest-20260920-attempt1.json',
    'ingest_receipt_attempt2': AUDITS + 'iteration15-ingest-20260920-attempt2.json',
    'ingest_run_attempt1_killed_log': AUDITS + 'iteration15-ingest-run-attempt1-killed.log',
    'ingest_run_log': AUDITS + 'iteration15-ingest-run.log',
    'ingest_run_err_log': AUDITS + 'iteration15-ingest-run.err.log',
    'trial_ledger': LEDGER,
    'trial_ledger_sidecar': LEDGER_SIDECAR,
    'design_review': SDD + 'cp15-design-review.md',
    't1_report': SDD + 'cp15-task-T1-report.md',
    't2_report': SDD + 'cp15-task-T2-report.md',
    't3_report': SDD + 'cp15-task-T3-report.md',
    'engine_test_executable': 'build-equity/bin/atx-engine-data-tests.exe',
    'impl_test_executable': 'build-equity/bin/atx-impl-tests.exe',
    'impl_executable': 'build-equity/bin/atx-impl.exe',
}
# Sources the runner pins before/after the run (its SOURCE_NAMES); each must be byte-identical
# now to what the run saw (digest membership, never path text).
RUN_PINNED_SOURCE_KEYS = ['stage_header', 'stage_source', 'ledger_header', 'ledger_source',
                          'impl_config_hpp', 'impl_config_cpp', 'impl_dispatch', 'impl_cmake',
                          'engine_header', 'engine_source', 'engine_cmake', 'design']

# Design section 6, restated (the verbatim table lines are also extracted from the pinned file).
PRE_REGISTRATION_FROZEN = {
    'rank_key': 'ADV$ = section 3.5 median of close x volume over the trailing 63 sessions ending at the rank session, inclusive (R15-1)',
    'window_sessions': 63,
    'min_valid_observations': 57,
    'missing_bar': 'NaN slot; never filled; section 2.3 validity (R15-1)',
    'price_floor': 'raw close on the rank session > 1.0, strict (R15-5)',
    'bar_on_rank_session': 'required (last_bar == rank ordinal) (R15-5)',
    'min_adv_usd': 0,
    'market_cap': 'reported only: shares x close, label no-filing-vintage; NaN when shares NaN or <= 0 (R15-2)',
    'instrument_type': 'unknown accepted; gics_missing_members reported (R15-6)',
    'cadence': 'monthly: rank session = attached session whose UTC month differs from the next attached session\'s month; the last attached session is never one; [start,end] filter after (R15-4, DR15-8)',
    'effective': 'the next observed session after the rank session (R15-4)',
    'rank_dates': '2012-12-31 .. 2019-11-29, 84 rebalances; membership covers 2013-01-02 .. 2019-12-31 (R15-4)',
    'real_run_flags': '--rank-start 2012-12-31 --rank-end 2019-11-29, the seven --segments-dirs and --preparation-manifests of section 2.1 in date order; the runner asserts rebalances == 84 from request.json (DR15-3)',
    'member_order': 'set = keep-then-fill; CSV/view order = rank ascending; .bin = id ascending + parallel rank (DR15-2)',
    'warmup': '63 sessions; 2012 data is warmup only; no 2012 membership emitted (R15-8)',
    'top_n_set': [1000, 2000, 3000],
    'band_set': {'band': ['0.00', '0.10'], 'band_bp': [0, 1000], 'k_band': [1000, 1100, 2000, 2200, 3000, 3300]},
    'band_rule': 'incumbent (member of the same cut at r-1) kept iff rank <= K_band; section 3.4 3a-3b ordering; best-ranked top_n when band-kept incumbents exceed top_n (R15-4)',
    'tie_break': 'key descending, then first-seen slot ascending (R15-12)',
    'median': 'section 3.5, (a+b)*0.5 binary64; dv = close*volume one product (R15-1, DR15-5)',
    'drop_classification': 'formula only: LastBar iff last_bar < rank ordinal, else Rank (R15-14, DR15-10)',
    'maxima': 'max_source_ids 32768, max_rebalances 4096, max_sessions 8192, kPitMaxCuts 8, overflow-checked; ID->slot by pre-sized open-addressing hash (capacity next pow2 >= 4*32768), any positive i64 ID accepted (R15-12 as revised by DR15-1)',
    'seal': 'refuse any segment >= 2020-01-01; rank-end <= 2019-12-31 and < last attached session; constants unchanged (R15-13)',
    'inputs': 'seven segment dirs, policy tickerhistory-qa-v1, quarantine-all duplicates (R15-9)',
    'ledger': 'one cp15 line, purpose point-in-time-universe-construction, trial_count_declared 0 (section 5.6) (R15-3)',
    'design_sha': 'embedded after review (section 5.8) (R15-16)',
    'trial_accounting': 'N_15 = 0. Universe cuts are restrictions to be reported side by side (AR-7 semantics); selecting among them later is a new trial. declared_trials_for_checkpoint(14) stays 30; the two existing ledger lines are untouched.',
}

NOT_ESTABLISHED = [
    'No alpha, no forecast, no Sharpe, no capacity. Membership lists are inputs to a future measurement; nothing here is evidence that anything predicts anything. (section 11.1)',
    'No float or common-stock eligibility (R15-6): ETFs, ADRs, preferreds and funds can rank into the top 3000 by dollar volume; gics_missing_members is reported, not applied. (section 11.2)',
    'Survivorship fractions are lower bounds (R15-10): backfill policy unknown; names never in the archive are invisible; no delisting return is imputed (Shumway -30 % / -55 % cited, not applied). (section 11.3)',
    'Vendor shares unreliable: lags splits, zeros; vendor_market_cap_usd must not be used as a size screen downstream without a filing-dated source. (section 11.4)',
    '2018 thinning: 83 dates (86 rows) with quarantined duplicate keys reduce valid bars on those sessions; a name hit on >= 7 of its trailing 63 sessions loses eligibility on that rank date. Counted in coverage_by_year.csv; not repaired. (section 11.5)',
    'ID reuse (77622) makes at most a handful of histories composite. (section 11.6)',
    'Ledger schema shape (section 5.6, DR15-16): the v1 line carries cp14 recipe defaults; readers must honour notes. (section 11.7)',
    'cp16 cap: if cumulative_t3000_b10 at 2019 exceeds 4096, cp16 must go per-year or raise kMaxIcInstruments; this checkpoint only measures the number (R15-7). (section 11.8)',
    'CMake pin drift: the two one-line CMake edits drift the cp13/cp14 receipt pins as cp14 drifted cp13; recorded in the cp15 addendum, never by editing receipts. (section 11.9)',
    'No gtest asserts a real-data number; no test claims survivorship completeness, instrument-type correctness, or point-in-time shares. (section 9.4)',
    'Not throughput, latency, production-readiness or investment-quality evidence. No live trading or broker actions performed or authorized.',
]


# ---------------------------------------------------------------------------
#  Pure helpers
# ---------------------------------------------------------------------------
def posix(path) -> str:
    return str(path).replace('\\', '/')


def rel(path: Path) -> str:
    try:
        return posix(path.resolve().relative_to(ROOT))
    except ValueError:
        return posix(path)


def sha256_file(path: Path):
    digest = hashlib.sha256()
    size = 0
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def pin(path: Path, label=None) -> dict:
    sha, size = sha256_file(path)
    return {'path': label if label is not None else rel(path), 'sha256': sha, 'bytes': size}


def read_json(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def read_text(path: Path) -> str:
    return path.read_text(encoding='utf-8', errors='replace')


def parse_ctest_summary(text: str):
    """Last `N% tests passed, F tests failed out of T` line, as integers."""
    found = None
    for m in CTEST_RE.finditer(text):
        found = {'percent': int(m.group(1)), 'failed': int(m.group(2)), 'total': int(m.group(3))}
    if found is not None:
        found['passed'] = found['total'] - found['failed']
    return found


def parse_junit(path: Path) -> dict:
    root = ET.parse(str(path)).getroot()
    suites = [root] if root.tag == 'testsuite' else list(root.iter('testsuite'))
    tests = sum(int(s.get('tests', 0)) for s in suites)
    failures = sum(int(s.get('failures', 0)) for s in suites)
    cases = [c.get('name') for c in root.iter('testcase')]
    return {'tests': tests, 'failures': failures, 'testcases': len(cases),
            'point_in_time_cases': sum(1 for c in cases if c and c.startswith('DataPointInTimeUniverse.'))}


def median_rule(values):
    """Design section 3.5: sorted middle, even count -> (a+b)*0.5 in binary64."""
    if not values:
        return None
    ordered = sorted(values)
    n = len(ordered)
    if n % 2:
        return ordered[n // 2]
    return (ordered[n // 2 - 1] + ordered[n // 2]) * 0.5


def fnv1a64(data: bytes) -> int:
    h = 14695981039346656037
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def decode_membership_bin(blob: bytes) -> dict:
    """Design section 4.7 layout; structural parse first, trailer last (section 15.4)."""
    if len(blob) < 8 + 4 + 4 + 4 + 8 + 4 + 4 + 4 + 8:
        raise ValueError('membership.bin too short')
    if blob[:8] != BIN_MAGIC:
        raise ValueError('membership.bin bad magic')
    body = memoryview(blob)[:-8]
    pos = 8

    def take(fmt):
        nonlocal pos
        size = struct.calcsize(fmt)
        if pos + size > len(body):
            raise ValueError('membership.bin short read')
        out = struct.unpack_from(fmt, body, pos)
        pos += size
        return out

    version, adv_window, min_valid = take('<III')
    (min_price,) = take('<d')
    (t,) = take('<I')
    top_n = list(take('<' + 'I' * t)) if t else []
    (b,) = take('<I')
    band_bp = list(take('<' + 'I' * b)) if b else []
    (r,) = take('<I')
    rebalances = []
    for _ in range(r):
        rank_key, eff_key = take('<qq')
        counts = []
        for _ in range(t * b):
            (n,) = take('<I')
            ids = list(take('<' + 'q' * n)) if n else []
            ranks = list(take('<' + 'I' * n)) if n else []
            if ids != sorted(ids):
                raise ValueError('membership.bin ids not ascending')
            counts.append(n)
            del ranks
        rebalances.append({'rank_session_key': rank_key, 'effective_session_key': eff_key,
                           'member_counts': counts})
    if pos != len(body):
        raise ValueError('membership.bin trailing bytes before trailer')
    (trailer,) = struct.unpack_from('<Q', blob, len(blob) - 8)
    computed = fnv1a64(bytes(body))
    return {'version': version, 'adv_window': adv_window, 'min_valid_observations': min_valid,
            'min_raw_price_exclusive': min_price, 'top_n': top_n, 'band_bp': band_bp,
            'rebalance_count': r, 'rebalances': rebalances,
            'fnv1a64_trailer': str(trailer), 'fnv1a64_recomputed': str(computed),
            'trailer_matches': trailer == computed}


def nanos_to_date(key: int) -> str:
    z = key // 86_400_000_000_000 + 719468
    era = (z if z >= 0 else z - 146096) // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    if m <= 2:
        y += 1
    return f'{y:04d}-{m:02d}-{d:02d}'


def walk_ledger(payload: bytes, sidecar_text=None) -> dict:
    """Independent walk of the append-only hash chain (cp14 design section 4.5 rules)."""
    if payload and not payload.endswith(b'\n'):
        raise ValueError('trial ledger has a torn final line')
    raw_lines = payload.split(b'\n')[:-1] if payload else []
    previous = GENESIS
    lines = []
    for index, raw in enumerate(raw_lines):
        if not raw:
            raise ValueError(f'trial ledger line {index} is empty')
        if b'\r' in raw:
            raise ValueError(f'trial ledger line {index} carries a CR')
        entry = json.loads(raw.decode('utf-8'))
        if entry.get('prev_sha256') != previous:
            raise ValueError(f'trial ledger chain breaks at line {index}')
        line_sha = hashlib.sha256(raw + b'\n').hexdigest()
        lines.append({'index': index, 'entry': entry, 'raw': raw, 'line_sha256': line_sha,
                      'bytes': len(raw) + 1})
        previous = line_sha
    anchored = False
    if sidecar_text is not None:
        recorded = json.loads(sidecar_text)
        if recorded.get('lines') != len(lines) or recorded.get('head_sha256') != previous:
            raise ValueError('trial ledger sidecar disagrees with the walked chain')
        anchored = True
    elif lines:
        raise ValueError('trial ledger sidecar is missing beside a non-empty ledger')
    return {'lines': lines, 'head_sha256': previous, 'anchored': anchored}


def digests_in(node, out=None) -> dict:
    """Every (file-name -> sha256) pair carried by an object with a `sha256` key."""
    out = {} if out is None else out
    if isinstance(node, dict):
        name = None
        for key in ('filename', 'file', 'name', 'path'):
            if isinstance(node.get(key), str):
                name = Path(node[key]).name
                break
        if name and isinstance(node.get('sha256'), str):
            out[name] = node['sha256'].lower()
        for value in node.values():
            digests_in(value, out)
    elif isinstance(node, list):
        for value in node:
            digests_in(value, out)
    return out


def hex64_values(node, out=None) -> set:
    out = set() if out is None else out
    if isinstance(node, dict):
        for value in node.values():
            hex64_values(value, out)
    elif isinstance(node, list):
        for value in node:
            hex64_values(value, out)
    elif isinstance(node, str) and re.fullmatch(r'[0-9a-fA-F]{64}', node):
        out.add(node.lower())
    return out


def find_ints(node, names, out=None) -> list:
    out = [] if out is None else out
    if isinstance(node, dict):
        for key, value in node.items():
            if key in names and isinstance(value, int) and not isinstance(value, bool):
                out.append(value)
            find_ints(value, names, out)
    elif isinstance(node, list):
        for value in node:
            find_ints(value, names, out)
    return out


def cut_key(top_n, band) -> str:
    bp = int(round(float(band) * 10000))
    return f't{int(top_n)}_b{bp}'


def section_lines(design_text: str, start: str, stop: str) -> list:
    lines = design_text.splitlines()
    out, active = [], False
    for line in lines:
        if line.startswith(start):
            active = True
        elif active and line.startswith(stop):
            break
        if active:
            out.append(line)
    return out


# ---------------------------------------------------------------------------
#  Extraction from the real-data outputs (recomputed, never trusted from prose)
# ---------------------------------------------------------------------------
def summarize_churn(path: Path) -> dict:
    per_cut = {}
    rank_dates = set()
    with path.open(encoding='utf-8', newline='') as stream:
        for row in csv.DictReader(stream):
            key = cut_key(row['top_n'], row['band'])
            rank_dates.add(row['rebalance_rank_date'])
            c = per_cut.setdefault(key, {'top_n': int(row['top_n']), 'band': row['band'],
                                         'rebalances': 0, 'adds_total': 0, 'drops_rank_total': 0,
                                         'drops_last_bar_total': 0, 'kept_total': 0,
                                         'members': [], 'turnover': [],
                                         'first_rank_date': None, 'last_rank_date': None,
                                         'last_effective_date': None})
            c['rebalances'] += 1
            c['adds_total'] += int(row['adds'])
            c['drops_rank_total'] += int(row['drops_rank'])
            c['drops_last_bar_total'] += int(row['drops_last_bar'])
            c['kept_total'] += int(row['kept'])
            c['members'].append(int(row['members']))
            c['turnover'].append(float(row['one_way_turnover']))
            c['first_rank_date'] = c['first_rank_date'] or row['rebalance_rank_date']
            c['last_rank_date'] = row['rebalance_rank_date']
            c['last_effective_date'] = row['effective_date']
    out = {}
    for key, c in per_cut.items():
        turnover = c['turnover']
        after_first = turnover[1:]
        out[key] = {
            'top_n': c['top_n'], 'band': c['band'], 'rebalances': c['rebalances'],
            'first_rank_date': c['first_rank_date'], 'last_rank_date': c['last_rank_date'],
            'last_effective_date': c['last_effective_date'],
            'members_median': median_rule(c['members']),
            'members_min': min(c['members']), 'members_max': max(c['members']),
            'members_final': c['members'][-1],
            'adds_total': c['adds_total'], 'adds_total_excluding_first_rebalance': c['adds_total'] - c['members'][0],
            'drops_rank_total': c['drops_rank_total'], 'drops_last_bar_total': c['drops_last_bar_total'],
            'drops_total': c['drops_rank_total'] + c['drops_last_bar_total'],
            'kept_total': c['kept_total'],
            'one_way_turnover_mean_all_rebalances': sum(turnover) / len(turnover),
            'one_way_turnover_annualised_x12_all_rebalances': 12.0 * sum(turnover) / len(turnover),
            'one_way_turnover_mean_excluding_first_rebalance': (sum(after_first) / len(after_first)) if after_first else None,
            'one_way_turnover_annualised_x12_excluding_first': (12.0 * sum(after_first) / len(after_first)) if after_first else None,
            'one_way_turnover_max_excluding_first': max(after_first) if after_first else None,
            'one_way_turnover_first_rebalance': turnover[0],
        }
    return {'distinct_rank_dates': len(rank_dates), 'cuts': out}


def summarize_coverage(path: Path) -> dict:
    years = []
    with path.open(encoding='utf-8', newline='') as stream:
        reader = csv.DictReader(stream)
        for row in reader:
            year = int(row['year'])
            entry = {'year': year, 'sessions': int(row['sessions']),
                     'dates_with_quarantined_duplicates': int(row['dates_with_quarantined_duplicates']),
                     'duplicate_positive_keys': int(row['duplicate_positive_keys']),
                     'rejected_rows': int(row['rejected_rows']),
                     'ids_seen': int(row['ids_seen']),
                     'ids_with_valid_bar_median': float(row['ids_with_valid_bar_median']),
                     'rebalances': int(row['rebalances']),
                     'eligible_median': float(row['eligible_median']) if row['eligible_median'] != '' else None,
                     'members_median': {}, 'nonmissing_fraction_median': {}, 'gics_missing_members_median': {}}
            for col, value in row.items():
                for prefix, slot in (('members_median_', 'members_median'),
                                     ('nonmissing_fraction_median_', 'nonmissing_fraction_median'),
                                     ('gics_missing_members_median_', 'gics_missing_members_median')):
                    if col.startswith(prefix):
                        entry[slot][col[len(prefix):]] = float(value) if value != '' else None
            years.append(entry)
    return {'years': years, 'sessions_total': sum(y['sessions'] for y in years),
            'rebalances_total': sum(y['rebalances'] for y in years),
            'quarantined_dates_by_year': {y['year']: y['dates_with_quarantined_duplicates'] for y in years}}


def summarize_union(path: Path) -> dict:
    years = []
    with path.open(encoding='utf-8', newline='') as stream:
        for row in csv.DictReader(stream):
            entry = {'year': int(row['year']), 'distinct': {}, 'cumulative': {}}
            for col, value in row.items():
                if col.startswith('distinct_'):
                    entry['distinct'][col[len('distinct_'):]] = int(value)
                elif col.startswith('cumulative_'):
                    entry['cumulative'][col[len('cumulative_'):]] = int(value)
            years.append(entry)
    final = years[-1]['cumulative'] if years else {}
    return {'years': years, 'cumulative_at_final_year': final,
            'final_year': years[-1]['year'] if years else None,
            'max_distinct_any_year': {k: max(y['distinct'].get(k, 0) for y in years) for k in final},
            'cumulative_vs_kMaxIcInstruments_4096': {k: v <= IC_INSTRUMENT_CAP for k, v in final.items()}}


def summarize_delisting(path: Path) -> dict:
    counts = {}
    with path.open(encoding='utf-8', newline='') as stream:
        for row in csv.DictReader(stream):
            key = cut_key(row['top_n'], row['band'])
            c = counts.setdefault(key, {'ever_members': 0, 'rank_drop': 0, 'last_bar_within_window': 0,
                                        'window_end': 0, 'other': 0})
            c['ever_members'] += 1
            c[row['exit_kind'] if row['exit_kind'] in c else 'other'] += 1
    return counts


def summarize_survivorship(doc: dict) -> dict:
    cuts = []
    for cut in doc.get('cuts', []):
        cuts.append({'cut': cut_key(cut.get('top_n'), cut.get('band')),
                     'ever_members': cut.get('ever_members'),
                     'ended_before_window_end': cut.get('ended_before_window_end'),
                     'censored': cut.get('censored'), 'fraction_ended': cut.get('fraction_ended'),
                     'per_year': cut.get('per_year')})
    return {'schema': doc.get('schema'), 'window_end': doc.get('window_end'), 'cuts': cuts,
            'comparison': doc.get('comparison'), 'caveat': doc.get('caveat')}


# ---------------------------------------------------------------------------
#  Receipt assembly
# ---------------------------------------------------------------------------
class Checks:
    def __init__(self):
        self.rows = []

    def add(self, name: str, ok, detail=None):
        self.rows.append({'check': name, 'ok': bool(ok), 'detail': detail})
        return bool(ok)

    @property
    def all_ok(self):
        return all(r['ok'] for r in self.rows)


def required_inputs(options) -> list:
    """(label, Path) for everything that must exist before a receipt can be written."""
    items = [(key, ROOT / value) for key, value in PINNED.items()]
    items.append(('measurement', ROOT / options.measurement))
    items += [(f'comparison[{i}]', ROOT / c) for i, c in enumerate(options.comparison)]
    items += [(f'log[{i}]', ROOT / c) for i, c in enumerate(options.log)]
    if options.end_review:
        items.append(('end_review', ROOT / options.end_review))
    items += [(f'finding[{i}]', ROOT / c) for i, c in enumerate(options.finding)]
    items += [(f'ingestion_manifest[{i}]', Path(d) / '_ingestion.manifest.json')
              for i, d in enumerate(SEGMENT_DIRS)]
    return items


def build(options, checks: Checks) -> dict:
    pins = {key: pin(ROOT / value) for key, value in PINNED.items()}
    if options.end_review:
        pins['end_review'] = pin(ROOT / options.end_review)
    design_text = read_text(ROOT / PINNED['design'])

    # --- design freeze --------------------------------------------------------------
    checks.add('design_sha256_is_frozen_4810fda2', pins['design']['sha256'] == FROZEN_DESIGN_SHA256,
               {'expected': FROZEN_DESIGN_SHA256, 'actual': pins['design']['sha256']})
    stage_embedded = STAGE_CONSTANT_RE.search(read_text(ROOT / PINNED['stage_source']))
    stage_embedded = stage_embedded.group(1).lower() if stage_embedded else None
    checks.add('stage_embeds_frozen_design_sha256', stage_embedded == FROZEN_DESIGN_SHA256,
               {'kEquityUniverseDesignNoteSha256': stage_embedded})
    runner_embedded = RUNNER_CONSTANT_RE.search(read_text(ROOT / PINNED['runner']))
    runner_embedded = runner_embedded.group(1).lower() if runner_embedded else None
    checks.add('runner_pins_frozen_design_sha256', runner_embedded == FROZEN_DESIGN_SHA256,
               {'DESIGN_NOTE_SHA256': runner_embedded})
    oracle = read_json(ROOT / PINNED['oracle_v1'])
    oracle_design = (oracle.get('design_note_sha256') or '').lower()
    checks.add('oracle_design_sha256_is_frozen_or_documented_predecessor',
               oracle_design in (FROZEN_DESIGN_SHA256, SUPERSEDED_DESIGN_SHA256),
               {'oracle_design_note_sha256': oracle_design,
                'is_superseded_section_14_digest': oracle_design == SUPERSEDED_DESIGN_SHA256})
    checks.add('oracle_schema_and_case_count', oracle.get('schema') == 'atx-iteration15-universe-oracle-v1'
               and oracle.get('case_count') == 42 and len(oracle.get('cases', [])) == 42,
               {'schema': oracle.get('schema'), 'case_count': oracle.get('case_count')})

    # --- comparator ------------------------------------------------------------------
    comparisons = []
    for i, relpath in enumerate(options.comparison):
        doc = read_json(ROOT / relpath)
        comparisons.append({'pin': pin(ROOT / relpath), 'schema': doc.get('schema'),
                            'status': doc.get('status'), 'counts': doc.get('counts'),
                            'oracle_sha256_recorded': ((doc.get('evidence') or {}).get('oracle') or {}).get('sha256'),
                            'comparator_sha256_recorded': ((doc.get('evidence') or {}).get('comparator_script') or {}).get('sha256'),
                            'logs_recorded': [{'sha256': l.get('sha256'), 'bytes': l.get('bytes'),
                                               'marker_lines': l.get('marker_lines')}
                                              for l in ((doc.get('evidence') or {}).get('logs') or [])],
                            'convention_cross_check_all_exact': (doc.get('convention_cross_check') or {}).get('all_exact'),
                            'not_measured_natively': len(doc.get('not_measured_natively') or [])})
    final = comparisons[-1] if comparisons else None
    counts = (final or {}).get('counts') or {}
    checks.add('comparator_final_status_pass', final is not None and final['status'] == 'pass'
               and final['schema'] == 'atx-iteration15-universe-comparison-v1'
               and counts.get('failed') == 0 and counts.get('not_measured_natively') == 0
               and counts.get('global_failures') == 0 and counts.get('compared') == counts.get('oracle_cases') == 42
               and final['convention_cross_check_all_exact'] is True,
               {'final': posix(options.comparison[-1]) if comparisons else None, 'counts': counts})
    checks.add('comparator_final_binds_pinned_oracle_and_comparator_by_digest',
               final is not None and final['oracle_sha256_recorded'] == pins['oracle_v1']['sha256']
               and final['comparator_sha256_recorded'] == pins['comparator']['sha256'],
               {'oracle_pinned': pins['oracle_v1']['sha256'], 'comparator_pinned': pins['comparator']['sha256']})

    # --- native logs and junits ----------------------------------------------------
    logs = []
    for relpath in options.log:
        path = ROOT / relpath
        text = read_text(path)
        entry = pin(path)
        entry['ctest_summary'] = parse_ctest_summary(text)
        entry['ninja_failed'] = ('FAILED:' in text) or ('ninja: build stopped' in text)
        entry['marker_lines'] = text.count('POINT_IN_TIME_UNIVERSE_MEASUREMENT {')
        logs.append(entry)
    log_digests = {l['sha256'] for l in logs}
    checks.add('comparator_final_log_is_a_pinned_log_by_digest',
               final is not None and bool(final['logs_recorded'])
               and all(l['sha256'] in log_digests for l in final['logs_recorded']),
               {'comparator_logs': [l['sha256'] for l in (final or {}).get('logs_recorded', [])]})
    junits = []
    for path in sorted((ROOT / 'build-equity').glob('iteration15-*.xml')):
        entry = pin(path)
        try:
            entry.update(parse_junit(path))
        except ET.ParseError as error:
            entry['parse_error'] = str(error)
        junits.append(entry)
    junit_by_stem = {Path(j['path']).stem: j for j in junits}
    families = {}
    for l in logs:
        if l['ctest_summary'] is None:
            continue
        stem = Path(l['path']).stem
        if '-data-' in stem:
            # the suite log carries the oracle markers; a group regression log carries none
            family = 'engine-data' if l['marker_lines'] > 0 else 'engine-data-group'
        else:
            family = 'impl' if '-impl-' in stem else 'other'
        families[family] = l  # last in the given order wins
        junit = junit_by_stem.get(stem)
        l['junit_agrees'] = (junit is not None and 'tests' in junit
                             and junit['tests'] == l['ctest_summary']['total']
                             and junit['failures'] == l['ctest_summary']['failed'])
    failed_attempts = [{'path': l['path'], 'ctest_summary': l['ctest_summary'], 'ninja_failed': l['ninja_failed']}
                       for l in logs if (l['ctest_summary'] and l['ctest_summary']['failed'] > 0) or l['ninja_failed']]
    engine_final = families.get('engine-data')
    engine_group = families.get('engine-data-group')
    impl_final = families.get('impl')
    checks.add('engine_data_suite_final_log_green_37_and_matches_junit',
               engine_final is not None and engine_final['ctest_summary']['failed'] == 0
               and engine_final['ctest_summary']['total'] == 37 and engine_final.get('junit_agrees') is True,
               {'log': (engine_final or {}).get('path'), 'summary': (engine_final or {}).get('ctest_summary')})
    checks.add('engine_data_group_regression_log_green_and_matches_junit',
               engine_group is not None and engine_group['ctest_summary']['failed'] == 0
               and engine_group.get('junit_agrees') is True,
               {'log': (engine_group or {}).get('path'), 'summary': (engine_group or {}).get('ctest_summary')})
    checks.add('impl_tests_final_log_green_and_matches_junit',
               impl_final is not None and impl_final['ctest_summary']['failed'] == 0
               and impl_final.get('junit_agrees') is True,
               {'log': (impl_final or {}).get('path'), 'summary': (impl_final or {}).get('ctest_summary')})
    checks.add('every_log_with_a_ctest_summary_agrees_with_its_junit_when_one_exists',
               all(l.get('junit_agrees', True) for l in logs
                   if l['ctest_summary'] is not None and Path(l['path']).stem in junit_by_stem))

    # --- measurement JSON ------------------------------------------------------------
    measurement_path = ROOT / options.measurement
    measurement = read_json(measurement_path)
    pins['measurement'] = pin(measurement_path)
    for key, value in (measurement.get('logs') or {}).items():
        p = Path(value)
        if p.exists():
            pins[f'measurement_{key}_log'] = pin(p)
    evidence = measurement.get('output_evidence') or {}
    preflight = measurement.get('preflight') or {}
    is_reeval = 'reeval' in measurement_path.name.lower()
    measurement_kind = 'reevaluation_of_attempt1' if is_reeval else 'runner_written'
    runner_defect = None
    if isinstance(measurement.get('runner_defect'), dict) or is_reeval:
        runner_defect = {
            'defect': measurement.get('runner_defect'),
            'measurement_kind_declared_by_file': measurement.get('measurement_kind'),
            'exit_code_source': measurement.get('exit_code_source'),
            'wall_seconds_source': measurement.get('wall_seconds_source'),
            'accepted_source': measurement.get('accepted_source'),
            'statement': 'The original runner crashed AFTER the stage completed (post-run pin step, '
                         'a runner defect); the stage outputs and the two ledger lines were intact. '
                         'The stage exit code in this receipt is INFERRED (manifest status complete, '
                         'terminal ledger line completed, stderr empty), not captured by a parent '
                         'process. No re-run was made (it would append two more ledger lines).',
            'launcher_digest': 'The launching runner\'s own digest at attempt 1 is UNRECOVERABLE: the '
                               'runner was fixed in place after its post-run crash and its pre-crash '
                               'bytes were not preserved. The pinned runner is the fixed one. The '
                               'producer binding therefore rests on manifest.json.producer_executable_sha256 '
                               '== sha256(current build-equity/bin/atx-impl.exe) == '
                               f'{EXPECTED_PRODUCER_EXE_SHA256}, verified above.',
        }
    findings = []
    for relpath in options.finding:
        path = ROOT / relpath
        head = read_text(path).splitlines()[:FINDING_EXCERPT_LINES]
        findings.append({'pin': pin(path), 'summary_excerpt_lines': len(head), 'summary_excerpt': head})
    checks.add('measurement_schema_and_accepted',
               measurement.get('schema') == 'atx-equity-universe-native-measurement-v1'
               and measurement.get('accepted') is True and measurement.get('exit_code') == 0
               and measurement.get('pins_unchanged') is True and measurement.get('budget_breach') is None
               and measurement.get('ledger_two_cp15_lines') is True and not evidence.get('missing')
               and evidence.get('membership_bin_ok') is True and evidence.get('request_rebalances_match') is True
               and evidence.get('manifest_files_match') is True,
               {'accepted': measurement.get('accepted'), 'exit_code': measurement.get('exit_code'),
                'pins_changed': measurement.get('pins_changed'), 'missing': evidence.get('missing')})
    checks.add('measurement_preflight_saw_frozen_design_in_note_and_stage',
               preflight.get('design_note_sha256') == FROZEN_DESIGN_SHA256
               and preflight.get('design_note_matches_frozen_digest') is True
               and preflight.get('stage_embeds_frozen_digest') is True,
               {'preflight_design_note_sha256': preflight.get('design_note_sha256')})
    run_digests = {v.get('sha256') for v in (measurement.get('source_and_input_pins_after') or {}).values()
                   if isinstance(v, dict)}
    drifted = [k for k in RUN_PINNED_SOURCE_KEYS if pins[k]['sha256'] not in run_digests]
    checks.add('sources_byte_identical_to_what_the_run_pinned', not drifted and bool(run_digests),
               {'drifted_since_run': drifted})
    checks.add('run_pinned_oracle_and_comparator_by_digest',
               (measurement.get('oracle') or {}).get('sha256') == pins['oracle_v1']['sha256']
               and (measurement.get('comparator') or {}).get('sha256') == pins['comparator']['sha256'])
    inference = (measurement.get('reevaluation') or {}).get('exit_code_inference') or {}
    checks.add('stage_exit_code_inferred_0_manifest_complete_stderr_empty_or_captured_0',
               (measurement.get('exit_code') == 0 and not is_reeval) or
               (is_reeval and inference.get('inferred_exit_code') == 0 and inference.get('manifest_status_complete') is True
                and inference.get('ledger_terminal_line_completed') is True and measurement.get('stage_stderr') == ''),
               {'is_reeval': is_reeval, 'inference': inference or None})

    # --- real-data output directory ------------------------------------------------
    out_dir = Path(measurement.get('output_root') or '')
    output_pins = {}
    for name in OUTPUT_FILES:
        p = out_dir / name
        if p.exists():
            output_pins[name] = pin(p, posix(p))
    checks.add('all_ten_output_files_present', len(output_pins) == len(OUTPUT_FILES),
               {'missing': [n for n in OUTPUT_FILES if n not in output_pins], 'output_root': posix(out_dir)})
    checks.add('no_pending_marker_no_failure_json',
               not (out_dir / '.pending').exists() and not (out_dir / 'failure.json').exists())
    manifest = read_json(out_dir / 'manifest.json') if 'manifest.json' in output_pins else {}
    recorded = digests_in(manifest)
    mismatched = [n for n in output_pins if n != 'manifest.json' and n in recorded
                  and recorded[n] != output_pins[n]['sha256']]
    unrecorded = [n for n in output_pins if n != 'manifest.json' and n not in recorded]
    checks.add('stage_manifest_digests_match_recomputed_output_digests', not mismatched and not unrecorded,
               {'mismatched': mismatched, 'unrecorded': unrecorded})
    manifest_hex = hex64_values(manifest)
    checks.add('stage_manifest_carries_frozen_design_sha256', FROZEN_DESIGN_SHA256 in manifest_hex)
    producer = (manifest.get('producer_executable_sha256') or '').lower()
    checks.add('manifest_producer_executable_sha256_equals_current_atx_impl_exe_and_expected_8425fc03',
               producer == pins['impl_executable']['sha256'] == EXPECTED_PRODUCER_EXE_SHA256
               and manifest.get('status') == 'complete',
               {'manifest_producer': producer, 'current_exe': pins['impl_executable']['sha256'],
                'expected': EXPECTED_PRODUCER_EXE_SHA256, 'manifest_status': manifest.get('status')})
    request = read_json(out_dir / 'request.json') if 'request.json' in output_pins else {}
    seal = read_json(out_dir / 'seal.json') if 'seal.json' in output_pins else {}
    checks.add('request_json_states_84_rebalances',
               EXPECTED_REBALANCES in find_ints(request, ('rebalances', 'rebalance_count', 'observations')))
    checks.add('seal_json_refused_nothing_latest_attached_2019_12_31',
               seal.get('policy') == 'RefuseAtOrAfterValidationBeginV1' and seal.get('segments_refused') == 0
               and seal.get('latest_attached') == '2019-12-31' and seal.get('validation_begin') == '2020-01-01'
               and seal.get('sealed_begin') == '2023-01-01', seal)

    churn = summarize_churn(out_dir / 'churn.csv') if 'churn.csv' in output_pins else {'distinct_rank_dates': 0, 'cuts': {}}
    coverage = summarize_coverage(out_dir / 'coverage_by_year.csv') if 'coverage_by_year.csv' in output_pins else {'years': [], 'sessions_total': 0, 'rebalances_total': 0, 'quarantined_dates_by_year': {}}
    union = summarize_union(out_dir / 'union_by_year.csv') if 'union_by_year.csv' in output_pins else {'years': [], 'cumulative_at_final_year': {}, 'final_year': None, 'max_distinct_any_year': {}, 'cumulative_vs_kMaxIcInstruments_4096': {}}
    delisting = summarize_delisting(out_dir / 'delisting.csv') if 'delisting.csv' in output_pins else {}
    survivorship = summarize_survivorship(read_json(out_dir / 'survivorship.json')) if 'survivorship.json' in output_pins else {}
    expected_cut_keys = [f't{t}_b{b}' for t, b in EXPECTED_CUTS]
    checks.add('churn_has_84_rebalances_in_each_of_the_six_cuts',
               churn['distinct_rank_dates'] == EXPECTED_REBALANCES and sorted(churn['cuts']) == sorted(expected_cut_keys)
               and all(c['rebalances'] == EXPECTED_REBALANCES for c in churn['cuts'].values())
               and all(c['first_rank_date'] == '2012-12-31' and c['last_rank_date'] == '2019-11-29'
                       and c['last_effective_date'] == '2019-12-02' for c in churn['cuts'].values()),
               {'distinct_rank_dates': churn['distinct_rank_dates'], 'cuts': sorted(churn['cuts'])})
    checks.add('coverage_sessions_total_1955_and_84_rebalances',
               coverage['sessions_total'] == EXPECTED_SESSIONS and coverage['rebalances_total'] == EXPECTED_REBALANCES
               and [y['year'] for y in coverage['years']] == list(range(2012, 2020)),
               {'sessions_total': coverage['sessions_total'], 'rebalances_total': coverage['rebalances_total']})
    checks.add('coverage_quarantined_duplicate_dates_match_design_2_2_7_3_10_0_83_0',
               coverage['quarantined_dates_by_year'] == EXPECTED_QUARANTINED_DATES,
               coverage['quarantined_dates_by_year'])
    checks.add('union_years_2013_to_2019_six_cuts',
               [y['year'] for y in union['years']] == list(range(2013, 2020))
               and sorted(union['cumulative_at_final_year']) == sorted(expected_cut_keys))
    checks.add('survivorship_schema_six_cuts_window_end_2019_12_31',
               survivorship.get('schema') == 'atx-equity-universe-survivorship-v1'
               and survivorship.get('window_end') == '2019-12-31' and len(survivorship.get('cuts', [])) == 6
               and all(c['ever_members'] == c['ended_before_window_end'] + c['censored'] for c in survivorship.get('cuts', [])))
    checks.add('delisting_ever_members_agree_with_survivorship',
               bool(delisting) and all(delisting.get(c['cut'], {}).get('ever_members') == c['ever_members']
                                       for c in survivorship.get('cuts', [])))
    bin_info = None
    if 'membership.bin' in output_pins:
        try:
            image = decode_membership_bin((out_dir / 'membership.bin').read_bytes())
            rebs = image.pop('rebalances')
            bin_info = dict(image)
            bin_info.update({'first_rank_date': nanos_to_date(rebs[0]['rank_session_key']) if rebs else None,
                             'last_rank_date': nanos_to_date(rebs[-1]['rank_session_key']) if rebs else None,
                             'last_effective_date': nanos_to_date(rebs[-1]['effective_session_key']) if rebs else None,
                             'all_effective_positive': all(r['effective_session_key'] > 0 for r in rebs),
                             'member_counts_final': rebs[-1]['member_counts'] if rebs else None})
            bin_final_counts = rebs[-1]['member_counts'] if rebs else []
            csv_final_counts = [churn['cuts'][k]['members_final'] for k in expected_cut_keys if k in churn['cuts']]
        except ValueError as error:
            bin_info = {'error': str(error)}
            bin_final_counts, csv_final_counts = [], [None]
    else:
        bin_final_counts, csv_final_counts = [], [None]
    checks.add('membership_bin_decodes_trailer_verified_header_frozen_84_rebalances',
               bin_info is not None and 'error' not in bin_info and bin_info['trailer_matches']
               and bin_info['version'] == 1 and bin_info['adv_window'] == 63 and bin_info['min_valid_observations'] == 57
               and bin_info['min_raw_price_exclusive'] == 1.0 and bin_info['top_n'] == [1000, 2000, 3000]
               and bin_info['band_bp'] == [0, 1000] and bin_info['rebalance_count'] == EXPECTED_REBALANCES
               and bin_info['first_rank_date'] == '2012-12-31' and bin_info['last_rank_date'] == '2019-11-29'
               and bin_info['last_effective_date'] == '2019-12-02' and bin_info['all_effective_positive'],
               bin_info)
    checks.add('membership_bin_final_member_counts_equal_churn_csv',
               bool(bin_final_counts) and bin_final_counts == csv_final_counts,
               {'bin': bin_final_counts, 'churn_csv': csv_final_counts})

    # --- ingestion manifests (digests only) ------------------------------------------
    ingestion = []
    for i, d in enumerate(SEGMENT_DIRS):
        p = Path(d) / '_ingestion.manifest.json'
        doc = read_json(p)
        recipe = doc.get('recipe')
        recipe = json.loads(recipe) if isinstance(recipe, str) else (recipe or {})
        ingestion.append({'role': SEGMENT_ROLES[i], 'pin': pin(p, posix(p)),
                          'segments_listed': len(doc.get('segments') or []),
                          'status': doc.get('status'), 'schema': doc.get('schema'),
                          'policy_version': (doc.get('preparation') or {}).get('policy_version'),
                          'preparation_manifest_sha256': (doc.get('preparation') or {}).get('manifest_sha256'),
                          'loader_executable_sha256': recipe.get('executable_sha256')})
    checks.add('ingestion_manifests_complete_qa_v1_expected_segment_counts',
               all(e['status'] == 'complete' and e['schema'] == 'atx-ingestion-v1'
                   and e['policy_version'] == 'tickerhistory-qa-v1' for e in ingestion)
               and tuple(e['segments_listed'] for e in ingestion) == EXPECTED_SEGMENTS_PER_DIR,
               {'segments_listed': [e['segments_listed'] for e in ingestion]})
    checks.add('loaders_heterogeneous_as_stated_M6_9415a6ab_then_ac3ab17f_x6',
               ingestion[0]['loader_executable_sha256'] == LOADER_2012_2013
               and all(e['loader_executable_sha256'] == LOADER_2014_2019 for e in ingestion[1:]),
               {'loaders': [e['loader_executable_sha256'] for e in ingestion]})
    ingest2 = read_json(ROOT / PINNED['ingest_receipt_attempt2'])
    receipt_ingestion_digests = {((y.get('manifest_sha256') or {}).get('ingestion_manifest') or '').lower()
                                 for y in ingest2.get('years', [])}
    checks.add('ingest_receipt_attempt2_complete_and_binds_the_six_2014_2019_manifests_by_digest',
               ingest2.get('status') == 'complete' and ingest2.get('accepted') is True
               and all(e['pin']['sha256'] in receipt_ingestion_digests for e in ingestion[1:]))
    manifest_seg_digests = digests_in(manifest)
    checks.add('stage_manifest_pins_1955_segments',
               sum(1 for n in manifest_seg_digests if n.endswith('.seg')) == EXPECTED_SESSIONS,
               {'segments_in_manifest': sum(1 for n in manifest_seg_digests if n.endswith('.seg'))})

    # --- trial ledger ----------------------------------------------------------------
    ledger_payload = (ROOT / LEDGER).read_bytes()
    try:
        walked = walk_ledger(ledger_payload, read_text(ROOT / LEDGER_SIDECAR))
        chain_error = None
    except ValueError as error:
        walked, chain_error = {'lines': [], 'head_sha256': None, 'anchored': False}, str(error)
    lines = walked['lines']
    cp14_receipt = read_json(ROOT / CP14_RECEIPT)
    cp14_pin = cp14_receipt['pins']['trial_ledger']
    cp14_prefix = b''.join(l['raw'] + b'\n' for l in lines[:2])
    cp14_lines = [l for l in lines if l['entry'].get('checkpoint') == 14]
    cp15_lines = [l for l in lines if l['entry'].get('checkpoint') == CHECKPOINT]
    checks.add('ledger_chain_valid_and_sidecar_anchored', chain_error is None and walked['anchored'], chain_error)
    checks.add('cp14_lines_byte_unchanged_vs_cp14_receipt_pin',
               len(lines) >= 2 and hashlib.sha256(cp14_prefix).hexdigest() == cp14_pin['sha256']
               and len(cp14_prefix) == cp14_pin['bytes'] and lines[1]['line_sha256'] == CP14_LEDGER_HEAD_SHA256
               and [l['index'] for l in cp14_lines] == [0, 1],
               {'cp14_receipt_pin': cp14_pin, 'first_two_lines_sha256': hashlib.sha256(cp14_prefix).hexdigest()})
    n14 = sum(l['entry'].get('trial_count_declared', 0) for l in cp14_lines if l['entry'].get('status') == 'pre-registered')
    n15 = sum(l['entry'].get('trial_count_declared', 0) for l in cp15_lines if l['entry'].get('status') == 'pre-registered')
    checks.add('declared_trials_checkpoint_14_is_30_unchanged', n14 == 30, {'N14': n14})
    checks.add('declared_trials_checkpoint_15_is_0', cp15_lines and n15 == 0, {'N15': n15})
    checks.add('exactly_two_cp15_lines_pre_registered_then_completed_same_trial_id',
               len(cp15_lines) == 2 and len(lines) == 4
               and [l['entry'].get('status') for l in cp15_lines] == ['pre-registered', 'completed']
               and cp15_lines[0]['entry'].get('trial_id') == cp15_lines[1]['entry'].get('trial_id')
               and all(l['entry'].get('purpose') == PURPOSE and l['entry'].get('trial_count_declared') == 0
                       and l['entry'].get('schema') == 'atx-trial-ledger-v1' for l in cp15_lines)
               and (cp15_lines[1]['entry'].get('result') or {}).get('outcome') == 'completed',
               {'lines_total': len(lines), 'cp15_statuses': [l['entry'].get('status') for l in cp15_lines]})
    checks.add('cp15_trial_id_is_iteration15_point_in_time_universe_0001',
               bool(cp15_lines) and cp15_lines[0]['entry'].get('trial_id') == 'iteration15-point-in-time-universe-0001')
    parents = (cp15_lines[0]['entry'].get('parents') or []) if cp15_lines else []
    parent_design = [p.get('sha256') for p in parents if p.get('role') == 'design-note']
    parent_segments = {p.get('sha256') for p in parents if str(p.get('role', '')).startswith('segments-')}
    checks.add('cp15_line_parents_bind_frozen_design_and_the_seven_ingestion_manifests_by_digest',
               parent_design == [FROZEN_DESIGN_SHA256] and parent_segments == {e['pin']['sha256'] for e in ingestion}
               and len([p for p in parents if str(p.get('role', '')).startswith('segments-')]) == 7,
               {'design_note_parent': parent_design, 'segment_parent_count': len(parent_segments)})
    checks.add('cp15_completed_line_manifest_sha256_equals_recomputed_manifest_json',
               bool(cp15_lines) and (cp15_lines[-1]['entry'].get('result') or {}).get('manifest_sha256')
               == output_pins.get('manifest.json', {}).get('sha256'))
    checks.add('cp15_line_window_84_observations_2013_01_02_to_2020_01_01',
               bool(cp15_lines) and (cp15_lines[0]['entry'].get('window') or {}) ==
               {'start': '2013-01-02', 'end_exclusive': '2020-01-01', 'observations': EXPECTED_REBALANCES},
               (cp15_lines[0]['entry'].get('window') if cp15_lines else None))
    # Design section 5.5 step 6: manifest.json is written BEFORE the terminal ledger line, so the
    # manifest can carry only the pre-registration line digest; the completed line binds the
    # manifest the other way round (result.manifest_sha256, checked above).
    manifest_ledger = manifest.get('trial_ledger') or {}
    checks.add('stage_manifest_pre_registration_line_sha256_equals_walked_cp15_line_digest',
               bool(cp15_lines) and manifest_ledger.get('pre_registration_line_sha256') == cp15_lines[0]['line_sha256']
               and manifest_ledger.get('trial_id') == cp15_lines[0]['entry'].get('trial_id')
               and manifest_ledger.get('trial_count_declared') == 0 and manifest_ledger.get('purpose') == PURPOSE,
               {'manifest': manifest_ledger.get('pre_registration_line_sha256'),
                'walked': cp15_lines[0]['line_sha256'] if cp15_lines else None})
    ledger_after_run = measurement.get('ledger_after') or {}
    checks.add('measurement_ledger_head_equals_current_head', ledger_after_run.get('head_sha256') == walked['head_sha256'],
               {'run': ledger_after_run.get('head_sha256'), 'now': walked['head_sha256']})

    # --- receipt ---------------------------------------------------------------------
    receipt = {
        'schema': SCHEMA,
        'checkpoint': CHECKPOINT,
        'date': '2026-09-20',
        'status': STATUS,
        'accepted': checks.all_ok,
        'summary_statement': (
            'Point-in-time universe construction over the 2013-2019 archive (seven segment '
            'directories, 1,955 sessions, 63-session median dollar-volume rank, 57/63 minimum, '
            'raw close > 1.0 on the rank session, monthly cadence ranked at t and effective at '
            't+1 session, six top_n x band cuts {1000,2000,3000} x {0.00,0.10} reported side by '
            'side, 84 rebalances 2012-12-31..2019-11-29). Engine unit agrees with an independent '
            'exact oracle on 42/42 synthetic cases; the atx-impl equity-universe subcommand ran '
            'once on real data under the 2020-01-01 seal, wrote one non-trial ledger line pair '
            '(trial_count_declared 0) and completed. Membership lists and counts only; no '
            'forecast evidence of any kind.'),
        'immutability': 'This receipt is written once and never edited. Corrections go in a '
                        'separate addendum (schema atx-validation-receipt-addendum-v1) that pins '
                        'this file by digest. Historical receipt writers are never rerun.',
        'path_policy': 'Every binding below is a sha256 + byte count. Cross-references between '
                       'documents (comparator -> oracle/logs, measurement -> sources/executable, '
                       'ledger parents -> ingestion manifests, stage manifest -> outputs and ledger '
                       'lines) are verified by digest membership, never by path-string equality. '
                       'Paths are recorded for humans only.',
        'design_sha256_lineage': {
            'frozen_receipt_time': FROZEN_DESIGN_SHA256,
            'section_14_pre_T1_digest_superseded_by_section_15_5': SUPERSEDED_DESIGN_SHA256,
            'stage_embedded': stage_embedded, 'runner_embedded': runner_embedded,
            'oracle_v1_document': oracle_design,
            'note': 'Oracle v1 was cut against the section-14 digest before T1 measurement amended '
                    'sections 15.4-15.5; the design records the supersession and the oracle expected '
                    'values were not changed by that amendment (section 15.5). The stage embeds and '
                    'the runner pins the frozen digest.'},
        'pre_registration': {
            'frozen_values': PRE_REGISTRATION_FROZEN,
            'design_section_6_verbatim': section_lines(design_text, '## 6. Pre-registration block', '## 7.'),
            'trial_id': cp15_lines[0]['entry'].get('trial_id') if cp15_lines else None,
            'trial_count_declared': 0 if cp15_lines else None,
            'declared_trials_checkpoint_14': n14, 'declared_trials_checkpoint_15': n15,
        },
        'summary': {
            'rebalances': churn['distinct_rank_dates'],
            'per_cut': churn['cuts'],
            'coverage_by_year': coverage['years'],
            'sessions_total': coverage['sessions_total'],
            'union_by_year': union['years'],
            'union_cumulative_at_2019': union['cumulative_at_final_year'],
            'union_max_distinct_any_year': union['max_distinct_any_year'],
            'union_cumulative_within_kMaxIcInstruments_4096': union['cumulative_vs_kMaxIcInstruments_4096'],
            'survivorship': survivorship,
            'delisting_exit_kind_counts': delisting,
            'membership_bin': bin_info,
            'runtime_seconds_from_stage_manifest': manifest.get('runtime_seconds'),
            'peak_working_set_bytes_from_stage_manifest': manifest.get('peak_working_set_bytes'),
            'wall_seconds_from_runner': measurement.get('wall_seconds'),
            'sampled_os_maxima_bytes_from_runner': measurement.get('sampled_os_maxima_bytes'),
            'runner_sample_count': measurement.get('sample_count'),
            'computation_note': 'Every number above is recomputed from churn.csv, coverage_by_year.csv, '
                                'union_by_year.csv, delisting.csv, survivorship.json, membership.bin, '
                                'manifest.json and the runner measurement JSON by this script. '
                                'Medians follow design section 3.5. Turnover means are arithmetic means '
                                'over rebalances of one_way_turnover as written by the stage; the first '
                                'rebalance (adds == members by construction) is excluded from the '
                                'excluding_first figures and the x12 annualisation is a naive multiple.',
        },
        'native_tests': {
            'toolchain': 'clang-cl 18, preset equity-dev, static Debug, /W4 /permissive- /WX (see the '
                         'pinned configure log; no hygiene claim)',
            'engine_final': (engine_final or {}).get('ctest_summary'),
            'engine_final_log': (engine_final or {}).get('path'),
            'engine_group_regression': (engine_group or {}).get('ctest_summary'),
            'engine_group_regression_log': (engine_group or {}).get('path'),
            'impl_final': (impl_final or {}).get('ctest_summary'),
            'impl_final_log': (impl_final or {}).get('path'),
            'logs': logs, 'junits': junits,
            'failed_attempts_preserved': failed_attempts,
        },
        'oracle_comparison': {
            'oracle': pins['oracle_v1'], 'comparator': pins['comparator'],
            'comparisons': comparisons,
            'final_status': (final or {}).get('status'), 'final_counts': counts,
            'method': 'exact equality per family; reals as Fraction(float) (DR15-5); medians as 2x '
                      'integers (DR15-15); closed marker key list; every oracle case required natively',
        },
        'measurement_kind': measurement_kind,
        'runner_defect': runner_defect,
        'data_quality_findings': findings,
        'data_hole_2016_12_30': DATA_HOLE_2016_12_30,
        'coverage_year_attribution': COVERAGE_YEAR_ATTRIBUTION,
        'cp16_capacity_blocker': {
            'kMaxIcInstruments': IC_INSTRUMENT_CAP,
            'cumulative_union_at_2019_recomputed': union['cumulative_at_final_year'],
            'cuts_exceeding_cap': sorted(k for k, v in union['cumulative_at_final_year'].items() if v > IC_INSTRUMENT_CAP),
            'cuts_within_cap': sorted(k for k, v in union['cumulative_at_final_year'].items() if v <= IC_INSTRUMENT_CAP),
            'distinct_2017_recomputed': next((y['distinct'] for y in union['years'] if y['year'] == 2017), None),
            'distinct_2017_contaminated_by_data_hole': True,
            'statement': 'kMaxIcInstruments 4096 is below the cumulative 2013-2019 union of the t2000 and t3000 '
                         'cuts (t1000 fits); the 2017 per-year t3000 union is inflated by the 2016-12-30 data '
                         'hole (R15-17). cp16 must use per-year contexts, raise the cap by explicit ruling, or '
                         're-run after hole remediation under a new pre-registration. This checkpoint only '
                         'measures the numbers (R15-7, design section 11.8).',
        },
        'real_data_run': {
            'attempt': measurement.get('attempt'),
            'measurement_kind': measurement_kind,
            'command_line': measurement.get('command_line'),
            'exit_code': measurement.get('exit_code'),
            'exit_code_source': measurement.get('exit_code_source') or ('INFERRED' if is_reeval else 'captured_by_runner'),
            'wall_seconds': measurement.get('wall_seconds'),
            'budget_breach': measurement.get('budget_breach'),
            'runner_accepted': measurement.get('accepted'),
            'output_directory': posix(out_dir),
            'output_pins': output_pins,
            'membership_csv_note': 'membership.csv is bound by digest only (not parsed by this receipt); '
                                   'its rows are derivable from membership.bin, which is decoded and '
                                   'trailer-verified above.',
            'seal': seal,
            'loaders': 'heterogeneous by construction (M-6): 2012-2013 segments loaded by atx-impl '
                       f'{LOADER_2012_2013[:8]}..., 2014-2019 by {LOADER_2014_2019[:8]}...; the stage '
                       'manifest and the ingestion manifests both record this',
            'ingestion_manifests': ingestion,
            'segments_note': 'The 1,955 .seg files are pinned by the stage manifest (and were '
                             'sha256-verified by the stage against each directory\'s ingestion '
                             'manifest before the ledger line, design section 5.5 step 2); this '
                             'receipt binds the ingestion manifests by digest and does not re-hash '
                             'the segments.',
        },
        'trial_ledger': {
            'pin': pins['trial_ledger'], 'sidecar_pin': pins['trial_ledger_sidecar'],
            'chain_valid': chain_error is None, 'chain_error': chain_error,
            'head_sha256': walked['head_sha256'], 'lines': len(lines),
            'line_digests': [{'index': l['index'], 'checkpoint': l['entry'].get('checkpoint'),
                              'trial_id': l['entry'].get('trial_id'), 'status': l['entry'].get('status'),
                              'purpose': l['entry'].get('purpose'),
                              'trial_count_declared': l['entry'].get('trial_count_declared'),
                              'line_sha256': l['line_sha256'], 'bytes': l['bytes']} for l in lines],
            'cp14_receipt_ledger_pin': cp14_pin,
            'cp14_lines_unchanged': hashlib.sha256(cp14_prefix).hexdigest() == cp14_pin['sha256'],
            'checkpoint_15_lines': [{k: v for k, v in l['entry'].items() if k not in ('recipe', 'source_exclusions')}
                                    | {'line_sha256': l['line_sha256']} for l in cp15_lines],
            'note': 'recipe/source_exclusions of the cp15 lines are the atx-trial-ledger-v1 defaults '
                    'declared NOT APPLICABLE in the line\'s notes (design section 5.6, DR15-16); they '
                    'are bound by the line digest, not restated here.',
        },
        'review': {
            'design_review': pins['design_review'], 't1_report': pins['t1_report'],
            't2_report': pins['t2_report'], 't3_report': pins['t3_report'],
            'end_review': pins.get('end_review'), 'research': pins['research'],
        },
        'qualifications': [
            'Stage 1 universe construction only: membership lists, churn, coverage, union and '
            'survivorship counts. No alpha, no forecast, no Sharpe, no capacity, no throughput claim.',
            'No float or common-stock eligibility screen; instrument type unknown is accepted; '
            'gics_missing_members is reported, not applied (R15-6).',
            'Both survivorship metrics are LOWER BOUNDS on bias: the archive backfill policy is '
            'unknown (historical_availability: unknown-archive-snapshot), names absent from the '
            'archive are invisible, no delisting return is imputed (R15-10).',
            'Vendor shares lag splits and carry zeros; vendor_market_cap_usd is labelled '
            'no-filing-vintage, never a rank key, and must not be used as a size screen downstream.',
            '2018 thinning: 83 dates with quarantined duplicate keys reduce valid bars; counted in '
            'coverage_by_year.csv, not repaired (R15-9 quarantine-all).',
            'ID reuse (securityID 77622) makes at most a handful of histories composite; reported, not resolved.',
            'Ingestion caveat: the source ZIP was not read-only during the 2014-2019 loads (mode 0666 '
            'as in the 2013 run); integrity was guarded by sha256 before/after each year, recorded in '
            'the pinned ingest receipt.',
            'The seven segment directories were loaded by two different atx-impl builds '
            f'({LOADER_2012_2013[:8]}... for 2012-2013, {LOADER_2014_2019[:8]}... for 2014-2019); '
            'the recipe (policy tickerhistory-qa-v1) is the same by manifest.',
            'The ledger line uses the cp14-shaped atx-trial-ledger-v1 schema with defaults declared '
            'inapplicable in notes (DR15-16).',
            'The oracle v1 document embeds the pre-T1 section-14 design digest; the design records its '
            'supersession (section 15.5) and the stage and runner carry the frozen digest.',
            'cp13/cp14 receipt CMake pins drift by the one-line cp15 CMake edits; recorded in an '
            'addendum, receipts untouched (section 11.9).',
            'The 2020-01-01 seal is asserted by code (segment filename dates) and is non-vacuous by '
            'data only in that no 2020+ directory was listed; it says nothing about leakage inside 2013-2019.',
            'DATA HOLE 2016-12-30 (R15-17): see data_hole_2016_12_30; churn at 2016-12-30/2017-01-31, '
            '2016 drops_last_bar totals, union_by_year 2017 and every cumulative column from 2017 on, '
            'and the 2016/2017 nonmissing medians are contaminated by a vendor OHLC defect and are '
            'labelled, not corrected; the run stands as measured.',
            'The measurement JSON is a post-hoc re-evaluation of attempt 1 (runner crashed after the '
            'stage completed); the stage exit code is inferred, see runner_defect.' if is_reeval else
            'The measurement JSON was written by the runner in-process.',
            'COVERAGE YEAR ATTRIBUTION (end review I-1): coverage_by_year rows attribute rebalances to the '
            'rank-session year, deviating from design section 15.2 pins 5/6 wording; see coverage_year_attribution.',
            'CP16 CAPACITY BLOCKER (end review I-3): cumulative union t2000/t3000 exceed kMaxIcInstruments '
            '4096 and the 2017 t3000 union is hole-contaminated; see cp16_capacity_blocker.',
        ],
        'not_established': NOT_ESTABLISHED,
        'checks': checks.rows,
        'sources': {k: pins[k] for k in ('engine_header', 'engine_source', 'engine_test', 'engine_cmake',
                                          'stage_header', 'stage_source', 'stage_test', 'ledger_header',
                                          'ledger_source', 'ledger_test', 'impl_config_hpp', 'impl_config_cpp',
                                          'impl_dispatch', 'impl_cmake')},
        'evidence': {
            'scripts': {k: pins[k] for k in ('oracle_script', 'comparator', 'runner', 'ingest_script')},
            'logs': [l['path'] for l in logs], 'junits': [j['path'] for j in junits],
            'binaries': {k: pins[k] for k in ('engine_test_executable', 'impl_test_executable', 'impl_executable')},
            'measurement': pins['measurement'],
            'ingest_receipts': {k: pins[k] for k in ('ingest_receipt_attempt1_killed', 'ingest_receipt_attempt2',
                                                     'ingest_run_attempt1_killed_log', 'ingest_run_log',
                                                     'ingest_run_err_log')},
        },
        'pins': pins,
    }
    return receipt


# ---------------------------------------------------------------------------
#  CLI
# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', default=DEFAULT_OUT, help='receipt path (worktree-relative); must not exist')
    parser.add_argument('--measurement', default=DEFAULT_MEASUREMENT, help='runner measurement JSON')
    parser.add_argument('--comparison', action='append', default=None,
                        help='comparator JSON, repeatable; the LAST one is the final verdict')
    parser.add_argument('--log', action='append', default=None,
                        help='native log, repeatable; appended to the default T1 list')
    parser.add_argument('--end-review', default=None,
                        help=f'cp15 end review path (required; default {DEFAULT_END_REVIEW})')
    parser.add_argument('--finding', action='append', default=None,
                        help='data-quality finding document, repeatable; pinned and its first '
                             f'{FINDING_EXCERPT_LINES} lines copied verbatim (default: the cp15 id-gap investigation)')
    parser.add_argument('--dry-run', action='store_true', help='verify and list missing inputs; write nothing')
    parser.add_argument('--selftest', action='store_true', help='exercise the pure helpers only')
    options = parser.parse_args(argv)
    if options.selftest:
        return selftest()
    options.comparison = options.comparison or list(DEFAULT_COMPARISONS)
    options.log = DEFAULT_LOGS + (options.log or [])
    options.finding = options.finding or list(DEFAULT_FINDINGS)
    options.end_review = options.end_review or DEFAULT_END_REVIEW  # required (end review I-2)
    out = ROOT / options.out

    missing = [f'{label}: {posix(path)}' for label, path in required_inputs(options) if not path.exists()]
    out_exists = out.exists()
    if options.dry_run:
        report = {'dry_run': True, 'root': posix(ROOT), 'out': posix(out), 'out_exists': out_exists,
                  'missing_inputs': missing, 'writable': not missing and not out_exists}
        if not missing:
            checks = Checks()
            try:
                receipt = build(options, checks)
                report['accepted_would_be'] = receipt['accepted']
            except Exception as error:  # a dry run reports, never raises past here
                checks.add('build_raised', False, f'{type(error).__name__}: {error}')
                report['accepted_would_be'] = False
            report['checks'] = checks.rows
            report['failed_checks'] = [c['check'] for c in checks.rows if not c['ok']]
        else:
            present_design = ROOT / PINNED['design']
            if present_design.exists():
                report['design_sha256_now'] = sha256_file(present_design)[0]
                report['design_sha256_is_frozen'] = report['design_sha256_now'] == FROZEN_DESIGN_SHA256
        print(json.dumps(report, indent=2))
        return 0 if report['writable'] and report.get('accepted_would_be') else 1

    if out_exists:
        print(json.dumps({'refused': True, 'reason': 'immutable receipt already exists', 'out': posix(out)}))
        return 1
    if missing:
        print(json.dumps({'refused': True, 'reason': 'missing inputs', 'missing_inputs': missing}, indent=2))
        return 1
    checks = Checks()
    receipt = build(options, checks)
    text = json.dumps(receipt, indent=2, allow_nan=False) + '\n'
    with out.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    print(json.dumps({'receipt': posix(out), 'sha256': hashlib.sha256(out.read_bytes()).hexdigest(),
                      'accepted': receipt['accepted'],
                      'failed_checks': [c['check'] for c in checks.rows if not c['ok']],
                      'rebalances': receipt['summary']['rebalances'],
                      'engine_tests': receipt['native_tests']['engine_final'],
                      'impl_tests': receipt['native_tests']['impl_final'],
                      'oracle': receipt['oracle_comparison']['final_counts'],
                      'ledger_lines': receipt['trial_ledger']['lines']}, indent=2))
    return 0 if receipt['accepted'] else 2


def selftest() -> int:
    failures = []

    def ok(condition, label):
        if not condition:
            failures.append(label)

    ok(fnv1a64(b'') == 14695981039346656037, 'fnv offset')
    ok(fnv1a64(b'a') == 0xaf63dc4c8601ec8c, 'fnv a')
    ok(median_rule([3, 1, 2]) == 2 and median_rule([1, 2, 3, 4]) == 2.5 and median_rule([]) is None, 'median')
    ok(parse_ctest_summary('x\n100% tests passed, 0 tests failed out of 37\n') == {'percent': 100, 'failed': 0, 'total': 37, 'passed': 37}, 'ctest parse')
    ok(parse_ctest_summary('nothing') is None, 'ctest none')
    ok(nanos_to_date(1356912000000000000) == '2012-12-31' and nanos_to_date(1575244800000000000) == '2019-12-02', 'civil')
    ok(cut_key('1000', '0.10') == 't1000_b1000' and cut_key(3000, '0.00') == 't3000_b0', 'cut key')
    ok(digests_in({'files': [{'filename': 'a.csv', 'sha256': 'AB'}], 'm': {'path': 'd/b.bin', 'sha256': 'cd'}}) == {'a.csv': 'ab', 'b.bin': 'cd'}, 'digests_in')
    ok(find_ints({'a': {'rebalances': 84, 'x': [{'observations': 84}]}, 'b': True}, ('rebalances', 'observations')) == [84, 84], 'find_ints')
    ok(hex64_values({'k': 'A' * 64, 'l': ['b' * 64, 'zz']}) == {'a' * 64, 'b' * 64}, 'hex64')
    # membership.bin round trip: 1 rebalance, 1x1 cuts, 2 members
    body = BIN_MAGIC + struct.pack('<III', 1, 63, 57) + struct.pack('<d', 1.0)
    body += struct.pack('<II', 1, 1000) + struct.pack('<II', 1, 0) + struct.pack('<I', 1)
    body += struct.pack('<qq', 1356912000000000000, 1357084800000000000)
    body += struct.pack('<I', 2) + struct.pack('<qq', 5, 9) + struct.pack('<II', 2, 1)
    blob = body + struct.pack('<Q', fnv1a64(body))
    image = decode_membership_bin(blob)
    ok(image['trailer_matches'] and image['rebalance_count'] == 1 and image['top_n'] == [1000]
       and image['rebalances'][0]['member_counts'] == [2], 'bin decode')
    bad = body + struct.pack('<Q', fnv1a64(body) ^ 1)
    ok(decode_membership_bin(bad)['trailer_matches'] is False, 'bin trailer mismatch detected')
    try:
        decode_membership_bin(blob[:-9] + blob[-8:])
        ok(False, 'bin short body must raise')
    except ValueError:
        pass
    # ledger chain
    first = json.dumps({'prev_sha256': GENESIS, 'checkpoint': 15, 'status': 'pre-registered'}).encode() + b'\n'
    head = hashlib.sha256(first).hexdigest()
    second = json.dumps({'prev_sha256': head, 'checkpoint': 15, 'status': 'completed'}).encode() + b'\n'
    head2 = hashlib.sha256(second).hexdigest()
    walked = walk_ledger(first + second, json.dumps({'lines': 2, 'head_sha256': head2}))
    ok(walked['anchored'] and walked['head_sha256'] == head2 and walked['lines'][0]['line_sha256'] == head, 'ledger walk')
    try:
        walk_ledger(first + second, json.dumps({'lines': 2, 'head_sha256': head}))
        ok(False, 'sidecar mismatch must raise')
    except ValueError:
        pass
    try:
        walk_ledger(first + first)
        ok(False, 'broken chain must raise')
    except ValueError:
        pass
    ok(section_lines('a\n## 6. Pre-registration block\nx\n## 7. y\nz', '## 6. Pre-registration block', '## 7.') == ['## 6. Pre-registration block', 'x'], 'section extraction')
    with tempfile.TemporaryDirectory() as room:
        churn = Path(room) / 'churn.csv'
        churn.write_text('rebalance_rank_date,effective_date,top_n,band,adds,drops_rank,drops_last_bar,kept,members,one_way_turnover\n'
                         '2012-12-31,2013-01-02,1000,0.00,1000,0,0,0,1000,0.5\n'
                         '2013-01-31,2013-02-01,1000,0.00,40,30,10,960,1000,0.04\n', encoding='utf-8')
        s = summarize_churn(churn)
        c = s['cuts']['t1000_b0']
        ok(s['distinct_rank_dates'] == 2 and c['rebalances'] == 2 and c['adds_total'] == 1040
           and c['drops_total'] == 40 and c['one_way_turnover_mean_excluding_first_rebalance'] == 0.04
           and abs(c['one_way_turnover_annualised_x12_excluding_first'] - 0.48) < 1e-12
           and c['members_median'] == 1000 and c['last_effective_date'] == '2013-02-01', 'churn summary')
        junit = Path(room) / 'j.xml'
        junit.write_text('<?xml version="1.0"?><testsuite name="x" tests="2" failures="1"><testcase name="DataPointInTimeUniverse.A"/><testcase name="B"/></testsuite>', encoding='utf-8')
        ok(parse_junit(junit) == {'tests': 2, 'failures': 1, 'testcases': 2, 'point_in_time_cases': 1}, 'junit parse')
    print(json.dumps({'selftest': 'pass' if not failures else 'fail', 'failures': failures}))
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
