#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>

#include "test_options.h"

// Change this salt in the evaluator's copy, then rebuild ALL test programs.
// It changes the generated data without changing the permutation's invariants.
inline constexpr uint64_t kGeneratorSalt = 0xd1b54a32d192ed03ULL;
inline constexpr uint64_t kGeneratorStep = 0x9e3779b97f4a7c15ULL;
inline constexpr uint64_t kMixMultiplier1 = 0xbf58476d1ce4e5b9ULL;
inline constexpr uint64_t kMixMultiplier2 = 0x94d049bb133111ebULL;

// Fixed algorithm and explicit byte order: independent of STL distributions.
// Keep the step and multipliers odd to preserve the bijection.
inline uint64_t mix_bits(uint64_t x) {
  x = (x ^ (x >> 30)) * kMixMultiplier1;
  x = (x ^ (x >> 27)) * kMixMultiplier2;
  return x ^ (x >> 31);
}

class Generator {
 public:
  explicit Generator(uint64_t seed) : seed_(seed ^ kGeneratorSalt) {}
  uint64_t next() { return mix_bits(seed_ += kGeneratorStep); }

 private:
  uint64_t seed_;
};

// memcpy handles unaligned string buffers without aliasing violations.
inline void store(char* output, uint64_t word, size_t size) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  std::memcpy(output, &word, size);
#else
  for (size_t i = 0; i < size; ++i) {
    output[i] = static_cast<char>(word >> (8 * i));
  }
#endif
}

// Preserve the prefix and generate only the remaining bytes from seed.
inline void fill_bytes(std::string& output, uint64_t seed, size_t offset = 0) {
  assert(offset <= output.size());
  if (offset == output.size()) {
    return;
  }
  Generator rng(seed);
  while (output.size() - offset >= sizeof(uint64_t)) {
    store(output.data() + offset, rng.next(), sizeof(uint64_t));
    offset += sizeof(uint64_t);
  }
  if (offset < output.size()) {
    store(output.data() + offset, rng.next(), output.size() - offset);
  }
}

inline void make_key(std::string& key, uint64_t id) {
  const uint64_t token = mix_bits(id ^ FLAGS_seed ^ kGeneratorSalt);
  if (key_mode == KeyMode::kSequential) {
    // Big-endian id makes fixed-length keys increase in bytewise order.
    for (size_t i = 0; i < sizeof(id); ++i) {
      key[i] = static_cast<char>(id >> (8 * (7 - i)));
    }
  } else {
    // A bijection guarantees uniqueness even for 8-byte keys.
    store(key.data(), token, 8);
  }
  if (key.size() > sizeof(id)) {
    fill_bytes(key, token, sizeof(id));
  }
}

inline void make_value(std::string& value, uint64_t id, uint64_t version) {
  store(value.data(), id, 8);
  store(value.data() + 8, version, 8);
  if (value.size() > 16) {
    const uint64_t data_seed = FLAGS_seed ^ mix_bits(id) ^ version;
    fill_bytes(value, data_seed, 16);
  }
}

// Visits every id once without allocating a permutation array.
class KeyOrder {
 public:
  explicit KeyOrder(uint64_t salt)
      : current_(mix_bits(FLAGS_seed ^ kGeneratorSalt ^ salt) %
                 FLAGS_num_keys) {
    step_ = mix_bits((FLAGS_seed ^ kGeneratorSalt) + salt) % FLAGS_num_keys;
    while (std::gcd(step_, FLAGS_num_keys) != 1) {
      step_ = (step_ + 1) % FLAGS_num_keys;
    }
  }
  uint64_t next() {
    const auto result = current_;
    current_ = (current_ + step_) % FLAGS_num_keys;
    return result;
  }

 private:
  uint64_t current_, step_;
};
