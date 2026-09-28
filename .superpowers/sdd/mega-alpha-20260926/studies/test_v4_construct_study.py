"""Synthetic postimplementation fixtures for v4_construct_study.py (no real data, no build).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .superpowers/sdd/mega-alpha-20260926/studies/test_v4_construct_study.py

Unit fixtures pin the FF49 grouping rule, the neutralizer, the liquidity scale, the partial-adjustment
recursion and the cost proxy by hand. The end-to-end world is a TRAIN-dated synthetic role (industry +
market factor returns, a signal with an industry bet, member gaps) with a saved combined, a fields
directory and a NAV CSV whose gross column is an independent lag-2 computation of the P0 book.
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

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
TOOLS = REPO / "atx-impl" / "tools"
sys.path.insert(0, str(HERE))

import postmortem_v3 as PM  # noqa: E402
import v4_construct_study as CS  # noqa: E402

FITTER = TOOLS / "fit_composition_weights.py"
F = CS.bind_fitter(FITTER)
DAY = F.DAY_NS


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ------------------------------------------------------------------ unit: grouping rule
def test_ff49_rule_keeps_large_groups_and_merges_small_ones_into_ff12_parent_or_unknown():
    nan = np.nan
    ff49 = np.array([1, 1, 1, 1, 1, 2, 2, 2, nan, nan, 3, 3, 3, 3, 3, 3, 4], dtype=float)
    ff12 = np.array([1, 1, 1, 1, 1, 5, 5, 5, 5, nan, 2, 2, 2, 2, 2, 2, 5], dtype=float)
    lab = CS.ff49_labels(ff49, ff12, 5)
    r = CS.REMAINDER
    assert lab.tolist() == [1] * 5 + [r + 5] * 4 + [CS.UNKNOWN] + [3] * 6 + [r + 5]
    # a larger minimum: group 1 (5 names) falls to remainder 101, which has 5 < 6 names -> unknown; the
    # remainder 105 (5 names) -> unknown; group 3 (6 names) survives
    lab6 = CS.ff49_labels(ff49, ff12, 6)
    assert lab6.tolist() == [CS.UNKNOWN] * 10 + [3] * 6 + [CS.UNKNOWN]
    assert CS.ff12_labels(ff12).tolist() == [1] * 5 + [5] * 4 + [CS.UNKNOWN] + [2] * 6 + [5]


def test_dummy_columns_drop_the_largest_group():
    assert CS.dummy_columns(np.array([7, 7, 7])).shape == (3, 0)
    lab = np.array([2, 0, 0, 5, 0, 2])
    d = CS.dummy_columns(lab)
    assert d.shape == (6, 2)  # groups {0 (3 names, reference), 2, 5}
    np.testing.assert_array_equal(d, [[1, 0], [0, 0], [0, 0], [0, 1], [0, 0], [1, 0]])


# ------------------------------------------------------------------ unit: neutralizer
def test_normalized_residual_is_group_and_price_risk_neutral_and_matches_an_independent_ols():
    rng = np.random.default_rng(11)
    m = 240
    z = rng.normal(size=(m, 3))
    base = np.linalg.qr(np.column_stack([np.ones(m), z]))[0]  # as the fitter's context basis
    lab = rng.integers(0, 6, size=m)
    lab[:3] = 9  # a 3-name group
    y = rng.normal(size=m)
    span = CS.orthonormal_span(np.column_stack([base, CS.dummy_columns(lab)]))
    assert span.shape[1] == 3 + np.unique(lab).size  # [1, z] + G dummies - 1 reference == 3 + G
    w, live = CS.normalized_residual(span, y)
    assert live and abs(np.abs(w).sum() - 1.0) < 1e-12
    assert np.max(np.abs(z.T @ w)) < 1e-12 and abs(w.sum()) < 1e-12
    for g in np.unique(lab):
        assert abs(w[lab == g].sum()) < 1e-12
    full = np.column_stack([np.ones(m), z, (lab[:, None] == np.unique(lab)[None, :]).astype(float)])
    res = y - full @ (np.linalg.pinv(full) @ y)  # rank-deficient full dummy set: pinv OLS
    np.testing.assert_allclose(w, res / np.abs(res).sum(), rtol=0, atol=1e-12)
    # a vector inside the span has no residual: not live, zero book
    w0, live0 = CS.normalized_residual(span, base[:, 1] * 3.0)
    assert not live0 and not w0.any()


def test_liquidity_scale_hand_values_and_refusal():
    s = CS.liquidity_scale(np.array([1.0, 2.0, 3.0, 4.0, 100.0]))
    np.testing.assert_allclose(s, [math.sqrt(1 / 3), math.sqrt(2 / 3), 1.0, 1.0, 1.0], rtol=0, atol=1e-15)
    with pytest.raises(CS.StudyError):
        CS.liquidity_scale(np.array([1.0, 0.0, 3.0]))


# ------------------------------------------------------------------ unit: partial adjustment + cost proxy
def test_partial_book_hand_recursion_with_hold_and_forced_exit():
    target = np.array([[0.5, -0.5, 0.0], [0.0, 0.5, -0.5], [0.5, 0.0, -0.5], [-0.5, 0.5, 0.0]])
    live = np.array([True, True, False, True])
    member = np.ones((4, 3), dtype=bool)
    member[3, 1] = False  # name 1 leaves the universe at decision 3: forced exit, then target move
    w, lw = CS.partial_book(target, live, member, 0.25)
    np.testing.assert_allclose(w[0], [0.5, -0.5, 0.0], atol=1e-15)  # deployed at the first live target
    np.testing.assert_allclose(w[1], [0.375, -0.25, -0.125], atol=1e-15)
    np.testing.assert_allclose(w[2], w[1], atol=0)  # non-live target: hold
    np.testing.assert_allclose(w[3], [0.15625, 0.125, -0.09375], atol=1e-15)
    assert lw.all()
    s = CS.book_series(w, np.zeros((4, 3)))
    np.testing.assert_allclose(s["turnover"], [0.0, 0.125 + 0.25 + 0.125, 0.0, 0.21875 + 0.375 + 0.03125], atol=1e-15)
    # a late first live target: flat until then
    w2, _ = CS.partial_book(target, np.array([False, True, True, True]), np.ones((4, 3), dtype=bool), 0.25)
    assert not w2[0].any() and np.array_equal(w2[1], target[1])


def test_book_series_cost_proxy_and_stats():
    w = np.array([[0.5, -0.5], [0.25, -0.25], [-0.5, 0.5], [-0.5, 0.5]])
    fwd = np.array([[0.02, -0.01], [0.01, 0.0], [-0.02, 0.01], [0.0, 0.03]])
    s = CS.book_series(w, fwd, 20.0)
    np.testing.assert_allclose(s["pnl"], [0.015, 0.0025, 0.015, 0.015], atol=1e-15)
    np.testing.assert_allclose(s["turnover"], [0.0, 0.5, 1.5, 0.0], atol=1e-15)  # deployment excluded
    np.testing.assert_allclose(s["cost"], [0.0, 0.001, 0.003, 0.0], atol=1e-18)
    np.testing.assert_allclose(s["net"], s["pnl"] - s["cost"], atol=0)
    years = np.array([2020, 2020, 2021, 2021])
    st = CS.book_stats(s, years)
    assert st["tau_mean"] == pytest.approx(2.0 / 3, abs=1e-15)
    assert st["tau_per_prior_gross_mean"] == pytest.approx((0.5 / 1 + 1.5 / 0.5 + 0 / 1) / 3, abs=1e-15)
    assert st["cost_proxy_total"] == pytest.approx(0.004, abs=1e-18)
    assert st["cost_proxy_ann"] == pytest.approx(0.001 * 252, abs=1e-12)
    assert set(st["by_year_sharpe"]) == {2020, 2021}
    assert st["gross"]["sharpe"] == pytest.approx(PM.sharpe(s["pnl"]), rel=1e-12)


# ------------------------------------------------------------------ end-to-end world
N = 90


def business_sessions(first="2019-06-03", last="2021-04-01") -> np.ndarray:
    days = np.arange(np.datetime64(first), np.datetime64(last))
    return days[np.is_busday(days)].astype("datetime64[ns]").astype(np.int64)


def write_arrays(directory: Path, arrays) -> dict:
    files = {}
    for name, arr in arrays:
        data = np.ascontiguousarray(arr).tobytes()
        (directory / name).write_bytes(data)
        files[name] = {"bytes": len(data), "sha256": sha(data)}
    return files


def write_role(directory: Path, w: SimpleNamespace, sessions: np.ndarray, end_ns: int | None = None) -> Path:
    directory.mkdir(parents=True)
    d, n = w.close.shape
    files = write_arrays(directory, (("sessions.i64", sessions.astype("<i8")), ("ids.u64", w.ids.astype("<u8")),
                                     ("present.u8", w.present), ("member.u8", w.member),
                                     ("close.f64", w.close.astype("<f8")), ("raw_close.f64", w.close.astype("<f8")),
                                     ("volume.f64", w.volume.astype("<f8"))))
    manifest = {"schema": F.ROLE_SCHEMA, "status": "complete", "dates": d, "instruments": n,
                "score_begin": w.score_begin, "score_end": d, "score_start_ns": int(sessions[w.score_begin]),
                "score_end_ns": int(end_ns if end_ns is not None else sessions[-1] + DAY),
                "source_sha256": "ab" * 32, "files": files}
    path = directory / "manifest.json"
    path.write_bytes(json.dumps(manifest, indent=2).encode())
    return path


def write_fields(directory: Path, w: SimpleNamespace, role_sha: str, last_session: str) -> str:
    directory.mkdir(parents=True)
    d, n = w.close.shape
    arrays = (("grp_ff12.f64", w.ff12), ("grp_ff49.f64", w.ff49), ("me_company.f64", w.me))
    files = write_arrays(directory, [(k, v.astype("<f8")) for k, v in arrays])
    fields = [{"name": k[:-4], "file": k, "dtype": "<f8", "shape": [d, n], "sha256": files[k]["sha256"]}
              for k, _ in arrays]
    manifest = {"schema": CS.FIELDS_SCHEMA, "status": "complete", "files": files, "fields": fields,
                "role": {"manifest_sha256": role_sha, "dates": d, "instruments": n, "last_session": last_session}}
    raw = json.dumps(manifest, indent=2).encode()
    (directory / "manifest.json").write_bytes(raw)
    return sha(raw)


def write_combined(directory: Path, w: SimpleNamespace, role_path: Path, sessions, fields_sha: str) -> str:
    directory.mkdir(parents=True)
    d, n = w.combined.shape
    member = np.isfinite(w.combined).astype(np.uint8)
    files = {}
    for suffix, arr in ((".f64", w.combined.astype("<f8")), ("_member.u8", member), ("_finite.u8", member),
                        ("_ids.u64", w.ids.astype("<u8")), ("_sessions.i64", sessions.astype("<i8"))):
        files.update(write_arrays(directory, ((f"train_combined{suffix}", arr),)))
    meta = {"schema": PM.COMBINED_SCHEMA, "status": "complete", "dates": d, "instruments": n, "files": files,
            "role": "train", "role_manifest_sha256": sha(role_path.read_bytes()), "score_begin": w.score_begin,
            "score_end": d, "composition_weights_sha256": "cd" * 32, "research_fields_manifest_sha256": fields_sha}
    raw = json.dumps(meta, indent=2).encode()
    (directory / "train_combined.json").write_bytes(raw)
    return sha(raw)


def write_nav(directory: Path, sessions, begin: int, gross: np.ndarray, comb_sha: str, role_sha: str,
              extra_row_ns: int | None = None) -> None:
    directory.mkdir(parents=True)
    (directory / "recipe.json").write_bytes(json.dumps(
        {"combined_sha256": comb_sha, "role_sha256": role_sha, "cadence": 1, "band_multiple": 1.0,
         "trade_fraction": 1.0, "rule": "baseline-target-v1+neutral-price-risk-v1+band-1"}).encode())
    with open(directory / f"daily_{CS.SCENARIO}.csv", "w", newline="", encoding="utf-8") as fh:
        wr = csv.writer(fh)
        wr.writerow(["session_index", "session_ns", "return_observation", "gross_return", "net_return"])
        wr.writerow([begin + 1, int(sessions[begin + 1]), 0, "", ""])  # deployment row
        for j, g in enumerate(gross):
            wr.writerow([begin + j + 2, int(sessions[begin + j + 2]), 1, repr(float(g)), repr(float(g) - 2e-4)])
        if extra_row_ns is not None:
            wr.writerow([len(sessions), extra_row_ns, 1, "0.0", "0.0"])


def make_world() -> SimpleNamespace:
    rng = np.random.default_rng(26)
    sessions = business_sessions()
    d, n = sessions.size, N
    score_begin = int(np.searchsorted(sessions, F.FIT_BEGIN_NS))
    ind = 1 + np.arange(n) % 12
    beta = rng.uniform(0.5, 1.5, n)
    mkt = rng.normal(0.0003, 0.01, d)
    ind_ret = rng.normal(0.0, 0.006, (d, 13))
    r = beta[None, :] * mkt[:, None] + ind_ret[:, ind] + rng.normal(0.0, 0.012, (d, n))
    r[0] = 0.0
    close = 40.0 * np.exp(rng.normal(0, 0.3, n))[None, :] * np.cumprod(1.0 + r, axis=0)
    volume = np.exp(rng.normal(12.0, 1.2, n))[None, :] * np.exp(rng.normal(0.0, 0.3, (d, n)))
    present = np.ones((d, n), dtype=np.uint8)
    member = np.ones((d, n), dtype=np.uint8)
    member[score_begin + 40:score_begin + 70, :10] = 0  # a member gap: P4 forced exits
    member[score_begin + 100:, 85:] = 0  # permanent exits
    fwd = np.full((d, n), np.nan)
    fwd[:-2] = r[2:]
    ind_bet = rng.normal(0.0, 1.0, 13)[ind]  # a persistent industry tilt in the signal
    combined = 0.2 * np.nan_to_num(fwd) / 0.015 + 0.5 * ind_bet[None, :] + rng.normal(0.0, 1.0, (d, n))
    combined = np.where(member == 1, combined, np.nan)
    ff12 = np.broadcast_to(ind.astype(float), (d, n)).copy()
    ff12[:, np.arange(n) % 15 == 0] = np.nan  # unknown industry names
    ff49 = np.broadcast_to((1 + np.arange(n) % 20).astype(float), (d, n)).copy()  # groups of 4-5 names
    ff49[:, np.arange(n) % 17 == 3] = np.nan  # FF49-unmapped, FF12 known
    me = close * np.exp(rng.normal(16.0, 1.0, n))[None, :]
    me[:, np.arange(n) % 11 == 0] = np.nan
    return SimpleNamespace(sessions=sessions, score_begin=score_begin, ids=np.arange(1, n + 1, dtype=np.uint64),
                           close=close, volume=volume, present=present, member=member, combined=combined,
                           ff12=ff12, ff49=ff49, me=me)


def independent_p0_gross(role_path: Path, w: SimpleNamespace) -> tuple[np.ndarray, object, object]:
    """P0 via the fitter's Context.book, P&L from close ratios directly (not the context's forward)."""
    role = F.RoleManifest(role_path, sha(role_path.read_bytes()))
    ctx = F.Context.build(role)
    comb = np.where((w.member == 1) & np.isfinite(w.combined), w.combined, np.nan)
    q, _ = ctx.book(comb, 1)
    c = w.close[:, ctx.columns]
    out = np.array([float(q[j] @ (c[ctx.begin + j + 2] / c[ctx.begin + j + 1] - 1.0)) for j in range(q.shape[0])])
    return out, ctx, q


@pytest.fixture(scope="module")
def world(tmp_path_factory):
    root = tmp_path_factory.mktemp("csworld")
    w = make_world()
    role_path = write_role(root / "role", w, w.sessions)
    role_sha = sha(role_path.read_bytes())
    fields_sha = write_fields(root / "fields", w, role_sha, "2021-03-31")
    comb_sha = write_combined(root / "run", w, role_path, w.sessions, fields_sha)
    gross, ctx, q0 = independent_p0_gross(role_path, w)
    write_nav(root / "nav", w.sessions, w.score_begin, gross, comb_sha, role_sha)
    argv = ["--fitter", str(FITTER), "--train-role", str(role_path), "--train-run", str(root / "run"),
            "--fields", str(root / "fields"), "--train-nav", str(root / "nav")]
    out = root / "out"
    assert CS.main(["--output", str(out), *argv]) == 0
    doc = json.loads((out / "construct_study.json").read_text())
    md = (out / "construct_study.md").read_text(encoding="utf-8")
    return SimpleNamespace(root=root, w=w, role_path=role_path, role_sha=role_sha, fields_sha=fields_sha,
                           comb_sha=comb_sha, gross=gross, ctx=ctx, q0=q0, argv=argv, doc=doc, md=md)


def slabs(world):
    ctx, w = world.ctx, world.w
    b, e, cols = ctx.begin, ctx.end, ctx.columns
    comb = np.where((w.member == 1) & np.isfinite(w.combined), w.combined, np.nan)
    p = F.PricePanel(w.close, w.close, w.volume, w.present)
    adv = np.array([p.dollars[d + 1 - F.ADV_WINDOW:d + 1][:, cols].sum(axis=0) / F.ADV_WINDOW for d in range(b, e)])
    member = ((w.member == 1) & (w.present == 1))[b:e][:, cols]
    return comb, w.ff12[b:e][:, cols], w.ff49[b:e][:, cols], adv, member


def test_world_p0_is_the_fitter_book_and_the_nav_alignment_is_lag_2(world):
    doc = world.doc
    b0 = doc["books"]["P0"]
    assert doc["context"]["forward_lag2_max_abs_diff"] == 0.0 and doc["context"]["refused_decisions"] == 0
    assert b0["nav"]["n"] == world.q0.shape[0] and b0["nav"]["corr_gross"] > 1 - 1e-12
    assert b0["gross"]["sharpe"] == pytest.approx(PM.sharpe(world.gross), rel=1e-10)
    assert b0["tau_equals_fitter_standalone_turnover"] is True
    al = b0["nav_alignment"]
    assert al["best_lag"] == "lag2" and al["lag2"]["corr_gross"] > 1 - 1e-12 and al["lag2"]["n"] == world.q0.shape[0]
    assert al["lag1"]["corr_gross"] < 0.5 and al["lag3"]["corr_gross"] < 0.5 and al["lag3"]["n"] == world.q0.shape[0] - 1
    assert b0["tau_mean"] == pytest.approx(F.standalone_turnover(world.q0), rel=0, abs=0)
    r = doc["receipts"]
    assert r["combined_binds_fields"] and not r["role_is_v4_pin"] and not r["combined_is_v4_pin"]
    assert doc["nav"]["binds_combined"] and doc["nav"]["binds_role"]
    # P0 series == postmortem section E's neutral lag-2 paper on the same book
    comb, ff12, ff49, adv, member = slabs(world)
    books, live, _ = CS.construct_books(world.ctx, comb, ff12, ff49, adv, member, ["P0"])
    np.testing.assert_array_equal(books["P0"], world.q0)
    p = F.PricePanel(world.w.close, world.w.close, world.w.volume, world.w.present)
    r0 = np.where(np.isnan(p.returns), 0.0, p.returns)
    pm = PM.neutral_paper(world.q0, r0, world.ctx.columns, world.ctx.begin, (2,))[2]
    np.testing.assert_allclose(CS.book_series(books["P0"], world.ctx.forward)["pnl"], pm, rtol=0, atol=1e-15)


def test_world_industry_books_are_group_neutral_and_price_risk_neutral(world):
    ctx = world.ctx
    comb, ff12, ff49, adv, member = slabs(world)
    books, live, info = CS.construct_books(ctx, comb, ff12, ff49, adv, member, ["P0", "P1", "P2", "P3"])
    assert live["P1"].all() and live["P2"].all() and live["P3"].all()
    for j in range(ctx.used.shape[0]):
        u = np.flatnonzero(ctx.used[j])
        base = ctx.basis[j][:, u]
        lab12 = CS.ff12_labels(ff12[j, u])
        lab49 = CS.ff49_labels(ff49[j, u], ff12[j, u])
        for name, lab in (("P1", lab12), ("P2", lab49), ("P3", lab12)):
            wv = books[name][j, u]
            assert abs(np.abs(books[name][j]).sum() - 1.0) < 1e-12
            assert np.max(np.abs(base @ wv)) < 1e-12, (name, j)
            for g in np.unique(lab):
                assert abs(wv[lab == g].sum()) < 1e-12, (name, j, g)
            assert not books[name][j][~ctx.used[j]].any()
    # the signal carries an industry tilt: P0 has FF12 net exposure, P1/P3 none
    doc = world.doc["books"]
    assert doc["P0"]["ff12_net_abs_mean"] > 0.05
    assert doc["P1"]["ff12_net_abs_mean"] < 1e-10 and doc["P3"]["ff12_net_abs_mean"] < 1e-10
    assert doc["P3"]["gross_share_below_median_adv"] < doc["P1"]["gross_share_below_median_adv"]
    g = world.doc["groups"]
    assert g["groups12_mean"] == pytest.approx(13.0, abs=0.5)  # 12 codes + unknown
    assert 0 < g["ff49_remainder_share_mean"] < 1 and g["unknown49_share_mean"] > 0


def test_world_degenerate_designs_reduce_to_the_simpler_book(world):
    ctx = world.ctx
    comb, ff12, ff49, adv, member = slabs(world)
    # one industry (all unknown): the dummies add nothing -> P1 == P0 and P2 == P0
    none = np.full(ff12.shape, np.nan)
    books, _, _ = CS.construct_books(ctx, comb, none, none, adv, member, ["P0", "P1", "P2"])
    np.testing.assert_allclose(books["P1"], books["P0"], rtol=0, atol=1e-13)
    np.testing.assert_allclose(books["P2"], books["P0"], rtol=0, atol=1e-13)
    # equal ADV: every scale is 1 -> P3 == P1
    books, _, _ = CS.construct_books(ctx, comb, ff12, ff49, np.ones_like(adv), member, ["P1", "P3"])
    np.testing.assert_allclose(books["P3"], books["P1"], rtol=0, atol=1e-13)


def test_world_partial_book_tracks_p1_with_less_turnover(world):
    ctx = world.ctx
    comb, ff12, ff49, adv, member = slabs(world)
    books, live, info = CS.construct_books(ctx, comb, ff12, ff49, adv, member, ["P1", "P4"])
    w4, _ = CS.partial_book(books["P1"], live["P1"], member, CS.PARTIAL)
    np.testing.assert_array_equal(books["P4"], w4)
    doc = world.doc["books"]
    assert doc["P4"]["tau_mean"] < 0.5 * doc["P1"]["tau_mean"]
    assert doc["P4"]["mean_gross"] < 1.0 and doc["P1"]["mean_gross"] == pytest.approx(1.0, abs=1e-12)
    assert doc["P4"]["tracking_l1_mean"] > 0 and doc["P4"]["corr_pnl_with_P1"] > 0
    # forced exits: names outside the member mask hold nothing on those decisions
    assert not books["P4"][~member].any()
    for name in ("P1", "P2", "P3", "P4"):
        v = doc[name]["vs_P0"]
        assert v["tau_ratio"] is not None and v["corr_pnl"] is not None
        assert doc[name]["cost_proxy_total"] == pytest.approx(
            CS.COST_BPS * 1e-4 * doc[name]["tau_mean"] * (world.q0.shape[0] - 1), rel=1e-9)


def test_world_markdown_lists_every_book(world):
    for name in CS.BOOKS:
        assert f"| {name} |" in world.md
    assert "render failed" not in world.md and "## Groups" in world.md


# ------------------------------------------------------------------ refusals (TRAIN only, bindings)
def test_refuses_a_role_scored_past_2022(world, tmp_path):
    shifted = world.w.sessions + 1096 * DAY  # 2019-06 .. 2021-03 -> 2022-06 .. 2024-03
    role = write_role(tmp_path / "role", world.w, shifted)
    argv = [*world.argv, "--train-role", str(role)]
    assert CS.main(["--output", str(tmp_path / "o"), *argv]) == 1
    assert not (tmp_path / "o" / "construct_study.json").exists()


def test_refuses_a_nav_row_dated_2023(world, tmp_path, capsys):
    write_nav(tmp_path / "nav", world.w.sessions, world.w.score_begin, world.gross, world.comb_sha, world.role_sha,
              extra_row_ns=CS.TRAIN_END_NS)
    assert CS.main(["--output", str(tmp_path / "o"), *world.argv, "--train-nav", str(tmp_path / "nav")]) == 1
    assert "TRAIN only" in capsys.readouterr().err


def test_refuses_fields_bound_to_another_role(world, tmp_path, capsys):
    write_fields(tmp_path / "fields", world.w, "ef" * 32, "2021-03-31")
    assert CS.main(["--output", str(tmp_path / "o"), *world.argv, "--fields", str(tmp_path / "fields")]) == 1
    assert "another role" in capsys.readouterr().err
    write_fields(tmp_path / "fields2", world.w, world.role_sha, "2023-01-02")
    assert CS.main(["--output", str(tmp_path / "o"), *world.argv, "--fields", str(tmp_path / "fields2")]) == 1
    assert "TRAIN-only" in capsys.readouterr().err


def test_refuses_unknown_books(world, tmp_path):
    assert CS.main(["--output", str(tmp_path / "o"), *world.argv, "--books", "P0,P9"]) == 1
