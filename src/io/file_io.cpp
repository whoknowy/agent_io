#include "io/file_io.h"

#include <fstream>

namespace vindex {

FileReader::FileReader(const std::string& path) : path_(path) {
  stream_.open(path_, std::ios::binary);
}

bool FileReader::Open(const std::string& path) {
  path_ = path;
  if (stream_.is_open()) {
    stream_.close();
  }
  stream_.open(path_, std::ios::binary);
  return stream_.is_open();
}

bool FileReader::IsOpen() const {
  return stream_.is_open();
}

bool FileReader::ReadAt(uint64_t offset, void* data, size_t size) {
  if (!stream_.is_open()) {
    return false;
  }
  stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream_) {
    return false;
  }
  stream_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
  return static_cast<size_t>(stream_.gcount()) == size;
}

FileWriter::FileWriter(const std::string& path) : path_(path) {
  stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
}

bool FileWriter::IsOpen() const {
  return stream_.is_open();
}

bool FileWriter::WriteAt(uint64_t offset, const void* data, size_t size) {
  if (!stream_.is_open()) {
    return false;
  }
  stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!stream_) {
    return false;
  }
  stream_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(stream_);
}

bool FileWriter::Flush() {
  if (!stream_.is_open()) {
    return false;
  }
  stream_.flush();
  return static_cast<bool>(stream_);
}

bool ReadFileAll(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return false;
  }
  stream.seekg(0, std::ios::end);
  std::streamoff size = stream.tellg();
  if (size < 0) {
    return false;
  }
  stream.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return static_cast<bool>(stream);
}

bool WriteFileAll(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(stream);
}

}  // namespace vindex
