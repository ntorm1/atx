"""v8 C-1: the fitter's store is shared by every library on a role and window, keyed by the signal payload SHA-256
and the producer fingerprint (synthetic data only).

Run: python -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_weights_store.py
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import unittest.mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_composition_weights as fcw  # noqa: E402
import test_fit_composition_weights as base  # noqa: E402

V4_ARGS = ["--orientation", "prior", "--composition", "ew-theme-v1"]
N_V70, N_V71 = 44, 48


def setUpModule():  # hermetic: the in-file theme list unless a test installs a registry
    base.setUpModule()


def tearDownModule():
    base.tearDownModule()


def library_world(n: int = N_V71, seed: int = 48):
    """screen_world's panel with ``n`` distinct persistent members spread over the v4 themes (v7.0-like: the first
    N_V70; v7.1-like: all N_V71)."""
    panel, signals, _ = base.screen_world()
    rng = np.random.default_rng(seed)
    live = panel["member"] == 1
    out, ids, extra = [], [], {}
    for k in range(n):
        s = signals[k % 3] + 0.4 * (k + 1) / n * rng.normal(size=signals[0].shape)
        out.append(np.where(live, s, np.nan))
        ids.append(f"m{k:02d}")
        extra[ids[-1]] = {"theme": fcw.V4_THEMES[k % len(fcw.V4_THEMES)], "tier": "B", "prior_sign": 1}
    return panel, out, ids, extra


def fixture(root: Path, panel, signals, ids, extra) -> base.Fixture:
    return base.Fixture(root, panel, signals, [1] * len(ids), ids=ids, families=["fam"] * len(ids),
                        candidate_extra=extra, vm_identity=base.NON_DEV_IDENTITY, layout="v2")


def run(module, fx, out: Path, work: Path | None, screen="v4-prior-v1"):
    """(published files, the 'fit: computed K, reused M' line) of one CLI run of ``module``."""
    err = io.StringIO()
    argv = fx.argv(out, screen, V4_ARGS + (["--work-dir", str(work)] if work else []))
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
        code = module.main(argv)
    assert code in (fcw.EXIT_OK, fcw.EXIT_NO_WEIGHTS), err.getvalue()
    lines = [x for x in err.getvalue().splitlines() if x.startswith("fit: computed ")]
    return {p.name: p.read_bytes() for p in out.iterdir()}, lines[-1]


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class StoreAcrossLibraries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.panel, cls.signals, cls.ids, cls.extra = library_world()
        cls.v70 = fixture(cls.root / "v70", cls.panel, cls.signals[:N_V70], cls.ids[:N_V70], cls.extra)
        cls.v71 = fixture(cls.root / "v71", cls.panel, cls.signals, cls.ids, cls.extra)
        cls.bare, _ = run(fcw, cls.v71, cls.root / "v71-bare", None)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_fit_store_reuses_across_libraries(self):
        self.assertEqual(self.v70.train_sha, self.v71.train_sha)  # one role: one store root
        work = self.root / "fit-work"
        _, line = run(fcw, self.v70, self.root / "v70-out", work)
        self.assertEqual(line, f"fit: computed {N_V70}, reused 0")
        files, line = run(fcw, self.v71, self.root / "v71-out", work)
        self.assertEqual(line, f"fit: computed {N_V71 - N_V70}, reused {N_V70}")
        self.assertEqual(files, self.bare)  # admission.json, admission.csv and the weights byte-identical
        files, line = run(fcw, self.v71, self.root / "v71-again", work)
        self.assertEqual(line, f"fit: computed 0, reused {N_V71}")
        self.assertEqual(files, self.bare)
        self.assertEqual([p.name for p in work.iterdir()], [f"{self.v71.train_sha[:16]}-{fcw.window_id()}"])
        # the store is a cache: deleted, the same bytes come back
        shutil.rmtree(work)
        files, line = run(fcw, self.v71, self.root / "v71-cold", work)
        self.assertEqual((files, line), (self.bare, f"fit: computed {N_V71}, reused 0"))

    def test_comment_edit_keeps_store(self):
        work = self.root / "edit-work"
        run(fcw, self.v71, self.root / "edit-ref", work)
        source = Path(fcw.__file__).read_bytes().replace(b"\r\n", b"\n")

        def edited(name: str, old: bytes, new: bytes):
            self.assertEqual(source.count(old), 1, old)
            (self.root / "mods").mkdir(exist_ok=True)
            path = self.root / "mods" / f"{name}.py"
            path.write_bytes(source.replace(old, new))
            return load_module(path, name)

        # comments, a docstring and orchestration code outside the producers' closure: every record is reused
        doc = b'    """The candidate\'s unsigned factor series (None on flat days), tau and live count: a function'
        cosmetic = edited("fcw_cosmetic", doc, b"    # a comment\n" + doc.replace(b"The candidate", b"Edited. The candidate"))
        files, line = run(cosmetic, self.v71, self.root / "cosmetic-out", work)
        self.assertEqual(line, f"fit: computed 0, reused {N_V71}")
        orchestration = edited("fcw_orchestration", b"def fit(args, log=None) -> tuple[int, dict]:\n",
                               b"def fit(args, log=None) -> tuple[int, dict]:\n    _probe = 1  # noqa: F841\n")
        files, line = run(orchestration, self.v71, self.root / "orchestration-out", work)
        self.assertEqual(line, f"fit: computed 0, reused {N_V71}")
        # outputs differ from the unedited fitter's only by the script SHA-256 they name
        adm = files[fcw.OUTPUT_ADMISSION].replace(orchestration.SCRIPT_SHA256.encode(), fcw.SCRIPT_SHA256.encode())
        self.assertEqual(adm, self.bare[fcw.OUTPUT_ADMISSION])
        # a value-neutral edit of producer code still counts as producer code: everything is recomputed
        producer = edited("fcw_producer", b"    ranks = (first.astype(np.float64) + last.astype(np.float64)) / "
                                          b"denominator[:, None] - 0.5\n",
                          b"    ranks = (first.astype(np.float64) + last.astype(np.float64)) / "
                          b"denominator[:, None] - 0.5 + 0.0\n")
        files, line = run(producer, self.v71, self.root / "producer-out", work)
        self.assertEqual(line, f"fit: computed {N_V71}, reused 0")
        adm = files[fcw.OUTPUT_ADMISSION].replace(producer.SCRIPT_SHA256.encode(), fcw.SCRIPT_SHA256.encode())
        self.assertEqual(adm, self.bare[fcw.OUTPUT_ADMISSION])


