// Clock, seal, canonical JSON and the formula fingerprint of the research field builders (migration
// slice 1).
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <string>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/finra_asof_field.hpp"
#include "atx/engine/research/fields/volume_mean_field.hpp"

namespace fields = atx::engine::research::fields;

TEST(ResearchFieldsClock, CivilDaysMatchPython) {
  // (date - date(1970, 1, 1)).days, computed by Python.
  EXPECT_EQ(fields::days_from_civil(1970, 1, 1), 0);
  EXPECT_EQ(fields::days_from_civil(1969, 12, 31), -1);
  EXPECT_EQ(fields::days_from_civil(2000, 3, 1), 11017);
  EXPECT_EQ(fields::days_from_civil(2020, 2, 29), 18321);
  EXPECT_EQ(fields::days_from_civil(2021, 6, 9), 18787);
  EXPECT_EQ(fields::days_from_civil(2024, 1, 1), 19723);
  for (atx::i64 day = -800; day < 40000; ++day) {
    const auto c = fields::civil_from_days(day);
    ASSERT_EQ(fields::days_from_civil(c.year, c.month, c.day), day);
  }
  EXPECT_EQ(fields::year_of(18787), 2021);
  EXPECT_EQ(fields::iso_date(18787), "2021-06-09");
  EXPECT_EQ(fields::iso_date(-1), "1969-12-31");
}

TEST(ResearchFieldsClock, IsoDatesAreStrict) {
  EXPECT_EQ(fields::parse_iso_day("2021-06-09"), 18787);
  EXPECT_EQ(fields::parse_iso_day("2020-02-29"), 18321);
  for (const char *bad :
       {"2021-6-09", "2021-02-30", "2023-02-29", "0000-01-01", "2021-13-01", "2021-00-10",
        "2021-06-09 ", " 2021-06-09", "20210609", "2021/06/09", "2021-06-0x", ""}) {
    EXPECT_FALSE(fields::parse_iso_day(bad).has_value()) << bad;
  }
}

TEST(ResearchFieldsClock, SealComesFromTheResearchWindow) {
  EXPECT_EQ(fields::seal_day(), atx::engine::data::kSealBeginNs / fields::kDayNs);
  EXPECT_EQ(fields::iso_date(fields::seal_day()), std::string(atx::engine::data::kSealBeginDate));
  EXPECT_TRUE(fields::is_sealed_day(fields::seal_day()));
  EXPECT_FALSE(fields::is_sealed_day(fields::seal_day() - 1));
  EXPECT_NE(fields::seal_refusal("x").find(std::string(atx::engine::data::kResearchWindowId)),
            std::string::npos);
}

TEST(ResearchFieldsClock, PythonFloatRepr) {
  // repr(x) of Python 3.12 for each value.
  const std::pair<double, const char *> cases[] = {{0.02, "0.02"},
                                                   {5.0, "5.0"},
                                                   {1e16, "1e+16"},
                                                   {1e15, "1000000000000000.0"},
                                                   {1e-05, "1e-05"},
                                                   {0.0001, "0.0001"},
                                                   {123456.789, "123456.789"},
                                                   {-2.5, "-2.5"},
                                                   {0.0, "0.0"},
                                                   {-0.0, "-0.0"},
                                                   {1.5e300, "1.5e+300"},
                                                   {std::log(100.0), "4.605170185988092"},
                                                   {1e22, "1e+22"},
                                                   {0.1, "0.1"},
                                                   {1.0 / 3.0, "0.3333333333333333"},
                                                   {100000.0, "100000.0"},
                                                   {2.5e-7, "2.5e-07"}};
  for (const auto &[value, text] : cases) {
    const auto got = fields::python_float_repr(value);
    ASSERT_TRUE(got.has_value()) << text;
    EXPECT_EQ(*got, text);
  }
  EXPECT_FALSE(fields::python_float_repr(std::nan("")).has_value());
  EXPECT_FALSE(fields::python_float_repr(HUGE_VAL).has_value());
}

TEST(ResearchFieldsClock, JsonStringEscapesAsPython) {
  const auto got = fields::json_string("a\"b\\c\n\t\x01\x7f/z");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, "\"a\\\"b\\\\c\\n\\t\\u0001\\u007f/z\"");
  EXPECT_FALSE(fields::json_string("caf\xc3\xa9").has_value());
}

TEST(ResearchFieldsClock, FormulaFingerprintsEqualThePython) {
  // prepare_research_fields.formula_id(name, spec_definition(name, 1)) at base 798d3b23.
  EXPECT_EQ(fields::formula_sha256(fields::vol_126_spec()).value(),
            "cb44b19e3eda43f3ca0969974a99d1b4439f5bbfa56d09436ad15e8cc572160e");
  ASSERT_NE(fields::finra_spec("si_shares"), nullptr);
  ASSERT_NE(fields::finra_spec("si_dtc"), nullptr);
  EXPECT_EQ(fields::finra_spec("si_volume"), nullptr);
  EXPECT_EQ(fields::formula_sha256(*fields::finra_spec("si_shares")).value(),
            "0d8dedaef04cb65862f0578ca059f3b326f86fe26af8e54b6903a1b45ff50861");
  EXPECT_EQ(fields::formula_sha256(*fields::finra_spec("si_dtc")).value(),
            "051996be07062c67aaa798bcaf61f32a1cf6bd9816acdbb19a1af17cf399fb4c");
}

TEST(ResearchFieldsClock, FormulaFingerprintMovesWithTheDefinition) {
  const std::string base = fields::formula_sha256(fields::vol_126_spec()).value();
  fields::FieldSpec revised = fields::vol_126_spec();
  revised.revision = 2;
  EXPECT_NE(fields::formula_sha256(revised).value(), base);
  fields::FieldSpec bounded = fields::vol_126_spec();
  bounded.definition.domain = std::array<double, 2>{0.0, 5e10};
  const auto document = fields::canonical_formula_document(bounded).value();
  EXPECT_NE(document.find("\"domain\":[0.0,50000000000.0]"), std::string::npos) << document;
  EXPECT_NE(fields::formula_sha256(bounded).value(), base);
  fields::FieldSpec caveated = fields::vol_126_spec();
  caveated.caveats.push_back("caveats are not part of the formula");
  EXPECT_EQ(fields::formula_sha256(caveated).value(), base);
}
