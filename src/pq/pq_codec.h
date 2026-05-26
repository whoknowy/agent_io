#pragma once

#include <cstdint>
#include <vector>

#include "pq/pq.h"

namespace vindex {

class PQCodec {
 public:
  static void Encode(const float* vectors, size_t count,
                     const PQCodebook& codebook,
                     std::vector<uint8_t>& out_codes);

  static void Decode(const uint8_t* code, const PQCodebook& codebook,
                     std::vector<float>& out_vector);

  static void BuildADCTable(const float* query, uint32_t dim,
                            const PQCodebook& codebook,
                            std::vector<float>& out_table);

  static float ADCDistance(const float* adc_table, const uint8_t* code, uint32_t M, uint32_t K);
};

}  // namespace vindex
