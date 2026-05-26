#include "pq/pq_codec.h"

#include <cstring>
#include <limits>

#include "core/distance.h"

namespace vindex {

void PQCodec::Encode(const float* vectors, size_t count,
                     const PQCodebook& codebook,
                     std::vector<uint8_t>& out_codes) {
  uint32_t M = codebook.M;
  uint32_t K = codebook.K;
  uint32_t d_sub = codebook.subspace_dim;

  out_codes.assign(count * M, 0);

  for (size_t i = 0; i < count; ++i) {
    const float* vec = vectors + i * d_sub * M;
    for (uint32_t m = 0; m < M; ++m) {
      const float* sv = vec + m * d_sub;
      const float* centroids_m = codebook.centroids.data() + m * K * d_sub;

      uint8_t best_k = 0;
      float best_dist = L2Squared(sv, centroids_m, d_sub);
      for (uint32_t k = 1; k < K; ++k) {
        float d = L2Squared(sv, centroids_m + k * d_sub, d_sub);
        if (d < best_dist) {
          best_dist = d;
          best_k = static_cast<uint8_t>(k);
        }
      }
      out_codes[i * M + m] = best_k;
    }
  }
}

void PQCodec::Decode(const uint8_t* code, const PQCodebook& codebook,
                     std::vector<float>& out_vector) {
  uint32_t M = codebook.M;
  uint32_t K = codebook.K;
  uint32_t d_sub = codebook.subspace_dim;
  uint32_t dim = M * d_sub;

  out_vector.resize(dim);

  for (uint32_t m = 0; m < M; ++m) {
    uint8_t k = code[m];
    const float* centroid = codebook.centroids.data() + m * K * d_sub +
                            k * d_sub;
    std::memcpy(out_vector.data() + m * d_sub, centroid,
                d_sub * sizeof(float));
  }
}

void PQCodec::BuildADCTable(const float* query, uint32_t dim,
                            const PQCodebook& codebook,
                            std::vector<float>& out_table) {
  uint32_t M = codebook.M;
  uint32_t K = codebook.K;
  uint32_t d_sub = codebook.subspace_dim;

  out_table.resize(static_cast<size_t>(M) * K);

  for (uint32_t m = 0; m < M; ++m) {
    const float* q_sub = query + m * d_sub;
    const float* centroids_m = codebook.centroids.data() + m * K * d_sub;
    float* table_m = out_table.data() + m * K;

    for (uint32_t k = 0; k < K; ++k) {
      table_m[k] = L2Squared(q_sub, centroids_m + k * d_sub, d_sub);
    }
  }
}

float PQCodec::ADCDistance(const float* adc_table, const uint8_t* code,
                           uint32_t M, uint32_t K) {
  float sum = 0.0f;
  for (uint32_t m = 0; m < M; ++m) {
    sum += adc_table[m * K + code[m]];
  }
  return sum;
}

}  // namespace vindex
