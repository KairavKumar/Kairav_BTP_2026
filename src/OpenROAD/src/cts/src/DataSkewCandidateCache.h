// SPDX-License-Identifier: BSD-3-Clause
// Internal research cache: included after Metrics/Choice, inside the CTS
// namespace. Library/SDC/RC provenance is supplied separately; fresh STA and
// geometry are checked.

std::string sourceFingerprint(const SourceModel& model)
{
  std::ostringstream out;
  out << std::setprecision(17);
  for (const auto& l : model.launches) {
    out << std::quoted(l.name) << ' ' << l.rf << '\n';
  }
  for (const auto& rows : model.rows) {
    out << rows.size() << '\n';
    for (const auto& r : rows) {
      out << r.source << ' ' << r.input << ' ' << r.early_offset << ' '
          << r.late_offset << '\n';
    }
  }
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : out.str()) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return std::to_string(hash);
}
void writeLaunchReference(std::ostream& out, const Metrics& base)
{
  out << base.launches.size() << '\n';
  for (size_t s = 0; s < base.launches.size(); ++s) {
    const auto& l = base.source_model->launches[s];
    out << std::quoted(l.name) << ' ' << l.rf << ' ' << base.launches[s].first
        << ' ' << base.launches[s].second << '\n';
  }
}
void readLaunchReference(std::istream& in, const Metrics& base)
{
  size_t n;
  in >> n;
  if (!in || !base.source_model || n != base.launches.size()) {
    throw std::runtime_error("Source cache launch dimension mismatch");
  }
  for (size_t s = 0; s < n; ++s) {
    std::string name;
    int rf;
    double early, late;
    in >> std::quoted(name) >> rf >> early >> late;
    const auto& l = base.source_model->launches[s];
    if (!in || name != l.name || rf != l.rf || !std::isfinite(early)
        || !std::isfinite(late)
        || std::abs(early - base.launches[s].first) > 1e-6
        || std::abs(late - base.launches[s].second) > 1e-6) {
      throw std::runtime_error("Source cache launch identity/value mismatch");
    }
  }
}
void writeLaunchDelta(std::ostream& out, const Metrics& m, const Metrics& base)
{
  std::vector<size_t> changed;
  for (size_t s = 0; s < base.launches.size(); ++s) {
    if (m.launches[s] != base.launches[s]) {
      changed.push_back(s);
    }
  }
  out << changed.size() << '\n';
  for (size_t s : changed) {
    out << s << ' ' << m.launches[s].first - base.launches[s].first << ' '
        << m.launches[s].second - base.launches[s].second << '\n';
  }
}
void readLaunchDelta(std::istream& in, Metrics& m, const Metrics& base)
{
  m.source_model = base.source_model;
  m.launches = base.launches;
  size_t n;
  in >> n;
  if (!in || n > base.launches.size()) {
    throw std::runtime_error("Invalid source delta count");
  }
  std::set<size_t> seen;
  for (size_t i = 0; i < n; ++i) {
    size_t s;
    double a, b;
    in >> s >> a >> b;
    if (!in || s >= base.launches.size() || !seen.insert(s).second
        || !std::isfinite(a) || !std::isfinite(b)) {
      throw std::runtime_error("Invalid/duplicate source delta");
    }
    m.launches[s].first += a;
    m.launches[s].second += b;
  }
}

