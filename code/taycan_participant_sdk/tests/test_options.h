#pragma once

#include <gflags/gflags.h>
DECLARE_uint64(seed);
DECLARE_uint64(num_keys);
DECLARE_uint64(mem_limit_bytes);
DECLARE_uint32(key_size);
DECLARE_string(key_mode);
DECLARE_uint32(value_size);
DECLARE_uint32(rounds);
DECLARE_uint32(reader_threads);
DECLARE_uint32(reclaim_timeout_seconds);
DECLARE_double(space_ratio);
DECLARE_uint64(space_slack_bytes);
DECLARE_string(data_dir);

enum class KeyMode { kRandom, kSequential };

// Initialized by validate_options() before any workload threads are started.
extern KeyMode key_mode;

bool validate_options();
