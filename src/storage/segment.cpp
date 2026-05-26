#include "storage/segment.h"

#include <cstring>
#include <vector>

<<<<<<< HEAD
#include "io/backend_factory.h"
=======
#include "io/sync_io.h"
>>>>>>> 500ef092e647c8152098a19ff41d5e15edd92f10
#include "pq/pq_codec.h"

namespace vindex {

namespace {

constexpr uint32_t kHeaderSize = 4096;
constexpr char kMagic[4] = {'V', 'S', 'G', '1'};

void WriteU32(std::vector<uint8_t>& buf, size_t& offset, uint32_t value) {
  std::memcpy(buf.data() + offset, &value, sizeof(uint32_t));
  offset += sizeof(uint32_t);
}

void WriteU64(std::vector<uint8_t>& buf, size_t& offset, uint64_t value) {
  std::memcpy(buf.data() + offset, &value, sizeof(uint64_t));
  offset += sizeof(uint64_t);
}

uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t& offset) {
  uint32_t value = 0;
  std::memcpy(&value, buf.data() + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);
  return value;
}

uint64_t ReadU64(const std::vector<uint8_t>& buf, size_t& offset) {
  uint64_t value = 0;
  std::memcpy(&value, buf.data() + offset, sizeof(uint64_t));
  offset += sizeof(uint64_t);
  return value;
}

}  // namespace

bool SegmentReader::Open(const std::string& path, uint64_t id_offset) {
<<<<<<< HEAD
  return OpenWithIO(path, id_offset, MakeDefaultIOBackend());
=======
  return OpenWithIO(path, id_offset, std::make_unique<SyncIOBackend>());
>>>>>>> 500ef092e647c8152098a19ff41d5e15edd92f10
}

bool SegmentReader::OpenWithIO(const std::string& path, uint64_t id_offset,
                               std::unique_ptr<IIOBackend> io) {
  path_ = path;
  id_offset_ = id_offset;
  io_ = std::move(io);
  if (!io_->Open(path_)) {
    return false;
  }
  if (!ReadHeader()) {
    return false;
  }
  if (HasPQ()) {
    if (!ReadPQSection()) {
      return false;
    }
  }
  return true;
}