class StoreKeying(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.panel, self.signals, self.ids, self.extra = base.v4_world()
        self.n = len(self.ids)

    def tearDown(self):
        self.tmp.cleanup()

    def test_fit_store_keyed_by_role_and_window(self):
        work = self.root / "work"
        a = fixture(self.root / "a", self.panel, self.signals, self.ids, self.extra)
        other = dict(self.panel, volume=self.panel["volume"] * 1.5)  # another role, the same signal bytes
        b = fixture(self.root / "b", other, self.signals, self.ids, self.extra)
        self.assertNotEqual(a.train_sha, b.train_sha)
        self.assertEqual(run(fcw, a, self.root / "a1", work)[1], f"fit: computed {self.n}, reused 0")
        self.assertEqual(run(fcw, b, self.root / "b1", work)[1], f"fit: computed {self.n}, reused 0")
        self.assertEqual(run(fcw, a, self.root / "a2", work)[1], f"fit: computed 0, reused {self.n}")
        self.assertEqual(sorted(p.name for p in work.iterdir()),
                         sorted(f"{s[:16]}-{fcw.window_id()}" for s in (a.train_sha, b.train_sha)))
        # another research window is another root: nothing is shared
        with unittest.mock.patch.object(fcw, "window_id", lambda: "research-window-vX"):
            self.assertEqual(run(fcw, a, self.root / "a3", work)[1], f"fit: computed {self.n}, reused 0")
        self.assertTrue((work / f"{a.train_sha[:16]}-research-window-vX").is_dir())
        # a record of role A placed where role B's record belongs (as a 16-hex root collision would) is a miss
        store_a = fcw.WorkStore(work, fcw.RoleManifest(a.manifest, a.train_sha))
        store_b = fcw.WorkStore(work, fcw.RoleManifest(b.manifest, b.train_sha))
        entry = {"payload_sha256": base.sha(a.payload_path("flip").read_bytes())}
        foreign = store_a.records.path("factor", store_a.key(entry, "factor")).read_bytes()
        store_b.records.path("factor", store_b.key(entry, "factor")).write_bytes(foreign)
        self.assertIsNone(store_b.get(entry))
        self.assertEqual(run(fcw, b, self.root / "b2", work)[1], f"fit: computed 1, reused {self.n - 1}")

    def test_stored_factor_series_serves_the_monitor(self):
        import book_monitor as bm
        work = self.root / "work"
        fx = fixture(self.root / "fx", self.panel, self.signals, self.ids, self.extra)
        files, _ = run(fcw, fx, self.root / "out", work)
        adm = json.loads(files[fcw.OUTPUT_ADMISSION])
        want = {r["id"]: r["cache_payload_sha256"] for r in adm["candidates"]}
        got = fcw.stored_factor_series(work, fx.train_sha, want, adm["inputs"]["context_sha256"])
        self.assertEqual(sorted(got), sorted(self.ids))
        store = fcw.WorkStore(work, fcw.RoleManifest(fx.manifest, fx.train_sha))
        f = store.get({"payload_sha256": want["flip"]})["f_unsigned"]
        np.testing.assert_array_equal(got["flip"], np.array([np.nan if v is None else v for v in f]))
        self.assertEqual(fcw.stored_factor_series(work, fx.train_sha, want, "00" * 32), {})  # another context
        members = adm["admitted"]
        series, note = bm.fit_records(work, adm, members)  # book_monitor M4 reads the v8 store
        self.assertIsNone(note)
        self.assertEqual(sorted(series), sorted(members))


class RegistryThemes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        panel, signals, ids, extra = base.v4_world()
        extra["flip"] = dict(extra["flip"], theme="event_driven")  # an admitted member of a registry-only theme
        self.fx = fixture(self.root / "fx", panel, signals, ids, extra)

    def tearDown(self):
        self.tmp.cleanup()

    def registry(self, themes) -> Path:
        path = self.root / "registry.json"
        path.write_text(json.dumps({"schema": "atx.alpha-registry/v1", "alphas": [], "fields": {},
                                    "themes": {t: f"{t} text" for t in themes}}), encoding="utf-8")
        return path

    def test_themes_come_from_the_registry_when_present(self):
        with self.assertRaises(fcw.FitError) as caught:  # the in-file list does not know the theme
            fcw.fit(self.fx.args(self.root / "no-registry", **base.V4_ARGS))
        self.assertIn("appended theme ('ownership_flow',) (in-file list)", str(caught.exception))
        path = self.registry(list(fcw.V4_THEMES) + ["event_driven", "ownership_flow"])
        with unittest.mock.patch.object(fcw, "REGISTRY_PATH", path):
            code, _ = fcw.fit(self.fx.args(self.root / "registry", **base.V4_ARGS))
        self.assertEqual(code, fcw.EXIT_OK)
        doc = json.loads((self.root / "registry" / fcw.OUTPUT_WEIGHTS).read_bytes())
        self.assertEqual(doc["provenance"]["themes_preregistered"], list(fcw.V4_THEMES) + ["event_driven"])
        self.assertIn("event_driven", doc["provenance"]["themes_present"])
        # a registry without the theme refuses it and names itself; a malformed registry refuses
        with unittest.mock.patch.object(fcw, "REGISTRY_PATH", self.registry(fcw.V4_THEMES)):
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(self.root / "narrow", **base.V4_ARGS))
        self.assertIn("(registry registry.json)", str(caught.exception))
        path.write_text(json.dumps({"schema": "atx.alpha-registry/v1", "themes": []}), encoding="utf-8")
        with unittest.mock.patch.object(fcw, "REGISTRY_PATH", path):
            with self.assertRaises(fcw.FitError) as caught:
                fcw.fit(self.fx.args(self.root / "malformed", **base.V4_ARGS))
        self.assertIn("themes table", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
