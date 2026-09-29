"""Synthetic known-answer checks for alpha_report_card (no real data).

Run: python -m pytest atx-impl/tools/test_alpha_report_card.py -q
"""
import contextlib
import datetime as dt
import hashlib
import io
import json
import math
from pathlib import Path
import re
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import alpha_report_card as arc  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
from mega_report import components as C  # noqa: E402

DAY = fcw.DAY_NS
VM = "dslvm1_clang18.1_fma"


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


# ------------------------------------------------------------------ literal references (independent of the tool)
def ref_guard(close, raw, present):
    d, n = close.shape
    out = np.zeros((d, n), dtype=np.int64)
    for t in range(1, d):
        for i in range(n):
            bad = False
            a, b = close[t - 1, i], close[t, i]
            if present[t - 1, i] and present[t, i] and math.isfinite(a) and math.isfinite(b) and a > 0 and b > 0:
                r = math.log(b) - math.log(a)
                bad = abs(r) > 1.5
                ra, rb = raw[t - 1, i], raw[t, i]
                if math.isfinite(ra) and math.isfinite(rb) and ra > 0 and rb > 0:
                    bad = bad or abs(r) > abs(math.log(rb) - math.log(ra)) + 0.10
            out[t, i] = out[t - 1, i] + int(bad)
    return out


def ref_label(p, bad, d, i, entry, exit_):
    """The runner's label for decision d and name i between sessions entry and exit (NaN when invalid)."""
    dd = p["close"].shape[0]
    if exit_ >= dd or not (p["present"][d, i] and p["member"][d, i]):
        return math.nan
    if not (p["present"][entry, i] and p["present"][exit_, i]) or bad[exit_, i] != bad[entry, i]:
        return math.nan
    c0, c1, cd = p["close"][entry, i], p["close"][exit_, i], p["close"][d, i]
    if not (math.isfinite(cd) and cd > 0 and math.isfinite(c0) and math.isfinite(c1) and c0 > 0 and c1 > 0):
        return math.nan
    r = c1 / c0 - 1.0
    return r if math.isfinite(r) else math.nan


def ref_avg_ranks(values):
    order = sorted(range(len(values)), key=lambda j: (values[j], j))
    ranks = [0.0] * len(values)
    k = 0
    while k < len(order):
        m = k
        while m + 1 < len(order) and values[order[m + 1]] == values[order[k]]:
            m += 1
        for j in range(k, m + 1):
            ranks[order[j]] = (k + m) / 2.0
        k = m + 1
    return ranks


def ref_pearson(x, y):
    n = len(x)
    if n < 3:
        return math.nan
    mx, my = sum(x) / n, sum(y) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else math.nan


def ref_labels(p, bad, h):
    """The runner's cumulative label for every (decision, name)."""
    d, n = p["close"].shape
    out = np.full((d, n), np.nan)
    for d_ in range(p["score_begin"], d):
        for i in range(n):
            out[d_, i] = ref_label(p, bad, d_, i, d_ + 1, d_ + 1 + h)
    return out


def ref_runner_ic(p, labels, signal, d, min_names):
    """ic_screen.cpp evaluate_row: Spearman re-ranked on the paired subset, with the coverage rules."""
    n = p["close"].shape[1]
    elig = [i for i in range(n) if p["present"][d, i] and p["member"][d, i]]
    lab = {i: labels[d, i] for i in elig}
    labels = [i for i in elig if math.isfinite(lab[i])]
    pairs = [i for i in labels if math.isfinite(signal[d, i])]
    if len(labels) < min_names or len(labels) < 0.8 * len(elig):
        return math.nan
    if len(pairs) < min_names or len(pairs) < 0.8 * len(labels) or len(pairs) < 0.8 * len(elig):
        return math.nan
    return ref_pearson(ref_avg_ranks([signal[d, i] for i in pairs]), ref_avg_ranks([lab[i] for i in pairs]))


