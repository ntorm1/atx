"""fit_composition_weights.py --era / --era-id (platform v8 H-1): the pooled prior screen over era roles.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_weights_pool.py

Synthetic data only (test_fit_composition_weights.Fixture: role dir, library, orientations, candidate cache and TRAIN
summary per era). Two history eras (2014-2015) of 178 scored decisions each: alone, every member is insufficient (and
the single path's TRAIN mask drops every decision: finding 3); pooled, the prior screen reads 356 decisions.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import time

import numpy as np
import pytest

import fit_composition_weights as fcw
from test_fit_composition_weights import DAY, NAN, Fixture, runner_accepts, store_base

NAMES, DATES, SCORE_BEGIN = 60, 330, 150
META = {"slow_a": ("value", "B", 1), "slow_a_twin": ("value", "B+", 1), "slow_b": ("price_momentum", "A", 1),
        "fast_a": ("low_risk", "A-", 1), "sparse": ("short_interest", "C+", 1), "unsigned": ("options_implied", "B", 0)}
IDS = list(META)
EXTRA = {i: {"theme": t, "tier": g, "prior_sign": s} for i, (t, g, s) in META.items()}
V4 = ["--orientation", "prior", "--composition", "ew-theme-v1"]


def era_world(seed: int, start: str):
    """A calendar-day panel with persistent planted alphas (screen_world's recipe), beginning at ``start``."""
    rng = np.random.default_rng(seed)

    def ar(phi=0.97):
        z = np.empty((DATES, NAMES))
        z[0] = rng.normal(size=NAMES)
        for t in range(1, DATES):
            z[t] = phi * z[t - 1] + math.sqrt(1 - phi * phi) * rng.normal(size=NAMES)
        return z
    z1, z2, u, v, w = ar(), ar(), ar(), ar(), ar()
    ret = rng.uniform(0.5, 1.5, NAMES)[None, :] * rng.normal(0.0003, 0.01, DATES)[:, None]
    ret = ret + rng.normal(0, 0.015, (DATES, NAMES))
    ret[1:] += 0.004 * (z1[:-1] + z2[:-1])
    close = 40.0 * np.exp(np.cumsum(np.log1p(ret), axis=0))
    present = np.ones((DATES, NAMES), dtype=np.uint8)
    member = present.copy()
    member[:63] = 0
    day0 = np.datetime64(start, "D").astype("datetime64[ns]").astype(np.int64)
    panel = {"close": close, "raw": close.copy(), "volume": rng.lognormal(12, 1, (DATES, NAMES)), "present": present,
             "member": member, "sessions": day0 + DAY * np.arange(DATES, dtype=np.int64),
             "ids": np.arange(101, 101 + NAMES, dtype=np.uint64), "score_begin": SCORE_BEGIN, "rng": rng}
    sparse = np.full((DATES, NAMES), NAN)
    sparse[SCORE_BEGIN:SCORE_BEGIN + 60] = z2[SCORE_BEGIN:SCORE_BEGIN + 60]
    signals = {"slow_a": z1 + 0.1 * u, "slow_a_twin": z1 + 0.15 * v, "slow_b": z2 + 0.1 * w,
               "fast_a": z1 + rng.normal(0, 3, (DATES, NAMES)), "sparse": sparse, "unsigned": z2 + 0.5 * u}
    live = member == 1
    return panel, [np.where(live, signals[i], NAN) for i in IDS]


def next_start(first: str) -> str:
    """The start of the next era's panel: its first scored decision is one day after the previous era's last."""
    first_day = np.datetime64(first, "D")
    last_decision = first_day + (DATES - 3)
    return str(last_decision + 1 - SCORE_BEGIN)


def fixture(root: Path, seed: int, start: str) -> Fixture:
    panel, signals = era_world(seed, start)
    return Fixture(root, panel, signals, [1] * len(IDS), ids=IDS, families=["fam"] * len(IDS),
                   candidate_extra=EXTRA, layout="v2")


def era_argv(eid: str, fx: Fixture) -> list[str]:
    return ["--era", eid, str(fx.manifest), fx.train_sha, str(fx.orientations), fx.orientations_sha, str(fx.summary),
            fx.summary_sha]


@pytest.fixture(scope="module")
def eras(tmp_path_factory):
    root = tmp_path_factory.mktemp("pool")
    e1 = fixture(root / "e1", 31, "2014-01-01")
    e2 = fixture(root / "e2", 32, next_start("2014-01-01"))
    return {"root": root, "e1": e1, "e2": e2}


def pooled_argv(t: dict, out: Path, screen: str = "v4-prior-v1", extra=()) -> list[str]:
    return t["e2"].argv(out, screen, [*V4, *era_argv("E1", t["e1"]), "--era-id", "E2", *extra])


def era_records(fx: Fixture) -> tuple[fcw.RoleManifest, list[dict]]:
    role = fcw.RoleManifest(fx.manifest, fx.train_sha)
    layout = fcw.CacheLayout(fx.summary, fx.summary_sha, role, fx.orientations_sha, "cd" * 32)
    library = fcw.load_library(fx.library, fx.library_sha)
    entries = [layout.resolve(c, role) for c in library]
    args = fx.args(Path("unused"), "v4-prior-v1")
    return role, fcw.ensure_records(args, role, library, entries, layout.vm_identity, time.perf_counter(), None)[0]


def factor_rows(records: list[dict], key: str = "f_unsigned") -> np.ndarray:
    return np.array([[np.nan if v is None else v for v in r[key]] for r in records], dtype=np.float64)


def test_pooled_admission_is_screen_v4_on_the_concatenated_era_factors(eras):
    t, out = eras, eras["root"] / "W"
    code, summary = fcw.fit(fcw.parse_args(pooled_argv(t, out)))
    assert code == fcw.EXIT_OK and summary["pool"] == {"anchor": "E2", "eras": ["E1", "E2"], "decisions": 356}
    adm = json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes())
    (r1, rec1), (r2, rec2) = era_records(t["e1"]), era_records(t["e2"])
    factors = np.hstack([factor_rows(rec1), factor_rows(rec2)])
    n1, n2 = r1.end - r1.begin, r2.end - r2.begin
    taus = [(a["tau"] * (n1 - 1) + b["tau"] * (n2 - 1)) / (n1 + n2 - 2) for a, b in zip(rec1, rec2)]
    library = fcw.load_library(t["e2"].library, t["e2"].library_sha)
    priors = fcw.load_priors(t["e2"].library, t["e2"].library_sha, None, None, library)
    want = fcw.screen_v4(factors, taus, IDS, np.ones(n1 + n2, dtype=bool), priors["tier_rank"], priors["prior_signs"])
    got = {c["id"]: c for c in adm["candidates"]}
    for cid, row in zip(IDS, want):
        for key in ("status", "failed_checks", "train_days", "train_mean", "train_sharpe", "hac_t", "tau",
                    "redundant_with", "admission_rank", "max_abs_rho", "max_abs_rho_with"):
            assert got[cid][key] == row[key], (cid, key)
    assert got["slow_b"]["status"] == "admitted" and got["slow_b"]["train_days"] == n1 + n2 == 356
    assert got["unsigned"]["status"] == "reject_no_prior" and got["sparse"]["status"] == "reject_insufficient"
    w1, w2 = fcw.era_window(r1), fcw.era_window(r2)
    assert adm["rules"]["train_window_ns"] == [list(w1), list(w2)] and w1[1] <= w2[0] < fcw.rw.TRAIN_BEGIN_NS
    pool = adm["pool"]
    assert pool["anchor"] == "E2" and [e["id"] for e in pool["eras"]] == ["E1", "E2"]
    assert [e["segment_start"] for e in pool["eras"]] == [0, n1]
    assert [e["role_manifest_sha256"] for e in pool["eras"]] == [t["e1"].train_sha, t["e2"].train_sha]
    digest = fcw.era_pool.pooled_sha256([rec1[0]["context_sha256"], rec2[0]["context_sha256"]])
    assert pool["context_sha256"] == adm["inputs"]["context_sha256"] == digest
    assert adm["inputs"]["train_manifest_sha256"] == t["e2"].train_sha


