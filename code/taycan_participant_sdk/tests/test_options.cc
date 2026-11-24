#include "test_options.h"

#include <iostream>

DEFINE_uint64(seed, 20261021, "Deterministic workload seed");
DEFINE_uint64(num_keys, 100000, "Number of distinct keys");
DEFINE_uint64(mem_limit_bytes, 256ULL << 20, "Index memory budget in bytes");
DEFINE_uint32(key_size, 64, "Fixed key length, at least 8 bytes");
DEFINE_string(key_mode, "random",
              "Key contents: random or sequential (bytewise order)");
DEFINE_uint32(value_size, 128, "Value length, at least 16 bytes");
DEFINE_uint32(rounds, 10, "Overwrite rounds, at least 2");
DEFINE_uint32(reader_threads, 4,
              "Readers in SingleWriterVersionBounds, between 1 and 64");
DEFINE_uint32(reclaim_timeout_seconds, 120,
              "Wait for background space reclamation");
DEFINE_double(space_ratio, 3.0, "Maximum directory bytes / live logical bytes");
DEFINE_uint64(space_slack_bytes, 16ULL << 20,
              "Fixed directory allowance for metadata and WAL");
DEFINE_string(data_dir, "", "Parent of a fresh temporary test directory");

KeyMode key_mode = KeyMode::kRandom;

bool validate_options() {
  if (!FLAGS_num_keys || FLAGS_num_keys > 100000000 || FLAGS_key_size < 8 ||
      FLAGS_key_size > (1u << 20) ||
      (FLAGS_key_mode != "random" && FLAGS_key_mode != "sequential") ||
      FLAGS_value_size < 16 || FLAGS_value_size > (1u << 30) ||
      !FLAGS_mem_limit_bytes || FLAGS_mem_limit_bytes > (1ULL << 60) ||
      FLAGS_rounds < 2 || FLAGS_rounds > 1000000 || FLAGS_reader_threads < 1 ||
      FLAGS_reader_threads > 64 ||
      !(FLAGS_space_ratio >= 1 && FLAGS_space_ratio <= 100)) {
    std::cerr << "Invalid workload parameters (see --help)\n";
    return false;
  }
  key_mode =
      FLAGS_key_mode == "sequential" ? KeyMode::kSequential : KeyMode::kRandom;
  std::cerr << "seed=" << FLAGS_seed << " num_keys=" << FLAGS_num_keys
            << " key_mode=" << FLAGS_key_mode << " key_size=" << FLAGS_key_size
            << " value_size=" << FLAGS_value_size
            << " reader_threads=" << FLAGS_reader_threads
            << " mem_limit_bytes=" << FLAGS_mem_limit_bytes << '\n';
  return true;
}
