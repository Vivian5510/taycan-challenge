// Experimental integration of pinned RocksDB 8.9.1 components. The queue,
// WriteBatch encoding, CRC/framing and reader are upstream implementations,
// linked unmodified. This file supplies the boundary where F2 would apply a
// durable batch; it does not implement a database or checkpoint/reclamation.
#pragma once
#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/write_batch_internal.h"
#include "db/write_thread.h"
#include "file/sequence_file_reader.h"
#include "file/writable_file_writer.h"
#include "options/db_options.h"
#include "rocksdb/file_system.h"

namespace reference {
namespace r = ROCKSDB_NAMESPACE;
using CommitClock = std::chrono::steady_clock;
#if defined(F2_WAL_PROFILE) && F2_WAL_PROFILE
inline constexpr bool kProfileWal=true;
#else
inline constexpr bool kProfileWal=false;
#endif
inline CommitClock::time_point ProfileNow() {
  if constexpr(kProfileWal)return CommitClock::now();
  return {};
}
inline uint64_t ElapsedNs(CommitClock::time_point begin) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(CommitClock::now()-begin).count();
}
inline void Check(const r::Status& s) {
  if (!s.ok()) throw std::runtime_error(s.ToString());
}
struct Counters {
  std::atomic<uint64_t> append_calls{0}, append_bytes{0}, file_syncs{0}, directory_syncs{0};
  std::atomic<bool> fail_append{false}, fail_sync{false};
  std::atomic<uint64_t> queue_wait_ns{0}, merge_ns{0}, append_ns{0}, sync_ns{0}, apply_ns{0}, leader_ns{0};
  std::atomic<uint64_t> collect_ns{0};
  std::atomic<uint64_t> grouped_requests{0}, grouped_operations{0};
  std::array<std::atomic<uint64_t>,9> group_histogram{};
};
class ObservedFile final : public r::FSWritableFileOwnerWrapper {
 public:
  ObservedFile(std::unique_ptr<r::FSWritableFile> file, Counters& stats)
      : r::FSWritableFileOwnerWrapper(std::move(file)), stats_(stats) {}
  r::IOStatus Append(const r::Slice& s, const r::IOOptions& o, r::IODebugContext* d) override {
    ++stats_.append_calls;
    if (stats_.fail_append || std::getenv("F2_SYNC_FAIL_WRITE")) return r::IOStatus::IOError("injected append failure");
    auto status = target()->Append(s, o, d);
    if (status.ok()) stats_.append_bytes += s.size();
    return status;
  }
  r::IOStatus Append(const r::Slice& s, const r::IOOptions& o,
                     const r::DataVerificationInfo&, r::IODebugContext* d) override {
    return Append(s, o, d);
  }
  r::IOStatus Sync(const r::IOOptions& o, r::IODebugContext* d) override {
    ++stats_.file_syncs;
    if (stats_.fail_sync || std::getenv("F2_SYNC_FAIL_SYNC")) return r::IOStatus::IOError("injected sync failure");
    return target()->Sync(o, d);
  }
 private:
  Counters& stats_;
};
class Committer {
 public:
  using Apply = std::function<r::Status(r::WriteBatch&, uint64_t)>;
  void SetCollectBudgetUs(uint64_t microseconds) {
    if(microseconds>1000)throw std::runtime_error("excessive WAL collect budget");
    collect_budget_ns_=microseconds*1000;
  }
  void UseDataLogCommit(bool enabled) {native_data_log_commit_=enabled;if(enabled)needs_directory_sync_=false;}
  void NoteDataLogSyncNs(uint64_t nanoseconds) {last_sync_ns_=nanoseconds;}
  static r::ImmutableDBOptions QueueOptions(size_t max_group_bytes) {
    r::DBOptions options;options.max_write_batch_group_size_bytes=max_group_bytes;
#ifdef F2_WRITE_ADAPTIVE_YIELD
    options.enable_write_thread_adaptive_yield=F2_WRITE_ADAPTIVE_YIELD;
    options.write_thread_max_yield_usec=F2_WRITE_MAX_YIELD_US;
#endif
    return r::ImmutableDBOptions(options);
  }
  Committer(const std::string& directory, Counters& stats, Apply apply = {},
            uint64_t recovered_sequence = 0, bool defer_open = false,
            size_t max_group_bytes=1<<20)
      : queue_(QueueOptions(max_group_bytes)), stats_(stats), apply_(std::move(apply)),
        last_sequence_(recovered_sequence) {
    auto fs = r::FileSystem::Default();
    Check(fs->CreateDirIfMissing(directory, r::IOOptions(), nullptr));
    Check(fs->NewDirectory(directory, r::IOOptions(), &directory_, nullptr));
    if (!defer_open) {
      path_ = directory + "/000001.log";
      writer_ = NewWal(path_, false);
    }
  }
  // Called only by the current leader, or before exposing the F2 instance.
  // A new empty WAL is stable before a checkpoint root can reference it.
  std::unique_ptr<r::log::Writer> PrepareWal(const std::string& path) {
    auto writer = NewWal(path, true);
    ++stats_.directory_syncs;
    Check(directory_->FsyncWithDirOptions(r::IOOptions(), nullptr,
        r::DirFsyncOptions(r::DirFsyncOptions::FsyncReason::kNewFileSynced)));
    return writer;
  }
  void InstallWal(const std::string& path, std::unique_ptr<r::log::Writer> writer) {
    if (writer_) Check(writer_->Close());
    path_ = path;
    writer_ = std::move(writer);
    needs_directory_sync_ = false;
  }
  void SetRecoveredSequence(uint64_t sequence) { last_sequence_ = sequence; }
  uint64_t Bytes() const { return writer_ ? writer_->file()->GetFileSize() : 0; }
 private:
  std::unique_ptr<r::log::Writer> NewWal(const std::string& path, bool sync_empty) {
    auto fs = r::FileSystem::Default();
    auto existing = fs->FileExists(path, r::IOOptions(), nullptr);
    if (existing.ok()) throw std::runtime_error("probe requires a new empty WAL; refusing overwrite");
    if (!existing.IsNotFound()) Check(existing);
    r::FileOptions options;
    std::unique_ptr<r::FSWritableFile> file;
    Check(fs->NewWritableFile(path, options, &file, nullptr));
    auto observed = std::make_unique<ObservedFile>(std::move(file), stats_);
    if (sync_empty) Check(observed->Sync(r::IOOptions(), nullptr));
    auto output = std::make_unique<r::WritableFileWriter>(std::move(observed), path, options);
    return std::make_unique<r::log::Writer>(std::move(output), 1, false);
  }
 public:
  r::Status Write(r::WriteBatch& batch) {
    struct ActiveCall {
      std::atomic<uint64_t>& count;
      bool enabled;
      ActiveCall(std::atomic<uint64_t>& n,bool on):count(n),enabled(on){if(enabled)++count;}
      ~ActiveCall(){if(enabled)--count;}
    } active(active_calls_,collect_budget_ns_!=0);
    if(collect_budget_ns_) {
      const uint64_t callers=active_calls_.load(std::memory_order_relaxed);
      auto peak=peak_calls_.load(std::memory_order_relaxed);
      while(peak<callers && !peak_calls_.compare_exchange_weak(peak,callers,std::memory_order_relaxed)){}
      ++incomplete_requests_;
    }
    r::WriteOptions options; options.sync = true; options.disableWAL = false;
    r::WriteThread::Writer self(options, &batch, nullptr, 0, false);
    const auto queue_start=ProfileNow();
    queue_.JoinBatchGroup(&self);
    if constexpr(kProfileWal)stats_.queue_wait_ns+=ElapsedNs(queue_start);
    if (self.state == r::WriteThread::STATE_COMPLETED) return self.FinalStatus();
    // Only observed concurrency can justify delaying a group snapshot. Cap
    // collection at a small fraction of the preceding storage sync latency.
    const auto target=std::min(peak_calls_.load(std::memory_order_relaxed),active_calls_.load(std::memory_order_relaxed));
    if(collect_budget_ns_ && target>1) {
      const auto begin=CommitClock::now();
      const uint64_t budget=std::min(collect_budget_ns_,last_sync_ns_/64);
      while(ElapsedNs(begin)<budget && incomplete_requests_.load(std::memory_order_relaxed)<target)
        std::this_thread::yield();
      if constexpr(kProfileWal)stats_.collect_ns+=ElapsedNs(begin);
    }
    r::WriteThread::WriteGroup group;
    queue_.EnterAsBatchGroupLeader(&self, &group);
    const auto leader_start=ProfileNow();
    if constexpr(kProfileWal) {
      stats_.grouped_requests+=group.size;
      size_t bucket=0;while(bucket<8 && group.size>(size_t{1}<<bucket))++bucket;
      ++stats_.group_histogram[bucket];
    }
    auto status = error_;
    r::WriteBatch merged;
    r::WriteBatch* logged = &batch;
    try {
      auto phase_start=ProfileNow();
      if (status.ok() && group.size > 1) {
        logged = &merged;
        for (auto* request : group) {
          status = r::WriteBatchInternal::Append(logged, request->batch);
          if (!status.ok()) break;
        }
      }
      if constexpr(kProfileWal)stats_.merge_ns+=ElapsedNs(phase_start);
      if (status.ok()) {
        const auto first = last_sequence_ + 1;
        r::WriteBatchInternal::SetSequence(logged, first);
        if(!native_data_log_commit_) {
          phase_start=ProfileNow();
          status = writer_->AddRecord(r::WriteBatchInternal::Contents(logged));
          if constexpr(kProfileWal)stats_.append_ns+=ElapsedNs(phase_start);
          if (status.ok()) {
            phase_start=CommitClock::now();
            status = writer_->file()->Sync(false);
            last_sync_ns_=ElapsedNs(phase_start);
            if constexpr(kProfileWal)stats_.sync_ns+=last_sync_ns_;
          }
        }
        if (status.ok() && needs_directory_sync_) {
          ++stats_.directory_syncs;
          status = directory_->FsyncWithDirOptions(r::IOOptions(), nullptr,
              r::DirFsyncOptions(r::DirFsyncOptions::FsyncReason::kNewFileSynced));
          if (status.ok()) needs_directory_sync_ = false;
        }
        // The real F2 integration must apply in this same sequence order and
        // complete pending F2 operations before releasing any group member.
        if (status.ok() && apply_) {
          phase_start=ProfileNow();
          status = apply_(*logged, first);
          if constexpr(kProfileWal)stats_.apply_ns+=ElapsedNs(phase_start);
        }
        if (status.ok()) {
          last_sequence_ += r::WriteBatchInternal::Count(logged);
          ++groups_;
          if constexpr(kProfileWal)stats_.grouped_operations+=r::WriteBatchInternal::Count(logged);
        }
      }
    } catch (const std::exception& e) {
      status = r::Status::IOError(e.what());
    }
    if (!status.ok()) error_ = status;
    if constexpr(kProfileWal)stats_.leader_ns+=ElapsedNs(leader_start);
    if(collect_budget_ns_)incomplete_requests_-=group.size;
    queue_.ExitAsBatchGroupLeader(group, status);
    return status.ok() ? self.FinalStatus() : status;
  }
  const std::string& path() const { return path_; }
  uint64_t groups() const { return groups_; }  // read after all callers joined
 private:
  r::WriteThread queue_;
  Counters& stats_;
  Apply apply_;
  std::unique_ptr<r::log::Writer> writer_;
  std::unique_ptr<r::FSDirectory> directory_;
  std::string path_;
  r::Status error_;
  uint64_t last_sequence_ = 0, groups_ = 0;
  bool needs_directory_sync_ = true;
  bool native_data_log_commit_=false;
  std::atomic<uint64_t> active_calls_{0},peak_calls_{0};
  std::atomic<uint64_t> incomplete_requests_{0};
  uint64_t collect_budget_ns_=0,last_sync_ns_=0;
};
struct RecoveryReporter : r::log::Reader::Reporter {
  r::Status status;
  void Corruption(size_t, const r::Status& s) override { if (status.ok()) status = s; }
};
inline uint64_t Recover(const std::string& path, r::WriteBatch::Handler& handler,
                        uint64_t recovered_sequence = 0) {
  auto fs = r::FileSystem::Default();
  std::unique_ptr<r::FSSequentialFile> file;
  Check(fs->NewSequentialFile(path, r::FileOptions(), &file, nullptr));
  auto input = std::make_unique<r::SequentialFileReader>(std::move(file), path);
  RecoveryReporter reporter;
  r::log::Reader reader(nullptr, std::move(input), &reporter, true, 1);
  r::Slice record; std::string scratch; uint64_t next = recovered_sequence + 1;
  while (reader.ReadRecord(&record, &scratch, r::WALRecoveryMode::kTolerateCorruptedTailRecords)) {
    Check(reporter.status);
    r::WriteBatch batch;
    Check(r::WriteBatchInternal::SetContents(&batch, record));
    if (r::WriteBatchInternal::Sequence(&batch) != next)
      throw std::runtime_error("non-contiguous recovery sequence");
    Check(batch.Iterate(&handler));
    next += r::WriteBatchInternal::Count(&batch);
  }
  Check(reporter.status);
  return next - 1;
}
}  // namespace reference