std::string cacheFingerprint(odb::dbBlock* block,
                             const std::vector<Reference>& refs,
                             const std::vector<Gate>& gates,
                             const std::vector<std::string>& cells,
                             int budget,
                             int depth,
                             bool delays,
                             bool include_ranking = true)
{
  std::ostringstream text;
  text << std::setprecision(17) << budget << ' ' << depth << ' ' << delays
       << ' ' << block->getDbUnitsPerMicron() << ' ';
  const auto core = block->getCoreArea();
  text << core.xMin() << ' ' << core.yMin() << ' ' << core.xMax() << ' '
       << core.yMax();
  for (const auto& cell : cells) {
    text << ' ' << std::quoted(cell);
  }
  for (auto* row : block->getRows()) {
    text << row->getOrigin().x() << ' ' << row->getOrigin().y() << ' '
         << row->getSpacing() << ' ' << row->getSiteCount() << ' '
         << int(row->getOrient().getValue()) << ' '
         << row->getSite()->getWidth() << ' ' << row->getSite()->getHeight()
         << '\n';
  }
  for (auto* inst : block->getInsts()) {
    text << std::quoted(inst->getName()) << ' '
         << std::quoted(inst->getMaster()->getName()) << ' '
         << inst->getLocation().x() << ' ' << inst->getLocation().y() << ' '
         << int(inst->getOrient().getValue()) << ' ' << inst->isDoNotTouch()
         << '\n';
    for (auto* pin : inst->getITerms()) {
      text << std::quoted(pin->getMTerm()->getName()) << ' ';
      text << std::quoted(pin->getNet() ? pin->getNet()->getName() : "")
           << '\n';
    }
  }
  for (const auto& ref : refs) {
    text << std::quoted(ref.pin->getInst()->getName()) << ' ' << ref.lo << ' '
         << ref.hi;
    auto* target
        = odb::dbDoubleProperty::find(ref.pin, "cts_data_skew_target_ns");
    text << ' ' << (target ? target->getValue() : 0) << '\n';
  }
  for (const auto& gate : gates) {
    text << std::quoted(gate.name) << ' ' << gate.weight;
    if (include_ranking) {
      text << ' ' << gate.ranking_floor << ' '
           << (std::isfinite(gate.ceiling) ? gate.ceiling : -1);
    }
    text << '\n';
    for (auto* pin : gate.pins) {
      text << std::quoted(pin->getMTerm()->getName()) << ' ';
    }
  }
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : text.str()) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return std::to_string(
      hash);  // Identity check, not a cryptographic authenticity claim.
}
void cacheScalars(std::ostream& out, const Metrics& m)
{
  out << m.cost << ' ' << m.shift << ' ' << m.setup << ' ' << m.hold << ' '
      << m.area << ' ' << m.clock_area << ' ' << m.electrical << ' ' << m.finite
      << '\n';
}
Metrics readCacheScalars(std::istream& in)
{
  Metrics m;
  in >> m.cost >> m.shift >> m.setup >> m.hold >> m.area >> m.clock_area
      >> m.electrical >> m.finite;
  if (!in || !std::isfinite(m.cost) || !std::isfinite(m.shift)
      || !std::isfinite(m.setup) || !std::isfinite(m.hold)
      || !std::isfinite(m.area) || !std::isfinite(m.clock_area)) {
    throw std::runtime_error("Invalid cache metric scalars");
  }
  return m;
}
void writeCacheReference(std::ostream& out, const Metrics& base)
{
  cacheScalars(out, base);
  out << base.clocks.size() << ' ' << base.envelopes.size() << '\n';
  for (const auto& clock : base.clocks) {
    out << clock.first << ' ' << clock.second << '\n';
  }
  for (const auto& gate : base.envelopes) {
    out << gate.size() << '\n';
    for (const auto& e : gate) {
      out << e.first << ' ' << e.second << '\n';
    }
  }
}
void validateCacheReference(std::istream& in, const Metrics& base)
{
  auto saved = readCacheScalars(in);
  constexpr double epsilon_ns = 1e-6;  // 0.001 ps reproducibility allowance.
  if (std::abs(saved.setup - base.setup) > epsilon_ns
      || std::abs(saved.hold - base.hold) > epsilon_ns
      || std::abs(saved.area - base.area) > 1e-7
      || std::abs(saved.clock_area - base.clock_area) > 1e-7
      || saved.electrical != base.electrical || saved.finite != base.finite) {
    throw std::runtime_error("Cache reference metrics changed");
  }
  size_t nc, ng;
  in >> nc >> ng;
  if (!in || nc != base.clocks.size() || ng != base.envelopes.size()) {
    throw std::runtime_error("Cache reference dimensions changed");
  }
  const auto pair = [&](const auto& expected) {
    double a, b;
    in >> a >> b;
    if (!in || !std::isfinite(a) || !std::isfinite(b)
        || std::max(std::abs(a - expected.first), std::abs(b - expected.second))
               > epsilon_ns) {
      throw std::runtime_error("Cache clock/input reference changed");
    }
  };
  for (const auto& clock : base.clocks) {
    pair(clock);
  }
  for (const auto& gate : base.envelopes) {
    size_t count;
    in >> count;
    if (!in || count != gate.size()) {
      throw std::runtime_error("Cache gate shape changed");
    }
    for (const auto& e : gate) {
      pair(e);
    }
  }
}
void writeCacheDelta(std::ostream& out, const Metrics& m, const Metrics& base)
{
  cacheScalars(out, m);
  std::vector<size_t> widths, clocks;
  std::vector<std::pair<size_t, size_t>> pins;
  for (size_t g = 0; g < base.widths.size(); ++g) {
    if (m.widths[g] != base.widths[g]) {
      widths.push_back(g);
    }
  }
  for (size_t r = 0; r < base.clocks.size(); ++r) {
    if (m.clocks[r] != base.clocks[r]) {
      clocks.push_back(r);
    }
  }
  for (size_t g = 0; g < base.envelopes.size(); ++g) {
    for (size_t p = 0; p < base.envelopes[g].size(); ++p) {
      if (m.envelopes[g][p] != base.envelopes[g][p]) {
        pins.emplace_back(g, p);
      }
    }
  }
  out << widths.size() << '\n';
  for (size_t g : widths) {
    out << g << ' ' << m.widths[g] - base.widths[g] << '\n';
  }
  out << clocks.size() << '\n';
  for (size_t r : clocks) {
    out << r << ' ' << m.clocks[r].first - base.clocks[r].first << ' '
        << m.clocks[r].second - base.clocks[r].second << '\n';
  }
  out << pins.size() << '\n';
  for (const auto& [g, p] : pins) {
    out << g << ' ' << p << ' '
        << m.envelopes[g][p].first - base.envelopes[g][p].first << ' '
        << m.envelopes[g][p].second - base.envelopes[g][p].second << '\n';
  }
}
Metrics readCacheDelta(std::istream& in, const Metrics& base)
{
  auto m = readCacheScalars(in);
  m.widths = base.widths;
  m.clocks = base.clocks;
  m.envelopes = base.envelopes;
  const auto count = [&](size_t maximum) {
    size_t n;
    in >> n;
    if (!in || n > maximum) {
      throw std::runtime_error("Invalid cache record count");
    }
    return n;
  };
  for (size_t n = count(base.widths.size()); n; --n) {
    size_t g;
    double delta;
    in >> g >> delta;
    if (!in || g >= m.widths.size() || !std::isfinite(delta)) {
      throw std::runtime_error("Invalid cache width");
    }
    m.widths[g] += delta;
  }
  for (size_t n = count(base.clocks.size()); n; --n) {
    size_t r;
    double a, b;
    in >> r >> a >> b;
    if (!in || r >= m.clocks.size() || !std::isfinite(a) || !std::isfinite(b)) {
      throw std::runtime_error("Invalid cache clock");
    }
    m.clocks[r].first += a;
    m.clocks[r].second += b;
  }
  size_t maximum = 0;
  for (const auto& gate : base.envelopes) {
    maximum += gate.size();
  }
  for (size_t n = count(maximum); n; --n) {
    size_t g, p;
    double a, b;
    in >> g >> p >> a >> b;
    if (!in || g >= m.envelopes.size() || p >= m.envelopes[g].size()
        || !std::isfinite(a) || !std::isfinite(b)) {
      throw std::runtime_error("Invalid cache input envelope");
    }
    m.envelopes[g][p].first += a;
    m.envelopes[g][p].second += b;
  }
  return m;
}
void saveCandidateCache(const std::string& path,
                        const std::string& signature,
                        const std::string& fingerprint,
                        const Metrics& base,
                        const std::vector<Choice>& choices)
{
  if (std::filesystem::exists(path)
      || std::filesystem::exists(path + ".partial")) {
    throw std::runtime_error("Refusing to overwrite candidate cache");
  }
  std::ofstream out(path + ".partial");
  out << std::setprecision(17);
  out << (base.source_model ? "CTS_SKEW_SOURCE_4\n" : "CTS_SKEW_SPARSE_3\n")
      << std::quoted(signature) << ' ' << std::quoted(fingerprint) << '\n';
  writeCacheReference(out, base);
  if (base.source_model) {
    writeLaunchReference(out, base);
  }
  out << choices.size() << '\n';
  for (const auto& c : choices) {
    out << std::quoted(c.inst->getName()) << ' '
        << std::quoted(c.original->name()) << ' '
        << std::quoted(c.replacement->name()) << ' '
        << std::quoted(c.delay_pin_name) << '\n';
    writeCacheDelta(out, c.measured, base);
    if (base.source_model) {
      writeLaunchDelta(out, c.measured, base);
    }
    const auto site = [&](const DelaySite& s) {
      out << s.point.x() << ' ' << s.point.y() << ' '
          << int(s.orient.getValue()) << '\n';
    };
    site(c.leaf_site);
    out << c.sites.size() << '\n';
    for (const auto& s : c.sites) {
      site(s);
    }
    out << c.boxes.size() << '\n';
    for (const auto& box : c.boxes) {
      out << box.xMin() << ' ' << box.yMin() << ' ' << box.xMax() << ' '
          << box.yMax() << '\n';
    }
  }
  out << "END_CACHE\n";
  out.close();
  if (!out) {
    throw std::runtime_error("Candidate cache write failed; partial retained");
  }
  std::filesystem::rename(path + ".partial", path);
}
std::vector<Choice> loadCandidateCache(
    const std::string& path,
    const std::string& signature,
    const std::string& fingerprint,
    const Metrics& base,
    const std::function<Choice(const std::string&,
                               const std::string&,
                               const std::string&)>& resolve,
    const std::string& legacy_fingerprint = "")
{
  std::ifstream in(path);
  std::string magic, saved_signature, saved_fingerprint;
  in >> magic >> std::quoted(saved_signature) >> std::quoted(saved_fingerprint);
  const bool v4 = magic == "CTS_SKEW_SOURCE_4";
  const bool v3 = magic == "CTS_SKEW_SPARSE_3" || v4;
  if (bool(base.source_model) != v4) {
    throw std::runtime_error(
        "Source model requires a freshly measured version-4 cache");
  }
  const bool current = (v3 || magic == "CTS_SKEW_SPARSE_2")
                       && saved_fingerprint == fingerprint;
  const bool legacy
      = magic == "CTS_SKEW_SPARSE_1"
        && saved_fingerprint
               == (legacy_fingerprint.empty() ? fingerprint
                                              : legacy_fingerprint);
  if (!in || (!current && !legacy) || saved_signature != signature) {
    throw std::runtime_error("Candidate cache provenance/geometry mismatch");
  }
  validateCacheReference(in, base);
  if (v4) {
    readLaunchReference(in, base);
  }
  size_t count;
  in >> count;
  if (!in || count > 4096) {
    throw std::runtime_error("Invalid candidate cache size");
  }
  std::vector<Choice> choices;
  const auto site = [&]() {
    int x, y, orient;
    in >> x >> y >> orient;
    if (!in || orient < 0 || orient > 7) {
      throw std::runtime_error("Invalid cache site");
    }
    return DelaySite{
        odb::Point(x, y),
        odb::dbOrientType(static_cast<odb::dbOrientType::Value>(orient))};
  };
  for (size_t i = 0; i < count; ++i) {
    std::string inst, original, replacement;
    in >> std::quoted(inst) >> std::quoted(original)
        >> std::quoted(replacement);
    auto c = resolve(inst, original, replacement);
    if (v3) {
      in >> std::quoted(c.delay_pin_name);
    }
    c.measured = readCacheDelta(in, base);
    if (v4) {
      readLaunchDelta(in, c.measured, base);
    }
    c.leaf_site = site();
    size_t n;
    in >> n;
    if (!in || n > 8) {
      throw std::runtime_error("Invalid cached delay depth");
    }
    for (; n; --n) {
      c.sites.push_back(site());
    }
    in >> n;
    if (!in || n > 9) {
      throw std::runtime_error("Invalid cached geometry count");
    }
    for (; n; --n) {
      int a, b, x, y;
      in >> a >> b >> x >> y;
      if (!in || x < a || y < b) {
        throw std::runtime_error("Invalid cache rectangle");
      }
      c.boxes.emplace_back(a, b, x, y);
    }
    choices.push_back(std::move(c));
  }
  in >> magic;
  if (!in || magic != "END_CACHE") {
    throw std::runtime_error("Truncated candidate cache");
  }
  return choices;
}