bool SegmentReader::ReadHeader() {
  std::vector<uint8_t> header_buf(kHeaderSize, 0);
  if (!io_->ReadAt(0, header_buf.data(), header_buf.size())) {
    return false;
  }
  if (std::memcmp(header_buf.data(), kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  size_t offset = sizeof(kMagic);
  header_.version = ReadU32(header_buf, offset);
  header_.dim = ReadU32(header_buf, offset);
  header_.count = ReadU64(header_buf, offset);
  header_.degree = ReadU32(header_buf, offset);
  header_.entry = ReadU32(header_buf, offset);
  header_.record_size = ReadU32(header_buf, offset);
  header_.data_offset = ReadU64(header_buf, offset);
  if (header_.version >= 2) {
    header_.pq_M = ReadU32(header_buf, offset);
    header_.pq_K = ReadU32(header_buf, offset);
  }
  return header_.dim > 0 && header_.count > 0;
}

bool SegmentReader::ReadPQSection() {
  uint64_t pq_offset = header_.data_offset +
                       header_.count * header_.record_size;

  uint32_t pq_header[3] = {0, 0, 0};
  if (!io_->ReadAt(pq_offset, pq_header, sizeof(pq_header))) {
    return false;
  }

  uint32_t M = pq_header[0];
  uint32_t K = pq_header[1];
  uint32_t d_sub = pq_header[2];

  if (M != header_.pq_M || K != header_.pq_K) {
    return false;
  }

  codebook_.M = M;
  codebook_.K = K;
  codebook_.subspace_dim = d_sub;

  size_t centroids_size = static_cast<size_t>(M) * K * d_sub;
  codebook_.centroids.resize(centroids_size);

  uint64_t centroids_offset = pq_offset + sizeof(pq_header);
  if (!io_->ReadAt(centroids_offset, codebook_.centroids.data(),
                    centroids_size * sizeof(float))) {
    return false;
  }

  size_t codes_size = static_cast<size_t>(header_.count) * M;
  pq_codes_.resize(codes_size);

  uint64_t codes_offset = centroids_offset +
                          centroids_size * sizeof(float);
  if (!io_->ReadAt(codes_offset, pq_codes_.data(), codes_size)) {
    return false;
  }

  return true;
}

float SegmentReader::PQDistance(const float* adc_table,
                                uint32_t local_id) const {
  return PQCodec::ADCDistance(adc_table,
                              pq_codes_.data() +
                                  static_cast<size_t>(local_id) * header_.pq_M,
                              header_.pq_M, header_.pq_K);
}

bool SegmentReader::ReadNode(uint32_t local_id, std::vector<float>& vector,
                             std::vector<uint32_t>& neighbors) const {
  if (local_id >= header_.count) {
    return false;
  }
  uint64_t offset = header_.data_offset +
                    static_cast<uint64_t>(local_id) * header_.record_size;
  std::vector<uint8_t> buffer(header_.record_size);
  if (!io_->ReadAt(offset, buffer.data(), buffer.size())) {
    return false;
  }

  vector.resize(header_.dim);
  neighbors.resize(header_.degree);

  std::memcpy(vector.data(), buffer.data(), header_.dim * sizeof(float));
  std::memcpy(neighbors.data(),
              buffer.data() + header_.dim * sizeof(float),
              header_.degree * sizeof(uint32_t));
  return true;
}

bool SegmentReader::ParseNode(const uint8_t* buffer, uint32_t dim,
                              uint32_t degree, std::vector<float>& vector,
                              std::vector<uint32_t>& neighbors) {
  vector.resize(dim);
  neighbors.resize(degree);
  std::memcpy(vector.data(), buffer, dim * sizeof(float));
  std::memcpy(neighbors.data(),
              buffer + dim * sizeof(float),
              degree * sizeof(uint32_t));
  return true;
}

bool SegmentWriter::WriteSegment(const std::string& path, uint32_t dim,
                                 uint64_t count, uint32_t degree,
                                 uint32_t entry,
                                 const std::vector<float>& vectors,
                                 const std::vector<std::vector<uint32_t>>& neighbors) {
  return WriteSegment(path, dim, count, degree, entry, vectors, neighbors,
                      PQCodebook{}, std::vector<uint8_t>{});
}

bool SegmentWriter::WriteSegment(const std::string& path, uint32_t dim,
                                 uint64_t count, uint32_t degree,
                                 uint32_t entry,
                                 const std::vector<float>& vectors,
                                 const std::vector<std::vector<uint32_t>>& neighbors,
                                 const PQCodebook& codebook,
                                 const std::vector<uint8_t>& pq_codes) {
  if (dim == 0 || count == 0 || degree == 0) {
    return false;
  }
  if (vectors.size() != static_cast<size_t>(count) * dim) {
    return false;
  }
  if (neighbors.size() != static_cast<size_t>(count)) {
    return false;
  }

  bool has_pq = codebook.IsValid() && !pq_codes.empty();

  if (has_pq) {
    if (pq_codes.size() != static_cast<size_t>(count) * codebook.M) {
      return false;
    }
  }

  SyncIOBackend writer(path, /*writable=*/true);
  if (!writer.IsOpen()) {
    return false;
  }

  uint32_t record_size = dim * sizeof(float) + degree * sizeof(uint32_t);
  uint64_t data_offset = kHeaderSize;
  uint32_t version = 2;

  std::vector<uint8_t> header_buf(kHeaderSize, 0);
  std::memcpy(header_buf.data(), kMagic, sizeof(kMagic));
  size_t offset = sizeof(kMagic);
  WriteU32(header_buf, offset, version);
  WriteU32(header_buf, offset, dim);
  WriteU64(header_buf, offset, count);
  WriteU32(header_buf, offset, degree);
  WriteU32(header_buf, offset, entry);
  WriteU32(header_buf, offset, record_size);
  WriteU64(header_buf, offset, data_offset);
  if (has_pq) {
    WriteU32(header_buf, offset, codebook.M);
    WriteU32(header_buf, offset, codebook.K);
  }

  if (!writer.WriteAt(0, header_buf.data(), header_buf.size())) {
    return false;
  }

  std::vector<uint8_t> record_buf(record_size);
  for (uint64_t i = 0; i < count; ++i) {
    if (neighbors[i].size() != degree) {
      return false;
    }
    const float* vec = vectors.data() + i * dim;
    std::memcpy(record_buf.data(), vec, dim * sizeof(float));
    std::memcpy(record_buf.data() + dim * sizeof(float),
                neighbors[i].data(), degree * sizeof(uint32_t));

    uint64_t rec_offset = data_offset + i * record_size;
    if (!writer.WriteAt(rec_offset, record_buf.data(), record_buf.size())) {
      return false;
    }
  }

  if (has_pq) {
    uint64_t pq_offset = data_offset + count * record_size;

    uint32_t pq_header[3] = {codebook.M, codebook.K, codebook.subspace_dim};
    if (!writer.WriteAt(pq_offset, pq_header, sizeof(pq_header))) {
      return false;
    }

    size_t centroids_size = static_cast<size_t>(codebook.M) * codebook.K *
                            codebook.subspace_dim;
    uint64_t centroids_offset = pq_offset + sizeof(pq_header);
    if (!writer.WriteAt(centroids_offset, codebook.centroids.data(),
                        centroids_size * sizeof(float))) {
      return false;
    }

    uint64_t codes_offset = centroids_offset +
                            centroids_size * sizeof(float);
    if (!writer.WriteAt(codes_offset, pq_codes.data(),
                        pq_codes.size() * sizeof(uint8_t))) {
      return false;
    }
  }

  return writer.Flush();
}

}  // namespace vindex
