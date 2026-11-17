// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "TreeBuilder.h"

namespace sta {
class dbNetwork;
class dbSta;
}  // namespace sta

namespace cts {

// Opt-in single-driver asymmetric tree. A bounded subtree frontier preserves
// discrete physical witnesses and distinct latency/energy choices. Its timing
// model is a construction estimate; measured STA remains the acceptance check.
class CostAwareTreeBuilder : public TreeBuilder
{
 public:
  CostAwareTreeBuilder(CtsOptions* options,
                       Clock& clock,
                       TreeBuilder* parent,
                       utl::Logger* logger,
                       odb::dbDatabase* db,
                       sta::dbNetwork* network,
                       sta::dbSta* sta)
      : TreeBuilder(options, clock, parent, logger, db),
        network_(network),
        sta_(sta)
  {
  }
  void run() override;

 private:
  sta::dbNetwork* network_;
  sta::dbSta* sta_;
};
}  // namespace cts
