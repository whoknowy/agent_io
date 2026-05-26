#include "pq/pq_trainer.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

#include "core/distance.h"
#include "io/file_io.h"

namespace vindex {

namespace {

constexpr char kMagic[4] = {'V', 'P', 'Q', 'C'};
constexpr uint32_t kVersion = 1;

void WriteU32(std::vector<uint8_t>& buf, size_t& offset, uint32_t value) {
  std::memcpy(buf.data() + offset, &value, sizeof(uint32_t));
  offset += sizeof(uint32_t);
}

uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t& offset) {
  uint32_t value = 0;
  std::memcpy(&value, buf.data() + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  return value;
}

bool RunKMeans(const std::vector<float>& subvectors, size_t count,
               uint32_t d_sub, uint32_t K, int max_iters,
               std::vector<float>& out_centroids) {
  out_centroids.resize(static_cast<size_t>(K) * d_sub);

  std::vector<uint32_t> assignments(count, 0);
  std::mt19937 rng(42);
  std::uniform_int_distribution<size_t> dist(0, count - 1);

  for (uint32_t k = 0; k < K; ++k) {
    size_t idx = dist(rng);
    std::memcpy(out_centroids.data() + k * d_sub,
                subvectors.data() + idx * d_sub,
                d_sub * sizeof(float));
  }

  std::vector<uint32_t> cluster_sizes(K);

  for (int iter = 0; iter < max_iters; ++iter) {
    // Assignment step
    for (size_t i = 0; i < count; ++i) {
      const float* sv = subvectors.data() + i * d_sub;
      uint32_t best_k = 0;
      float best_dist = L2Squared(sv, out_centroids.data(), d_sub);
      for (uint32_t k = 1; k < K; ++k) {
        float d = L2Squared(sv, out_centroids.data() + k * d_sub, d_sub);
        if (d < best_dist) {
          best_dist = d;
          best_k = k;
        }
      }
      assignments[i] = best_k;
    }

    // Update step
    std::fill(cluster_sizes.begin(), cluster_sizes.end(), 0);
    std::fill(out_centroids.begin(), out_centroids.end(), 0.0f);

    for (size_t i = 0; i < count; ++i) {
      uint32_t k = assignments[i];
      cluster_sizes[k]++;
      const float* sv = subvectors.data() + i * d_sub;
      float* centroid = out_centroids.data() + k * d_sub;
      for (uint32_t j = 0; j < d_sub; ++j) {
        centroid[j] += sv[j];
      }
    }

    for (uint32_t k = 0; k < K; ++k) {
      if (cluster_sizes[k] > 0) {
        float* centroid = out_centroids.data() + k * d_sub;
        float inv = 1.0f / static_cast<float>(cluster_sizes[k]);
        for (uint32_t j = 0; j < d_sub; ++j) {
          centroid[j] *= inv;
        }
      } else {
        float max_dist = -1.0f;
        size_t far_idx = 0;
        for (size_t i = 0; i < count; ++i) {
          const float* sv = subvectors.data() + i * d_sub;
          float d = L2Squared(sv,
                              out_centroids.data() + assignments[i] * d_sub,
                              d_sub);
          if (d > max_dist) {
            max_dist = d;
            far_idx = i;
          }
        }
        const float* sv = subvectors.data() + far_idx * d_sub;
        std::memcpy(out_centroids.data() + k * d_sub, sv,
                    d_sub * sizeof(float));
      }
    }
  }

  return true;
}

}  // namespace

bool PQTrainer::Train(const float* vectors, size_t count, uint32_t dim,
                      const PQConfig& config, PQCodebook& out_codebook) {
  if (dim % config.M != 0) {
    return false;
  }
  if (count == 0 || dim == 0 || config.M == 0 || config.K == 0) {
    return false;
  }

  uint32_t d_sub = dim / config.M;
  out_codebook.M = config.M;
  out_codebook.K = config.K;
  out_codebook.subspace_dim = d_sub;
  out_codebook.centroids.resize(static_cast<size_t>(config.M) * config.K * d_sub);

  std::vector<float> subvectors(count * d_sub);

  for (uint32_t m = 0; m < config.M; ++m) {
    for (size_t i = 0; i < count; ++i) {
      const float* vec = vectors + i * dim;
      std::memcpy(subvectors.data() + i * d_sub,
                  vec + m * d_sub,
                  d_sub * sizeof(float));
    }

    std::vector<float> subspace_centroids;
    if (!RunKMeans(subvectors, count, d_sub, config.K, config.max_iters,
                   subspace_centroids)) {
      return false;
    }

    std::memcpy(out_codebook.centroids.data() + m * config.K * d_sub,
                subspace_centroids.data(),
                config.K * d_sub * sizeof(float));
  }

  return true;
}

bool PQTrainer::SaveCodebook(const std::string& path,
                             const PQCodebook& codebook) {
  if (!codebook.IsValid()) {
    return false;
  }

  size_t centroids_size = static_cast<size_t>(codebook.M) * codebook.K *
                          codebook.subspace_dim * sizeof(float);
  size_t total = 4 + 4 + 4 + 4 + 4 + centroids_size;

  std::vector<uint8_t> buf(total, 0);
  size_t offset = 0;
  std::memcpy(buf.data() + offset, kMagic, sizeof(kMagic));
  offset += sizeof(kMagic);
  WriteU32(buf, offset, kVersion);
  WriteU32(buf, offset, codebook.M);
  WriteU32(buf, offset, codebook.K);
  WriteU32(buf, offset, codebook.subspace_dim);
  std::memcpy(buf.data() + offset, codebook.centroids.data(), centroids_size);

  return WriteFileAll(path, buf);
}

bool PQTrainer::LoadCodebook(const std::string& path, PQCodebook& codebook) {
  std::vector<uint8_t> buf;
  if (!ReadFileAll(path, buf)) {
    return false;
  }

  if (buf.size() < 20) {
    return false;
  }

  if (std::memcmp(buf.data(), kMagic, sizeof(kMagic)) != 0) {
    return false;
  }

  size_t offset = sizeof(kMagic);
  uint32_t version = ReadU32(buf, offset);
  if (version != 1) {
    return false;
  }

  codebook.M = ReadU32(buf, offset);
  codebook.K = ReadU32(buf, offset);
  codebook.subspace_dim = ReadU32(buf, offset);

  size_t centroids_size = static_cast<size_t>(codebook.M) * codebook.K *
                          codebook.subspace_dim * sizeof(float);
  if (buf.size() < offset + centroids_size) {
    return false;
  }

  codebook.centroids.resize(static_cast<size_t>(codebook.M) * codebook.K *
                            codebook.subspace_dim);
  std::memcpy(codebook.centroids.data(), buf.data() + offset, centroids_size);

  return codebook.IsValid();
}

}  // namespace vindex
