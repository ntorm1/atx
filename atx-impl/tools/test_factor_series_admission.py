"""P9 B1 (contract K-P9-4, DEC-7): the C++ `factors` verb and research/admission against the fitter.

The gtests FactorsVerb.EqualsFixture (atx-impl/tests/strategy_factors_verb_test.cpp) and
ResearchAdmission.EqualsFitterFixture / .CsvEqualsFitterBytes (atx-engine/tests/research/
research_admission_fixture_test.cpp) compare the C++ with two committed fixtures. This module generates both
from fit_composition_weights.py used as a library -- PricePanel / Context.build / factor_record for the factor
series, screen_v4 and admission_csv for the screen; the fitter is not edited -- and requires the committed files
to hold exactly what the fitter computes today, so the C++ goldens cannot drift from the fitter. ATX_B1_REGEN=1
rewrites them.

* factors fixture: the synthetic role of strategy_factors_verb_test.cpp factor_fixture(), built operation for
  operation (the same LCG), its unsigned factor series f, tau and live counts from factor_record, and the
  report-only 21-session series h (F-3) from a numpy reference over the fitter's own book q and forward returns
  (the fitter has no h; this pins the C++ rule's arithmetic, not a fitter value).
* screen fixture: synthetic factor rows (integer multiples of 2^-24, exact in both languages) that reach every
  status, a multi-failure row, tier-before-roster order, low overlap, undefined rho, the n <= lag HAC branch
  and both screens (v4-prior-v1 / -v2); the fitter's rows and admission.csv bytes; and Python repr cases for the
  C++ float spelling.

When ATX_EQUITY_TARGETS_EXE and ATX_RESEARCH_ADMISSION_EXE name built executables, the real verbs run on the
synthetic role: factor.f64 / tau.f64 equal the fitter's records to 1e-12, and the C++ admission.csv equals the
fitter's screen of the same series (decision cells byte for byte, float cells to 1e-12; Ruling P12).
compare_admission_csv() is the comparison root uses on X-5 / Y-S (see the B1 report).
"""

import hashlib
import json
import math
import os
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fit_composition_weights as fcw  # noqa: E402

REPO = HERE.parents[1]
FACTOR_FIXTURE = REPO / "atx-impl" / "tests" / "fixtures" / "factors_verb_v1.json"
SCREEN_FIXTURE = REPO / "atx-engine" / "tests" / "fixtures" / "research_admission" / "screen_v4_v1.json"
GENERATOR = "atx-impl/tools/test_factor_series_admission.py"
REGEN = os.environ.get("ATX_B1_REGEN") == "1"
MASK = (1 << 64) - 1
DAY_NS = 86_400_000_000_000

# strategy_factors_verb_test.cpp factor_fixture(): keep the two in step.
N, ROLE_DATES, SCORE_BEGIN, MEMBER_FROM = 60, 420, 383, 63
REFUSED_SESSION, DOUBLED_FROM, FIRST_DAY = 400, 395, 17879  # first session 2018-12-14: decisions in TRAIN
HORIZON = 21
SIGNALS = ("s1", "s2", "s3")


class Lcg:
    def __init__(self, seed):
        self.state = seed

    def next(self):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) & MASK
        return float(self.state >> 11) * 2.0 ** -53


