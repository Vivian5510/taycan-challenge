// A completed callback may drive the same handler while waiting for a sibling.
// The sibling must remain reachable, rather than hidden in an outer stack batch.
#include "core/async.h"
#include "environment/native_file.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>

namespace fc = FASTER::core;
namespace fe = FASTER::environment;
struct Result {
  fe::QueueIoHandler* handler;
  std::atomic<unsigned> entered{0};
  std::atomic<bool> sibling{false};
  std::atomic<bool> timed_out{false};
  std::atomic<unsigned> errors{0};
};
struct Context : fc::IAsyncContext {
  Result* result;
  explicit Context(Result* p) : result(p) {}
  fc::Status DeepCopy_Internal(fc::IAsyncContext*& copy) override {
    return IAsyncContext::DeepCopy_Internal(*this, copy);
  }
};
void Done(fc::IAsyncContext* pointer, fc::Status status, size_t bytes) {
  fc::CallbackContext<Context> context(pointer);
  auto& result = *context->result;
  if (status != fc::Status::Ok || bytes != 4096) ++result.errors;
  if (result.entered.fetch_add(1) != 0) {
    result.sibling = true;
    return;
  }
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!result.sibling && std::chrono::steady_clock::now() < end) {
    result.handler->TryComplete();
    std::this_thread::yield();
  }
  if (!result.sibling) result.timed_out = true;
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::filesystem::create_directories(argv[1]);
  const std::string path = std::string(argv[1]) + "/completed.data";
  fe::QueueIoHandler handler;
  fe::QueueFile file(path);
  if (file.Open(fe::FileCreateDisposition::CreateOrTruncate, {false, false}, &handler) != fc::Status::Ok) return 3;
  Result result{&handler};
  std::array<uint8_t, 4096> data{};
  data.fill(37);
  for (unsigned i = 0; i < 2; ++i) {
    Context context(&result);
    if (file.Write(i * data.size(), data.size(), data.data(), context, Done) != fc::Status::Ok) return 4;
  }
  // Ensure both OS writes finished before the first poll; callbacks stay queued.
  int fd = ::open(path.c_str(), O_RDONLY);
  std::array<uint8_t, 8192> copy{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool ready = false;
  while (std::chrono::steady_clock::now() < deadline) {
    ready = ::pread(fd, copy.data(), copy.size(), 0) == ssize_t(copy.size()) && copy.back() == 37;
    if (ready) break;
    std::this_thread::yield();
  }
  ::close(fd);
  if (!ready) return 5;
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  handler.Drain();
  std::printf("REENTRY callbacks=%u sibling=%u timed_out=%u errors=%u\n",
              result.entered.load(), unsigned(result.sibling.load()),
              unsigned(result.timed_out.load()), result.errors.load());
  return result.timed_out || result.errors || result.entered != 2 ? 1 : 0;
}