def test_one_weights_file_per_era_bound_to_its_role(eras):
    out = eras["root"] / "W"
    if not out.exists():
        fcw.fit(fcw.parse_args(pooled_argv(eras, out)))
    anchor = (out / fcw.OUTPUT_WEIGHTS).read_bytes()
    e1 = (out / "composition_weights.E1.json").read_bytes()
    a, b = json.loads(anchor), json.loads(e1)
    assert a.pop("train_manifest_sha256") == eras["e2"].train_sha and b.pop("train_manifest_sha256") == eras["e1"].train_sha
    assert a == b and a["provenance"]["pool"]["anchor"] == "E2"
    library_sha = eras["e2"].library_sha
    assert runner_accepts(anchor, library_sha, IDS, eras["e2"].train_sha) == runner_accepts(
        e1, library_sha, IDS, eras["e1"].train_sha)
    assert sorted(p.name for p in out.iterdir()) == sorted([fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV,
                                                            fcw.OUTPUT_WEIGHTS, "composition_weights.E1.json"])


def test_the_single_path_masks_history_decisions_out(eras):
    """Finding 3: without --era the TRAIN mask drops every 2014 decision (every member insufficient); a one-era pool
    (--era-id alone) scores them with the explicit mask."""
    fx, root = eras["e1"], eras["root"]
    code, _ = fcw.fit(fx.args(root / "single", "v4-prior-v1", orientation="prior", composition="ew-theme-v1"))
    adm = json.loads((root / "single" / fcw.OUTPUT_ADMISSION).read_bytes())
    assert code == fcw.EXIT_NO_WEIGHTS and {c["train_days"] for c in adm["candidates"]} == {0}
    assert "pool" not in adm and adm["rules"]["train_window_ns"] == [fcw.FIT_BEGIN_NS, fcw.TRAIN_END_NS]
    fcw.fit(fcw.parse_args(fx.argv(root / "one", "v4-prior-v1", [*V4, "--era-id", "E1"])))
    one = json.loads((root / "one" / fcw.OUTPUT_ADMISSION).read_bytes())
    role, recs = era_records(fx)
    assert {c["id"]: c["train_days"] for c in one["candidates"]}["slow_b"] == role.end - role.begin
    assert one["pool"]["eras"][0]["id"] == "E1" and one["inputs"]["context_sha256"] == recs[0]["context_sha256"]
    assert [c["tau"] for c in one["candidates"]] == [r["tau"] for r in recs]   # one era: its taus exactly


