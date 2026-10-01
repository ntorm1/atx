"""fit_composition_weights.py --era / --era-id (platform v8 H-1): the pooled prior screen over era roles.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_weights_pool.py

Synthetic data only (test_fit_composition_weights.Fixture: role dir, library, orientations, candidate cache and TRAIN
summary per era). Two history eras (2014-2015) of 178 scored decisions each: alone, every member is insufficient (and
the single path's TRAIN mask drops every decision: finding 3); pooled, the prior screen reads 356 decisions.

Ruling E-35 (the v8 compositions ew-theme-std-v1 and ew-theme-std-aim-v1): a one-era pool of a role scored inside TRAIN
(test_fit_composition_weights.aim_world, the R-1 fitter world of test_composition_rules) against the single-window fit
of that role, and the two history eras pooled against composition_rules.ew_theme_std and the aim gain of the
block-diagonal rank panel of the eras. Ruling PM4-7 (finding R6B-C-1): the same one-era equality for ic-shrink-v1,
ic-shrink-aim-v1 and --theme-resid theme-resid-v1 on each of its parents, and the two history eras pooled against
composition_ic_shrink.ic_shrink on the pooled admission.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import re
import time
import unittest.mock

import numpy as np
import pytest

import composition_ic_shrink as cis
import composition_resid as cres
import composition_rules as cr
import fit_composition_weights as fcw
import test_fit_composition_weights as tfw
from test_composition_rules import ref_cap
from test_fit_composition_weights import DAY, NAN, Fixture, runner_accepts, store_base

NAMES, DATES, SCORE_BEGIN = 60, 330, 150
META = {"slow_a": ("value", "B", 1), "slow_a_twin": ("value", "B+", 1), "slow_b": ("price_momentum", "A", 1),
        "fast_a": ("low_risk", "A-", 1), "sparse": ("short_interest", "C+", 1), "unsigned": ("options_implied", "B", 0)}
IDS = list(META)
EXTRA = {i: {"theme": t, "tier": g, "prior_sign": s} for i, (t, g, s) in META.items()}
V4 = ["--orientation", "prior", "--composition", "ew-theme-v1"]


def ar_panel(rng, phi: float = 0.97) -> np.ndarray:
    """A DATES x NAMES AR(phi) panel of unit-variance names."""
    z = np.empty((DATES, NAMES))
    z[0] = rng.normal(size=NAMES)
    for t in range(1, DATES):
        z[t] = phi * z[t - 1] + math.sqrt(1 - phi * phi) * rng.normal(size=NAMES)
    return z


def era_world(seed: int, start: str):
    """A calendar-day panel with persistent planted alphas (screen_world's recipe), beginning at ``start``."""
    rng = np.random.default_rng(seed)
    z1, z2, u, v, w = (ar_panel(rng) for _ in range(5))
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
    (dict(extra=["--composition", "mv-shrink-0.9-nonneg-netcost-v1"]), "prior screens"),
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


# ------------------------------------------------------------------ Ruling E-35: the v8 compositions
# AIM: Ruling E-27a's aim rule on an ew-theme-v1 parent, ew-theme-aim-v2 (Ruling E-27b; the pool's E-35a rule).
STD, STD_AIM, AIM = cr.STD_RULE_ID, cr.STD_AIM_RULE_ID, cr.AIM_V2_RULE_ID
SHRINK, SHRINK_AIM = cis.RULE_ID, cis.AIM_RULE_ID                       # R-10 (Ruling PM4-7)
STANDARDISED = (STD, STD_AIM, SHRINK, SHRINK_AIM)                       # the parents --theme-resid rides on
RESID = cres.RULE_ID


def resid_key(parent: str) -> str:
    """The train_world run key of --theme-resid theme-resid-v1 on ``parent``."""
    return f"{parent}+{RESID}"


# The keys a pool adds or replaces (H-1): everything else of a one-era pool is the single-window fit's.
POOL_KEYS_WEIGHTS = (("provenance", "pool"), ("provenance", "window"), ("provenance", "admission_sha256"))
POOL_KEYS_ADMISSION = (("pool",), ("window",), ("rules", "train_window_ns"))


def without(doc: dict, paths) -> dict:
    out = json.loads(json.dumps(doc))
    for path in paths:
        node = out
        for key in path[:-1]:
            node = node[key]
        node.pop(path[-1], None)
    return out


def outputs(out: Path) -> dict:
    return {p.name: p.read_bytes() for p in sorted(out.iterdir())}


@pytest.fixture(scope="module")
def train_world(tmp_path_factory):
    """The R-1 fitter world (test_composition_rules.FitterEndToEnd): one role scored inside TRAIN; each v8
    composition fitted on the single window and as a one-era pool (--era-id E3), registry absent; --theme-resid on each
    standardised parent too (Ruling PM4-7, key ``resid_key(parent)``)."""
    root = tmp_path_factory.mktemp("e35")
    panel, signals, ids, extra = tfw.aim_world()
    extra = dict(extra)
    extra["flip"] = dict(extra["flip"], theme="value")           # two themes, five admitted members: M > 2T
    extra["medium_half"] = dict(extra["medium_half"], tier="C+")  # a scored grade
    fx = Fixture(root / "fx", panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                 candidate_extra=extra)
    runs = {}
    with unittest.mock.patch.object(cr, "REGISTRY_PATH", root / "no-registry.json"):
        for comp in (STD, STD_AIM, AIM, SHRINK, SHRINK_AIM):
            for key, resid in ((comp, []),) + (((resid_key(comp), ["--theme-resid", RESID]),)
                                               if comp in STANDARDISED else ()):
                flags = ["--orientation", "prior", "--composition", comp, *resid]
                for kind, extra_flags in (("single", []), ("pool", ["--era-id", "E3"])):
                    out = root / f"{kind}-{key}"
                    runs[key, kind] = fcw.fit(fcw.parse_args(fx.argv(out, "v4-prior-v1", [*flags, *extra_flags]))) + (
                        outputs(out),)
    return {"root": root, "fx": fx, "ids": ids, "runs": runs}


def one_era_equals_single(t: dict, run: str) -> tuple[dict, dict]:
    """The one-era pool's outputs equal the single-window fit's byte for byte once the pool's own keys are removed
    (``run``: a composition, or ``resid_key(parent)``)."""
    comp = run.split("+")[0]
    (c1, s1, single), (c2, s2, pooled) = t["runs"][run, "single"], t["runs"][run, "pool"]
    assert (c1, c2) == (fcw.EXIT_OK, fcw.EXIT_OK) and s1["composition"] == s2["composition"] == comp
    assert sorted(single) == sorted(pooled) == sorted([fcw.OUTPUT_ADMISSION, fcw.OUTPUT_ADMISSION_CSV,
                                                       fcw.OUTPUT_WEIGHTS])        # one era: no era weights file
    assert single[fcw.OUTPUT_ADMISSION_CSV] == pooled[fcw.OUTPUT_ADMISSION_CSV]
    a, b = (json.loads(x[fcw.OUTPUT_WEIGHTS]) for x in (single, pooled))
    assert fcw.canonical_bytes(without(a, POOL_KEYS_WEIGHTS)) == fcw.canonical_bytes(without(b, POOL_KEYS_WEIGHTS))
    for key in ("schema", "weights", "signs", "theme_standardise", cres.BLOCK):   # nothing removed from these
        assert fcw.canonical_bytes({key: a.get(key)}) == fcw.canonical_bytes({key: b.get(key)}), key
    for key in ("std", "aim", "ic_shrink", "resid"):
        assert fcw.canonical_bytes({key: a["provenance"].get(key)}) == fcw.canonical_bytes(
            {key: b["provenance"].get(key)}), key
    adm_a, adm_b = (json.loads(x[fcw.OUTPUT_ADMISSION]) for x in (single, pooled))
    assert fcw.canonical_bytes(without(adm_a, POOL_KEYS_ADMISSION)) == fcw.canonical_bytes(
        without(adm_b, POOL_KEYS_ADMISSION))
    # the removed keys are the pool's: its block, the era window as the train window, the window of the era list
    role = fcw.RoleManifest(t["fx"].manifest, t["fx"].train_sha)
    assert adm_b["rules"]["train_window_ns"] == [list(fcw.era_window(role))] and adm_b["pool"]["anchor"] == "E3"
    assert b["provenance"]["pool"] == adm_b["pool"] and [e["id"] for e in b["provenance"]["window"]["eras"]] == ["E3"]
    assert b["provenance"]["admission_sha256"] == s2["admission_sha256"] != s1["admission_sha256"]
    return a, b


def test_pooled_std_fit_over_one_era_equals_the_single_window_fit(train_world):
    """(a) ew-theme-std-v1: the five registered rules (composition_rules.ew_theme_std) on the pooled admission."""
    a, b = one_era_equals_single(train_world, STD)
    assert b["provenance"]["rule"] == STD and b["theme_standardise"]["rule"] == STD and "aim" not in b["provenance"]
    assert sum(1 for w in b["weights"].values() if w > 0) == 5


def test_pooled_std_aim_fit_over_one_era_equals_the_single_window_fit(train_world):
    """(b) ew-theme-std-aim-v1: E-27's gains inside each theme, from the pooled aim records (one era = the era's)."""
    a, b = one_era_equals_single(train_world, STD_AIM)
    assert fcw.canonical_bytes(a["provenance"]["aim"]) == fcw.canonical_bytes(b["provenance"]["aim"])
    assert b["provenance"]["rule"] == STD_AIM and b["theme_standardise"]["rule"] == STD
    std = json.loads(train_world["runs"][STD, "pool"][2][fcw.OUTPUT_WEIGHTS])
    assert b["weights"] != std["weights"]                                       # the gains moved weight
    gains = b["provenance"]["aim"]["gain"]
    assert b["provenance"]["std"]["aim_gains"] == {i: gains[i] for i in b["provenance"]["std"]["aim_gains"]}


def e27a_reference(members: list[str], themes: dict, gains: dict) -> dict:
    """Ruling E-27a written out: the ew-theme-v1 weights times the gains, renormalised inside each theme (share 1/T),
    then the member cap 1/(2T) (test_composition_rules.ref_cap, the loop port of rule 4)."""
    present = sorted({themes[i] for i in members})
    total = {t: sum(gains[i] for i in members if themes[i] == t) for t in present}
    base = {i: gains[i] / (len(present) * total[themes[i]]) for i in members}
    return ref_cap(base, {i: themes[i] for i in members}, 1.0 / (2 * len(present)))


def aim_members(doc: dict, adm: dict) -> tuple[list[str], dict, dict]:
    rows = {c["id"]: c for c in adm["candidates"]}
    members = doc["provenance"]["aim"]["members"]                 # fit_prior's order (admission rank)
    return members, {i: rows[i]["theme"] for i in members}, doc["provenance"]["aim"]["gain"]


def test_pooled_aim_fit_over_one_era_is_e27a_on_the_single_window_gains(train_world):
    """Rulings E-35a, E-27b: ew-theme-aim-v2 in the pool, by E-27a's rule, on the gains of the pooled aim records (one
    era: the single window's gains and admission, byte for byte once the pool's own keys are removed)."""
    (c1, _, single), (c2, summary, pooled) = train_world["runs"][AIM, "single"], train_world["runs"][AIM, "pool"]
    assert (c1, c2) == (fcw.EXIT_OK, fcw.EXIT_OK)
    a, b = (json.loads(x[fcw.OUTPUT_WEIGHTS]) for x in (single, pooled))
    adm_a, adm_b = (json.loads(x[fcw.OUTPUT_ADMISSION]) for x in (single, pooled))
    assert fcw.canonical_bytes(without(adm_a, POOL_KEYS_ADMISSION)) == fcw.canonical_bytes(
        without(adm_b, POOL_KEYS_ADMISSION))
    assert fcw.canonical_bytes(a["provenance"]["aim"]["gain"]) == fcw.canonical_bytes(b["provenance"]["aim"]["gain"])
    assert a["provenance"]["aim"]["rho"] == b["provenance"]["aim"]["rho"] and a["signs"] == b["signs"]
    members, themes, gains = aim_members(b, adm_b)
    want = e27a_reference(members, themes, gains)
    assert len(set(themes.values())) == 2 and len(members) == 5
    for i, w in b["weights"].items():
        assert w == pytest.approx(want.get(i, 0.0), rel=0, abs=1e-15), i
    assert max(b["weights"].values()) <= 0.25 * (1 + cr.CAP_TOLERANCE)        # the member cap 1/(2T) holds
    assert b["provenance"]["composition"] == cr.AIM_V2_TEXT and b["schema"] == fcw.WEIGHTS_SCHEMA
    themes_doc = b["provenance"]["themes"]
    assert summary["aim_theme_weights"] == {t: e["aim_theme_weight"] for t, e in sorted(themes_doc.items())}


@pytest.mark.parametrize("comp", [STD, STD_AIM, AIM])
def test_pooled_aim_fit_over_one_era_equals_the_single_window_fit(train_world, comp):
    """Rulings E-35a, E-27b (integration 6 part B): the pooled path runs the single-window code (FIX-3's shared
    theme_gain_weights for ew-theme-aim-v2), so the one-era pool equals the single-window fit byte for byte once the
    pool's own keys are removed, for ew-theme-std-v1, ew-theme-std-aim-v1 and ew-theme-aim-v2 alike; the single
    window's ew-theme-aim-v2 is E-27a written out (no skip)."""
    single = json.loads(train_world["runs"][AIM, "single"][2][fcw.OUTPUT_WEIGHTS])
    adm = json.loads(train_world["runs"][AIM, "single"][2][fcw.OUTPUT_ADMISSION])
    want = e27a_reference(*aim_members(single, adm))
    for i, w in single["weights"].items():
        assert w == pytest.approx(want.get(i, 0.0), rel=0, abs=1e-15), i
    a, b = one_era_equals_single(train_world, comp)
    assert a["provenance"]["rule"] == b["provenance"]["rule"] == comp


@pytest.mark.parametrize("comp", [SHRINK, SHRINK_AIM])
def test_pooled_ic_shrink_fit_over_one_era_equals_the_single_window_fit(train_world, comp):
    """Ruling PM4-7 (finding R6B-C-1): ic-shrink-v1 and ic-shrink-aim-v1 in the pool, through
    composition_ic_shrink.ic_shrink on the pooled admission rows' train_mean (the variant also on the pooled aim gains,
    as ew-theme-std-aim-v1 reads them): the one-era pool equals the single-window fit byte for byte once the pool's own
    keys are removed, and the block records the pooled admission's ICs."""
    a, b = one_era_equals_single(train_world, comp)
    assert a["provenance"]["rule"] == b["provenance"]["rule"] == b["theme_standardise"]["rule"] == comp
    assert ("aim" in b["provenance"]) == (comp == SHRINK_AIM)
    adm = json.loads(train_world["runs"][comp, "pool"][2][fcw.OUTPUT_ADMISSION])
    rows = {c["id"]: c for c in adm["candidates"]}
    members = b["theme_standardise"]["ic_shrink"]["members"]
    assert len(members) == 5 and all(m["ic"] == rows[i]["train_mean"] for i, m in members.items())
    if comp == SHRINK_AIM:
        assert all(m["gain"] == b["provenance"]["aim"]["gain"][i] for i, m in members.items())
        std_aim = json.loads(train_world["runs"][STD_AIM, "pool"][2][fcw.OUTPUT_WEIGHTS])
        assert b["provenance"]["aim"]["gain"] == std_aim["provenance"]["aim"]["gain"]
    std = json.loads(train_world["runs"][STD, "pool"][2][fcw.OUTPUT_WEIGHTS])
    assert b["weights"] != std["weights"]                                       # the ICs moved weight


@pytest.mark.parametrize("parent", STANDARDISED)
def test_pooled_theme_resid_over_one_era_equals_the_single_window_fit(train_world, parent):
    """Ruling PM4-7 (finding R6B-C-1): --theme-resid theme-resid-v1 in the pool on each of its parents
    (ew-theme-std-v1, ew-theme-std-aim-v1, ic-shrink-v1, ic-shrink-aim-v1), through composition_resid.apply on the
    pooled document as in the single window: the one-era pool equals the single-window fit byte for byte once the pool's
    own keys are removed, and the pooled file minus the block and provenance.resid is the pooled parent's file."""
    a, b = one_era_equals_single(train_world, resid_key(parent))
    assert b[cres.BLOCK]["rule"] == RESID and b["provenance"]["resid"]["parent_composition"] == parent
    plain = train_world["runs"][parent, "pool"][2][fcw.OUTPUT_WEIGHTS]
    del b[cres.BLOCK]
    del b["provenance"]["resid"]
    assert fcw.canonical_bytes(b) == plain


def test_the_pooled_fit_refuses_the_v5_aim_rule_naming_v2(eras, tmp_path):
    """Ruling E-27b: ew-theme-aim-v1 keeps the v5 rule and is never pooled; the refusal names ew-theme-aim-v2 and
    happens before anything is read or written."""
    args = fcw.parse_args(pooled_argv(eras, tmp_path / "o"))
    args.composition = fcw.AIM_RULE_ID
    with pytest.raises(fcw.FitError, match=r"ew-theme-aim-v1 is not implemented by the pooled fit.*ew-theme-aim-v2"):
        fcw.fit(args)
    assert not (tmp_path / "o").exists() and fcw.AIM_RULE_ID not in fcw.POOLED_COMPOSITIONS


@pytest.mark.parametrize("comp", ["ew-theme-new-v9", "mv-shrink-0.9-nonneg-v1", "mv-shrink-0.9-nonneg-netcost-v1"])
def test_a_composition_the_pooled_fit_does_not_implement_is_refused_by_name(eras, comp, tmp_path):
    """(c) an id outside POOLED_COMPOSITIONS (unknown, or registered for the single window only) is refused, named."""
    args = fcw.parse_args(pooled_argv(eras, tmp_path / "o"))
    args.composition = comp
    with pytest.raises(fcw.FitError, match=f"--composition {re.escape(comp)} is not implemented by the pooled fit"):
        fcw.fit(args)
    assert not (tmp_path / "o").exists()
    assert fcw.POOLED_COMPOSITIONS == ("ew-theme-v1", AIM, "ew-theme-v6", STD, STD_AIM, SHRINK, SHRINK_AIM)
    assert fcw.POOLED_THEME_RESID == (RESID,)


def test_a_theme_resid_rule_the_pooled_fit_does_not_implement_is_refused_by_name(eras, tmp_path):
    """Ruling PM4-7: a --theme-resid id outside POOLED_THEME_RESID is refused by name before anything is read."""
    args = fcw.parse_args(pooled_argv(eras, tmp_path / "o"))
    args.composition, args.theme_resid = STD, "theme-resid-v9"
    with pytest.raises(fcw.FitError, match="--theme-resid theme-resid-v9 is not implemented by the pooled fit"):
        fcw.fit(args)
    assert not (tmp_path / "o").exists()


def test_a_prior_composition_without_a_weight_rule_never_falls_back_to_ew_theme_v1(train_world, eras, tmp_path,
                                                                                    monkeypatch):
    """A prior composition registered without a rule: the single window refuses it in fit_prior (no silent ew-theme-v1
    weights) and the pooled fit refuses it by name before reading anything."""
    monkeypatch.setattr(fcw, "PRIOR_COMPOSITIONS", fcw.PRIOR_COMPOSITIONS + ("ew-theme-new-v9",))
    monkeypatch.setattr(fcw, "COMPOSITIONS", fcw.COMPOSITIONS + ("ew-theme-new-v9",))
    args = fcw.parse_args(train_world["fx"].argv(tmp_path / "s", "v4-prior-v1", V4))
    args.composition = "ew-theme-new-v9"
    with pytest.raises(fcw.FitError, match="--composition ew-theme-new-v9 has no prior weight rule"):
        fcw.fit(args)
    args = fcw.parse_args(pooled_argv(eras, tmp_path / "p"))
    args.composition = "ew-theme-new-v9"
    with pytest.raises(fcw.FitError, match="ew-theme-new-v9 is not implemented by the pooled fit"):
        fcw.fit(args)
    assert not (tmp_path / "s").exists() and not (tmp_path / "p").exists()


def block_ranks(fx_list: list[Fixture], k: int) -> np.ndarray:
    """Candidate k's standardized ranks of every era (every scored decision, used rows with a finite signal) on one
    block-diagonal panel: decisions in date order, each era's names its own columns, NaN elsewhere."""
    blocks = []
    for fx in fx_list:
        ctx = fcw.Context.build(fcw.RoleManifest(fx.manifest, fx.train_sha), None)
        slab = fx.signals[k][ctx.begin:ctx.end][:, ctx.columns]
        blocks.append(fcw.standardized_ranks(slab, ctx.used & np.isfinite(slab)))
    rows, cols = sum(b.shape[0] for b in blocks), sum(b.shape[1] for b in blocks)
    z, r, c = np.full((rows, cols), np.nan), 0, 0
    for b in blocks:
        z[r:r + b.shape[0], c:c + b.shape[1]] = b
        r, c = r + b.shape[0], c + b.shape[1]
    return z


# Two history eras whose pooled admission holds five members in two themes (M > 2T: the member cap 1/(2T) is
# feasible): era_world's planted slow_a / slow_b and three independent AR(.97) panels (no return loading, no veto).
V8_META = {"val_a": ("value", "A", 1), "val_b": ("value", "B+", 1), "val_c": ("value", "C+", 1),
           "mom_a": ("price_momentum", "A-", 1), "mom_b": ("price_momentum", "B", 1)}
V8_IDS = list(V8_META)


def v8_fixture(root: Path, seed: int, start: str) -> Fixture:
    panel, signals = era_world(seed, start)
    rng = np.random.default_rng(seed + 100)
    live = panel["member"] == 1
    p1, p2, p3 = (np.where(live, ar_panel(rng), NAN) for _ in range(3))
    planted = dict(zip(IDS, signals))
    v8 = [planted["slow_a"], p1, p2, planted["slow_b"], p3]
    extra = {i: {"theme": t, "tier": g, "prior_sign": s} for i, (t, g, s) in V8_META.items()}
    return Fixture(root, panel, v8, [1] * len(V8_IDS), ids=V8_IDS, families=["fam"] * len(V8_IDS),
                   candidate_extra=extra, layout="v2")


@pytest.fixture(scope="module")
def pooled_v8(tmp_path_factory):
    root = tmp_path_factory.mktemp("v8pool")
    e1 = v8_fixture(root / "e1", 61, "2014-01-01")      # seeds with every member admitted (HAC t > -2 each)
    e2 = v8_fixture(root / "e2", 62, next_start("2014-01-01"))
    runs = {"root": root, "e1": e1, "e2": e2}
    with unittest.mock.patch.object(cr, "REGISTRY_PATH", root / "no-registry.json"):
        for comp in (STD, STD_AIM, AIM, SHRINK, SHRINK_AIM):
            out = root / f"P-{comp}"
            argv = e2.argv(out, "v4-prior-v1", ["--orientation", "prior", "--composition", comp, *era_argv("E1", e1),
                                                "--era-id", "E2", "--work-dir", str(root / "work")])
            runs[comp] = fcw.fit(fcw.parse_args(argv)) + (out,)
    return runs


def test_two_history_eras_pooled_std_is_the_rule_on_the_pooled_admission(pooled_v8):
    t = pooled_v8
    code, summary, out = t[STD]
    assert code == fcw.EXIT_OK and summary["pool"]["eras"] == ["E1", "E2"]
    adm_bytes = (out / fcw.OUTPUT_ADMISSION).read_bytes()
    assert adm_bytes == (t[STD_AIM][2] / fcw.OUTPUT_ADMISSION).read_bytes()   # the screen never sees the rule
    adm, doc = json.loads(adm_bytes), json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
    rows = {c["id"]: c for c in adm["candidates"]}
    assert sorted(adm["admitted"]) == sorted(V8_IDS) and all(r["train_days"] == 356 for r in rows.values())
    weighted = {c["id"]: c for c in doc["provenance"]["candidates"]}
    members = [i for i in adm["admitted"] if weighted[i]["status"] == "fitted"]     # fit_prior's order
    with unittest.mock.patch.object(cr, "REGISTRY_PATH", t["root"] / "no-registry.json"):
        want = cr.ew_theme_std(members, [rows[i]["theme"] for i in members], [rows[i]["tier"] for i in members])
    assert doc["weights"] == {i: float(dict(zip(members, want.weights)).get(i, 0.0)) for i in V8_IDS}
    assert doc["theme_standardise"] == want.block and doc["schema"] == cr.WEIGHTS_SCHEMA_V2
    assert doc["provenance"]["std"] == json.loads(json.dumps(want.provenance))
    assert doc["provenance"]["rule"] == STD and doc["provenance"]["pool"]["anchor"] == "E2"
    era1 = json.loads((out / "composition_weights.E1.json").read_bytes())    # the E1 w pass reads the same rule
    assert era1 == dict(doc, train_manifest_sha256=t["e1"].train_sha)


def test_two_history_eras_pooled_std_aim_gains_are_the_block_panel_gains(pooled_v8):
    """The pooled gain of each member is aim_gain of the rank autocorrelation of the eras' block-diagonal rank panel
    (every scored decision of each era; no lag pair across eras); the weights are ew_theme_std on those gains."""
    t = pooled_v8
    code, summary, out = t[STD_AIM]
    assert code == fcw.EXIT_OK
    doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
    aim = doc["provenance"]["aim"]
    for k, cid in enumerate(V8_IDS):
        rho = fcw.rank_autocorrelation(block_ranks([t["e1"], t["e2"]], k), fcw.AIM_LAGS)
        assert aim["gain"][cid] == pytest.approx(fcw.aim_gain(rho, fcw.AIM_LAGS), rel=0, abs=1e-12), cid
        np.testing.assert_allclose([np.nan if v is None else v for v in aim["rho"][cid]], rho, rtol=0, atol=1e-12)
    assert aim["half_split_decision_index"] == 178                 # 356 pooled decisions, all in the mask
    assert min(aim["gain"].values()) > 0.5 > fcw.AIM_GAIN_MIN        # history ranks read (a TRAIN mask reads none)
    assert all(0 < v <= 1 for v in aim["coverage_mean"].values())
    adm = json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes())
    rows = {c["id"]: c for c in adm["candidates"]}
    members = list(doc["provenance"]["std"]["aim_gains"])
    with unittest.mock.patch.object(cr, "REGISTRY_PATH", t["root"] / "no-registry.json"):
        want = cr.ew_theme_std(members, [rows[i]["theme"] for i in members], [rows[i]["tier"] for i in members],
                               gains=[aim["gain"][i] for i in members])
    assert doc["weights"] == {i: float(dict(zip(members, want.weights)).get(i, 0.0)) for i in V8_IDS}
    std = json.loads((t[STD][2] / fcw.OUTPUT_WEIGHTS).read_bytes())
    assert sorted(members) == sorted(i for i in V8_IDS if std["weights"][i] > 0) and doc["weights"] != std["weights"]
    assert (doc["provenance"]["rule"], doc["theme_standardise"]) == (STD_AIM, std["theme_standardise"])
    assert summary["aim_gain_min"] == min(aim["gain"][i] for i in members)