# ------------------------------------------------------------------ the synthetic world
def make_panel(dates=420, names=330, score_begin=100, start="2020-03-01", seed=5, returns=None):
    rng = np.random.default_rng(seed)
    t0 = int(dt.datetime.fromisoformat(start).replace(tzinfo=dt.timezone.utc).timestamp()) * 10**9
    sessions = t0 + DAY * np.arange(dates, dtype=np.int64)
    r = 0.01 * rng.standard_normal((dates, names)) if returns is None else returns
    r = np.array(r, dtype=float)
    r[0] = 0.0
    close = 50.0 * np.exp(np.cumsum(np.log1p(r), axis=0))
    member = np.ones((dates, names), dtype=np.uint8)
    member[:, -10:] = 0
    size = np.broadcast_to(np.exp(np.linspace(18.0, 26.0, names))[None, :], (dates, names)).copy()
    ff12 = np.broadcast_to((np.arange(names) % 12 + 1.0)[None, :], (dates, names)).copy()
    return {"sessions": sessions, "ids": np.arange(1, names + 1, dtype=np.uint64) * 7, "close": close,
            "raw": close.copy(), "volume": np.full((dates, names), 1e6), "present": np.ones((dates, names), np.uint8),
            "member": member, "score_begin": score_begin, "size": size, "ff12": ff12}