def test_each_era_keeps_its_own_role_keyed_work_store_and_a_rerun_reuses_it(eras):
    t, root = eras, eras["root"]
    work = root / "work"
    argv = pooled_argv(t, root / "W1", extra=["--work-dir", str(work)])
    fcw.fit(fcw.parse_args(argv))
    assert store_base(work, t["e1"].train_sha).is_dir() and store_base(work, t["e2"].train_sha).is_dir()
    code, summary = fcw.fit(fcw.parse_args(pooled_argv(t, root / "W2", extra=["--work-dir", str(work)])))
    assert code == fcw.EXIT_OK and summary["computed_this_run"] == 0 and summary["reused"] == 2 * len(IDS)
    for name in (fcw.OUTPUT_ADMISSION, fcw.OUTPUT_WEIGHTS, "composition_weights.E1.json"):
        assert (root / "W1" / name).read_bytes() == (root / "W2" / name).read_bytes(), name


def test_report_f_theta_uses_the_pooled_theta_series(eras):
    out = eras["root"] / "WT"
    fcw.fit(fcw.parse_args(pooled_argv(eras, out, extra=["--report-f-theta"])))
    adm = json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes())
    (_, rec1), (_, rec2) = era_records(eras["e1"]), era_records(eras["e2"])
    theta = np.hstack([factor_rows(rec1, "f_theta_unsigned"), factor_rows(rec2, "f_theta_unsigned")])
    k = IDS.index("slow_b")
    x = theta[k][np.isfinite(theta[k])]
    assert adm["candidates"][k]["f_theta"] == pytest.approx(float(x.mean()), rel=1e-12)
    assert adm["report_only"]["f_theta"]["window_ns"] == adm["rules"]["train_window_ns"]


@pytest.mark.parametrize("change, needle", [
    (dict(screen="none", extra=[]), "prior screens"),
    (dict(extra=["--composition", "ew-theme-aim-v1"]), "prior screens"),
    (dict(drop_era_id=True), "needs --era-id"),
    (dict(era_id="E1"), "duplicate era id"),
    (dict(era_id="E/2"), "must match"),
    (dict(swap=True), "date order"),
    (dict(bad_pin=5), "orientations: SHA-256 pin differs"),
    (dict(bad_pin=7), "runner summary: SHA-256 pin differs"),
    (dict(bad_pin=3), "train_manifest_sha256 differs"),   # the era's orientations bind the era's own role
])
def test_refusals(eras, change, needle, tmp_path):
    t = eras
    e1 = era_argv("E1", t["e1"])
    if change.get("bad_pin"):
        e1[change["bad_pin"]] = "0" * 64
    if change.get("swap"):   # the anchor (the --train role) must be the last era in date order
        argv = t["e1"].argv(tmp_path / "o", "v4-prior-v1", [*V4, *era_argv("E2", t["e2"]), "--era-id", "E1"])
    else:
        tail = [] if change.get("drop_era_id") else ["--era-id", change.get("era_id", "E2")]
        extra = V4 if change.get("screen") != "none" else []
        argv = t["e2"].argv(tmp_path / "o", change.get("screen", "v4-prior-v1"), [*extra, *e1, *tail,
                                                                                    *change.get("extra", [])])
    with pytest.raises(fcw.FitError, match=needle):
        fcw.fit(fcw.parse_args(argv))
    assert not (tmp_path / "o").exists()


def test_overlapping_eras_are_refused(eras, tmp_path):
    late = fixture(tmp_path / "late", 33, str(np.datetime64(next_start("2014-01-01"), "D") - 30))
    argv = late.argv(tmp_path / "o", "v4-prior-v1", [*V4, *era_argv("E1", eras["e1"]), "--era-id", "E2"])
    with pytest.raises(fcw.FitError, match="overlaps era E1"):
        fcw.fit(fcw.parse_args(argv))


def test_without_era_flags_the_parse_is_the_single_path(eras):
    args = eras["e2"].args(Path("x"), "v4-prior-v1")
    assert args.era is None and args.era_id is None and not fcw.pooled(args)
