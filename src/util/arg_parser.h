#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace vindex {

class ArgParser {
 public:
  explicit ArgParser(int argc, char** argv);

  bool HasFlag(const std::string& key) const;
  std::string Get(const std::string& key, const std::string& def = "") const;
  int GetInt(const std::string& key, int def) const;
  size_t GetSizeT(const std::string& key, size_t def) const;

  const std::vector<std::string>& positionals() const { return positionals_; }

 private:
  std::unordered_map<std::string, std::string> kv_;
  std::vector<std::string> positionals_;
};

}  // namespace vindex
