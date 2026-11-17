// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>
#include <vector>
namespace odb {
class dbBlock;
}
namespace sta {
class dbNetwork;
class dbSta;
}  // namespace sta
namespace rsz {
class Resizer;
}
namespace est {
class EstimateParasitics;
}
namespace utl {
class Logger;
}
namespace cts {
void constructDataAlignedBranches(odb::dbBlock* block,
                                  sta::dbNetwork* network,
                                  sta::dbSta* sta,
                                  rsz::Resizer* resizer,
                                  est::EstimateParasitics* parasitics,
                                  utl::Logger* logger,
                                  const std::vector<std::string>& cells);
void sizeDataSkewLeaves(odb::dbBlock* block,
                        sta::dbNetwork* network,
                        sta::dbSta* sta,
                        rsz::Resizer* resizer,
                        est::EstimateParasitics* parasitics,
                        utl::Logger* logger,
                        const std::vector<std::string>& cells);
}  // namespace cts
