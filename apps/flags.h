#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lldstore::app {

// --name=value flags and positional arguments. Unknown flags are an error (a typo in a benchmark
// flag must not silently run the default).
class Flags {
 public:
  Flags(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string_view a = argv[i];
      if (a.rfind("--", 0) == 0) {
        const size_t eq = a.find('=');
        values_[std::string(a.substr(2, eq == std::string_view::npos ? std::string_view::npos : eq - 2))] =
            eq == std::string_view::npos ? "true" : std::string(a.substr(eq + 1));
      } else {
        positional_.emplace_back(a);
      }
    }
  }

  std::string Get(const std::string& name, const std::string& def = "") {
    used_[name] = true;
    auto it = values_.find(name);
    return it == values_.end() ? def : it->second;
  }
  long long Int(const std::string& name, long long def) {
    const std::string v = Get(name);
    return v.empty() ? def : std::strtoll(v.c_str(), nullptr, 10);
  }
  double Double(const std::string& name, double def) {
    const std::string v = Get(name);
    return v.empty() ? def : std::strtod(v.c_str(), nullptr);
  }
  const std::vector<std::string>& positional() const { return positional_; }

  // Returns the first flag that was given but never read, or "".
  std::string Unused() const {
    for (const auto& [k, v] : values_) {
      if (!used_.count(k)) return k;
    }
    return "";
  }

 private:
  std::map<std::string, std::string> values_;
  std::map<std::string, bool> used_;
  std::vector<std::string> positional_;
};

inline std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t end = s.find(sep, start);
    out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

}  // namespace lldstore::app
