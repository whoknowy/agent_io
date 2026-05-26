cmake_minimum_required(VERSION 3.16)
project(vindex_mvp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

<<<<<<< HEAD
option(VINDEX_USE_IO_URING "Enable io_uring backend" OFF)

=======
>>>>>>> 500ef092e647c8152098a19ff41d5e15edd92f10
add_executable(vindex
    src/main.cpp
    src/core/types.h
    src/core/distance.h
    src/core/topk.h
    src/io/file_io.h
    src/io/file_io.cpp
    src/io/io_backend.h
<<<<<<< HEAD
    src/io/backend_factory.h
=======
>>>>>>> 500ef092e647c8152098a19ff41d5e15edd92f10
    src/io/sync_io.h
    src/io/sync_io.cpp
    src/io/cached_io.h
    src/io/cached_io.cpp
    src/io/prefetch_scheduler.h
    src/io/prefetch_scheduler.cpp
<<<<<<< HEAD
    src/io/uring_io.h
    src/io/uring_io.cpp
=======
>>>>>>> 500ef092e647c8152098a19ff41d5e15edd92f10
    src/cache/cache_types.h
    src/cache/cache_pool.h
    src/cache/cache_pool.cpp
    src/dataset/fvecs.h
    src/dataset/fvecs.cpp
    src/dataset/ivecs.h
    src/dataset/ivecs.cpp
    src/pq/pq.h
    src/pq/pq_trainer.h
    src/pq/pq_trainer.cpp
    src/pq/pq_codec.h
    src/pq/pq_codec.cpp
    src/storage/segment.h
    src/storage/segment.cpp
    src/storage/manifest.h
    src/storage/manifest.cpp
    src/storage/compaction.h
    src/storage/compaction.cpp
    src/index/graph_builder.h
    src/index/graph_builder.cpp
    src/index/vamana_builder.h
    src/index/vamana_builder.cpp
    src/index/graph_search.h
    src/index/graph_search.cpp
    src/mem/memtable.h
    src/mem/memtable.cpp
    src/util/arg_parser.h
    src/util/arg_parser.cpp
    src/util/thread_pool.h
    src/util/thread_pool.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(vindex PRIVATE Threads::Threads)

target_include_directories(vindex PRIVATE src)
<<<<<<< HEAD

if(VINDEX_USE_IO_URING)
    find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
    find_library(LIBURING_LIBRARY uring REQUIRED)
    target_compile_definitions(vindex PRIVATE VINDEX_IO_URING)
    target_include_directories(vindex PRIVATE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(vindex PRIVATE ${LIBURING_LIBRARY})
endif()
=======
>>>>>>> 500ef092e647c8152098a19ff41d5e15edd92f10
