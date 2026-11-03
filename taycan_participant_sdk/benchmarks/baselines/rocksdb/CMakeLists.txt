if(NOT TARGET contest_sdk)
  message(FATAL_ERROR "Configure the SDK root with CONTEST_ENGINE_DIR pointing here")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(TAYCAN_ROCKSDB REQUIRED IMPORTED_TARGET rocksdb)
add_library(contestant_engine STATIC rocksdb_engine.cc)
target_link_libraries(contestant_engine PUBLIC contest_sdk PkgConfig::TAYCAN_ROCKSDB)
target_compile_options(contestant_engine PRIVATE -Wall -Wextra -Wpedantic)
# These definitions identify the linked implementation in the shared workload driver.
target_compile_definitions(contestant_engine PUBLIC
  TAYCAN_BENCH_ENGINE_ID="rocksdb"
  TAYCAN_BENCH_ENGINE_VERSION="${TAYCAN_ROCKSDB_VERSION}"
  TAYCAN_BENCH_ENGINE_POLICY="sync=true,disableWAL=false,compression=none,one_key_per_API,shared_cache=5/8_budget,write_buffer_manager=1/4_budget,VmRSS_monitored")
