#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vindex {

class ArgParser {
 public:
  explicit ArgParser(int argc, char** argv);

  bool HasFlag(const std::string& key) const;
  std::string Get(const std::string& key, const std::string& def = "") const;
  int GetInt(const std::string& key, int def) const;
  size_t GetSizeT(const std::string& key, size_t def) const;
  float GetFloat(const std::string& key, float def) const;

  bool LoadConfigFile(const std::string& path);
  bool LoadDatasetProfile(const std::string& name);

  std::vector<std::string> GetUnknownKeys() const;

  const std::vector<std::string>& positionals() const { return positionals_; }

  static void PrintHelp(const std::string& subcommand = "");

 private:
  std::unordered_map<std::string, std::string> kv_;
  std::vector<std::string> positionals_;
  mutable std::unordered_set<std::string> consumed_;
};

}  // namespace vindex
