// SPDX-License-Identifier: BSD-3-Clause
#include "gtest/gtest.h"
#include "src/cts/src/DataSkewResponse.h"

namespace cts {

TEST(DataSkewResponse, HiddenSourceBecomesLatestBeforeCollapse)
{
  const DataSkewSourceRow a{0, 0, 0, 0};
  const DataSkewSourceRow b{1, 0, 0, 0};
  const std::vector<std::pair<double, double>> baseline{{0, 0}, {.01, .01}};
  auto first = baseline, second = baseline, joint = baseline;
  first[0] = {.02, .02};
  second[1] = {.03, .03};
  joint[0] = {.02, .02};
  joint[1] = {.03, .03};
  const auto late = [&](const auto& q) {
    return std::max(sourceArrival(a, q).second, sourceArrival(b, q).second);
  };
  EXPECT_NEAR(late(first) + late(second) - late(baseline), .04, 1e-15);
  EXPECT_NEAR(late(joint), .03, 1e-15);
}

TEST(DataSkewResponse, OutputTransitionAndPathOffsetsRemainDistinct)
{
  const std::vector<std::pair<double, double>> q{{.10, .11}, {.12, .14}};
  const auto rise = sourceArrival({0, 0, .02, .03}, q);
  const auto fall = sourceArrival({1, 0, .04, .05}, q);
  EXPECT_NEAR(rise.first, .12, 1e-15);
  EXPECT_NEAR(rise.second, .14, 1e-15);
  EXPECT_NEAR(fall.first, .16, 1e-15);
  EXPECT_NEAR(fall.second, .19, 1e-15);
  EXPECT_THROW(sourceArrival({2, 0, 0, 0}, q), std::runtime_error);
}

TEST(DataSkewResponse, ArchiveUsesActualLexicographicQuality)
{
  const DataSkewScore baseline{.1, 100, 0};
  EXPECT_FALSE(betterDataSkewScore({.1, 100.01, .01}, baseline));
  EXPECT_FALSE(betterDataSkewScore({.11, 99, .01}, baseline));
  EXPECT_TRUE(betterDataSkewScore({.1, 99, .01}, baseline));
  EXPECT_TRUE(betterDataSkewScore({.09, 101, .01}, baseline));
  EXPECT_TRUE(betterDataSkewScore({.1, 100, .01}, {.1, 100, .02}));
}

}  // namespace cts
