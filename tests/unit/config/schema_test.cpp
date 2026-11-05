// The schema table is the single source of truth for keys, ranges and reload
// policy (research R-04). These cases are what make that claim true: a row
// cannot be added without a policy, a pointer cannot be duplicated, and the
// shipped sample file cannot drift from the table.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "support/temp_config_dir.hpp"
#include "tcu/config/config_schema.hpp"

namespace tcu::config {
namespace {

// C7: every row carries a deliberate policy. The enum has no "unspecified"
// value, and the hot/restart split is a spec fact (6 hot, 7 restart), so a row
// added without thinking about reload policy changes this count and fails here.
TEST(Schema, EveryFieldHasReloadPolicy) {
  std::size_t hot = 0;
  std::size_t restart = 0;
  for (const FieldSpec& spec : kFieldSpecs) {
    ASSERT_FALSE(spec.pointer.empty());
    switch (spec.policy) {
      case ReloadPolicy::Hot: ++hot; break;
      case ReloadPolicy::Restart: ++restart; break;
    }
  }
  EXPECT_EQ(hot, 6u);
  EXPECT_EQ(restart, 7u);
  EXPECT_EQ(hot + restart, kFieldSpecs.size());
}

// C8: no pointer twice, or the validator would check one row and the diff
// classify the other.
TEST(Schema, PointersAreUnique) {
  std::set<std::string_view> seen;
  for (const FieldSpec& spec : kFieldSpecs) {
    EXPECT_TRUE(seen.insert(spec.pointer).second)
        << "duplicate pointer: " << spec.pointer;
  }
  EXPECT_EQ(seen.size(), kFieldSpecs.size());
}

// Every pointer must start at the root and name at least one segment.
TEST(Schema, PointersAreRootedJsonPointers) {
  for (const FieldSpec& spec : kFieldSpecs) {
    EXPECT_EQ(spec.pointer.front(), '/') << spec.pointer;
    EXPECT_NE(spec.pointer.back(), '/') << spec.pointer;
  }
}

// An int row whose bounds are inverted would reject every possible value.
TEST(Schema, IntRowsHaveMinLessThanOrEqualMax) {
  for (const FieldSpec& spec : kFieldSpecs) {
    if (spec.type == FieldType::Int) {
      EXPECT_LE(spec.min, spec.max) << spec.pointer;
    }
  }
}

// C9, both directions: the shipped baseline contains exactly the table keys.
// Checked on the file text rather than through a parser, so this case holds
// even before the loader exists - and so a schema change that forgets the
// sample file fails here rather than at runtime.
TEST(Schema, SampleFileMatchesTable) {
  const std::string text = tcu::config::test::TempConfigDir::readFixture("tcu.json");
  ASSERT_FALSE(text.empty()) << "configs/tcu.json not readable";

  // Direction 1: every table pointer has its leaf key present in the file.
  for (const FieldSpec& spec : kFieldSpecs) {
    const std::string_view pointer = spec.pointer;
    const std::size_t slash = pointer.rfind('/');
    const std::string leaf(pointer.substr(slash + 1));
    EXPECT_NE(text.find("\"" + leaf + "\""), std::string::npos)
        << "configs/tcu.json is missing " << spec.pointer;
  }

  // Direction 2: the file holds no key the table does not list. Keys are the
  // only place a quote is immediately followed by a colon, so counting that
  // two-character sequence counts keys: 13 leaves plus the 4 group objects.
  std::size_t keys = 0;
  for (std::size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == '"' && text[i + 1] == ':') {
      ++keys;
    }
  }
  EXPECT_EQ(keys, kFieldSpecs.size() + 4)
      << "configs/tcu.json has a key the schema table does not list, or vice versa";
}

}  // namespace
}  // namespace tcu::config
