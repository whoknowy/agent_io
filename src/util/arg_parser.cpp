#include "util/arg_parser.h"

#include <string>
#include <vector>

namespace vindex {

ArgParser::ArgParser(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
      std::string key = arg.substr(2);
      if (i + 1 < argc) {
        std::string next(argv[i + 1]);
        if (next.size() >= 2 && next[0] == '-' && next[1] == '-') {
          kv_[key] = "1";
        } else {
          kv_[key] = next;
          ++i;
        }
      } else {
        kv_[key] = "1";
      }
    } else {
      positionals_.push_back(arg);
    }
  }
}

bool ArgParser::HasFlag(const std::string& key) const {
  return kv_.find(key) != kv_.end();
}

std::string ArgParser::Get(const std::string& key,
                           const std::string& def) const {
  auto it = kv_.find(key);
  return it != kv_.end() ? it->second : def;
}

int ArgParser::GetInt(const std::string& key, int def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) {
    return def;
  }
  return std::stoi(it->second);
}

size_t ArgParser::GetSizeT(const std::string& key, size_t def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) {
    return def;
  }
  return static_cast<size_t>(std::stoull(it->second));
}

}  // namespace vindex
