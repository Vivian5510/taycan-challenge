#include "core/guid.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>

using FASTER::core::Guid;

namespace {

enum class Entropy { Real, Interrupted, Split, FailAfterSplit, Empty };
thread_local Entropy entropy = Entropy::Real;
thread_local unsigned calls = 0;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool IsV4(const std::string& value) {
  return value.size() == 36 && value[14] == '4' &&
         std::string("89ab").find(value[19]) != std::string::npos;
}

void CheckEntropy(Entropy mode, unsigned expected_calls) {
  unsigned actual_calls = 0;
  Guid guid;
  std::thread worker([&] {
    entropy = mode;
    guid = Guid::CreateForSession();
    actual_calls = calls;
  });
  worker.join();
  Check(actual_calls == expected_calls, "unexpected entropy call count");
  Check(!Guid::IsNull(guid), "entropy fallback returned a null GUID");
}

void CheckConcurrent() {
  constexpr unsigned kThreads = 16;
  constexpr unsigned kPerThread = 4096;
  std::vector<std::vector<std::string>> ids(kThreads);
  std::vector<std::thread> threads;
  for (auto& list : ids) {
    list.reserve(kPerThread);
    threads.emplace_back([&list] {
      for (unsigned i = 0; i < kPerThread; ++i)
        list.push_back(Guid::CreateForSession().ToString());
    });
  }
  for (auto& thread : threads) thread.join();
  std::unordered_set<std::string> unique;
  for (const auto& list : ids) {
    for (const auto& id : list) {
      Check(IsV4(id), "pooled GUID does not have valid v4 bits");
      Check(unique.insert(id).second, "duplicate GUID");
    }
  }
}

void CheckFork() {
  const auto before = Guid::CreateForSession();
  int fds[2];
  Check(::pipe(fds) == 0, "pipe failed");
  const pid_t child = ::fork();
  Check(child >= 0, "fork failed");
  if (child == 0) {
    ::close(fds[0]);
    entropy = Entropy::FailAfterSplit;
    calls = 0;
    const auto fallback = Guid::CreateForSession().ToString();
    if (calls != 2) _exit(2);
    entropy = Entropy::Real;
    const auto fresh = Guid::CreateForSession().ToString();
    if (calls != 3) _exit(3);
    const std::string pair = fallback + fresh;
    _exit(::write(fds[1], pair.data(), pair.size()) == static_cast<ssize_t>(pair.size()) ? 0 : 4);
  }

  ::close(fds[1]);
  const auto parent = Guid::CreateForSession().ToString();
  std::array<char, 72> child_ids{};
  size_t offset = 0;
  while (offset < child_ids.size()) {
    const auto count = ::read(fds[0], child_ids.data() + offset, child_ids.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<size_t>(count);
  }
  ::close(fds[0]);
  int status = 0;
  Check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "child reused inherited entropy or failed to refill");
  Check(offset == child_ids.size(), "short child result");
  const std::string fallback(child_ids.data(), 36);
  const std::string fresh(child_ids.data() + 36, 36);
  Check(parent != fallback && parent != fresh && fallback != fresh && before.ToString() != fresh,
        "fork duplicated a GUID");
  Check(IsV4(fresh), "fork refill did not generate a valid v4 GUID");
}

} // namespace

extern "C" ssize_t __real_getrandom(void*, size_t, unsigned int);

extern "C" ssize_t __wrap_getrandom(void* buffer, size_t length, unsigned int flags) {
  ++calls;
  if (entropy == Entropy::Interrupted && calls == 1) {
    errno = EINTR;
    return -1;
  }
  if ((entropy == Entropy::Split || entropy == Entropy::FailAfterSplit) && calls == 1)
    return __real_getrandom(buffer, std::min(length, size_t{13}), flags);
  if (entropy == Entropy::FailAfterSplit) {
    errno = EIO;
    return -1;
  }
  if (entropy == Entropy::Empty) return 0;
  return __real_getrandom(buffer, length, flags);
}

int main() {
  try {
    Check(::setenv("F2_SESSION_UUID_POOL", "1", 1) == 0, "setenv failed");
    CheckEntropy(Entropy::Interrupted, 2);
    CheckEntropy(Entropy::Split, 2);
    CheckEntropy(Entropy::FailAfterSplit, 2);
    CheckEntropy(Entropy::Empty, 1);
    CheckConcurrent();
    CheckFork();
    std::cout << "PASS entropy, 65536 concurrent GUIDs, fork and child refill\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