def factor_panel():
    """strategy_factors_verb_test.cpp factor_fixture(), operation for operation."""
    close = np.zeros((ROLE_DATES, N))
    rng = Lcg(2027)
    for i in range(N):
        close[0, i] = 20.0 + i
    for t in range(1, ROLE_DATES):
        common = 0.03 * (rng.next() - 0.5)
        for i in range(N):
            loading = 0.5 + 0.02 * i
            idio = 0.005 + 0.0002 * ((i * 7) % N)
            close[t, i] = close[t - 1, i] * (1 + loading * common + idio * (rng.next() - 0.5))
    raw = close.copy()
    for t in range(DOUBLED_FROM, ROLE_DATES):
        close[t, 5] = close[t, 5] * 2.0
    volume = np.zeros((ROLE_DATES, N))
    for t in range(ROLE_DATES):
        for i in range(N):
            volume[t, i] = 1e4 * (1 + (i * 11) % N) * (0.5 + rng.next())
    present = np.ones((ROLE_DATES, N), dtype=np.uint8)
    member = np.zeros((ROLE_DATES, N), dtype=np.uint8)
    for t in range(ROLE_DATES):
        for i in range(N):
            if (t * 7 + i * 3) % 97 == 0 or (t == REFUSED_SESSION and i < 15):
                present[t, i] = 0
            member[t, i] = 1 if present[t, i] and (t + 2 * i) % 19 != 0 and t >= MEMBER_FROM else 0
    for a in (close, raw, volume):
        a[present == 0] = np.nan
    s1, s2, s3 = (np.zeros((ROLE_DATES, N)) for _ in range(3))
    g = Lcg(78)
    for t in range(ROLE_DATES):
        for i in range(N):
            s1[t, i] = g.next() - 0.5
            if (t * 3 + i) % 23 == 0:
                s1[t, i] = np.nan
            s2[t, i] = (0.5 + 0.02 * i) + 0.2 * (g.next() - 0.5)
            s3[t, i] = float(math.floor(4.0 * g.next()))
            if t % 9 == 0:
                s3[t, i] = np.nan
    return {"close": close, "raw_close": raw, "volume": volume, "present": present, "member": member,
            "s1": s1, "s2": s2, "s3": s3}


def fitter_context(panel):
    payload = {k: panel[k] for k in ("close", "raw_close", "volume", "present", "member")}
    role = SimpleNamespace(payload=lambda: payload, begin=SCORE_BEGIN, end=ROLE_DATES - 2, instruments=N,
                           sha="f" * 64)
    return fcw.Context.build(role)


def horizon_series(ctx, signal):
    """Reference of the C++ h_k(d): sum_i q_i(d) sum_{j<21} fwd_i(d+j) on the fitter's q and forward."""
    q, live = ctx.book(signal, 1)
    t = q.shape[0]
    out = [None] * t
    for j in range(t):
        if live[j] and j + HORIZON <= t:
            held = ctx.forward[j:j + HORIZON].sum(axis=0)
            out[j] = float((q[j] * held).sum())
    return out


def factor_fixture() -> dict:
    panel = factor_panel()
    ctx = fitter_context(panel)
    records = {name: fcw.factor_record(ctx, panel[name]) for name in SIGNALS}
    return {"schema": "atx.factors-verb-fixture/v1",
            "generator": GENERATOR + " (fit_composition_weights.py Context.build + factor_record)",
            "geometry": {"names": N, "role_dates": ROLE_DATES, "score_begin": SCORE_BEGIN,
                         "decision_begin": SCORE_BEGIN, "decision_end_exclusive": ROLE_DATES - 2,
                         "member_from": MEMBER_FROM, "refused_session": REFUSED_SESSION,
                         "doubled_from": DOUBLED_FROM, "first_day": FIRST_DAY},
            "candidates": list(SIGNALS),
            "used_rows": [int(x) for x in ctx.used_rows],
            "refused": ctx.refused,
            "factor": {name: records[name]["f_unsigned"] for name in SIGNALS},
            "tau": {name: records[name]["tau"] for name in SIGNALS},
            "live_decisions": {name: records[name]["live_decisions"] for name in SIGNALS},
            "factor_h21": {name: horizon_series(ctx, panel[name]) for name in SIGNALS},
            "factor_h21_reference": "numpy over the fitter's Context.book q and Context.forward: "
                                    "sum_i q_i(d) * forward[d:d+21].sum(axis=0)_i (the fitter has no h)"}


# ------------------------------------------------------------------ screen fixture
SCREEN_DECISIONS, TRAIN_FROM, SCALE_EXPONENT = 330, 10, -24
NOISE = 16000
FIRST_SESSION_NS = fcw.rw.TRAIN_BEGIN_NS - TRAIN_FROM * DAY_NS


