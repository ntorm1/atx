"""Synthetic postimplementation fixtures for postmortem_v3.py (no real data, no build).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q .superpowers/sdd/mega-alpha-20260926/studies/test_postmortem_v3.py

The end-to-end world reuses the fitter's own synthetic Fixture/screen_world (atx-impl/tools/
test_fit_composition_weights.py): a real v3-admit-v1 fit with a work dir is produced, then the post-mortem
must reproduce it bit for bit (fixture 1), and its section E runs on a TRAIN-dated and a validation-dated
copy of the same panel (fixtures 4 and 5).
"""
from __future__ import annotations

import csv
import hashlib
import json
import math
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest
from scipy.stats import rankdata

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
TOOLS = REPO / "atx-impl" / "tools"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TOOLS))

import postmortem_v3 as PM  # noqa: E402

F = PM.import_fitter(TOOLS / "fit_composition_weights.py")
NR = PM.import_nav_recon()
import test_fit_composition_weights as TF  # noqa: E402  (module alias only: pytest collects nothing from it)

DAY = F.DAY_NS
SHIFT_DAYS = 1096  # 2020-05-01 + 1096 d = 2023-05-02: the validation-dated copy


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def business_sessions(first="2020-01-02", last="2022-12-29") -> np.ndarray:
    days = np.arange(np.datetime64(first), np.datetime64(last))
    return days[np.is_busday(days)].astype("datetime64[ns]").astype(np.int64)


def panel_ns(factors, sessions, ids=None, families=None, taus=None) -> SimpleNamespace:
    k = factors.shape[0]
    ids = ids or [f"c{i:03d}" for i in range(k)]
    return SimpleNamespace(
        factors=factors, taus=taus or [0.05] * k, ids=ids, families=families or ["fam"] * k, sessions=sessions,
        years=PM.years_of(sessions), fit_mask=(sessions >= F.FIT_BEGIN_NS) & (sessions < F.HOLD_BEGIN_NS),
        hold_mask=(sessions >= F.HOLD_BEGIN_NS) & (sessions < F.TRAIN_END_NS),
        admission={i: {"status": "reject_unstable"} for i in ids}, library_json={"candidates": []})


