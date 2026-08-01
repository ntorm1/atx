"""Tests for the deterministic SPX Wilmott reproduction renderer."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import struct
import sys
import tempfile
import unittest


_PYTHON_ROOT = pathlib.Path(__file__).resolve().parents[1]
_TOOL = _PYTHON_ROOT / "render_spx_wilmott_repro.py"

_spec = importlib.util.spec_from_file_location("render_spx_wilmott_repro", _TOOL)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"cannot load renderer from {_TOOL}")
renderer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = renderer
_spec.loader.exec_module(renderer)


_REPRO_CSV = """\
#META snapshot=2019-08-26T19:30:00Z,expiry=2019-09-20,exercise=European,settlement=AM,T=0.0678,F=2880.0,sigma0=0.18,observations=3,visual_z_min=-0.5,visual_z_max=0.5
#SUMMARY family,rmse_iv,max_abs_iv,in_band_percent,fit_us
Svi,0.0100,0.0200,88.0,50
ConvexDense,0.0200,0.0300,90.0,75
#CURVE family,z,fitted_iv
Svi,-1.0,0.30
Svi,0.0,0.20
Svi,1.0,0.10
ConvexDense,-1.0,0.32
ConvexDense,0.0,0.22
ConvexDense,1.0,0.12
#QUOTES family,z,strike,side,market_iv,bid_iv,ask_iv,fitted_iv,residual_iv,in_band
Svi,-1.0,2700,P,0.31,0.29,0.33,0.30,-0.01,1
Svi,0.0,2880,C,0.20,0.19,0.21,0.20,0.00,1
Svi,1.0,3060,C,0.11,0.09,0.13,0.10,-0.01,1
ConvexDense,-1.0,2700,P,0.31,0.29,0.33,0.32,0.01,1
ConvexDense,0.0,2880,C,0.20,0.19,0.21,0.22,0.02,1
ConvexDense,1.0,3060,C,0.11,0.09,0.13,0.12,0.01,1
"""

_TARGET_TSV = """\
ns\tvol\tpixel_x\tpixel_y\tinterpolated
-1.0\t0.30\t1\t2\t0
0.0\t0.20\t2\t3\t0
0.5\t0.15\t3\t4\t0
1.0\t0.10\t4\t5\t0
"""


class ParserTests(unittest.TestCase):
    def test_parses_all_mixed_sections_and_selects_summary_rmse(self) -> None:
        parsed = renderer.parse_repro_text(_REPRO_CSV, source="memory.csv")

        self.assertEqual(parsed.meta["settlement"], "AM")
        self.assertEqual(len(parsed.curves["Svi"]), 3)
        self.assertEqual(len(parsed.quotes["ConvexDense"]), 3)
        self.assertEqual(renderer.select_family(parsed), "Svi")

    def test_falls_back_to_derived_quote_rmse_then_convex_dense(self) -> None:
        parsed = renderer.parse_repro_text(
            _REPRO_CSV.replace(
                "#SUMMARY family,rmse_iv,max_abs_iv,in_band_percent,fit_us\n"
                "Svi,0.0100,0.0200,88.0,50\n"
                "ConvexDense,0.0200,0.0300,90.0,75\n",
                "",
            ),
            source="memory.csv",
        )
        self.assertEqual(renderer.select_family(parsed), "Svi")

        without_quotes = renderer.parse_repro_text(
            _REPRO_CSV.replace(
                "#SUMMARY family,rmse_iv,max_abs_iv,in_band_percent,fit_us\n"
                "Svi,0.0100,0.0200,88.0,50\n"
                "ConvexDense,0.0200,0.0300,90.0,75\n",
                "",
            ).split("#QUOTES", maxsplit=1)[0],
            source="memory.csv",
        )
        self.assertEqual(renderer.select_family(without_quotes), "ConvexDense")

    def test_honors_metadata_recommended_family_before_rmse(self) -> None:
        parsed = renderer.parse_repro_text(_REPRO_CSV, source="memory.csv")
        parsed.meta["recommended_family"] = "ConvexDense"

        self.assertEqual(renderer.select_family(parsed), "ConvexDense")

    def test_vendor_metrics_use_interpolation_over_common_domain(self) -> None:
        parsed = renderer.parse_repro_text(_REPRO_CSV, source="memory.csv")
        target = renderer.parse_vendor_text(_TARGET_TSV, source="target.tsv")

        metrics = renderer.compare_to_vendor(parsed.curves["Svi"], target)

        self.assertEqual(metrics["n_common"], 4)
        self.assertAlmostEqual(metrics["rmse_iv"], 0.0)
        self.assertAlmostEqual(metrics["max_abs_iv"], 0.0)
        self.assertEqual(metrics["common_domain_ns"], [-1.0, 1.0])


class RenderingTests(unittest.TestCase):
    def test_renders_exact_dimensions_metrics_and_deterministic_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            csv_path = directory / "repro.csv"
            target_path = directory / "target.tsv"
            first_png = directory / "first.png"
            second_png = directory / "second.png"
            first_json = directory / "first.json"
            second_json = directory / "second.json"
            csv_path.write_text(_REPRO_CSV, encoding="utf-8")
            target_path.write_text(_TARGET_TSV, encoding="utf-8")

            first = renderer.render_reproduction(
                csv_path, first_png, metrics_path=first_json, vendor_path=target_path
            )
            second = renderer.render_reproduction(
                csv_path, second_png, metrics_path=second_json, vendor_path=target_path
            )

            self.assertEqual(first["selected_family"], "Svi")
            self.assertEqual(second["selected_family"], "Svi")
            png = first_png.read_bytes()
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack(">II", png[16:24]), (1318, 1139))
            self.assertEqual(
                hashlib.sha256(png).digest(),
                hashlib.sha256(second_png.read_bytes()).digest(),
            )
            self.assertEqual(first_json.read_bytes(), second_json.read_bytes())

            sidecar = json.loads(first_json.read_text(encoding="utf-8"))
            self.assertEqual(sidecar["selected_family"], "Svi")
            self.assertEqual(sidecar["vendor_comparison"]["n_common"], 4)
            self.assertEqual(
                sidecar["vendor_comparison"]["supported_domain"]["n_common"], 2
            )
            self.assertEqual(
                sidecar["vendor_comparison"]["supported_domain"]["common_domain_ns"],
                [0.0, 0.5],
            )
            self.assertEqual(sidecar["inputs"]["repro_sha256"], hashlib.sha256(csv_path.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
