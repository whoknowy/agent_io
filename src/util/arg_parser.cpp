#include "util/arg_parser.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
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
  consumed_.insert(key);
  return kv_.find(key) != kv_.end();
}

std::string ArgParser::Get(const std::string& key,
                           const std::string& def) const {
  consumed_.insert(key);
  auto it = kv_.find(key);
  return it != kv_.end() ? it->second : def;
}

int ArgParser::GetInt(const std::string& key, int def) const {
  consumed_.insert(key);
  auto it = kv_.find(key);
  if (it == kv_.end()) {
    return def;
  }
  return std::stoi(it->second);
}

size_t ArgParser::GetSizeT(const std::string& key, size_t def) const {
  consumed_.insert(key);
  auto it = kv_.find(key);
  if (it == kv_.end()) {
    return def;
  }
  return static_cast<size_t>(std::stoull(it->second));
}

float ArgParser::GetFloat(const std::string& key, float def) const {
  consumed_.insert(key);
  auto it = kv_.find(key);
  if (it == kv_.end()) {
    return def;
  }
  return std::stof(it->second);
}

std::vector<std::string> ArgParser::GetUnknownKeys() const {
  std::vector<std::string> unknown;
  for (const auto& kv : kv_) {
    if (consumed_.find(kv.first) == consumed_.end()) {
      unknown.push_back(kv.first);
    }
  }
  return unknown;
}

// --- Minimal flat JSON parser ---
namespace {

void SkipWhitespace(const std::string& text, size_t& pos) {
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                text[pos] == '\n' || text[pos] == '\r')) {
    ++pos;
  }
}

bool ParseJsonString(const std::string& text, size_t& pos, std::string& out) {
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;  // skip opening quote
  out.clear();
  while (pos < text.size()) {
    char ch = text[pos];
    if (ch == '"') {
      ++pos;
      return true;
    }
    if (ch == '\\' && pos + 1 < text.size()) {
      ++pos;
      char esc = text[pos];
      if (esc == '"' || esc == '\\' || esc == '/') {
        out.push_back(esc);
      } else if (esc == 'n') {
        out.push_back('\n');
      } else if (esc == 't') {
        out.push_back('\t');
      } else if (esc == 'r') {
        out.push_back('\r');
      } else {
        out.push_back(ch);  // unknown escape, keep literal
      }
      ++pos;
    } else {
      out.push_back(ch);
      ++pos;
    }
  }
  return false;  // unterminated string
}

bool ParseJsonValue(const std::string& text, size_t& pos, std::string& out) {
  SkipWhitespace(text, pos);
  if (pos >= text.size()) return false;

  char ch = text[pos];

  // String
  if (ch == '"') {
    return ParseJsonString(text, pos, out);
  }

  // Number (integer or float)
  if ((ch >= '0' && ch <= '9') || ch == '-') {
    size_t start = pos;
    if (ch == '-') ++pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos < text.size() && text[pos] == '.') {
      ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
      ++pos;
      if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    }
    out = text.substr(start, pos - start);
    return true;
  }

  // true / false / null
  if (text.compare(pos, 4, "true") == 0) {
    pos += 4;
    out = "1";
    return true;
  }
  if (text.compare(pos, 5, "false") == 0) {
    pos += 5;
    out = "0";
    return true;
  }
  if (text.compare(pos, 4, "null") == 0) {
    pos += 4;
    out = "";
    return true;
  }

  return false;
}

bool ParseFlatJson(const std::string& text,
                   std::unordered_map<std::string, std::string>& out) {
  size_t pos = 0;
  SkipWhitespace(text, pos);
  if (pos >= text.size() || text[pos] != '{') return false;
  ++pos;  // skip '{'

  bool first = true;
  while (pos < text.size()) {
    SkipWhitespace(text, pos);
    if (pos >= text.size()) return false;

    if (text[pos] == '}') {
      ++pos;
      return true;
    }

    if (!first) {
      if (text[pos] != ',') return false;
      ++pos;
      SkipWhitespace(text, pos);
    }
    first = false;

    // Key must be a string
    std::string key;
    if (!ParseJsonString(text, pos, key)) return false;

    SkipWhitespace(text, pos);
    if (pos >= text.size() || text[pos] != ':') return false;
    ++pos;  // skip ':'

    // Value
    std::string value;
    if (!ParseJsonValue(text, pos, value)) return false;

    // Only set if not already provided via CLI
    if (out.find(key) == out.end()) {
      out[key] = value;
    }
  }
  return false;  // missing closing brace
}

}  // namespace