def screen_candidates():
    """(id, family, theme, tier, prior_sign, runner_sign, tau, cache_entry): roster order."""
    return [("val_a", "value", "value", "A", 1, 1, 0.05, "base"),
            ("mom_b", "momentum", "price_momentum", "B", 1, 1, 0.06, "base"),
            ("noprior", "quality", "profitability_quality", "B", 0, 0, 0.05, "fields"),
            ("short", "issuance", "investment_issuance", "C", 1, 1, 0.04, "base"),
            ("fast", "reversal", "reversal_seasonality", "B", 1, -1, 0.75, "base"),
            ("veto", "earnings", "earnings_momentum", "B-", 1, 1, 0.05, "fields"),
            ("cost", "short_interest", "short_interest", "C+", 1, 1, 0.3, "fields"),
            ("left", "low_risk", "low_risk", "C", 1, 0, 0.08, "base"),
            ("right", "options", "options_implied", "C", 1, 1, 0.05, "base"),
            ("const", "flow", "ownership_flow", "C-", 1, 1, 0.02, "fields"),
            ("same_tier", "value", "value", "A", 1, 1, 0.05, "base"),
            ("b_early", "events", "filing_events", "B+", 1, 1, 0.05, "fields"),
            ("aplus_late", "events", "filing_events", "A+", 1, 1, 0.05, "fields"),
            ("tiny", "pv", "price_volume", "D", 1, 1, 0.05, "base"),
            ("single", "pv", "price_volume", "D", 1, 1, 0.05, "base"),
            ("empty", "merger", "merger_arbitrage", "D", 0, 0, 0.05, "base"),
            ("pre_train", "value", "value", "C", 1, 1, 0.05, "base"),
            ("moderate", "momentum", "price_momentum", "C", 1, 1, 0.7, "base")]


def screen_units():
    """decisions x candidates integer units (None = flat): value = units * 2^-24 exactly."""
    t, cands = SCREEN_DECISIONS, screen_candidates()
    rng = Lcg(4242)

    def noise():
        return [int((rng.next() - 0.5) * 2 * NOISE) for _ in range(t)]

    n = {k: noise() for k in range(len(cands))}
    base0 = [3000 + x for x in n[0]]
    base11 = [2500 + x for x in n[11]]
    cols = {
        0: base0,
        1: [b + int(x / 20) for b, x in zip(base0, n[1])],
        2: [500 + x if 10 <= j < 100 else None for j, x in enumerate(n[2])],
        3: [800 + x if 10 <= j < 200 else None for j, x in enumerate(n[3])],
        4: [-1500 + x for x in n[4]],
        5: [-2000 + x for x in n[5]],
        6: [1000 + x for x in n[6]],
        7: [600 + x if 10 <= j < 270 else None for j, x in enumerate(n[7])],
        8: [700 + x if 70 <= j < 330 else None for j, x in enumerate(n[8])],
        9: [2048] * t,  # 2^-13: a constant whose sums and mean are exact
        10: [b + int(x / 5) for b, x in zip(base0, n[10])],
        11: base11,
        12: [b + int(x / 20) for b, x in zip(base11, n[12])],
        13: [{10: -16000, 11: -20000, 12: -18000, 13: -17000}.get(j) for j in range(t)],
        14: [1234 if j == 50 else None for j in range(t)],
        15: [None] * t,
        16: [400 + x if j < 10 else None for j, x in enumerate(n[16])],
        17: [b + int(0.6 * x) for b, x in zip(base0, n[17])],
    }
    return [[cols[k][j] for k in range(len(cands))] for j in range(t)]


def screen_inputs():
    cands = screen_candidates()
    units = screen_units()
    factors = np.array([[np.nan if units[j][k] is None else math.ldexp(units[j][k], SCALE_EXPONENT)
                         for j in range(SCREEN_DECISIONS)] for k in range(len(cands))], dtype=np.float64)
    sessions = np.array([FIRST_SESSION_NS + j * DAY_NS for j in range(SCREEN_DECISIONS)], dtype=np.int64)
    train_mask = (sessions >= fcw.FIT_BEGIN_NS) & (sessions < fcw.TRAIN_END_NS)
    meta = [{"id": c[0], "family": c[1], "theme": c[2], "tier": c[3], "prior_sign": c[4], "runner_sign": c[5],
             "tau": c[6], "cache_entry": c[7], "cache_payload_sha256": hashlib.sha256(c[0].encode()).hexdigest()}
            for c in cands]
    return units, factors, sessions, train_mask, meta


