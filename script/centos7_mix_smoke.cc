// Diagnostic wrapper around the existing benchmark; timing stays in kv_bench.cc.
#define main ExistingBenchmarkMain
#include "kv_bench.cc"
#undef main

int main(int argc, char** argv) {
  const auto options = Parse(argc, argv);
  const int benchmark_result = ExistingBenchmarkMain(argc, argv);
  if (benchmark_result != 0) return benchmark_result;
  try {
    std::vector<std::string> previous;
    std::vector<bool> previous_present;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      KVEngine* pointer = nullptr;
      const auto status = KVEngine::Open(options.data_dir, options.mem_limit_bytes, &pointer);
      std::unique_ptr<KVEngine> engine(pointer);
      if (!status.IsOk() || !engine) throw std::runtime_error("reopen failed: " + status.ToString());
      std::vector<std::string> current;
      std::vector<bool> present;
      uint64_t found = 0;
      for (uint64_t id = 0; id < options.num_keys; ++id) {
        std::string key(options.key_size, '\0'), value;
        Key(key, id, options.seed);
        const auto read = engine->Get(key, &value);
        if (read.IsNotFound()) {
          present.push_back(false);
          current.emplace_back();
          continue;
        }
        if (!read.IsOk() || value.size() != options.value_size || ReadWord(value, 0) != id)
          throw std::runtime_error("invalid reopened value at key " + std::to_string(id));
        std::string expected(options.value_size, '\0');
        Value(expected, id, ReadWord(value, 8), options.seed);
        if (expected != value) throw std::runtime_error("reopened value checksum mismatch");
        present.push_back(true);
        current.push_back(std::move(value));
        ++found;
      }
      if (attempt && (current != previous || present != previous_present))
        throw std::runtime_error("state changed across two successive reopens");
      previous = std::move(current);
      previous_present = std::move(present);
      std::cout << "{\"type\":\"reopen_check\",\"status\":\"PASS\",\"attempt\":"
                << attempt + 1 << ",\"keys_checked\":" << options.num_keys
                << ",\"present\":" << found << ",\"absent\":" << options.num_keys - found << "}" << std::endl;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cout << "{\"type\":\"reopen_check\",\"status\":\"FAIL\",\"error\":\""
              << Escape(error.what()) << "\"}" << std::endl;
    return 1;
  }
}