bool ArgParser::LoadConfigFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Warning: could not open config file: " << path << "\n";
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string text = buffer.str();

  std::unordered_map<std::string, std::string> file_kv;
  if (!ParseFlatJson(text, file_kv)) {
    std::cerr << "Warning: failed to parse config file: " << path << "\n";
    return false;
  }

  for (auto& entry : file_kv) {
    // Convert underscore to hyphen for CLI compatibility
    std::string key = entry.first;
    for (size_t i = 0; i < key.size(); ++i) {
      if (key[i] == '_') key[i] = '-';
    }
    // Only load if not already set from CLI
    if (kv_.find(key) == kv_.end()) {
      kv_[key] = entry.second;
    }
  }

  return true;
}

bool ArgParser::LoadDatasetProfile(const std::string& name) {
  // Try current working directory, then executable-relative paths.
  std::ifstream file("datasets.json");
  if (!file.is_open()) {
    std::cerr << "Warning: could not open datasets.json\n";
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string text = buffer.str();

  std::unordered_map<std::string, std::string> file_kv;
  if (!ParseFlatJson(text, file_kv)) {
    std::cerr << "Warning: failed to parse datasets.json\n";
    return false;
  }

  std::string prefix = name + ".";
  for (const auto& entry : file_kv) {
    const std::string& full_key = entry.first;
    if (full_key.size() <= prefix.size() ||
        full_key.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }

    std::string role = full_key.substr(prefix.size());
    // Map dataset role to CLI parameter name.
    std::string cli_key;
    if (role == "base" || role == "query" || role == "learn") {
      cli_key = "input";
    } else if (role == "manifest") {
      cli_key = "manifest";
    } else if (role == "groundtruth") {
      cli_key = "groundtruth";
    } else {
      cli_key = role;
    }

    // Only inject if not already set from CLI.
    if (kv_.find(cli_key) == kv_.end()) {
      kv_[cli_key] = entry.second;
    }
  }

  return true;
}

void ArgParser::PrintHelp(const std::string& subcommand) {
  if (subcommand.empty()) {
    std::cout << "vindex - Vector index with I/O optimizations\n\n"
              << "Usage:\n"
              << "  vindex <command> [options]\n\n"
              << "Commands:\n"
              << "  build      Build a vector index from input vectors\n"
              << "  query      Search the index for nearest neighbors\n"
              << "  eval       Evaluate recall against ground truth\n"
              << "  insert     Insert new vectors into an existing index\n"
              << "  train-pq   Train a Product Quantization codebook\n\n"
              << "Global options:\n"
              << "  --config <path>    Load parameters from a JSON config file\n"
              << "  --dataset <name>   Use a dataset profile (e.g. siftsmall, siftsmall-pq)\n"
              << "  --help             Show help for a command (e.g. vindex build --help)\n";
    return;
  }

  if (subcommand == "build") {
    std::cout << "Usage: vindex build --input <fvecs> --output <dir> [options]\n\n"
              << "Build a graph-based vector index from input vectors.\n\n"
              << "Required:\n"
              << "  --input <path>         Input vector file (.fvecs)\n\n"
              << "Options:\n"
              << "  --output <dir>         Output directory (default: data)\n"
              << "  --degree <int>         Max out-degree of graph nodes (default: 32)\n"
              << "  --builder <str>        Graph builder: brute, vamana, auto (default: auto)\n"
              << "  --codebook <path>      PQ codebook file for compression (.pcb)\n"
              << "  --limit <int>          Max vectors to load, 0 = all (default: 0)\n"
              << "  --segment <path>       Segment file path (default: <output>/segment_0.vsg)\n"
              << "  --manifest <path>      Manifest file path (default: <output>/manifest.txt)\n"
              << "  --build-beam <int>     Beam width during graph construction (default: 64)\n"
              << "  --alpha <float>        Vamana pruning parameter (default: 1.2)\n"
              << "  --max-build-visits <int>  Max nodes visited during build (default: 5000)\n"
              << "  --help                 Show this help text\n";
    return;
  }

  if (subcommand == "query") {
    std::cout << "Usage: vindex query --manifest <file> --input <fvecs> [options]\n\n"
              << "Search the index for approximate nearest neighbors.\n\n"
              << "Required:\n"
              << "  --manifest <path>      Manifest file from a previous build\n"
              << "  --input <path>         Query vector file (.fvecs)\n\n"
              << "Options:\n"
              << "  --topk <int>           Number of nearest neighbors to return (default: 10)\n"
              << "  --beam <int>           Beam width for search (default: 8)\n"
              << "  --max-visits <int>     Max nodes to visit per search (default: 1000)\n"
              << "  --limit <int>          Max query vectors to process, 0 = all (default: 0)\n"
              << "  --cache <int>          Cache size in MB, 0 = disabled (default: 0)\n"
              << "  --prefetch             Enable topology-aware async I/O prefetch\n"
              << "  --threads <int>        Number of parallel query threads (default: 4)\n"
              << "  --help                 Show this help text\n";
    return;
  }

  if (subcommand == "eval") {
    std::cout << "Usage: vindex eval --manifest <file> --input <fvecs> --groundtruth <ivecs> [options]\n\n"
              << "Evaluate search recall against ground truth annotations.\n\n"
              << "Required:\n"
              << "  --manifest <path>      Manifest file from a previous build\n"
              << "  --input <path>         Query vector file (.fvecs)\n"
              << "  --groundtruth <path>   Ground truth file (.ivecs)\n\n"
              << "Options:\n"
              << "  --topk <int>           Number of nearest neighbors (default: 10)\n"
              << "  --beam <int>           Beam width for search (default: 8)\n"
              << "  --max-visits <int>     Max nodes to visit per search (default: 1000)\n"
              << "  --limit <int>          Max queries to evaluate, 0 = all (default: 0)\n"
              << "  --cache <int>          Cache size in MB, 0 = disabled (default: 0)\n"
              << "  --prefetch             Enable topology-aware async I/O prefetch\n"
              << "  --threads <int>        Number of parallel query threads (default: 4)\n"
              << "  --help                 Show this help text\n";
    return;
  }

  if (subcommand == "insert") {
    std::cout << "Usage: vindex insert --manifest <file> --input <fvecs> --output <dir> [options]\n\n"
              << "Insert new vectors into an existing index (LSM-Tree style).\n\n"
              << "Required:\n"
              << "  --manifest <path>      Existing manifest file\n"
              << "  --input <path>         New vectors to insert (.fvecs)\n\n"
              << "Options:\n"
              << "  --output <dir>         Output directory for new segments (default: data)\n"
              << "  --degree <int>         Graph out-degree (default: 32)\n"
              << "  --builder <str>        Graph builder: brute, vamana, auto (default: auto)\n"
              << "  --flush <int>          Vectors per batch before flushing, 0 = all (default: 0)\n"
              << "  --limit <int>          Max vectors to load, 0 = all (default: 0)\n"
              << "  --build-beam <int>     Beam width during graph construction (default: 64)\n"
              << "  --alpha <float>        Vamana pruning parameter (default: 1.2)\n"
              << "  --max-build-visits <int>  Max nodes visited during build (default: 5000)\n"
              << "  --help                 Show this help text\n";
    return;
  }

  if (subcommand == "train-pq") {
    std::cout << "Usage: vindex train-pq --input <fvecs> --output <codebook> [options]\n\n"
              << "Train a Product Quantization (PQ) codebook using K-Means.\n\n"
              << "Required:\n"
              << "  --input <path>         Training vector file (.fvecs)\n"
              << "  --output <path>        Output codebook file (.pcb)\n\n"
              << "Options:\n"
              << "  --pq-m <int>           Number of subspaces (default: 64)\n"
              << "  --pq-k <int>           Number of centroids per subspace (default: 256)\n"
              << "  --pq-iters <int>       Max K-Means iterations (default: 25)\n"
              << "  --limit <int>          Max training vectors to use (default: 100000)\n"
              << "  --help                 Show this help text\n";
    return;
  }

  // Unknown subcommand, print top-level help
  PrintHelp();
}

}  // namespace vindex