def table_rows(meta, rows):
    """The candidates of fit_prior's admission table (fit:2249-2259), from screen_v4's rows."""
    out = []
    for c, row in zip(meta, rows):
        out.append({"id": c["id"], "family": c["family"], "theme": c["theme"], "tier": c["tier"],
                    "prior_sign": c["prior_sign"], "status": row["status"], "failed_checks": row["failed_checks"],
                    "redundant_with": row["redundant_with"], "redundant_rho": row["redundant_rho"],
                    "undefined_rho_with": row["undefined_rho_with"], "cache_entry": c["cache_entry"],
                    "admission_rank": row["admission_rank"], "s_k": row["s_k"], "runner_sign": c["runner_sign"],
                    "sign_agrees": c["runner_sign"] == row["s_k"], "tau": row["tau"],
                    "train_days": row["train_days"], "train_mean": row["train_mean"],
                    "train_sharpe": row["train_sharpe"], "hac_t": row["hac_t"], "max_abs_rho": row["max_abs_rho"],
                    "max_abs_rho_with": row["max_abs_rho_with"], "low_overlap_with": row["low_overlap_with"],
                    "cache_payload_sha256": c["cache_payload_sha256"]})
    return out


def fitter_screen(factors, train_mask, meta, screen):
    tier_rank = [fcw.TIER_GRADES.index(c["tier"]) for c in meta]
    rows = fcw.screen_v4(factors, [c["tau"] for c in meta], [c["id"] for c in meta], train_mask, tier_rank,
                         [c["prior_sign"] for c in meta],
                         cost_tau_limit=fcw.V42_COST_TAU_LIMIT if screen == fcw.PRIOR_SCREEN_V2_ID else None)
    csv = fcw.admission_csv(table_rows(meta, rows), fcw.V4_CSV_COLUMNS).decode("utf-8")
    return rows, csv


REPR_CASES = (0.0, -0.0, 1.0, -2.5, 0.1, 1 / 3, 2 / 3, 0.30000000000000004, 1e-05, 1.5e-05, 0.0001, 0.00012345,
              -0.00099, 123.0, 1234.5, 1e15, 1e16, 1.5e16, 9999999999999998.0, 12345678901234567.0, 1e22, 1e100,
              2.5e-300, 5e-324, 1.7976931348623157e308, 2 ** -13, 6.103515625e-05, 3.0517578125e-05, 252.0 ** 0.5,
              -3.141592653589793, 0.70, 0.08, 0.9988, 4.000000000000001)


def screen_fixture() -> dict:
    units, factors, sessions, train_mask, meta = screen_inputs()
    expected = {}
    for screen in (fcw.PRIOR_SCREEN_ID, fcw.PRIOR_SCREEN_V2_ID):
        rows, csv = fitter_screen(factors, train_mask, meta, screen)
        expected[screen] = {"rows": rows, "csv": csv}
    return {"schema": "atx.research-admission-fixture/v1",
            "generator": GENERATOR + " (fit_composition_weights.py screen_v4 + admission_csv)",
            "scale_exponent": SCALE_EXPONENT, "decisions": SCREEN_DECISIONS,
            "decision_sessions_ns": [int(s) for s in sessions], "train_mask": [int(m) for m in train_mask],
            "candidates": meta, "factor_units": units, "expected": expected,
            "repr_cases": [[x, repr(x)] for x in REPR_CASES]}


