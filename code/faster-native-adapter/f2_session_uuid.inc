  static Guid CreateForSession() {
    static const bool enabled = [] {
      const char* value = std::getenv("F2_SESSION_UUID_POOL");
      return value && std::strcmp(value, "1") == 0;
    }();
    if (!enabled) return Create();

    struct Pool {
      std::array<uint8_t, 256 * sizeof(uuid_t)> bytes;
      size_t next = bytes.size();
      pid_t pid = 0;
    };
    static thread_local Pool pool;
    const pid_t pid = ::getpid();
    if (pool.pid != pid || pool.next == pool.bytes.size()) {
      // Discard the inherited cache after fork, including on a failed refill.
      pool.next = pool.bytes.size();
      size_t filled = 0;
      while (filled < pool.bytes.size()) {
        const auto count = ::getrandom(pool.bytes.data() + filled, pool.bytes.size() - filled, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return Create();
        filled += static_cast<size_t>(count);
      }
      pool.pid = pid;
      pool.next = 0;
    }

    uuid_t uuid;
    std::memcpy(uuid, pool.bytes.data() + pool.next, sizeof(uuid));
    pool.next += sizeof(uuid);
    uuid[6] = (uuid[6] & 0x0f) | 0x40;
    uuid[8] = (uuid[8] & 0x3f) | 0x80;
    return uuid;
  }