# ------------------------------------------------------------------ fixture 1a: hand-known weights
def test_known_weights_reproduce_hand_case():
    """Orthogonal +-1 patterns: sample cov is exactly diagonal, so Sh = diag(S) and w ~ mu_k / var_k."""
    sessions = np.datetime64("2020-01-01", "D") + np.arange(1096)  # calendar days 2020-01-01 .. 2022-12-31
    sessions = sessions.astype("datetime64[ns]").astype(np.int64)
    t = sessions.size
    assert t % 4 == 0
    h1 = np.tile([1.0, -1.0], t // 2)
    h2 = np.tile([1.0, 1.0, -1.0, -1.0], t // 4)
    h3 = np.tile([1.0, -1.0, -1.0, 1.0], t // 4)
    f1 = 0.001 + 0.01 * h1
    f2 = -(0.002 + 0.03 * h2)  # negative orientation: s = -1
    f3 = f1 + 0.004 * h3  # ~f1 with a lower Sharpe -> reject_redundant(f1)
    fit = (sessions >= F.FIT_BEGIN_NS) & (sessions < F.HOLD_BEGIN_NS)
    f4 = np.where(fit, 0.001, -0.001) + 0.01 * h3  # good in FIT, bad in HOLD -> reject_unstable
    factors = np.vstack([f1, f2, f3, f4])
    P = panel_ns(factors, sessions, ids=["f1", "f2", "f3", "f4"])
    res = PM.run_protocol(F, P.factors, P.taus, P.ids, P.fit_mask, P.hold_mask, np.ones(t, dtype=bool))
    status = [r["status"] for r in res["rows"]]
    assert status == ["admitted", "admitted", "reject_redundant", "reject_unstable"]
    assert res["signs"][:2] == [1, -1]
    var1 = 0.01 ** 2 * t / (t - 1)
    var2 = 0.03 ** 2 * t / (t - 1)
    raw = np.array([0.001 / var1, 0.002 / var2])
    expected = raw / raw.sum()
    np.testing.assert_allclose(res["weights"][:2], expected, rtol=1e-10, atol=0)
    assert res["weights"][2] == 0 and res["weights"][3] == 0
    series = PM.book_series(res)
    np.testing.assert_allclose(series, expected[0] * f1 - expected[1] * f2, rtol=0, atol=1e-15)


# ------------------------------------------------------------------ fixture 2: noise walk-forward
def test_pure_noise_walk_forward_is_positive_in_sample_and_zero_out_of_sample():
    sessions = business_sessions()
    is_sr, oos_sr = [], []
    for seed in range(8):
        rng = np.random.default_rng(100 + seed)
        P = panel_ns(rng.normal(0.0, 0.01, (40, sessions.size)), sessions)
        b = PM.section_b(F, P)
        for split in b["splits"]:
            if split["fit_error"] is None:
                is_sr.append(split["is"]["sharpe"])
                oos_sr.append(split["oos"]["sharpe"])
                assert split["is"]["sharpe"] > 0  # selection + MV fit always look good in sample
    assert len(is_sr) >= 40
    assert np.mean(is_sr) > 2.0
    assert abs(np.mean(oos_sr)) < 0.6
    c = PM.section_c(F, P)
    assert {s["id"] for s in c["splits"]} == {"B1", "F20"}
    assert "skipped" in c["splits"][0]["cells"]["C2"]  # no declared prior sign -> skipped and said so


# ------------------------------------------------------------------ fixture 3: bootstrap structure
def test_block_bootstrap_keeps_cross_and_auto_correlation():
    rng = np.random.default_rng(3)
    t = 1500
    x1 = np.empty(t)
    x1[0] = rng.normal()
    for i in range(1, t):
        x1[i] = 0.6 * x1[i - 1] + 0.8 * rng.normal()
    x2 = 0.8 * x1 + 0.6 * rng.normal(size=t) * x1.std()
    X = np.vstack([x1, x2])
    idx = PM.block_bootstrap_indices(t, PM.BLOCK, np.random.default_rng([7, 0]))
    assert idx.shape == (t,) and idx.min() >= 0 and idx.max() < t
    within = np.arange(t - 1) % PM.BLOCK != PM.BLOCK - 1
    assert np.all(idx[1:][within] == (idx[:-1][within] + 1) % t)  # contiguous (circular) blocks
    Xb = X[:, idx]  # one index vector for every candidate, as section_d does
    assert abs(np.corrcoef(Xb)[0, 1] - np.corrcoef(X)[0, 1]) < 0.05
    ac = lambda v: np.corrcoef(v[:-1], v[1:])[0, 1]  # noqa: E731
    assert ac(Xb[0]) > 0.45  # AR(1) phi .6 survives (x 20/21 inside blocks)
    iid = X[:, np.random.default_rng(1).integers(0, t, t)]
    assert ac(iid[0]) < 0.1  # an iid date bootstrap would destroy it
    # the null is centred: demeaned noise -> is_sharpe distribution straddles its median, admitted varies
    sessions = business_sessions()
    P = panel_ns(np.random.default_rng(4).normal(0.0005, 0.01, (12, sessions.size)), sessions)
    d = PM.merge_d(None, PM.section_d(F, P, 6, 3, float("inf")))
    assert d["reps_total"] == 6 and d["seeds"] == ["3"]
    again = PM.section_d(F, P, 6, 3, float("inf"))
    assert again["run"]["samples"] == d["runs"]["3"]["samples"]  # (seed, rep) determines the sample
    prev = PM.clean(d)
    same = PM.merge_d(prev, PM.section_d(F, P, 4, 3, float("inf")))  # same seed replaces, never doubles
    assert same["reps_total"] == 4 and same["discarded_seeds_inputs_changed"] == []
    P.factors = P.factors + 0.001  # inputs changed: earlier seeds are not pooled with the new observed
    moved = PM.merge_d(prev, PM.section_d(F, P, 2, 5, float("inf")))
    assert moved["seeds"] == ["5"] and moved["discarded_seeds_inputs_changed"] == ["3"]


# ------------------------------------------------------------------ fixture 4: paper book toy
def test_plain_paper_three_name_toy_matches_hand_series():
    r = np.array([[0.0, 0.0, 0.0],
                  [0.1, 0.0, -0.1],
                  [0.02, 0.1, -0.04],
                  [0.1, -0.05, 0.1],
                  [0.0, -0.5, 0.2],
                  [-0.1, 0.0, 0.0]])
    close = np.array([10.0, 20.0, 40.0]) * np.cumprod(1.0 + r, axis=0)
    raw = close.copy()
    raw[4:, 2] = raw[3, 2]  # adjusted +20% with a flat raw close: |log adj| > |log raw| + .10 -> guarded -> 0
    present = np.ones_like(close, dtype=np.uint8)
    panel = F.PricePanel(close, raw, np.ones_like(close), present)
    r0 = np.where(np.isnan(panel.returns), 0.0, panel.returns)
    assert r0[4, 2] == 0.0
    comb = np.full((6, 3), np.nan)
    comb[0] = [1.0, 2.0, 3.0]  # w = [-.5, 0, .5]
    comb[1] = [5.0, 5.0, 1.0]  # tie: w = [.25, .25, -.5]
    comb[2] = [np.nan, 1.0, 2.0]  # non-member: w = [0, -.5, .5]
    comb[3] = [7.0, 7.0, 7.0]  # all tied: flat
    series, tau, flat = PM.plain_paper(NR, comb, r0, 0, 4)
    # p2(d) = w(d) . r[d+2]
    np.testing.assert_allclose(series[2], [-0.5 * 0.02 + 0.5 * -0.04,
                                           0.25 * 0.1 + 0.25 * -0.05 - 0.5 * 0.1,
                                           -0.5 * -0.5 + 0.5 * 0.0,  # guarded cell realizes 0
                                           0.0], rtol=0, atol=1e-12)
    # lag test: one extra day of delay, p3(d) = w(d) . r[d+3]; the last decision has no r[d+3]
    np.testing.assert_allclose(series[3][:3], [-0.5 * 0.1 + 0.5 * 0.1, 0.25 * -0.5 - 0.5 * 0.0, 0.0],
                               rtol=0, atol=1e-12)
    assert math.isnan(series[3][3])
    np.testing.assert_allclose(series[1], [-0.5 * 0.1 + 0.5 * -0.1, 0.25 * 0.02 + 0.25 * 0.1 - 0.5 * -0.04,
                                           -0.5 * -0.05 + 0.5 * 0.1, 0.0], rtol=0, atol=1e-12)
    assert flat == 1
    assert tau == pytest.approx((2.0 + 2.0 + 1.0) / 3, abs=1e-15)


# ------------------------------------------------------------------ end-to-end world
IDS_FAMILIES = ["fam_x", "fam_x", "fam_y", "fam_y", "fam_z", "fam_z"]


def write_role(directory: Path, p: dict, sessions: np.ndarray) -> Path:
    directory.mkdir(parents=True)
    d, n = p["close"].shape
    files = {}
    for name, arr in (("sessions.i64", sessions.astype("<i8")), ("ids.u64", p["ids"].astype("<u8")),
                      ("present.u8", p["present"]), ("member.u8", p["member"]),
                      ("close.f64", p["close"].astype("<f8")), ("raw_close.f64", p["raw"].astype("<f8")),
                      ("volume.f64", p["volume"].astype("<f8"))):
        data = np.ascontiguousarray(arr).tobytes()
        (directory / name).write_bytes(data)
        files[name] = {"bytes": len(data), "sha256": sha(data)}
    manifest = {"schema": F.ROLE_SCHEMA, "status": "complete", "dates": d, "instruments": n,
                "score_begin": p["score_begin"], "score_end": d, "score_start_ns": int(sessions[p["score_begin"]]),
                "score_end_ns": int(sessions[-1] + DAY), "source_sha256": "ab" * 32, "files": files}
    path = directory / "manifest.json"
    path.write_bytes(json.dumps(manifest, indent=2).encode())
    return path


def write_run(directory: Path, prefix: str, role_path: Path, p: dict, sessions, combined, cw_bytes: bytes,
              roles_in_summary, sign_patch=None) -> str:
    """<prefix>_combined.* + summary.json + <prefix>_candidates.jsonl (with fake per-candidate IC)."""
    directory.mkdir(parents=True)
    d, n = combined.shape
    member = np.isfinite(combined).astype(np.uint8)
    files = {}
    for suffix, arr in ((".f64", combined.astype("<f8")), ("_member.u8", member), ("_finite.u8", member),
                        ("_ids.u64", p["ids"].astype("<u8")), ("_sessions.i64", sessions.astype("<i8"))):
        data = np.ascontiguousarray(arr).tobytes()
        name = f"{prefix}_combined{suffix}"
        (directory / name).write_bytes(data)
        files[name] = {"bytes": len(data), "sha256": sha(data)}
    cw = json.loads(cw_bytes)
    meta = {"schema": PM.COMBINED_SCHEMA, "status": "complete", "dates": d, "instruments": n, "files": files,
            "role": prefix, "role_manifest_sha256": sha(role_path.read_bytes()), "score_begin": p["score_begin"],
            "score_end": d, "composition_weights_sha256": sha(cw_bytes), "composition_signs": "pinned-candidate-signs",
            "library_sha256": cw["library_sha256"]}
    meta_bytes = json.dumps(meta, indent=2).encode()
    (directory / f"{prefix}_combined.json").write_bytes(meta_bytes)
    records = []
    for cid, w in cw["weights"].items():
        sign = cw["signs"].get(cid, 0)
        if sign_patch and cid in sign_patch:
            sign = sign_patch[cid]
        records.append({"id": cid, "composition_sign": sign, "composition_weight": w, "family": "f",
                        "horizons": [{"horizon": 21, "rank": {"mean": 0.0123, "oriented_mean": 0.0123}}]})
    summary = {"status": "complete", "composition_weights_sha256": sha(cw_bytes),
               "roles": [{"role": r, "candidates": records} for r in roles_in_summary]}
    (directory / "summary.json").write_bytes(json.dumps(summary).encode())
    lines = [json.dumps({"id": r["id"], "number": k, "status": "started"}) for k, r in enumerate(records)]
    lines += [json.dumps(r) for r in records]
    (directory / f"{prefix}_candidates.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return sha(meta_bytes)


def expected_plain_lag2(p: dict, combined: np.ndarray, begin: int, end: int) -> np.ndarray:
    """Independent: scipy average ranks -> centered, demeaned, gross 1; r[d+2] valid (raw == close here)."""
    close, present = p["close"], p["present"].astype(bool)
    out = []
    for d in range(begin, end):
        row = combined[d]
        idx = np.flatnonzero(np.isfinite(row))
        w = np.zeros(row.size)
        if idx.size >= 2:
            x = (rankdata(row[idx]) - 1.0) / (idx.size - 1) - 0.5
            x -= x.mean()
            w[idx] = x / np.abs(x).sum()
        t = d + 2
        ok = present[t] & present[t - 1]
        with np.errstate(all="ignore"):
            r = np.where(ok, close[t] / close[t - 1] - 1.0, 0.0)
            r = np.where(ok & (np.abs(np.log(close[t]) - np.log(close[t - 1])) <= 1.5), r, 0.0)
        out.append(float(w @ r))
    return np.array(out)


def write_nav(directory: Path, sessions, begin: int, gross: np.ndarray, combined_sha: str, role_sha: str) -> None:
    directory.mkdir(parents=True)
    (directory / "recipe.json").write_bytes(json.dumps(
        {"combined_sha256": combined_sha, "role_sha256": role_sha, "cadence": 1,
         "rule": "baseline-target-v1+neutral-price-risk-v1+band-1"}).encode())
    with open(directory / f"daily_{PM.SCENARIO}.csv", "w", newline="", encoding="utf-8") as fh:
        wr = csv.writer(fh)
        wr.writerow(["session_index", "session_ns", "return_observation", "gross_return", "net_return"])
        wr.writerow([begin + 1, int(sessions[begin + 1]), 0, "", ""])  # deployment row: not an observation
        for j, g in enumerate(gross):
            wr.writerow([begin + j + 2, int(sessions[begin + j + 2]), 1, repr(float(g)), repr(float(g) - 1e-4)])


@pytest.fixture(scope="module")
def world(tmp_path_factory):
    root = tmp_path_factory.mktemp("pmworld")
    p, signals, ids = TF.screen_world()
    fx = TF.Fixture(root / "fx", p, signals, TF.SCREEN_RUNNER_SIGNS, ids=ids, families=IDS_FAMILIES,
                    vm_identity="dslvm1_clang18.1_fma", field_ids={"slow_b", "flip"})
    code, _ = F.fit(fx.args(root / "weights", "v3-admit-v1", work_dir=root / "work"))
    assert code == F.EXIT_OK
    cw_bytes = (root / "weights" / "composition_weights.json").read_bytes()
    member = p["member"] == 1
    combined = np.where(member, signals[0] + 0.3 * np.nan_to_num(signals[2]), np.nan)
    begin, end = p["score_begin"], p["close"].shape[0] - 2
    gross = expected_plain_lag2(p, combined, begin, end)
    train_sha = sha(fx.manifest.read_bytes())
    train_comb_sha = write_run(root / "train-run", "train", fx.manifest, p, p["sessions"], combined, cw_bytes, ["train"])
    write_nav(root / "train-nav", p["sessions"], begin, gross, train_comb_sha, train_sha)
    val_sessions = p["sessions"] + SHIFT_DAYS * DAY
    val_role = write_role(root / "val-role", p, val_sessions)
    val_sha = sha(val_role.read_bytes())
    val_comb_sha = write_run(root / "val-run", "validation", val_role, p, val_sessions, combined, cw_bytes,
                             ["train", "validation"])
    write_nav(root / "val-nav", val_sessions, begin, gross, val_comb_sha, val_sha)
    cw = json.loads(cw_bytes)
    weighted = sorted(i for i, w in cw["weights"].items() if w > 0)
    write_run(root / "val-run-bad", "validation", val_role, p, val_sessions, combined, cw_bytes, ["validation"],
              sign_patch={weighted[0]: -cw["signs"][weighted[0]]})
    argv = ["--fitter", str(TOOLS / "fit_composition_weights.py"), "--work-dir", str(root / "work"),
            "--weights-dir", str(root / "weights"), "--library", str(fx.library), "--train-role", str(fx.manifest),
            "--train-run", str(root / "train-run"), "--train-nav", str(root / "train-nav"),
            "--val-role", str(val_role), "--val-run", str(root / "val-run"), "--val-nav", str(root / "val-nav")]
    paths = PM.parse_args(["--output", str(root / "unused"), *argv])
    return SimpleNamespace(root=root, fx=fx, ids=ids, p=p, cw=cw, gross=gross, argv=argv, paths=paths,
                           t=end - begin)


# ------------------------------------------------------------------ fixture 1b: a real fitter run reproduces
def test_section_a_reproduces_the_fitter_bit_for_bit(world):
    P = PM.load_train_panel(F, world.paths)
    a = PM.section_a(F, P)
    rep = a["reproduction"]
    assert rep["weights_bitwise_equal"] and rep["weights_max_abs_diff"] == 0.0
    assert rep["mv_solution_max_abs_diff"] == 0.0
    assert rep["admitted_equal"] and rep["admitted_count"] >= 2
    assert rep["status_mismatch_ids"] == [] and rep["screen_sign_mismatch_ids"] == []
    assert rep["pinned_sign_mismatch_ids"] == [] and rep["fitter_unchanged"]
    assert rep["blend_diagnostic_recomputed"] == rep["blend_diagnostic_frozen"]
    ins = a["in_sample"]
    assert sum(v["pnl_share"] for v in ins["by_year"].values()) == pytest.approx(1.0)
    assert ins["sharpe"] == pytest.approx(rep["blend_diagnostic_frozen"]["factor_sharpe_annualized"], rel=1e-12)
    f = PM.section_f(P)
    assert sum(r["pnl"] for r in f["candidates"]) == pytest.approx(f["total_pnl"], rel=1e-12)
    assert f["total_pnl"] == pytest.approx(ins["pnl"], rel=1e-12)
    assert {r["id"] for r in f["candidates"]} == {i for i, w in world.cw["weights"].items() if w > 0}


def test_tampered_or_foreign_factor_record_is_refused(world, tmp_path):
    import shutil
    work = tmp_path / "work"
    shutil.copytree(world.root / "work", work)
    rec = next(work.rglob("factors/*.json"))
    j = json.loads(rec.read_bytes())
    j["tau"] = j["tau"] + 1e-9  # seal no longer matches
    rec.write_bytes(json.dumps(j).encode())
    paths = PM.parse_args(["--output", str(tmp_path / "o"), *world.argv, "--work-dir", str(work)])
    with pytest.raises(PM.PostmortemError, match="seal"):
        PM.load_train_panel(F, paths)


# ------------------------------------------------------------------ fixtures 4 + 5: section E
def test_section_e_train_and_validation_book_level_only(world):
    out = PM.section_e(F, NR, world.paths, ["train", "val"], lambda: PM.load_train_panel(F, world.paths))
    tr, va = out["train"], out["val"]
    assert tr["context"]["source"] == "work-cache" and tr["context"]["matches_frozen_fit_context"]
    assert va["context"]["source"] == "built"
    for r in (tr, va):
        assert r["decisions"] == world.t
        assert r["context"]["forward_lag2_max_abs_diff"] == 0.0
        c = r["nav"]["corr"]["plain"]
        assert c["lag2"]["n"] == world.t and c["lag2"]["corr_gross"] > 1 - 1e-12  # NAV == hand plain lag-2 book
        assert c["best_lag"] == "lag2"
        assert r["paper"]["plain"]["lag2"]["sharpe"] == pytest.approx(PM.sharpe(world.gross), rel=1e-12)
        m = r["metadata"]
        assert m["combined_binds_weights"] and m["nav_binds_combined"] and m["nav_binds_role"]
        assert m["summary_binds_weights"]
    # identical arrays, validation-dated copy: the non-TRAIN path gives the same books (Context.build == cached)
    for book in ("plain", "neutral"):
        for lag in ("lag1", "lag2", "lag3"):
            assert va["paper"][book][lag] == pytest.approx(tr["paper"][book][lag], rel=1e-12)
    assert va["metadata"]["summary_role_validation"]["sign_mismatches"] == 0
    assert va["metadata"]["summary_role_validation"]["weights_exact"]
    assert va["metadata"]["validation_candidates_jsonl"]["compared"] == len(world.ids)
    assert tr["frozen_factor_blend"]["corr_neutral_lag2"] is not None
    # fixture 5: nothing on the validation side names a candidate
    text = json.dumps(PM.clean(va))
    for cid in world.ids:
        assert cid not in text, cid
    assert "frozen_factor_blend" not in va


def test_validation_sign_drift_is_counted_without_naming_the_candidate(world):
    paths = PM.parse_args(["--output", str(world.root / "unused"), *world.argv,
                           "--val-run", str(world.root / "val-run-bad")])
    out = PM.section_e(F, NR, paths, ["val"], None)["val"]
    m = out["metadata"]["summary_role_validation"]
    assert m["sign_mismatches"] == 1 and m["sign_mismatches_weighted"] == 1
    assert out["metadata"]["validation_candidates_jsonl"]["sign_mismatches"] == 1
    text = json.dumps(PM.clean(out))
    for cid in world.ids:
        assert cid not in text, cid


# ------------------------------------------------------------------ CLI: split invocations merge
def test_cli_split_invocations_merge_into_one_report(world, tmp_path):
    out = tmp_path / "pm"
    base = ["--output", str(out), *world.argv]
    assert PM.main([*base, "--sections", "A,B,C,F,D", "--reps", "4", "--seed", "1"]) == 0
    assert PM.main([*base, "--sections", "D", "--reps", "3", "--seed", "2"]) == 0
    assert PM.main([*base, "--sections", "E", "--e-roles", "train"]) == 0
    assert PM.main([*base, "--sections", "E", "--e-roles", "val"]) == 0
    doc = json.loads((out / "postmortem.json").read_text())
    assert set(doc["sections"]) == set(PM.SECTIONS)
    assert doc["sections"]["D"]["reps_total"] == 7 and doc["sections"]["D"]["seeds"] == ["1", "2"]
    assert set(doc["sections"]["E"]) == {"train", "val"}
    md = (out / "postmortem.md").read_text(encoding="utf-8")
    for head in ("## A.", "## B.", "## C.", "## D.", "## E.", "## F.", "### train", "### validation"):
        assert head in md, head
    assert "render failed" not in md
    assert PM.main([*base, "--sections", "Z"]) == 1  # refused, nothing half-written