class CardWorld:
    """Role + v2 candidate cache + u pass (summary, orientations, recipe, daily IC via the literal runner) +
    research fields (me_company, grp_ff12) + admission table, under ``root``."""

    def __init__(self, root: Path, panel: dict, signals: dict, *, admitted=None, themes=None, min_names=100,
                 fields=True, ref_horizons=(5, 21, 63)):
        self.root, self.p, self.signals = root, panel, signals
        self.ids = list(signals)
        p = panel
        d, n = p["close"].shape
        role = root / "role"
        role.mkdir(parents=True)
        files = {}
        for name, arr in (("sessions.i64", p["sessions"].astype("<i8")), ("ids.u64", p["ids"].astype("<u8")),
                          ("present.u8", p["present"]), ("member.u8", p["member"]),
                          ("close.f64", p["close"].astype("<f8")), ("raw_close.f64", p["raw"].astype("<f8")),
                          ("volume.f64", p["volume"].astype("<f8"))):
            data = np.ascontiguousarray(arr).tobytes()
            (role / name).write_bytes(data)
            files[name] = {"bytes": len(data), "sha256": sha(data)}
        man = {"schema": fcw.ROLE_SCHEMA, "status": "complete", "dates": d, "instruments": n,
               "score_begin": p["score_begin"], "score_end": d, "score_start_ns": int(p["sessions"][p["score_begin"]]),
               "score_end_ns": int(p["sessions"][-1] + DAY), "source_sha256": "ab" * 32, "files": files}
        self.manifest = role / "manifest.json"
        self.manifest.write_bytes(json.dumps(man, indent=2).encode())
        self.train_sha = sha(self.manifest.read_bytes())
        role_obj = fcw.RoleManifest(self.manifest, self.train_sha)
        self.dsl = {i: sha(f"rank(close) * {k}".encode()) for k, i in enumerate(self.ids)}
        cache_root = root / "cache" / VM / self.train_sha
        cache_root.mkdir(parents=True)
        entries = []
        for cid in self.ids:
            data = np.ascontiguousarray(np.asarray(signals[cid], dtype="<f8")).tobytes()
            stem = f"{cid}.{self.dsl[cid][:16]}"
            (cache_root / f"{stem}.f64").write_bytes(data)
            key = fcw.signal_key_sha256(VM, role_obj, self.dsl[cid], {})
            side = {"schema": fcw.CACHE_SCHEMA_V2, "candidate_id": cid, "dsl_sha256": self.dsl[cid],
                    "role_manifest_sha256": self.train_sha, "role": "train", "eval_mode": fcw.VM_EVAL_MODE,
                    "layout": fcw.CACHE_LAYOUT, "dates": d, "instruments": n, "bytes": len(data),
                    "payload": f"{stem}.f64", "payload_sha256": sha(data), "vm_identity": VM,
                    "field_payload_sha256": {}, "signal_key_sha256": key}
            (cache_root / f"{stem}.json").write_bytes(json.dumps(side).encode())
            entries.append({"id": cid, "layout": "v2", "sidecar": str(cache_root / f"{stem}.json"),
                            "payload": str(cache_root / f"{stem}.f64"), "payload_sha256": sha(data),
                            "field_payload_sha256": {}, "signal_key_sha256": key})
        self.u = root / "u"
        self.u.mkdir()
        recipe_sha = "cd" * 32
        orient = {"schema": fcw.ORIENTATIONS_SCHEMA, "recipe_sha256": recipe_sha, "library_sha256": "11" * 32,
                  "train_manifest_sha256": self.train_sha,
                  "candidates": [{"id": i, "family": "fam", "dsl_sha256": self.dsl[i], "sign": 1} for i in self.ids]}
        (self.u / "orientations.json").write_bytes(json.dumps(orient, indent=2).encode())
        train = {"role": "train", "manifest_sha256": self.train_sha,
                 "candidate_cache": {"directory": str(cache_root), "vm_identity": VM, "entries": entries,
                                     "layout": fcw.CACHE_SCHEMA_V2}}
        if fields:
            fd = root / "fields"
            fd.mkdir()
            ff = {}
            for name, arr in (("me_company", p["size"]), ("grp_ff12", p["ff12"])):
                data = np.ascontiguousarray(arr.astype("<f8")).tobytes()
                (fd / f"{name}.f64").write_bytes(data)
                ff[f"{name}.f64"] = {"bytes": len(data), "sha256": sha(data)}
            fman = {"schema": "atx.research-role-fields/v1", "status": "complete",
                    "role": {"manifest_sha256": self.train_sha}, "files": ff}
            (fd / "manifest.json").write_bytes(json.dumps(fman).encode())
            train["research_fields"] = {"directory": str(fd), "manifest_sha256": sha((fd / "manifest.json").read_bytes())}
        summary = {"status": "complete", "recipe_sha256": recipe_sha, "roles": [train],
                   "orientations_artifact_sha256": sha((self.u / "orientations.json").read_bytes())}
        (self.u / "summary.json").write_bytes(json.dumps(summary, indent=2).encode())
        (self.u / "recipe.json").write_bytes(json.dumps({"min_names": min_names, "min_dates": 8}).encode())
        # the runner's daily series, from the literal reference
        self.bad = ref_guard(p["close"], p["raw"], p["present"])
        sb = p["score_begin"]
        lines = ["id,horizon,decision_index,session_ns,pearson,rank_ic,oriented_rank_ic"]
        self.ref, self.labels = {}, {h: ref_labels(p, self.bad, h) for h in ref_horizons}
        for cid in self.ids:
            for h in ref_horizons:
                vals = []
                for d_ in range(sb, d - 1 - h):
                    v = ref_runner_ic(p, self.labels[h], signals[cid], d_, min_names)
                    vals.append((d_, v))
                    s = "" if math.isnan(v) else repr(v)
                    lines.append(f"{cid},{h},{d_},{int(p['sessions'][d_])},,{s},{s}")
                self.ref[(cid, h)] = vals
        (self.u / "train_daily_ic.csv").write_bytes(("\n".join(lines) + "\n").encode())
        (self.u / "train_candidates.jsonl").write_bytes(b"".join(
            json.dumps({"id": i, "frozen_train_sign": 1, "horizons": []}).encode() + b"\n" for i in self.ids))
        admitted = list(self.ids if admitted is None else admitted)
        themes = themes or {i: "value" for i in self.ids}
        adm = {"schema": fcw.ADMISSION_SCHEMA, "screen": "v4-prior-v1",
               "inputs": {"train_manifest_sha256": self.train_sha, "runner_summary_sha256": "ee" * 32},
               "rules": {"tau_limit": 0.7, "veto_t": -2.0, "rho_limit": 0.9},
               "admitted": admitted,
               "candidates": [{"id": i, "theme": themes[i], "tier": "B", "status": "admitted" if i in admitted
                               else "reject_redundant", "failed_checks": [], "redundant_with": None,
                               "redundant_rho": None, "max_abs_rho": 0.5, "max_abs_rho_with": None, "tau": 0.1,
                               "hac_t": 1.0, "train_sharpe": 0.5, "s_k": 1, "admission_rank": None,
                               "cache_payload_sha256": entries[k]["payload_sha256"]} for k, i in enumerate(self.ids)]}
        self.admission = root / "admission.json"
        self.admission.write_bytes(json.dumps(adm, indent=2).encode())

    def argv(self, out: Path, extra=()) -> list[str]:
        return ["--u-pass", str(self.u), "--train", str(self.manifest), "--train-sha256", self.train_sha,
                "--admission", str(self.admission), "--output", str(out), *extra]

    def run(self, out: Path, extra=()) -> tuple[int, str]:
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            code = arc.main(self.argv(out, extra))
        return code, err.getvalue()


