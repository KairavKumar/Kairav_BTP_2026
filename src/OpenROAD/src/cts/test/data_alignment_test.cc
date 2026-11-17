// SPDX-License-Identifier: BSD-3-Clause
#include "gtest/gtest.h"
#include "src/cts/src/DataAwareClustering.h"

namespace cts {
TEST(DataAlignment, EqualRootTargetsCanShare)
{
  const std::vector<AlignmentSink> sinks{{0, 0, 1, .20, .02},
                                         {1, 0, 1, .23, .05}};
  const std::vector<AlignmentGate> gates{{1, {{0, 0, 0}, {1, 0, 0}}}};
  const auto groups = clusterAlignedSinks(sinks, gates, 2, 10, 2, .001);
  ASSERT_EQ(groups.size(), 1);
  EXPECT_EQ(groups.front().size(), 2);
}
TEST(DataAlignment, SameShiftDoesNotImplyCompatibleArrivals)
{
  const std::vector<AlignmentSink> sinks{{0, 0, 1, .20, .02},
                                         {1, 0, 1, .23, .02}};
  const std::vector<AlignmentGate> gates{{1, {{0, 0, 0}, {1, 0, 0}}}};
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .005).size(), 2);
}
TEST(DataAlignment, DataPenaltyPreventsGeometricallyValidMerge)
{
  const std::vector<AlignmentSink> sinks{{0, 0, 1, .20, 0}, {1, 0, 1, .204, 0}};
  // Each source is aligned with a fixed input. Sharing a root worsens both.
  const std::vector<AlignmentGate> gates{{1, {{0, 0, 0}, {-1, .20, .20}}},
                                         {1, {{1, 0, 0}, {-1, .204, .204}}}};
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .01).size(), 2);
}
TEST(DataAlignment, PhysicalLimitsAndMalformedSourceFailClosed)
{
  const std::vector<AlignmentSink> sinks{{0, 0, 1, .20, 0}, {20, 0, 1, .20, 0}};
  const std::vector<AlignmentGate> gates{{1, {{0, 0, 0}, {1, 0, 0}}}};
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .01).size(), 2);
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 1, 30, 2, .01).size(), 2);
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 30, 1, .01).size(), 2);
  EXPECT_THROW(clusterAlignedSinks(sinks, {{1, {{2, 0, 0}}}}, 2, 30, 2, .01),
               std::invalid_argument);
}

TEST(DataAlignment, SharingBudgetCannotOverrideGateOrMovementCaps)
{
  std::vector<AlignmentSink> sinks{{0, 0, 1, .20, 0}, {1, 0, 1, .204, 0}};
  std::vector<AlignmentGate> gates{{1, {{0, 0, 0}, {-1, .20, .20}}, .003},
                                   {1, {{1, 0, 0}, {-1, .204, .204}}, .003}};
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .01, .005).size(), 1);
  gates[0].ceiling = .001;
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .01, .005).size(), 2);
  gates[0].ceiling = .003;
  sinks[0].upper = .201;
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .01, .005).size(), 2);
}
TEST(DataAlignment, ADataCompatibleMergeMustAlsoProtectEndpointTiming)
{
  std::vector<AlignmentSink> sinks{{0, 0, 1, .21, 0}, {1, 0, 1, .20, 0}};
  sinks[0].reference = sinks[1].reference = .20;
  const std::vector<AlignmentGate> gates{{1, {{0, 0, 0}, {1, .01, .01}}}};
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .02, 1).size(), 1);
  // Hold requires launch-capture >=9ps. Shared-root merging loses that control.
  const std::vector<AlignmentTimingCheck> checks{{0, 1, -1, -.009}};
  EXPECT_EQ(clusterAlignedSinks(sinks, gates, 2, 10, 2, .02, 1, checks).size(),
            2);
}
}  // namespace cts
