# Loaded by the official SDK as a child directory.
if(NOT TARGET contest_sdk)
  message(FATAL_ERROR "Configure from the official SDK root; contest_sdk is supplied by the parent")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This engine package targets Linux x86_64")
endif()

set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
set(TBB4PY_BUILD OFF CACHE BOOL "" FORCE)
set(TBB_INSTALL OFF CACHE BOOL "" FORCE)
set(TBB_STRICT OFF CACHE BOOL "" FORCE)
set(TBB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON)
set(TBB_OUTPUT_DIR_BASE "taycan_tbb")
add_subdirectory(vendor/oneTBB "${CMAKE_CURRENT_BINARY_DIR}/oneTBB" EXCLUDE_FROM_ALL)

add_library(taycan_libaio STATIC
  vendor/libaio/src/io_queue_init.c
  vendor/libaio/src/io_queue_release.c
  vendor/libaio/src/io_queue_wait.c
  vendor/libaio/src/io_queue_run.c
  vendor/libaio/src/io_getevents.c
  vendor/libaio/src/io_submit.c
  vendor/libaio/src/io_cancel.c
  vendor/libaio/src/io_setup.c
  vendor/libaio/src/io_destroy.c
  vendor/libaio/src/io_pgetevents.c
  vendor/libaio/src/raw_syscall.c
  vendor/libaio/src/compat-0_1.c)
target_include_directories(taycan_libaio PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/libaio/src>")
target_compile_definitions(taycan_libaio PRIVATE _GNU_SOURCE)
set_target_properties(taycan_libaio PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(taycan_uuid STATIC
  vendor/util-linux/libuuid/src/clear.c
  vendor/util-linux/libuuid/src/compare.c
  vendor/util-linux/libuuid/src/copy.c
  vendor/util-linux/libuuid/src/gen_uuid.c
  vendor/util-linux/libuuid/src/isnull.c
  vendor/util-linux/libuuid/src/pack.c
  vendor/util-linux/libuuid/src/parse.c
  vendor/util-linux/libuuid/src/predefined.c
  vendor/util-linux/libuuid/src/unpack.c
  vendor/util-linux/libuuid/src/unparse.c
  vendor/util-linux/libuuid/src/uuid_time.c
  vendor/util-linux/lib/randutils.c
  vendor/util-linux/lib/md5.c
  vendor/util-linux/lib/sha1.c)
include(CheckSymbolExists)
check_symbol_exists(reallocarray "stdlib.h" TAYCAN_SYSTEM_HAS_REALLOCARRAY)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(strndup "string.h" TAYCAN_SYSTEM_HAS_STRNDUP)
unset(CMAKE_REQUIRED_DEFINITIONS)
target_include_directories(taycan_uuid
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/util-linux/include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/util-linux/libuuid/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/util-linux/include")
target_compile_definitions(taycan_uuid PRIVATE
  _GNU_SOURCE
  HAVE_UNISTD_H HAVE_STDLIB_H HAVE_SYS_TIME_H HAVE_SYS_FILE_H
  HAVE_SYS_IOCTL_H HAVE_SYS_SOCKET_H HAVE_SYS_UN_H HAVE_NET_IF_H
  HAVE_NETINET_IN_H HAVE_SYS_SYSCALL_H HAVE_LIBPTHREAD HAVE_NANOSLEEP
  HAVE_TLS HAVE_ERR_H HAVE___PROGNAME
  _PATH_LOCALSTATEDIR=\"/var\"
  _PATH_RUNSTATEDIR=\"/run\")
if(TAYCAN_SYSTEM_HAS_REALLOCARRAY)
  target_compile_definitions(taycan_uuid PRIVATE HAVE_REALLOCARRAY)
endif()
if(TAYCAN_SYSTEM_HAS_STRNDUP)
  target_compile_definitions(taycan_uuid PRIVATE HAVE_STRNDUP)
endif()
target_link_libraries(taycan_uuid PRIVATE Threads::Threads)
set_target_properties(taycan_uuid PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(faster_cpp STATIC
  vendor/faster/core/address.cc
  vendor/faster/core/lss_allocator.cc
  vendor/faster/core/thread.cc
  vendor/faster/environment/file_linux.cc)
target_include_directories(faster_cpp PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/faster>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/oneTBB/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/libaio/src>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/util-linux/include>")
target_compile_features(faster_cpp PUBLIC cxx_std_17)
target_compile_definitions(faster_cpp PUBLIC
  F2_COMMIT_EXPERIMENT=1
  F2_BATCHED_FILE_SYNC=1
  F2_DURABLE_COMPACTION=1
  F2_DATA_SEGMENT_BITS=23)
target_compile_definitions(faster_cpp PRIVATE _GNU_SOURCE)
target_link_libraries(faster_cpp PUBLIC
  TBB::tbb
  taycan_libaio
  taycan_uuid
  Threads::Threads
  ${CMAKE_DL_LIBS})

add_library(contestant_engine STATIC
  engine.cc
  src/faster_backend.cc)
target_include_directories(contestant_engine PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(contestant_engine PUBLIC cxx_std_17)
target_compile_definitions(contestant_engine PRIVATE
  _GNU_SOURCE
  F2_SYNC_EXPERIMENT=1
  F2_SYNC_STRATEGY="commit-log"
  F2_LOG_PAGE_BITS=20
  F2_OVERFLOW_PAGE_BITS=12
  F2_RECOVERY_VERSION_FIX=1
  F2_WAL_OPTIMIZED=1
  F2_DURABLE_COMPACTION=1
  F2_DATA_SEGMENT_BITS=23)
target_link_libraries(contestant_engine
  PUBLIC contest_sdk
  PRIVATE faster_cpp)
target_link_options(contestant_engine INTERFACE
  "LINKER:-rpath,$<TARGET_FILE_DIR:tbb>")