# ------------------------------------------------------------------ fixture identity
def _same(want, got, where="$"):
    """Equal JSON values, floats bit for bit (a float never equals an int)."""
    assert type(want) is type(got), f"{where}: {want!r} vs {got!r}"
    if isinstance(want, float):
        assert want == got and math.copysign(1, want) == math.copysign(1, got), f"{where}: {want!r} vs {got!r}"
    elif isinstance(want, dict):
        assert sorted(want) == sorted(got), f"{where}: keys"
        for key in want:
            _same(want[key], got[key], f"{where}.{key}")
    elif isinstance(want, list):
        assert len(want) == len(got), f"{where}: length"
        for i, (a, b) in enumerate(zip(want, got)):
            _same(a, b, f"{where}[{i}]")
    else:
        assert want == got, f"{where}: {want!r} vs {got!r}"


def _check_or_regen(path: Path, document: dict):
    text = json.dumps(document, indent=1, sort_keys=True, allow_nan=False) + "\n"
    if REGEN:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
    committed = json.loads(path.read_text(encoding="utf-8"))  # parsed: a CRLF checkout changes no value
    _same(json.loads(text), committed)


def test_factor_fixture_is_the_fitters_output():
    _check_or_regen(FACTOR_FIXTURE, factor_fixture())


def test_screen_fixture_is_the_fitters_output():
    _check_or_regen(SCREEN_FIXTURE, screen_fixture())


def test_factor_fixture_covers_flat_refused_and_horizon():
    doc = factor_fixture()
    assert doc["refused"] == [{"decision_index": REFUSED_SESSION, "reason": "too-few-usable-names",
                               "used_rows": doc["refused"][0]["used_rows"]}]
    assert doc["used_rows"][REFUSED_SESSION - SCORE_BEGIN] == 0
    decisions = ROLE_DATES - 2 - SCORE_BEGIN
    for name in SIGNALS:
        f, h = doc["factor"][name], doc["factor_h21"][name]
        assert len(f) == len(h) == decisions
        assert f[REFUSED_SESSION - SCORE_BEGIN] is None  # refused: flat
        assert all(x is None for x in h[decisions - HORIZON + 1:])  # past the 21-session window
        assert sum(x is not None for x in h) > 0
    # s3: whole rows NaN every 9th session (flat decisions) and tied ranks elsewhere.
    flat = [j for j in range(decisions) if (SCORE_BEGIN + j) % 9 == 0]
    assert flat and all(doc["factor"]["s3"][j] is None for j in flat)
    assert doc["live_decisions"]["s3"] == decisions - len(flat) - 1


def test_screen_fixture_covers_every_branch():
    doc = screen_fixture()
    ids = [c["id"] for c in doc["candidates"]]
    v1 = dict(zip(ids, doc["expected"][fcw.PRIOR_SCREEN_ID]["rows"]))
    v2 = dict(zip(ids, doc["expected"][fcw.PRIOR_SCREEN_V2_ID]["rows"]))
    assert {r["status"] for r in v1.values()} == set(fcw.V4_STATUSES)
    assert {r["status"] for r in v2.values()} == set(fcw.V42_STATUSES)
    assert v1["fast"]["failed_checks"] == ["turnover", "veto"] and v1["fast"]["status"] == "reject_turnover"
    assert v2["fast"]["failed_checks"] == ["turnover", "turnover_cost", "veto"]
    assert v1["noprior"]["failed_checks"] == ["no_prior", "insufficient"]
    assert v1["tiny"]["failed_checks"] == ["insufficient", "veto"] and v1["tiny"]["train_days"] == 4
    assert v1["single"]["hac_t"] is None and v1["single"]["train_sharpe"] is None
    assert v1["empty"]["train_mean"] is None and v1["pre_train"]["train_days"] == 0
    # (tier, roster) order: the A+ string late in the roster is admitted first; the earlier B+ is redundant.
    assert v1["aplus_late"]["admission_rank"] == 1 and v1["b_early"]["redundant_with"] == "aplus_late"
    assert v1["same_tier"]["redundant_with"] == "val_a" and v1["mom_b"]["redundant_with"] == "val_a"
    assert v1["right"]["low_overlap_with"] == ["left"]
    assert v1["const"]["hac_t"] is None and v1["const"]["train_sharpe"] is None
    assert v1["const"]["undefined_rho_with"]
    assert v1["moderate"]["status"] == "admitted" and v2["moderate"]["status"] == "reject_turnover_cost"
    assert v2["left"]["status"] == "admitted"  # tau .08 is not above the .08 cost limit (strict >)
    assert v1["moderate"]["max_abs_rho_with"] == "val_a" and 0.8 < v1["moderate"]["max_abs_rho"] < 0.9


