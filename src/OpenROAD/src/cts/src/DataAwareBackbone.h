// SPDX-License-Identifier: BSD-3-Clause
// Internal construction block included in HTreeBuilder::run after leaf
// grouping.
{
  auto* block = clock_.getNetObj()->getBlock();
  auto* fast = odb::dbIntProperty::find(block, "cts_align_fast_backbone");
  auto* mode = odb::dbIntProperty::find(block, "cts_data_alignment");
  if (fast && fast->getValue() == 1 && mode && mode->getValue() == 1) {
    unsigned degree = 4;
    if (auto* p
        = odb::dbIntProperty::find(block, "cts_align_backbone_fanout")) {
      if (p->getValue() < 2 || p->getValue() > 16) {
        logger_->error(CTS, 334, "Backbone fanout must be 2..16.");
      }
      degree = p->getValue();
    }
    if (options_->getMaxFanout()) {
      degree = std::min(degree, options_->getMaxFanout());
    }
    if (degree < 2 || topLevelSinksClustered_.empty()) {
      logger_->error(
          CTS, 613, "Fast data-aware backbone requires sinks and fanout >=2.");
    }
    std::vector<ClockInst*> nodes;
    for (const auto& location : topLevelSinksClustered_) {
      nodes.push_back(mapLocationToSink_.at(
          Point<double>(location.first, location.second)));
    }
    const auto core = block->getCoreArea();
    auto morton = [&](const ClockInst* inst) {
      const auto x = static_cast<std::uint32_t>(std::clamp<long long>(
          (static_cast<long long>(inst->getX()) - core.xMin()) * 65535
              / std::max(1, core.xMax() - core.xMin()),
          0,
          65535));
      const auto y = static_cast<std::uint32_t>(std::clamp<long long>(
          (static_cast<long long>(inst->getY()) - core.yMin()) * 65535
              / std::max(1, core.yMax() - core.yMin()),
          0,
          65535));
      std::uint32_t result = 0;
      for (unsigned bit = 0; bit < 16; ++bit) {
        result |= ((x >> bit) & 1u) << (2 * bit);
        result |= ((y >> bit) & 1u) << (2 * bit + 1);
      }
      return result;
    };
    size_t serial = 0;
    auto bank = [&](const std::vector<ClockInst*>& children, bool root) {
      double x = 0, y = 0;
      for (const auto* child : children) {
        x += child->getX();
        y += child->getY();
      }
      Point<double> centre(x / children.size() / wireSegmentUnit_,
                           y / children.size() / wireSegmentUnit_);
      const auto legal = legalizeOneBuffer(centre, options_->getRootBuffer());
      commitMoveLoc(centre, legal);
      const auto id = std::to_string(serial++);
      auto& driver
          = clock_.addClockBuffer(root ? "clkbuf_0" : "clkbuf_align_bank_" + id,
                                  options_->getRootBuffer(),
                                  legal.getX() * wireSegmentUnit_,
                                  legal.getY() * wireSegmentUnit_);
      addTreeLevelBuffer(&driver);
      auto& net
          = clock_.addSubNet(root ? "clknet_0" : "clknet_align_bank_" + id);
      net.addInst(driver);
      for (auto* child : children) {
        net.addInst(*child);
      }
      mapLocationToSink_[Point<double>(static_cast<float>(legal.getX()),
                                       static_cast<float>(legal.getY()))]
          = &driver;
      if (root) {
        topBufferName_ = driver.getName();
      }
      return &driver;
    };
    const size_t leaves = nodes.size();
    while (nodes.size() > degree) {
      std::stable_sort(nodes.begin(), nodes.end(), [&](auto* a, auto* b) {
        const auto ka = morton(a), kb = morton(b);
        return ka == kb ? a->getName() < b->getName() : ka < kb;
      });
      std::vector<ClockInst*> parents;
      for (size_t i = 0; i < nodes.size(); i += degree) {
        const auto end = std::min(nodes.size(), i + degree);
        if (end - i == 1) {
          parents.push_back(nodes[i]);
          continue;
        }
        const std::vector<ClockInst*> children(nodes.begin() + i,
                                               nodes.begin() + end);
        parents.push_back(bank(children, false));
      }
      nodes = std::move(parents);
      ++treeBufLevels_;
      if (treeBufLevels_ >= clockTreeMaxDepth_) {
        logger_->error(
            CTS, 614, "Fast backbone exceeds configured clock-tree depth.");
      }
    }
    bank(nodes, true);
    ++treeBufLevels_;
    logger_->info(CTS,
                  333,
                  "Fast data-aware backbone: {} leaves, {} banks, fanout <= "
                  "{}, {} levels.",
                  leaves,
                  serial,
                  degree,
                  treeBufLevels_);
    return;
  }
}
