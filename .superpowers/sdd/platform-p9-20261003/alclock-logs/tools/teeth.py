"""AL-CLOCK teeth (r6-wave3-common: every new refusal gets a test that fails with the check removed).

usage: python teeth.py 1|2 (working-tree mutations of pool-30). It does NOT restore the bytes itself, so under ruling
TEETH-RESTORE it is not run again as it stands (never revert with git checkout / restore / stash).

Set 1 removes each new refusal / identity guard at once; set 2 over-loosens the ALCLOCK-3 line (capacity accepts any
rate, aim-partial-v6 accepts info-clock-rate-v1). Each mutation is an exact, unique text replacement (asserted).
"""
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-30/atx-impl/src")

SET1 = {
    # M1 validate_nav_config: the rule's config refusal (aim-partial-v5 at .05, no rate parameters)
    "book/nav_validate.cpp": [
        ("  if (cfg.rate == NavRateRule::InfoClockV1 &&\n      (cfg.target.rule != TargetReplayRule::AimPartialV5 ||",
         "  if (false && cfg.rate == NavRateRule::InfoClockV1 &&\n      (cfg.target.rule != TargetReplayRule::AimPartialV5 ||"),
        # M2 validate_nav_input: clock geometry
        ("  if (!clock.empty() && clock.size() != cells)\n",
         "  if (false && !clock.empty() && clock.size() != cells)\n"),
        # M3 validate_nav_input: the rule without a clock
        ("  if (cfg.rate == NavRateRule::InfoClockV1 && clock.empty())\n",
         "  if (false && cfg.rate == NavRateRule::InfoClockV1 && clock.empty())\n"),
        # M4 validate_nav_input: the clock's domain
        ("    if (!(std::isnan(clock[k]) || (std::isfinite(clock[k]) && clock[k] >= 0)))\n",
         "    if (false && !(std::isnan(clock[k]) || (std::isfinite(clock[k]) && clock[k] >= 0)))\n"),
    ],
    # M5 replay_books: a grid variant keeps the base cadence under the rule
    "book/nav_lockstep.cpp": [
        ("  if (base.rate == NavRateRule::InfoClockV1)\n    for (const auto& variant : variants)",
         "  if (false && base.rate == NavRateRule::InfoClockV1)\n    for (const auto& variant : variants)"),
    ],
    "book/nav_load.cpp": [
        # M6 admit_and_load: the rule and the print need --fields
        ("  if (clock != ClockLoad::None && !tiered)\n", "  if (false && clock != ClockLoad::None && !tiered)\n"),
        # M7 identity guard: with fields the clock is always loaded as an input of the run (flag absent moves bytes)
        ("  const ClockLoad clock = base.rate == NavRateRule::InfoClockV1 ? ClockLoad::Rule\n",
         "  const ClockLoad clock = !fields.manifest_path.empty()           ? ClockLoad::Rule\n"),
    ],
    "book/nav_decide.cpp": [
        # M8 the kernel: cadence and slow-rate range, member flags and the clock's domain (the span-size check stays:
        # removing it would write out of bounds)
        ("  if (cadence < 1 || !std::isfinite(theta_slow) || !(theta_slow > 0.0) || theta_slow > 1.0)\n",
         "  if (false && (cadence < 1 || !std::isfinite(theta_slow) || !(theta_slow > 0.0) || theta_slow > 1.0))\n"),
        ("    if (member[i] > u8{1} || !(std::isnan(v) || (std::isfinite(v) && v >= 0.0)))\n",
         "    if (false && (member[i] > u8{1} || !(std::isnan(v) || (std::isfinite(v) && v >= 0.0))))\n"),
    ],
    "book/nav_cli.cpp": [
        # M9 --info-clock-print given twice
        ("        if (!seen.insert(key).second) throw std::invalid_argument(\"duplicate/missing flag\");\n"
         "        emit.info_clock_print = true;\n",
         "        seen.insert(key);\n        emit.info_clock_print = true;\n"),
    ],
    "strategy_nav_v7.cpp": [
        # M10 ALCLOCK-3 reverted: --capacity-curve refuses the rule again (the pre-lane line)
        ("    if ((o.aim_v6 || o.capacity) && rate && *rate != \"fixed\" &&\n"
         "        (o.aim_v6 || *rate != nav_info_clock_rate_id))\n",
         "    if ((o.aim_v6 || o.capacity) && rate && *rate != \"fixed\")\n"),
    ],
}
SET2 = {
    "strategy_nav_v7.cpp": [
        # M11 ALCLOCK-3 over-loosened: the capacity curve takes any rate, aim-partial-v6 takes this one
        ("    if ((o.aim_v6 || o.capacity) && rate && *rate != \"fixed\" &&\n"
         "        (o.aim_v6 || *rate != nav_info_clock_rate_id))\n",
         "    if (o.aim_v6 && rate && *rate != \"fixed\" && *rate != nav_info_clock_rate_id)\n"),
    ],
}


def main() -> int:
    which = {"1": SET1, "2": SET2}[sys.argv[1]]
    for rel, edits in which.items():
        path = ROOT / rel
        text = path.read_bytes().decode("utf-8")
        crlf = "\r\n" in text   # core.autocrlf working copies
        for old, new in edits:
            if crlf:
                old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
            assert text.count(old) == 1, (rel, old)
            text = text.replace(old, new)
        path.write_bytes(text.encode("utf-8"))
        print(f"atx-impl/src/{rel}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
