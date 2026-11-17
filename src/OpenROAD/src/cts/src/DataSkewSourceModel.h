// SPDX-License-Identifier: BSD-3-Clause
// Internal implementation, included inside CTS after Gate/Metrics/arrivalNs.
struct SourceModel
{
  struct Launch
  {
    const sta::Pin* pin;
    int rf;
    std::string name;
  };
  std::vector<Launch> launches;
  std::vector<std::pair<double, double>> baseline_launches;
  std::vector<std::vector<DataSkewSourceRow>> rows;
  double audit_error = 0;

  std::vector<std::pair<double, double>> measure(sta::dbSta* sta) const
  {
    std::vector<std::pair<double, double>> result;
    result.reserve(launches.size());
    for (const auto& launch : launches) {
      const auto* rf
          = launch.rf ? sta::RiseFallBoth::fall() : sta::RiseFallBoth::rise();
      const double lo = arrivalNs(sta, launch.pin, rf, sta::MinMax::min());
      const double hi = arrivalNs(sta, launch.pin, rf, sta::MinMax::max());
      if (!std::isfinite(lo) || !std::isfinite(hi)) {
        throw std::runtime_error("Unconstrained data launch: " + launch.name);
      }
      result.emplace_back(lo, hi);
    }
    return result;
  }

