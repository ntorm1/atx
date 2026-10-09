import unittest
from pathlib import Path

from check_benchmark_names import missing_required_names


class BenchmarkNameCoverageTest(unittest.TestCase):
    def test_reports_only_names_absent_from_list_output(self) -> None:
        output = """fit/e2e/spy_real
fit/e2e/100name
price/backtest/spy_real/cold
"""
        required = [
            "fit/e2e/spy_real",
            "fit/e2e/100name",
            "price/backtest/spy_real/cold",
            "price/backtest/spy_real/representative_eager",
            "price/backtest/spy_real/representative_warm",
            "price/backtest/spy_real/representative_screen_cold_confirm",
        ]

        self.assertEqual(
            missing_required_names(output, required),
            [
                "price/backtest/spy_real/representative_eager",
                "price/backtest/spy_real/representative_warm",
                "price/backtest/spy_real/representative_screen_cold_confirm",
            ],
        )

    def test_requires_exact_registered_name(self) -> None:
        output = "fit/e2e/spy_real/threads:8\n"

        self.assertEqual(
            missing_required_names(output, ["fit/e2e/spy_real"]),
            ["fit/e2e/spy_real"],
        )

    def test_strips_min_time_suffix_alongside_min_warmup_time(self) -> None:
        # P-R gate C2 / I-3: _REGISTRATION_SUFFIX omitted `min_time`, so a name
        # carrying both `min_time` (e.g. an explicit ->MinTime() registration) and
        # the always-present `min_warmup_time` truncated at the LATER suffix,
        # leaving `min_time:...` attached to the "base" name and false-redding two
        # real registrations (serve/convexdense_iv_strip/synth,
        # fit/surface_cold_altdriver/spy_real) that both call ->MinTime().
        output = (
            "serve/convexdense_iv_strip/synth/min_time:1.000/min_warmup_time:0.500/repeats:5\n"
            "fit/surface_cold_altdriver/spy_real/min_time:2.000/min_warmup_time:0.500/repeats:5\n"
        )

        self.assertEqual(
            missing_required_names(
                output,
                ["serve/convexdense_iv_strip/synth", "fit/surface_cold_altdriver/spy_real"],
            ),
            [],
        )

    def test_ignores_framework_owned_one_shot_registration_metadata(self) -> None:
        output = """fit/e2e/spy_real/iterations:1/real_time
fit/e2e/100name/iterations:1/real_time
"""

        self.assertEqual(
            missing_required_names(output, ["fit/e2e/spy_real", "fit/e2e/100name"]),
            [],
        )

    def test_fit_e2e_source_pins_the_corpus_scale_contract(self) -> None:
        source = (Path(__file__).parent / "e2e_hotpath_bench.cpp").read_text(encoding="utf-8")
        fit_body = source.split(
            "[[nodiscard]] FitIterationReport run_fit_iteration", maxsplit=1
        )[1].split("void publish_fit_counters", maxsplit=1)[0]
        registration = source.split("const int kRegistered", maxsplit=1)[1]

        self.assertIn("config.fit_workers = bench_fit_workers();", fit_body)
        self.assertIn("constexpr unsigned kGateDefault = 1u;", source)
        self.assertIn("config.n_threads = 0u;", fit_body)
        self.assertIn(
            "fitter.value_chain(*chain, OutputField::Prices, 0u);",
            fit_body,
        )
        self.assertNotIn("OutputField::Bands", fit_body)
        self.assertIn('register_corpus_scale("fit/e2e/spy_real"', registration)
        self.assertIn('register_corpus_scale("fit/e2e/100name"', registration)
        self.assertIn("->Iterations(1)", source)
        self.assertIn(
            'apply_common(benchmark::RegisterBenchmark("price/backtest/spy_real/cold"',
            registration,
        )


if __name__ == "__main__":
    unittest.main()