def test_two_history_eras_pooled_aim_is_e27a_on_the_pooled_gains(pooled_v8):
    """Ruling E-35a on two history eras: ew-theme-aim-v2 reads the same pooled aim records as ew-theme-std-aim-v1 (its
    era aim parts are reused from the store) and weights them by E-27a; each era's file carries the same weights."""
    t = pooled_v8
    code, summary, out = t[AIM]
    assert code == fcw.EXIT_OK and summary["computed_this_run"] == 0             # every record reused (same keys)
    doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
    adm = json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes())
    std_aim = json.loads((t[STD_AIM][2] / fcw.OUTPUT_WEIGHTS).read_bytes())
    assert doc["provenance"]["aim"]["gain"] == std_aim["provenance"]["aim"]["gain"]
    assert doc["provenance"]["aim"]["rho"] == std_aim["provenance"]["aim"]["rho"]
    want = e27a_reference(*aim_members(doc, adm))
    assert all(doc["weights"][i] == pytest.approx(want.get(i, 0.0), rel=0, abs=1e-15) for i in V8_IDS)
    assert doc["weights"] != std_aim["weights"]                                  # equal base weights, not tiers
    assert doc["provenance"]["rule"] == AIM and "theme_standardise" not in doc
    era1 = json.loads((out / "composition_weights.E1.json").read_bytes())
    assert era1 == dict(doc, train_manifest_sha256=t["e1"].train_sha)