def load(out: Path, cid: str) -> dict:
    return json.loads((out / f"card-{cid}.json").read_bytes())


# ------------------------------------------------------------------ tests
class Ranks(unittest.TestCase):
    def test_centered_ranks_ties_groups_and_order_reuse(self):
        v = np.array([[3.0, 1.0, 1.0, 5.0, np.nan, 2.0, 2.0, 9.0]])
        valid = np.isfinite(v)
        got, order = arc.centered_ranks(v, valid)
        ref = ref_avg_ranks([3.0, 1.0, 1.0, 5.0, 2.0, 2.0, 9.0])
        want = [r - 3.0 for r in ref]
        np.testing.assert_allclose(np.delete(got[0], 4), want)
        self.assertTrue(math.isnan(got[0, 4]))
        g = np.array([[0, 0, 1, 1, 0, 1, -1, 0]])
        got_g, _ = arc.centered_ranks(v, valid, g, 2, order)
        for grp in (0, 1):
            idx = [j for j in range(8) if g[0, j] == grp and valid[0, j]]
            rr = ref_avg_ranks([v[0, j] for j in idx])
            np.testing.assert_allclose([got_g[0, j] for j in idx], [r - (len(idx) - 1) / 2 for r in rr])
        self.assertTrue(math.isnan(got_g[0, 6]) and math.isnan(got_g[0, 4]))

    def test_half_life_fit_recovers_rho(self):
        hs = np.arange(1, 64)
        fit = arc.half_life_fit(0.05 * 0.9 ** (hs - 1.0), hs)
        self.assertAlmostEqual(fit["half_life_days"], math.log(2) / -math.log(0.9), delta=0.02)
        self.assertAlmostEqual(fit["a"], 0.05, delta=1e-3)
        self.assertIsNone(arc.half_life_fit(np.full(63, np.nan), hs))


