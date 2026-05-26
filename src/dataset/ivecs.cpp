#include "dataset/ivecs.h"

#include <cstdint>
#include <fstream>
#include <vector>

namespace vindex {

bool LoadIvecs(const std::string& path, IvecsData& out, size_t max_vectors) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return false;
  }

  uint32_t dim = 0;
  uint32_t first_dim = 0;
  out.dim = 0;
  out.values.clear();

  std::vector<int32_t> buf;
  size_t count = 0;

  while (max_vectors == 0 || count < max_vectors) {
    int32_t d = 0;
    if (!stream.read(reinterpret_cast<char*>(&d), sizeof(d))) {
      break;
    }
    if (d <= 0) {
      break;
    }
    dim = static_cast<uint32_t>(d);

    if (count == 0) {
      first_dim = dim;
      buf.resize(dim);
    } else if (dim != first_dim) {
      return false;
    }

    if (!stream.read(reinterpret_cast<char*>(buf.data()),
                     static_cast<std::streamsize>(dim * sizeof(int32_t)))) {
      break;
    }

    out.values.insert(out.values.end(), buf.begin(), buf.begin() + dim);
    ++count;
  }

  out.dim = first_dim;
  return out.dim > 0 && out.count() > 0;
}

}  // namespace vindex