@pytest.mark.parametrize("comp", [SHRINK, SHRINK_AIM])
def test_two_history_eras_pooled_ic_shrink_is_the_rule_on_the_pooled_admission(pooled_v8, comp):
    """Ruling PM4-7 on two history eras: the ICs are the pooled admission rows' train_mean (356 decisions each) and the
    weights and block are composition_ic_shrink.ic_shrink on them (the variant also on the pooled aim gains, the ones
    ew-theme-std-aim-v1 reads); each era's file carries the same weights and block."""
    t = pooled_v8
    code, _, out = t[comp]
    assert code == fcw.EXIT_OK
    doc = json.loads((out / fcw.OUTPUT_WEIGHTS).read_bytes())
    adm = json.loads((out / fcw.OUTPUT_ADMISSION).read_bytes())
    assert adm == json.loads((t[STD][2] / fcw.OUTPUT_ADMISSION).read_bytes())    # the screen never sees the rule
    rows = {c["id"]: c for c in adm["candidates"]}
    weighted = {c["id"]: c for c in doc["provenance"]["candidates"]}
    members = [i for i in adm["admitted"] if weighted[i]["status"] == "fitted"]  # fit_prior's order
    gains = None
    if comp == SHRINK_AIM:
        std_aim = json.loads((t[STD_AIM][2] / fcw.OUTPUT_WEIGHTS).read_bytes())
        assert doc["provenance"]["aim"]["gain"] == std_aim["provenance"]["aim"]["gain"]
        gains = [doc["provenance"]["aim"]["gain"][i] for i in members]
    want = cis.ic_shrink(members, [rows[i]["theme"] for i in members], [rows[i]["train_mean"] for i in members],
                         gains=gains)
    assert all(rows[i]["train_days"] == 356 for i in members) and len(members) == 5
    assert doc["weights"] == {i: float(dict(zip(members, want.weights)).get(i, 0.0)) for i in V8_IDS}
    assert doc["theme_standardise"] == json.loads(json.dumps(want.block)) and doc["provenance"]["rule"] == comp
    era1 = json.loads((out / "composition_weights.E1.json").read_bytes())
    assert era1 == dict(doc, train_manifest_sha256=t["e1"].train_sha)


def test_era_aim_parts_live_in_each_eras_store_and_a_rerun_reuses_them(pooled_v8, tmp_path):
    t = pooled_v8
    work = t["root"] / "work"
    for fx in (t["e1"], t["e2"]):
        base = store_base(work, fx.train_sha)
        assert len(list((base / fcw.AIM_ERA_KIND).glob("*.json"))) == len(V8_IDS) and not (base / "aim").exists()
    out = tmp_path / "again"
    argv = t["e2"].argv(out, "v4-prior-v1", ["--orientation", "prior", "--composition", STD_AIM,
                                             *era_argv("E1", t["e1"]), "--era-id", "E2", "--work-dir", str(work)])
    with unittest.mock.patch.object(cr, "REGISTRY_PATH", t["root"] / "no-registry.json"):
        code, summary = fcw.fit(fcw.parse_args(argv))
    assert code == fcw.EXIT_OK and summary["computed_this_run"] == 0 and summary["reused"] == 2 * len(V8_IDS)
    assert outputs(out) == outputs(t[STD_AIM][2])