def test_hac_short_series_keeps_the_declared_lag():
    """The n <= lag branch the C++ FitterNeweyWestV1 reproduces: weights 1 - l/6 for l < n, not 1 - l/n."""
    x = np.array([-1.0, -1.2, -0.9, -1.1])
    e = x - x.mean()
    lrv = float(e @ e) / 4 + sum(2.0 * (1.0 - ell / 6.0) * float(e[ell:] @ e[:-ell]) / 4 for ell in (1, 2, 3))
    assert fcw.newey_west_t(x) == pytest.approx(float(x.mean()) / math.sqrt(lrv / 4), rel=1e-15)


# ------------------------------------------------------------------ helpers root uses on real roles
def signals_document(role_sha: str, entries: list[dict]) -> dict:
    """The `factors` verb's DIR/signals.json from resolved cache entries in roster order -- each the fitter's
    CacheLayout.resolve(candidate, role) dict (or a runner summary's candidate_cache.entries[] row): only id,
    payload and payload_sha256 are kept."""
    return {"schema": "atx.factor-signals/v1", "role_manifest_sha256": role_sha,
            "candidates": [{"id": e["id"], "payload": str(e["payload"]), "payload_sha256": e["payload_sha256"]}
                           for e in entries]}


def compare_factor_series(directory: Path, records: list[dict], tolerance: float = 1e-12) -> list[str]:
    """Differences of a K-P9-4 directory against the fitter's factor records in the same candidate order
    (factor_record dicts: f_unsigned with None on flat decisions, tau): the flat decisions exactly, f and tau to
    `tolerance` (K-P9-4: 0 where reduction order allows, else 1e-12)."""
    manifest = json.loads((Path(directory) / "manifest.json").read_text(encoding="utf-8"))
    t, k = manifest["decisions"], len(manifest["candidates"])
    factor = np.frombuffer((Path(directory) / "factor.f64").read_bytes(), dtype="<f8").reshape(t, k)
    taus = np.frombuffer((Path(directory) / "tau.f64").read_bytes(), dtype="<f8")
    out = [] if len(records) == k else [f"{k} candidates vs {len(records)} records"]
    for c, record in enumerate(records[:k]):
        f = np.array([np.nan if v is None else v for v in record["f_unsigned"]], dtype=np.float64)
        if f.shape != (t,) or not np.array_equal(np.isnan(f), np.isnan(factor[:, c])):
            out.append(f"{manifest['candidates'][c]['id']}: live decisions differ")
            continue
        worst = float(np.nanmax(np.abs(f - factor[:, c]))) if np.isfinite(f).any() else 0.0
        if worst > tolerance:
            out.append(f"{manifest['candidates'][c]['id']}: max |f - f_cpp| {worst!r}")
        if abs(record["tau"] - float(taus[c])) > tolerance:
            out.append(f"{manifest['candidates'][c]['id']}: tau {record['tau']!r} vs {float(taus[c])!r}")
    return out


ADMISSION_FLOAT_COLUMNS = ("redundant_rho", "tau", "train_mean", "train_sharpe", "hac_t", "max_abs_rho")


