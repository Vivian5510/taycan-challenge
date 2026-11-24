#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <memory>

#include "kv_engine.h"
#include "workload.h"

DEFINE_string(mode, "", "prepare, serve or verify");
DEFINE_string(db_path, "", "Exact database directory (required)");
DEFINE_uint64(operations, 0,
              "Completed operation count for preparation/verification");
DEFINE_uint64(possible_operation, 0, "One unacknowledged operation, or zero");
DEFINE_int32(progress_fd, -1, "Dedicated protocol output descriptor");

namespace {
// 操作编号从 1 开始，每轮依次操作 key_id=0..N-1，轮号就是写入版本。
// 根据已完成操作数推导每个 key 的预期状态，无需保存完整的参考数据库。
// 例如 N=4、completed=6：完整执行 1 轮，第 2 轮又执行了前 2 个 key。
// 因此版本为 [2, 2, 1, 1]；再按下面的删除规则，最终为 [不存在, v2, v1, v1]。
// 返回 0 表示 key 尚未写入或已被删除。
uint64_t version_at(uint64_t id, uint64_t completed) {
  // 商是完整执行的轮数，余数是下一轮已执行的 key 数。
  const uint64_t rounds = completed / FLAGS_num_keys;
  const uint64_t version = rounds + (id < completed % FLAGS_num_keys ? 1 : 0);
  // Rounds 2, 5, 8, 11... delete one third of keys; later rounds restore them.
  // Both default preparations (1 and 10 rounds) lead into an actual delete
  // round.
  return version % 3 == 2 && id % 3 == 0 ? 0 : version;
}
bool report(const std::string& message) {
  size_t offset = 0;
  while (offset < message.size()) {
    const auto n = write(FLAGS_progress_fd, message.data() + offset,
                         message.size() - offset);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    offset += n;
  }
  return true;
}

bool validate_worker_options() {
  return validate_options() && !FLAGS_db_path.empty() &&
         (FLAGS_mode == "prepare" || FLAGS_mode == "serve" ||
          FLAGS_mode == "verify") &&
         (FLAGS_mode != "serve" || FLAGS_progress_fd >= 0) &&
         (!FLAGS_possible_operation ||
          FLAGS_possible_operation == FLAGS_operations + 1);
}

// key/value 缓冲由调用方复用，避免每次操作重新分配。
bool apply_operation(KVEngine& engine, uint64_t operation, std::string& key,
                     std::string& value) {
  const uint64_t id = (operation - 1) % FLAGS_num_keys;
  const auto version = version_at(id, operation);
  make_key(key, id);
  make_value(value, id, version);
  const auto result = version ? engine.Put(key, value) : engine.Delete(key);
  if (!result.IsOk()) {
    std::cerr << "operation=" << operation << " " << result.ToString() << '\n';
    return false;
  }
  return true;
}

int prepare(KVEngine& engine) {
  std::string key(FLAGS_key_size, '\0'), value(FLAGS_value_size, '\0');
  for (uint64_t operation = 1; operation <= FLAGS_operations; ++operation) {
    if (!apply_operation(engine, operation, key, value)) {
      return 1;
    }
  }
  return 0;
}

int serve(KVEngine& engine) {
  std::string key(FLAGS_key_size, '\0'), value(FLAGS_value_size, '\0');
  if (!report("READY\n")) {
    return 1;
  }
  uint64_t operation = FLAGS_operations;
  char command;
  while (std::cin.get(command)) {
    if (command != 'G' || !apply_operation(engine, ++operation, key, value)) {
      return 1;
    }
    // Put/Delete 成功后才回复 ACK，Python 收到 ACK 后才发送下一条命令。
    if (!report("ACK " + std::to_string(operation) + "\n")) {
      return 1;
    }
  }
  return 0;
}

// 检查读取结果是否符合“执行完 operation_count 条操作”的状态。
// 预期版本由操作序列推导，不从读取结果中获取；再生成完整 value 比较。
bool matches_state_after(uint64_t id, uint64_t operation_count,
                         const Status& status, const std::string& output,
                         std::string& expected) {
  const auto version = version_at(id, operation_count);
  if (version == 0) {
    return status.IsNotFound();
  }
  make_value(expected, id, version);
  return status.IsOk() && output == expected;
}

int verify(KVEngine& engine) {
  std::string key(FLAGS_key_size, '\0'), value(FLAGS_value_size, '\0'), output;
  output.reserve(FLAGS_value_size);
  // confirmed 是确定成功的操作数；pending 是最后一条未确认操作的编号。
  // pending 为 0 表示没有未确认操作，否则一定等于 confirmed + 1。
  const uint64_t confirmed = FLAGS_operations;
  const uint64_t pending = FLAGS_possible_operation;
  uint64_t recovered = FLAGS_operations;
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    make_key(key, id);
    const auto status = engine.Get(key, &output);

    // 只有 pending 涉及的这个 key 可以接受新状态，其余 key 必须匹配 confirmed。
    // 例如 N=4、confirmed=6、pending=7：第 7 条操作只修改 key_id=2（C）。
    // C 可以是 v1 或 v2，但 B 必须是 v2，D 必须是 v1。
    // 先检查 C 的新状态：若匹配，则恢复进度记为 7，供第二次打开时严格校验。
    const bool is_pending_key =
        pending != 0 && id == (pending - 1) % FLAGS_num_keys;
    if (is_pending_key &&
        matches_state_after(id, pending, status, output, value)) {
      recovered = pending;
      continue;
    }

    // pending key 的新状态未匹配时，仍可接受操作前的状态；其他 key 也走这里。
    // 若新旧状态都不匹配，则属于丢失操作、错误值或数据损坏，测试失败。
    if (matches_state_after(id, confirmed, status, output, value)) {
      continue;
    }

    std::cerr << "seed=" << FLAGS_seed << " key_id=" << id
              << " confirmed=" << confirmed << " possible=" << pending
              << " unexpected recovery state: " << status.ToString() << '\n';
    return 1;
  }
  // Python 用该进度再次打开数据库校验，第二次不再允许两种可能状态。
  std::cout << "RECOVERED_PREFIX " << recovered << std::endl;
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 1 || !validate_worker_options()) {
    return 2;
  }

  KVEngine* raw = nullptr;
  const auto status =
      KVEngine::Open(FLAGS_db_path, FLAGS_mem_limit_bytes, &raw);
  std::unique_ptr<KVEngine> engine(raw);
  if (!status.IsOk() || !engine) {
    std::cerr << "Open failed: " << status.ToString() << '\n';
    return 1;
  }

  if (FLAGS_mode == "prepare") {
    return prepare(*engine);
  }
  if (FLAGS_mode == "serve") {
    return serve(*engine);
  }
  return verify(*engine);
}