class KnownAnswers(unittest.TestCase):
    """One clean world: every label valid, known group, turnover, coverage and correlation answers."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        p = make_panel(names=450)
        d, n = p["close"].shape
        rng = np.random.default_rng(9)
        bad = ref_guard(p["close"], p["raw"], p["present"])
        l21 = ref_labels(p, bad, 21)
        size_rank = np.argsort(np.argsort(p["size"][0, :n - 10]))  # eligible names: all but the last 10
        terc = np.full(n, -1)
        terc[:n - 10] = np.floor(3 * size_rank / (n - 10)).astype(int)
        sign_t = np.where(terc == 1, -1.0, 1.0)
        sign_f = np.where(p["ff12"][0].astype(int) % 2 == 1, 1.0, -1.0)
        static = np.broadcast_to(np.arange(n, dtype=float)[None, :], (d, n)).copy()
        flip = static * np.where(np.arange(d) % 2 == 0, 1.0, -1.0)[:, None]
        sparse = rng.standard_normal((d, n))
        years = arc.years_of(p["sessions"])
        elig_cols = np.arange(n - 10)
        sparse[np.ix_(np.flatnonzero(years == 2021), elig_cols[:110])] = np.nan  # 110 of 440 eligible in 2021
        cls.signals = {"size_look": sign_t[None, :] * l21, "ff_look": sign_f[None, :] * l21, "static": static,
                       "twin": static.copy(), "anti": -static, "flip": flip, "sparse": sparse}
        cls.world = CardWorld(root / "w", p, cls.signals, admitted=["static", "twin", "flip", "sparse", "anti"],
                              themes={"size_look": "value", "ff_look": "value", "static": "momentum",
                                      "twin": "momentum", "anti": "quality", "flip": "reversal", "sparse": "quality"})
        cls.out = root / "cards"
        cls.code, cls.err = cls.world.run(cls.out)
        cls.p, cls.bad, cls.years = p, bad, years

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_run_completes_and_publishes_every_file(self):
        self.assertEqual(self.code, 0, self.err)
        names = sorted(x.name for x in self.out.iterdir())
        for cid in self.signals:
            self.assertIn(f"card-{cid}.json", names)
            self.assertIn(f"card-{cid}.html", names)
        for f in ("index.json", "index.html", "daily_sleeve.csv", "manifest.json"):
            self.assertIn(f, names)
        man = json.loads((self.out / "manifest.json").read_bytes())
        self.assertEqual(man["files"], {x: sha((self.out / x).read_bytes()) for x in names if x != "manifest.json"})

    def test_ic_equals_the_literal_runner_where_the_paired_set_is_the_marginal_set(self):
        for cid in ("static", "flip", "anti"):
            chk = load(self.out, cid)["runner_check"]
            for h in ("5", "21", "63"):
                self.assertEqual(chk[h]["valid_date_mismatches"], 0, (cid, h))
                self.assertLess(chk[h]["max_abs_daily_diff"], 1e-12, (cid, h))
                self.assertLess(chk[h]["abs_mean_diff"], 1e-12)

    def test_ic_by_year_is_the_runner_series(self):
        card = load(self.out, "static")
        for h in (5, 21, 63):
            vals = [(dd, v) for dd, v in self.world.ref[("static", h)] if math.isfinite(v)]
            for y in (2020, 2021):
                xs = [v for dd, v in vals if self.years[dd] == y]
                got = card["ic_by_year"][str(h)][str(y)]
                self.assertEqual(got["n"], len(xs))
                self.assertAlmostEqual(got["mean"], sum(xs) / len(xs), places=9)
                sd = float(np.std(xs, ddof=1))
                self.assertAlmostEqual(got["ir"], (sum(xs) / len(xs)) / sd, places=6)

    def test_size_tercile_and_ff12_known_signs(self):
        size = load(self.out, "size_look")["ic_by_size_tercile"]
        self.assertEqual(size["size_field"], "me_company")
        for name, want in (("small", 1.0), ("mid", -1.0), ("large", 1.0)):
            self.assertAlmostEqual(size["groups"][name]["mean"], want, places=9, msg=name)
            self.assertGreater(size["groups"][name]["n"], 200)
        ff = load(self.out, "ff_look")["ic_by_ff12"]["groups"]
        for code in range(1, 13):
            self.assertAlmostEqual(ff[str(code)]["mean"], 1.0 if code % 2 else -1.0, places=9, msg=code)

    def test_turnover_known_answers(self):
        self.assertAlmostEqual(load(self.out, "static")["turnover"]["mean"], 0.0, places=12)
        self.assertAlmostEqual(load(self.out, "flip")["turnover"]["mean"], 2.0, places=9)

    def test_coverage_by_year(self):
        cov = load(self.out, "sparse")["coverage"]
        self.assertAlmostEqual(cov["2021"], 0.75, places=12)
        self.assertAlmostEqual(cov["2020"], 1.0, places=12)
        # the runner's 80% coverage rule removes every 2021 row of this candidate
        self.assertEqual(load(self.out, "sparse")["ic_by_year"]["21"].get("2021", {"n": 0})["n"], 0)

    def test_worldquant_metrics_from_a_literal_book(self):
        p, bad = self.p, self.bad
        d, n = p["close"].shape
        sb = p["score_begin"]
        s = self.signals["flip"]
        pnl, turn, prev = [], [], None
        for d_ in range(sb, d):
            elig = [i for i in range(n) if p["member"][d_, i]]
            r = ref_avg_ranks([s[d_, i] for i in elig])
            c = [x - (len(elig) - 1) / 2 for x in r]
            g = sum(abs(x) for x in c)
            qv = {i: x / g for i, x in zip(elig, c)}
            if d_ < d - 2:
                r1 = [ref_label(p, bad, d_, i, d_ + 1, d_ + 2) for i in elig]
                pnl.append(sum(qv[i] * (0.0 if math.isnan(x) else x) for i, x in zip(elig, r1)))
            if prev is not None:
                turn.append(sum(abs(qv.get(i, 0.0) - prev.get(i, 0.0)) for i in range(n)))
            prev = qv
        wq = load(self.out, "flip")["worldquant"]
        mean, sd = float(np.mean(pnl)), float(np.std(pnl, ddof=1))
        sharpe = mean / sd * math.sqrt(252)
        rets = 2 * 252 * mean
        tau = float(np.mean(turn))
        self.assertAlmostEqual(wq["sharpe"], sharpe, places=6)
        self.assertAlmostEqual(wq["returns"], rets, places=9)
        self.assertAlmostEqual(wq["turnover"], tau, places=9)
        self.assertAlmostEqual(wq["fitness"], sharpe * math.sqrt(abs(rets) / max(tau, 0.125)), places=6)
        margin = 1e4 * sum(pnl[1:]) / sum(turn[:len(pnl) - 1])  # rows 1 .. T-3 carry both a pnl and a turnover
        self.assertAlmostEqual(wq["margin_bps"], margin, places=5)
        self.assertIsNone(load(self.out, "static")["worldquant"]["margin_bps"])  # no turnover: margin undefined

    def test_correlations_to_members_and_themes(self):
        card = load(self.out, "static")
        sig, pnl = card["correlation"]["signal"], card["correlation"]["pnl"]
        self.assertAlmostEqual(sig["members"]["twin"], 1.0, places=9)
        self.assertAlmostEqual(sig["members"]["anti"], -1.0, places=9)
        self.assertNotIn("static", sig["members"])
        self.assertAlmostEqual(sig["themes"]["momentum"], 1.0, places=9)  # composite of static and its twin
        self.assertAlmostEqual(pnl["members"]["twin"], 1.0, places=9)
        self.assertAlmostEqual(pnl["members"]["anti"], -1.0, places=9)
        self.assertIn(sig["max_abs"]["with"], ("anti", "twin"))
        self.assertAlmostEqual(abs(sig["max_abs"]["rho"]), 1.0, places=9)
        self.assertEqual(sorted(json.loads((self.out / "index.json").read_bytes())["themes"]),
                         ["momentum", "quality", "reversal"])

    def test_admission_status_carried(self):
        self.assertEqual(load(self.out, "static")["admission"]["status"], "admitted")
        self.assertTrue(load(self.out, "static")["admission"]["payload_matches_admission"])
        self.assertEqual(load(self.out, "size_look")["admission"]["status"], "reject_redundant")

    def test_index_ranks_by_fitness_and_pages_validate(self):
        idx = json.loads((self.out / "index.json").read_bytes())
        fit = [r["fitness"] for r in idx["candidates"]]
        known = [f for f in fit if f is not None]
        self.assertEqual(known, sorted(known, reverse=True))
        self.assertEqual([r["rank"] for r in idx["candidates"]], list(range(1, len(fit) + 1)))
        html = (self.out / "index.html").read_text(encoding="utf-8")
        for cid in self.signals:
            self.assertIn(f'href="card-{cid}.html"', html)
        for cid in self.signals:
            page = (self.out / f"card-{cid}.html").read_text(encoding="utf-8")
            for frag in re.findall(r"<svg.*?</svg>", page, flags=re.S):
                self.assertEqual(C.validate_svg(frag), [], cid)

    def test_daily_sleeve_csv(self):
        lines = (self.out / "daily_sleeve.csv").read_text().splitlines()
        self.assertEqual(lines[0], "id,decision_index,session_ns,turnover,pnl,coverage")
        rows = [x.split(",") for x in lines[1:] if x.startswith("flip,")]
        self.assertEqual(len(rows), self.p["close"].shape[0] - self.p["score_begin"])
        self.assertEqual(rows[0][3], "")
        self.assertAlmostEqual(float(rows[5][3]), 2.0, places=9)


class DecayAndGuard(unittest.TestCase):
    def test_lagged_ic_decays_geometrically_and_half_life_is_recovered(self):
        rng = np.random.default_rng(3)
        d, n, rho = 420, 330, 0.9
        s = rng.standard_normal((d, n))
        r = 1e-4 * rng.standard_normal((d, n))
        for j in range(2, 90):  # interval return t carries rho^(j-2) of the signal at t-j
            r[j:] += 0.01 * rho ** (j - 2) * s[:-j]
        with tempfile.TemporaryDirectory() as tmp:
            world = CardWorld(Path(tmp) / "w", make_panel(returns=r), {"decay": s}, ref_horizons=(5,))
            code, err = world.run(Path(tmp) / "out")
            self.assertEqual(code, 0, err)
            card = load(Path(tmp) / "out", "decay")
        fit = card["decay"]["fit"]
        self.assertAlmostEqual(fit["rho"], rho, delta=0.01)
        self.assertAlmostEqual(fit["half_life_days"], math.log(2) / -math.log(rho), delta=0.6)
        ic = card["decay"]["ic"]
        self.assertGreater(ic[0], 0.3)
        self.assertLess(abs(ic[40]), 0.05)
        self.assertEqual(card["runner_check"]["5"]["valid_date_mismatches"], 0)

    def test_guard_and_missing_endpoints_match_the_runner(self):
        p = make_panel(dates=260, names=330, score_begin=100, seed=8)
        p["close"][150:, 7] *= 1.3     # adjusted step without a raw step: |log 1.3| > 0 + .10, guarded
        p["close"][170:, 9] *= 10.0    # |log 10| = 2.3 > 1.5: guarded
        p["raw"][170:, 9] *= 10.0
        p["present"][180:190, 11] = 0  # endpoints missing
        for key in ("close", "raw", "volume"):
            p[key][180:190, 11] = np.nan
        rng = np.random.default_rng(4)
        s = rng.standard_normal(p["close"].shape)
        s[rng.random(s.shape) < 0.02] = np.nan  # a few paired-set differences: marginal-rank gap
        with tempfile.TemporaryDirectory() as tmp:
            world = CardWorld(Path(tmp) / "w", p, {"g": s})
            code, err = world.run(Path(tmp) / "out")
            self.assertEqual(code, 0, err)
            chk = load(Path(tmp) / "out", "g")["runner_check"]
            np.testing.assert_array_equal(arc.guard_prefix(p["close"], p["raw"], p["present"].astype(bool)),
                                          world.bad)
        for h in ("5", "21", "63"):
            self.assertEqual(chk[h]["valid_date_mismatches"], 0, h)
            self.assertLess(chk[h]["max_abs_daily_diff"], 5e-3, h)
            self.assertLess(chk[h]["abs_mean_diff"], 5e-4, h)


class DeterminismAndRefusals(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        p = make_panel(dates=240, names=330, score_begin=100, seed=12)
        rng = np.random.default_rng(1)
        self.world = CardWorld(self.root / "w", p, {"a": rng.standard_normal(p["close"].shape),
                                                     "b": rng.standard_normal(p["close"].shape)}, fields=False)

    def tearDown(self):
        self.tmp.cleanup()

    def test_two_runs_write_identical_bytes(self):
        self.assertEqual(self.world.run(self.root / "one")[0], 0)
        self.assertEqual(self.world.run(self.root / "two")[0], 0)
        one = {x.name: x.read_bytes() for x in (self.root / "one").iterdir()}
        two = {x.name: x.read_bytes() for x in (self.root / "two").iterdir()}
        self.assertEqual(one, two)
        card = json.loads(one["card-a.json"])
        self.assertTrue(card["ic_by_size_tercile"]["size_field"].startswith("adv63"))
        self.assertIsNone(card["ic_by_ff12"]["groups"])

    def test_worker_count_does_not_change_bytes(self):
        self.assertEqual(self.world.run(self.root / "w1", ["--workers", "1"])[0], 0)
        self.assertEqual(self.world.run(self.root / "w3", ["--workers", "3"])[0], 0)
        one = {x.name: x.read_bytes() for x in (self.root / "w1").iterdir()}
        three = {x.name: x.read_bytes() for x in (self.root / "w3").iterdir()}
        self.assertEqual(one, three)

    def test_refusals(self):
        (self.root / "exists").mkdir()
        code, err = self.world.run(self.root / "exists")
        self.assertEqual(code, 1)
        self.assertIn("refusing overwrite", err)
        adm = json.loads(self.world.admission.read_bytes())
        adm["inputs"]["train_manifest_sha256"] = "00" * 32
        self.world.admission.write_bytes(json.dumps(adm).encode())
        code, err = self.world.run(self.root / "other_role")
        self.assertEqual(code, 1)
        self.assertIn("TRAIN manifest differs", err)
        self.assertFalse((self.root / "other_role").exists())

    def test_tampered_payload_refused(self):
        payload = next((self.root / "w" / "cache").rglob("a.*.f64"))
        data = bytearray(payload.read_bytes())
        data[100] ^= 1
        payload.write_bytes(bytes(data))
        code, err = self.world.run(self.root / "tampered")
        self.assertEqual(code, 1)
        self.assertIn("payload SHA-256 mismatch", err)

    def test_admission_pin(self):
        code, err = self.world.run(self.root / "pinned", ["--admission-sha256", "0" * 64])
        self.assertEqual(code, 1)
        self.assertIn("admission: SHA-256 pin differs", err)


if __name__ == "__main__":
    unittest.main()