def compare_admission_csv(reference: bytes, candidate: bytes, tolerance: float = 1e-12) -> list[str]:
    """Differences of two admission.csv files under Ruling P12: every non-float cell byte for byte (ids, statuses,
    failed checks, order, named candidates, counts), the float cells to tolerance * max(1, |reference|)."""
    a = reference.decode("utf-8").splitlines()
    b = candidate.decode("utf-8").splitlines()
    out = []
    if len(a) != len(b) or a[:1] != b[:1]:
        return [f"shape or header differs: {len(a)} vs {len(b)} lines"]
    header = a[0].split(",")
    for n, (ra, rb) in enumerate(zip(a[1:], b[1:]), start=1):
        ca, cb = ra.split(","), rb.split(",")
        for col, x, y in zip(header, ca, cb):
            if col in ADMISSION_FLOAT_COLUMNS and x and y:
                if abs(float(x) - float(y)) > tolerance * max(1.0, abs(float(x))):
                    out.append(f"line {n} {col}: {x} vs {y}")
            elif x != y:
                out.append(f"line {n} {col}: {x!r} vs {y!r}")
    return out


def test_compare_factor_series_on_a_written_directory(tmp_path):
    """The helper on a K-P9-4 directory written from the factor fixture: equal, then a moved flat day and a
    moved value are reported."""
    doc = factor_fixture()
    decisions = len(doc["factor"]["s1"])
    matrix = np.array([[np.nan if doc["factor"][n][j] is None else doc["factor"][n][j] for n in SIGNALS]
                       for j in range(decisions)], dtype="<f8")
    (tmp_path / "factor.f64").write_bytes(matrix.tobytes())
    (tmp_path / "tau.f64").write_bytes(np.array([doc["tau"][n] for n in SIGNALS], dtype="<f8").tobytes())
    (tmp_path / "manifest.json").write_text(json.dumps({"decisions": decisions,
                                                        "candidates": [{"id": n} for n in SIGNALS]}))
    records = [{"f_unsigned": doc["factor"][n], "tau": doc["tau"][n]} for n in SIGNALS]
    assert compare_factor_series(tmp_path, records) == []
    moved = [dict(r) for r in records]
    moved[0]["f_unsigned"] = [None] + moved[0]["f_unsigned"][1:]
    moved[1]["f_unsigned"] = [moved[1]["f_unsigned"][0] + 1e-9] + moved[1]["f_unsigned"][1:]
    assert len(compare_factor_series(tmp_path, moved)) == 2
    assert signals_document("a" * 64, [{"id": "s1", "payload": Path("x/s1.f64"), "payload_sha256": "b" * 64,
                                        "layout": "v2"}])["candidates"] == [
        {"id": "s1", "payload": str(Path("x/s1.f64")), "payload_sha256": "b" * 64}]


def test_compare_admission_csv_separates_decisions_from_rounding():
    _, factors, _, train_mask, meta = screen_inputs()
    _, csv = fitter_screen(factors, train_mask, meta, fcw.PRIOR_SCREEN_ID)
    lines = csv.splitlines()
    header = lines[0].split(",")
    cells = lines[1].split(",")
    t = header.index("hac_t")
    nudged = list(cells)
    nudged[t] = repr(float(cells[t]) * (1 + 1e-15))
    assert compare_admission_csv(csv.encode(), "\n".join([lines[0], ",".join(nudged)] + lines[2:]).encode() +
                                 b"\n") == []
    moved = list(cells)
    moved[header.index("status")] = "reject_veto"
    assert compare_admission_csv(csv.encode(), "\n".join([lines[0], ",".join(moved)] + lines[2:]).encode() +
                                 b"\n")


