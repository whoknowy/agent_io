#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "io/io_backend.h"
#include "pq/pq.h"

namespace vindex {

struct SegmentHeader {
  uint32_t version = 1;
  uint32_t dim = 0;
  uint64_t count = 0;
  uint32_t degree = 0;
  uint32_t entry = 0;
  uint32_t record_size = 0;
  uint64_t data_offset = 0;
  uint32_t pq_M = 0;
  uint32_t pq_K = 0;
};

class SegmentReader {
 public:
  SegmentReader() = default;
  bool Open(const std::string& path, uint64_t id_offset);

  // Open with an externally-provided I/O backend (e.g. cached, uring).
  bool OpenWithIO(const std::string& path, uint64_t id_offset,
                  std::unique_ptr<IIOBackend> io);
  bool ReadNode(uint32_t local_id, std::vector<float>& vector,
                std::vector<uint32_t>& neighbors) const;

  // Parse a node from a pre-read buffer (used with prefetch).
  static bool ParseNode(const uint8_t* buffer, uint32_t dim, uint32_t degree,
                        std::vector<float>& vector,
                        std::vector<uint32_t>& neighbors);

  const SegmentHeader& header() const { return header_; }
  uint32_t dim() const { return header_.dim; }
  uint64_t count() const { return header_.count; }
  uint32_t degree() const { return header_.degree; }
  uint32_t entry() const { return header_.entry; }
  uint64_t id_offset() const { return id_offset_; }
  const std::string& path() const { return path_; }

  bool HasPQ() const { return header_.pq_M > 0; }
  uint32_t PqM() const { return header_.pq_M; }
  const PQCodebook& codebook() const { return codebook_; }
  const std::vector<uint8_t>& pq_codes() const { return pq_codes_; }
  float PQDistance(const float* adc_table, uint32_t local_id) const;

  // For prefetch: expose node byte range on disk.
  uint64_t NodeOffset(uint32_t local_id) const {
    return header_.data_offset +
           static_cast<uint64_t>(local_id) * header_.record_size;
  }
  uint32_t NodeSize() const { return header_.record_size; }

 private:
  bool ReadHeader();
  bool ReadPQSection();

  std::string path_;
  uint64_t id_offset_ = 0;
  SegmentHeader header_;
  std::unique_ptr<IIOBackend> io_;
  PQCodebook codebook_;
  std::vector<uint8_t> pq_codes_;
};

class SegmentWriter {
 public:
  static bool WriteSegment(const std::string& path, uint32_t dim, uint64_t count,
                           uint32_t degree, uint32_t entry,
                           const std::vector<float>& vectors,
                           const std::vector<std::vector<uint32_t>>& neighbors);

  static bool WriteSegment(const std::string& path, uint32_t dim, uint64_t count,
                           uint32_t degree, uint32_t entry,
                           const std::vector<float>& vectors,
                           const std::vector<std::vector<uint32_t>>& neighbors,
                           const PQCodebook& codebook,
                           const std::vector<uint8_t>& pq_codes);
};

}  // namespace vindex
