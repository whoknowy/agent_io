#pragma once

#include <memory>

#include "io/sync_io.h"

#ifdef VINDEX_IO_URING
#include "io/uring_io.h"
#endif

namespace vindex {

inline std::unique_ptr<IIOBackend> MakeDefaultIOBackend() {
#ifdef VINDEX_IO_URING
  return std::make_unique<UringIOBackend>();
#else
  return std::make_unique<SyncIOBackend>();
#endif
}

}  // namespace vindex