# ------------------------------------------------------------------ the built executables (optional)
def _write_role(root: Path, panel) -> tuple[Path, str]:
    """strategy_factors_verb_test.cpp write_role(): an engine-conformant research role."""
    root.mkdir()
    sessions = np.array([(FIRST_DAY + t) * DAY_NS for t in range(ROLE_DATES)], dtype="<i8")
    ids = np.array([1000 + 7 * i for i in range(N)], dtype="<u8")
    files = {}
    for name, data in (("sessions.i64", sessions), ("ids.u64", ids),
                       ("close.f64", panel["close"].astype("<f8")), ("raw_close.f64", panel["raw_close"].astype("<f8")),
                       ("volume.f64", panel["volume"].astype("<f8")), ("present.u8", panel["present"].astype("u1")),
                       ("member.u8", panel["member"].astype("u1"))):
        raw = np.ascontiguousarray(data).tobytes()
        (root / name).write_bytes(raw)
        files[name] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    membership = {"rule": "research-prior63-usd-adv-topn-v1", "top_n": N, "lookback_sessions": 63,
                  "lag_sessions": 1, "min_raw_price_exclusive": 5, "min_adv_exclusive": 5000000,
                  "ties": "securityID-ascending", "missing": "complete-prior-calendar-window-required",
                  "common_stock_verified": False}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete",
                "instrument_namespace": "spiderrock.securityID", "dates": ROLE_DATES, "instruments": N,
                "score_begin": SCORE_BEGIN, "score_end": ROLE_DATES,
                "score_start_ns": int(sessions[SCORE_BEGIN]), "score_end_ns": int(sessions[-1]) + DAY_NS,
                "source_sha256": "a" * 64, "membership_recipe": json.dumps(membership, separators=(",", ":")),
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1",
                "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor", "volume_basis": "raw-share-volume",
                "common_stock_verified": False, "historical_vintage_verified": False,
                "declared_output_bytes": ROLE_DATES * N * 26 + ROLE_DATES * 8 + N * 8, "files": files}
    text = (json.dumps(manifest, indent=2) + "\n").encode()
    (root / "manifest.json").write_bytes(text)
    return root / "manifest.json", hashlib.sha256(text).hexdigest()


@pytest.mark.skipif(not (os.environ.get("ATX_EQUITY_TARGETS_EXE") and os.environ.get("ATX_RESEARCH_ADMISSION_EXE")),
                    reason="needs built atx-equity-strategy-targets and atx-research-admission")
def test_executables_reproduce_the_fitter(tmp_path):
    panel = factor_panel()
    manifest, sha = _write_role(tmp_path / "role", panel)
    signals = tmp_path / "signals"
    signals.mkdir()
    entries = []
    for name in SIGNALS:
        raw = np.ascontiguousarray(panel[name].astype("<f8")).tobytes()
        (signals / f"{name}.f64").write_bytes(raw)
        entries.append({"id": name, "payload": f"{name}.f64", "payload_sha256": hashlib.sha256(raw).hexdigest()})
    (signals / "signals.json").write_text(json.dumps(signals_document(sha, entries)))
    out = tmp_path / "factors"
    subprocess.run([os.environ["ATX_EQUITY_TARGETS_EXE"], "factors", "--role", str(manifest), "--role-sha256", sha,
                    "--signals", str(signals), "--output", str(out)], check=True)
    ctx = fitter_context(panel)
    records = [fcw.factor_record(ctx, panel[name]) for name in SIGNALS]
    assert compare_factor_series(out, records) == []
    # The screen of the same series: the fitter's on its records vs the C++ on the C++ series.
    decisions = ROLE_DATES - 2 - SCORE_BEGIN
    sessions = np.array([(FIRST_DAY + SCORE_BEGIN + j) * DAY_NS for j in range(decisions)], dtype=np.int64)
    train_mask = (sessions >= fcw.FIT_BEGIN_NS) & (sessions < fcw.TRAIN_END_NS)
    meta = [{"id": name, "family": "f", "theme": "value", "tier": "B", "prior_sign": 1, "runner_sign": 1,
             "tau": float(r["tau"]), "cache_entry": "base", "cache_payload_sha256": e["payload_sha256"]}
            for name, r, e in zip(SIGNALS, records, entries)]
    fitter_rows = np.array([[np.nan if v is None else v for v in r["f_unsigned"]] for r in records])
    _, csv = fitter_screen(fitter_rows, train_mask, meta, fcw.PRIOR_SCREEN_ID)
    candidates = tmp_path / "candidates.json"
    candidates.write_text(json.dumps({"candidates": meta}))
    screened = tmp_path / "screen"
    subprocess.run([os.environ["ATX_RESEARCH_ADMISSION_EXE"], "screen", "--factors", str(out), "--candidates",
                    str(candidates), "--output", str(screened)], check=True)
    assert compare_admission_csv(csv.encode(), (screened / "admission.csv").read_bytes()) == []