  SourceModel(odb::dbBlock* block,
              sta::dbNetwork* network,
              sta::dbSta* sta,
              const std::vector<Gate>& gates)
  {
    // A plain graph does not carry arbitrary exception state. Reject that
    // unsupported scope instead of silently propagating illegal source paths.
    if (!sta->cmdMode()->sdc()->exceptions().empty()) {
      throw std::runtime_error("Source model does not support path exceptions");
    }
    auto* graph = sta->ensureGraph();
    auto add = [&](const sta::Pin* pin) {
      if (!graph->pinLoadVertex(pin)) {
        return;
      }
      for (int rf : {0, 1}) {
        launches.push_back({pin, rf, network->pathName(pin)});
      }
    };
    for (auto* inst : block->getInsts()) {
      auto* cell = network->libertyCell(network->dbToSta(inst));
      if (cell && cell->isSequential()) {
        for (auto* term : inst->getITerms()) {
          if (term->isOutputSignal() && term->getNet()) {
            add(network->dbToSta(term));
          }
        }
      }
    }
    for (auto* term : block->getBTerms()) {
      if (term->getIoType() == odb::dbIoType::INPUT && term->getNet()
          && term->getNet()->getSigType() != odb::dbSigType::CLOCK) {
        add(network->dbToSta(term));
      }
    }
    std::sort(
        launches.begin(), launches.end(), [](const auto& a, const auto& b) {
          return std::tie(a.name, a.rf) < std::tie(b.name, b.rf);
        });
    baseline_launches = measure(sta);
    std::vector<sta::Vertex*> vertices;
    std::unordered_map<sta::Vertex*, size_t> index;
    sta::VertexIterator vi(graph);
    while (vi.hasNext()) {
      auto* v = vi.next();
      if (v->isBidirectDriver()) {
        throw std::runtime_error("Source model: bidirectional pin");
      }
      index[v] = vertices.size();
      vertices.push_back(v);
    }
    struct Arc
    {
      size_t target;
      int from_rf, to_rf;
      double early, late;
    };
    std::vector<std::vector<Arc>> arcs(vertices.size());
    std::vector<size_t> degree(vertices.size(), 0);
    std::vector<bool> is_source(vertices.size(), false);
    using State = std::map<size_t, std::pair<double, double>>;
    std::vector<State> states(vertices.size());
    for (size_t s = 0; s < launches.size(); ++s) {
      const size_t v = index.at(graph->pinLoadVertex(launches[s].pin));
      is_source[v] = true;
      states[v][2 * s + launches[s].rf] = baseline_launches[s];
    }
    auto* scene = sta->scenes().front();
    for (size_t v = 0; v < vertices.size(); ++v) {
      sta::VertexOutEdgeIterator ei(vertices[v], graph);
      while (ei.hasNext()) {
        auto* edge = ei.next();
        if (edge->role() != sta::TimingRole::wire()
            && edge->role() != sta::TimingRole::combinational()) {
          continue;
        }
        const size_t target = index.at(edge->to(graph));
        if (is_source[target]) {
          continue;
        }
        for (auto* arc : edge->timingArcSet()->arcs()) {
          if (!sta::searchThru(edge, arc, sta->cmdMode())) {
            continue;
          }
          const auto* from = arc->fromEdge()->asRiseFall();
          const auto* to = arc->toEdge()->asRiseFall();
          if (!from || !to) {
            throw std::runtime_error("Source model: unsupported transition");
          }
          const double early
              = sta::delayAsFloat(graph->arcDelay(
                    edge, arc, scene->dcalcAnalysisPtIndex(sta::MinMax::min())))
                / ns;
          const double late
              = sta::delayAsFloat(graph->arcDelay(
                    edge, arc, scene->dcalcAnalysisPtIndex(sta::MinMax::max())))
                / ns;
          if (!std::isfinite(early) || !std::isfinite(late)) {
            throw std::runtime_error("Source model: nonfinite arc");
          }
          arcs[v].push_back(
              {target, int(from->index()), int(to->index()), early, late});
          ++degree[target];
        }
      }
    }
    std::queue<size_t> ready;
    for (size_t v = 0; v < degree.size(); ++v) {
      if (!degree[v]) {
        ready.push(v);
      }
    }
    size_t visited = 0, count = 0;
    while (!ready.empty()) {
      const size_t v = ready.front();
      ready.pop();
      ++visited;
      count += states[v].size();
      if (count > 2000000) {
        throw std::runtime_error("Source model state budget exceeded");
      }
      for (const auto& arc : arcs[v]) {
        auto& target = states[arc.target];
        for (const auto& [key, window] : states[v]) {
          if (int(key % 2) != arc.from_rf) {
            continue;
          }
          const size_t next = (key / 2) * 2 + arc.to_rf;
          const auto value
              = std::pair{window.first + arc.early, window.second + arc.late};
          const auto it = target.find(next);
          if (it == target.end()) {
            target[next] = value;
          } else {
            it->second.first = std::min(it->second.first, value.first);
            it->second.second = std::max(it->second.second, value.second);
          }
        }
        if (--degree[arc.target] == 0) {
          ready.push(arc.target);
        }
      }
    }
    if (visited != vertices.size()) {
      throw std::runtime_error("Source model: combinational cycle");
    }
    for (const auto& gate : gates) {
      rows.emplace_back();
      for (size_t p = 0; p < gate.pins.size(); ++p) {
        auto* pin = network->dbToSta(gate.pins[p]);
        const auto& source_states = states[index.at(graph->pinLoadVertex(pin))];
        for (int rf : {0, 1}) {
          double lo = std::numeric_limits<double>::infinity(), hi = -lo;
          for (const auto& [key, value] : source_states) {
            if (int(key % 2) != rf) {
              continue;
            }
            const size_t s = key / 2;
            rows.back().push_back({s,
                                   p * 2 + rf,
                                   value.first - baseline_launches[s].first,
                                   value.second - baseline_launches[s].second});
            lo = std::min(lo, value.first);
            hi = std::max(hi, value.second);
          }
          if (!std::isfinite(lo) || !std::isfinite(hi)) {
            throw std::runtime_error("Source model: unresolved marked input");
          }
          const auto* edge
              = rf ? sta::RiseFallBoth::fall() : sta::RiseFallBoth::rise();
          audit_error = std::max(
              {audit_error,
               std::abs(lo - arrivalNs(sta, pin, edge, sta::MinMax::min())),
               std::abs(hi - arrivalNs(sta, pin, edge, sta::MinMax::max()))});
        }
      }
    }
    if (audit_error > 5e-6) {
      throw std::runtime_error("Source model baseline union audit >0.005 ps");
    }
  }
};

size_t responseSize(const Metrics& m, size_t g)
{
  return m.source_model ? m.source_model->rows.at(g).size()
                        : m.envelopes.at(g).size();
}
std::pair<double, double> responseAt(const Metrics& m, size_t g, size_t p)
{
  return m.source_model
             ? sourceArrival(m.source_model->rows.at(g).at(p), m.launches)
             : m.envelopes.at(g).at(p);
}
