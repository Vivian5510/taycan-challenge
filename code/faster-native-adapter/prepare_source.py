"""Prepare an auditable FASTER build copy; never modify the research checkout."""
import argparse
import difflib
from pathlib import Path
import shutil
import sys

parser = argparse.ArgumentParser()
parser.add_argument('destination', type=Path)
parser.add_argument('--log-page-bits', type=int, default=25)
parser.add_argument('--overflow-page-bits', type=int, default=20)
parser.add_argument('--fix-recovery-version', type=int, choices=(0, 1), default=0)
parser.add_argument('--fix-recovery-frontier', type=int, choices=(0, 1), default=0)
parser.add_argument('--commit-experiment', type=int, choices=(0, 1), default=0)
parser.add_argument('--sync-experiment', type=int, choices=(0, 1), default=0)
parser.add_argument('--hybrid-experiment', type=int, choices=(0, 1), default=0)
parser.add_argument('--prefix-experiment', type=int, choices=(0, 1), default=0)
args = parser.parse_args()
here = Path(__file__).resolve().parent
upstream = here.parents[1] / 'research/FASTER/cc/src'
destination = args.destination.resolve()
assert 20 <= args.log_page_bits <= 25
assert 10 <= args.overflow_page_bits <= 20
shutil.copytree(upstream, destination, dirs_exist_ok=True)

def replace(relative, old, new):
    path = destination / relative
    text = path.read_text()
    assert text.count(old) == 1, (relative, old)
    path.write_text(text.replace(old, new))

# Allow an adapter that owns a safe session lifetime to supply its session GUID.
# Existing callers keep the upstream behavior through the default argument.
replace('core/f2.h', '  Guid StartSession();',
        '  Guid StartSession(Guid guid = Guid());')
replace('core/f2.h', 'inline Guid F2Kv<K, V, D, HHI, CHI>::StartSession() {',
        'inline Guid F2Kv<K, V, D, HHI, CHI>::StartSession(Guid guid) {')
replace('core/f2.h', '  Guid guid = Guid::Create();\n  Guid ret_guid = hot_store.StartSession(guid);',
        '  if (Guid::IsNull(guid)) guid = Guid::Create();\n'
        '  Guid ret_guid = hot_store.StartSession(guid);')

if sys.platform.startswith('linux') and args.prefix_experiment:
    shutil.copyfile(here / 'f2_session_uuid.inc', destination / 'core/f2_session_uuid.inc')
    replace('core/guid.h', '#include <cassert>',
            '#include <array>\n#include <cassert>\n#include <cerrno>\n#include <cstdlib>\n'
            '#include <sys/random.h>\n#include <unistd.h>')
    replace('core/guid.h', '  static Guid Parse(',
            '#include "f2_session_uuid.inc"\n\n  static Guid Parse(')
    replace('core/f2.h', 'if (Guid::IsNull(guid)) guid = Guid::Create();',
            'if (Guid::IsNull(guid)) guid = Guid::CreateForSession();')

if sys.platform == 'darwin':
    replace('environment/file.h', '#else\n#include "file_linux.h"',
            '#elif defined(__APPLE__)\n#include "native_file.h"\n#else\n#include "file_linux.h"')
    shutil.copyfile(here / 'native_file.h', destination / 'environment/native_file.h')
    # Drain before member destruction while log buffers and index callback
    # targets still exist. No-op for the unchanged inline experiment.
    replace('core/faster.h', '      compaction_thread_.join();\n    }\n  }\n\n  // No copy constructor.',
            '      compaction_thread_.join();\n    }\n'
            '#ifdef F2_NATIVE_POSIX_AIO\n    disk.handler().Drain();\n#endif\n  }\n\n  // No copy constructor.')
if args.log_page_bits != 25:
    replace('core/address.h', 'static constexpr uint64_t kOffsetBits = 25;',
            'static constexpr uint64_t kOffsetBits = {};'.format(args.log_page_bits))
if args.overflow_page_bits != 20:
    replace('core/malloc_fixed_page_size.h', 'static constexpr uint64_t kOffsetBits = 20;',
            'static constexpr uint64_t kOffsetBits = {};'.format(args.overflow_page_bits))
if args.fix_recovery_version:
    # RecoverDone resets log_metadata.version to UINT32_MAX. Preserve it before
    # clearing checkpoint state; otherwise the subsequent version wraps to zero.
    replace('core/faster.h',
            '  checkpoint_.RecoverDone();\n'
            '  system_state_.store(SystemState{ Action::None, Phase::REST,\n'
            '                                   checkpoint_.log_metadata.version + 1 });',
            '  const uint32_t recovered_version = checkpoint_.log_metadata.version;\n'
            '  checkpoint_.RecoverDone();\n'
            '  system_state_.store(SystemState{ Action::None, Phase::REST,\n'
            '                                   recovered_version + 1 });')
if args.fix_recovery_frontier:
    # The recovered committed prefix already exists on disk. Keeping its
    # read-only frontier at tail but flushed frontier at page start causes the
    # next checkpoint to wait forever if this log receives no additional data.
    replace('core/persistent_memory_malloc.h',
            '  flushed_until_address.store(Address{ tail_address.page(), 0 });',
            '  flushed_until_address.store(tail_address);')
if args.commit_experiment:
    shutil.copyfile(here / 'f2_commit_experiment.h', destination / 'core/f2_commit_experiment.h')
    shutil.copyfile(here / 'f2_commit_members.inc', destination / 'core/f2_commit_members.inc')
    replace('core/f2.h', '#include "checkpoint_state_f2.h"',
            '#include "f2_commit_experiment.h"\n#include "checkpoint_state_f2.h"')
    replace('core/f2.h', '  // checkpoint-related\n  HotColdCheckpointState checkpoint_;',
            '  // checkpoint-related\n  HotColdCheckpointState checkpoint_;\n#include "f2_commit_members.inc"')
    replace('core/f2.h', '    InitializeCompaction();',
            '    checkpoint_.experiment_cv = &experiment_cv_;\n    InitializeCompaction();')
    replace('core/checkpoint_state_f2.h', 'class HotColdCheckpointState {\n public:',
            'class HotColdCheckpointState {\n public:\n'
            '  std::condition_variable* experiment_cv = nullptr;\n'
            '  std::atomic<uint32_t> experiment_cold_callbacks{0};\n'
            '  std::atomic<bool> experiment_cold_failed{false};')
    replace('core/f2.h', '  checkpoint_.hot_store_status.store(StoreCheckpointStatus::REQUESTED);\n  return true;',
            '  checkpoint_.hot_store_status.store(StoreCheckpointStatus::REQUESTED);\n'
            '  experiment_cv_.notify_one();\n  return true;')
    replace('core/f2.h', '    checkpoint->cold_store_status.store(StoreCheckpointStatus::REQUESTED);',
            '    checkpoint->cold_store_status.store(StoreCheckpointStatus::REQUESTED);\n'
            '    checkpoint->experiment_cv->notify_one();')
    # Subsequent successful sessions must not spin forever when the first
    # callback has already set the global result to Ok.
    replace('core/f2.h',
            '    Status global_result = checkpoint->hot_store_checkpoint_result.load();\n'
            '    if (global_result == Status::Corruption || result != Status::Ok) {',
            '    Status global_result = checkpoint->hot_store_checkpoint_result.load();\n'
            '    if (global_result != Status::Corruption && result == Status::Ok) break;\n'
            '    if (global_result == Status::Corruption || result != Status::Ok) {')
    replace('core/f2.h',
            '        StoreCheckpointStatus status;\n'
            '        if ((status = checkpoint->cold_store_status.load()) != StoreCheckpointStatus::ACTIVE) {',
            '        if (result != Status::Ok) checkpoint->experiment_cold_failed.store(true);\n'
            '        if (checkpoint->experiment_cold_callbacks.fetch_sub(1) != 1) return;\n'
            '        if (checkpoint->experiment_cold_failed.load()) result = Status::IOError;\n'
            '        StoreCheckpointStatus status;\n'
            '        if ((status = checkpoint->cold_store_status.load()) != StoreCheckpointStatus::ACTIVE) {')
    replace('core/f2.h',
            '      bool success = cold_store.InternalCheckpoint(nullptr, callback, checkpoint_.token,',
            '      checkpoint_.experiment_cold_callbacks.store(cold_store.NumActiveSessions());\n'
            '      checkpoint_.experiment_cold_failed.store(false);\n'
            '      bool success = cold_store.InternalCheckpoint(nullptr, callback, checkpoint_.token,')
    replace('core/f2.h', '    background_worker_active_.store(false);',
            '    background_worker_active_.store(false);\n    experiment_cv_.notify_one();')
    replace('core/f2.h', '      std::this_thread::sleep_for(check_interval);',
            '      if (!experiment_wakeup_) {\n'
            '        std::this_thread::sleep_for(check_interval);\n'
            '      } else {\n'
            '        std::unique_lock<std::mutex> lock(experiment_mutex_);\n'
            '        experiment_cv_.wait_for(lock, check_interval, [&] {\n'
            '          return !background_worker_active_.load() ||\n'
            '            checkpoint_.hot_store_status.load() == StoreCheckpointStatus::REQUESTED ||\n'
            '            checkpoint_.cold_store_status.load() == StoreCheckpointStatus::REQUESTED;\n'
            '        });\n'
            '      }')
if args.sync_experiment:
    assert args.commit_experiment
    shutil.copyfile(here / 'f2_sync_experiment.h', destination / 'core/f2_sync_experiment.h')
    shutil.copyfile(here / 'f2_sync_members.inc', destination / 'core/f2_sync_members.inc')
    replace('core/f2.h', '#include "f2_commit_experiment.h"',
            '#include "f2_commit_experiment.h"\n#include "f2_sync_experiment.h"')
    replace('core/f2.h', '#include "f2_commit_members.inc"',
            '#include "f2_commit_members.inc"\n#include "f2_sync_members.inc"')
if args.hybrid_experiment or args.prefix_experiment:
    # Both durable paths must reject a failed checkpoint rather than publish
    # a new recovery root based on an unconditional successful callback.
    replace('core/checkpoint_state.h',
            '      callback(callback_context, Status::Ok, serial_num);',
            '      callback(callback_context, failed.load() ? Status::IOError : Status::Ok, serial_num);')
    replace('core/checkpoint_state.h',
            '      callback(Status::Ok, serial_num);',
            '      callback(failed.load() ? Status::IOError : Status::Ok, serial_num);')
    replace('core/faster.h', '            WriteCprContext();',
            '            if(WriteCprContext() != Status::Ok) checkpoint_.failed = true;')
if args.hybrid_experiment or args.prefix_experiment:
    assert args.sync_experiment
    shutil.copyfile(here / 'f2_hybridlog_members.inc', destination / 'core/f2_hybridlog_members.inc')
    replace('core/f2.h', '#include "f2_sync_members.inc"',
            '#include "f2_sync_members.inc"\n#include "f2_hybridlog_members.inc"')
    replace('core/faster.h',
            'std::vector<Guid>& session_ids);',
            'std::vector<Guid>& session_ids, bool committed_prefix = false);')
    replace('core/faster.h',
            'std::vector<Guid>& session_ids) {\n  version = 0;',
            'std::vector<Guid>& session_ids, bool committed_prefix) {\n  committed_prefix_recovery_ = committed_prefix;\n  version = 0;')
    replace('core/faster.h',
            '  Status RecoverHybridLog();',
            '  bool committed_prefix_recovery_ = false;\n  Status RecoverHybridLog();')
    replace('core/faster.h',
            '    if(checkpoint_.index_metadata.version != checkpoint_.log_metadata.version) {',
            '    if((!committed_prefix && checkpoint_.index_metadata.version != checkpoint_.log_metadata.version) ||\n'
            '       (committed_prefix && (checkpoint_.index_metadata.version > checkpoint_.log_metadata.version ||\n'
            '        checkpoint_.index_metadata.checkpoint_start_address > checkpoint_.log_metadata.final_address ||\n'
            '        !fold_over_snapshot || checkpoint_.log_metadata.use_snapshot_file))) {')
    replace('core/faster.h',
            '    if(record->header.checkpoint_version <= checkpoint_.log_metadata.version) {',
            '    if(committed_prefix_recovery_ || record->header.checkpoint_version <= checkpoint_.log_metadata.version) {')
    replace('core/faster.h',
            '  return status;\n#undef BREAK_NOT_OK',
            '  committed_prefix_recovery_ = false;\n  return status;\n#undef BREAK_NOT_OK')
    # A committed prefix may never be altered by a later delete. Upserts already
    # use the adapter's append-only PutAtomic=false callback.
    relative = destination / 'core/faster.h'
    source = relative.read_text()
    start = source.index('inline OperationStatus FasterKv<K, V, D, H, OH>::InternalDelete(')
    end = source.index('return OperationStatus::SUCCESS;', start)
    segment = source[start:end]
    assert segment.count('if(address >= read_only_address) {') == 1
    segment = segment.replace('if(address >= read_only_address) {', 'if(address >= read_only_address && !force_tombstone) {')
    source = source[:start] + segment + source[end:]
    relative.write_text(source)
    # F2 appends a fresh tombstone; its variable-length Value header must be zero.
    replace('core/faster.h',
            '  pending_context.write_deep_key_at(const_cast<key_t*>(&record->key()));\n\n  index_status = hash_index_.TryUpdateEntry(thread_context, pending_context, new_address);',
            '  pending_context.write_deep_key_at(const_cast<key_t*>(&record->key()));\n'
            '  new (&record->value()) value_t{};\n\n  index_status = hash_index_.TryUpdateEntry(thread_context, pending_context, new_address);')
    replace('core/persistent_memory_malloc.h',
            '  begin_address.store(begin_address_);\n  tail_page_offset_.store(tail_address);',
            '  begin_address.store(begin_address_);\n'
            '  if (tail_address.offset() != 0) std::memset(Page(tail_address.page()) + tail_address.offset(), 0, kPageSize - tail_address.offset());\n'
            '  tail_page_offset_.store(tail_address);')
if args.prefix_experiment:
    if sys.platform.startswith('linux'):
        replace('environment/file_linux.h', '#include <libaio.h>',
                '#include <libaio.h>\n#include <linux/fs.h>\n#include <sys/uio.h>')
        replace('environment/file_linux.h',
                '    core::AsyncIOCallback callback;\n  };\n\n  inline io_context_t',
                '    core::AsyncIOCallback callback;\n'
                '    std::string write_path;\n'
                '    struct iovec durable_vector{};\n'
                '    bool durable_write=false;\n'
                '    void PrepareDurableWrite(int fd,uint8_t* buffer,uint32_t length,size_t offset) {\n'
                '      durable_vector.iov_base=buffer;\n'
                '      durable_vector.iov_len=length;\n'
                '      ::io_prep_pwritev2(&parent_iocb,fd,&durable_vector,1,offset,RWF_DSYNC);\n'
                '      ::io_set_callback(&parent_iocb,IoCompletionCallback);\n'
                '      durable_write=true;\n'
                '    }\n'
                '  };\n\n  inline io_context_t')
        replace('environment/file_linux.cc', '#include "file_linux.h"',
                '#include "file_linux.h"\n#include "../core/f2_sync_experiment.h"')
        replace('environment/file_linux.cc',
                '  callback_context->callback(callback_context->caller_context, return_status, bytes_transferred);',
                '  if (!callback_context->write_path.empty()) {\n'
                '    const auto& path = callback_context->write_path;\n'
                '    const size_t expected_bytes = callback_context->durable_write\n'
                '      ? callback_context->durable_vector.iov_len : iocb->u.c.nbytes;\n'
                '    if (res2 != 0 || bytes_transferred != expected_bytes || return_status != Status::Ok ||\n'
                '        (!callback_context->durable_write &&\n'
                '         !experimental::MarkDirtyDataDescriptor(path, iocb->aio_fildes))) {\n'
                '      return_status = Status::IOError;\n'
                '      experimental::MarkIoFailure(path);\n'
                '    } else {\n'
                '#ifdef F2_AIO_RWF_DSYNC\n'
                '      if(callback_context->durable_write) experimental::MarkDurable(path);\n'
                '      else experimental::MarkDirty(path);\n'
                '#else\n'
                '      experimental::MarkDirty(path);\n'
                '#endif\n'
                '      if (path.find("/hot/log.log") != std::string::npos) {\n'
                '        ++experimental::prefix_write_calls;\n'
                '        experimental::prefix_write_bytes += bytes_transferred;\n'
                '      }\n'
                '    }\n'
                '  }\n'
                '  callback_context->callback(callback_context->caller_context, return_status, bytes_transferred);')
        replace('environment/file_linux.cc',
                '  IAsyncContext* caller_context_copy;\n  RETURN_NOT_OK(context.DeepCopy(caller_context_copy));\n\n'
                '  new(io_context.get()) QueueIoHandler::IoCallbackContext(operationType, fd_, offset, length,\n'
                '      buffer, caller_context_copy, callback);',
                '  new(io_context.get()) QueueIoHandler::IoCallbackContext(operationType, fd_, offset, length,\n'
                '      buffer, nullptr, callback);\n'
                '  if (operationType == FileOperationType::Write) {\n'
                '    io_context->write_path = filename();\n'
                '#ifdef F2_AIO_RWF_DSYNC\n'
                '    if (filename().find("/hot/log.log") != std::string::npos)\n'
                '      io_context->PrepareDurableWrite(fd_,buffer,length,offset);\n'
                '#endif\n'
                '    if (std::getenv("F2_SYNC_FAIL_WRITE")) {\n'
                '      experimental::MarkIoFailure(filename());\n'
                '      return Status::IOError;\n'
                '    }\n'
                '  }\n'
                '  RETURN_NOT_OK(context.DeepCopy(io_context->caller_context));')
        replace('environment/file_linux.cc',
                '  int result = ::io_submit(io_object_, 1, iocbs);\n  if(result != 1) {\n    return Status::IOError;',
                '  int result;\n'
                '  do { result = ::io_submit(io_object_, 1, iocbs); } while (result == -EINTR);\n'
                '  if(result != 1) {\n'
                '    if (operationType == FileOperationType::Write) experimental::MarkIoFailure(filename());\n'
                '    if (io_context->caller_context != &context)\n'
                '      core::make_context_unique_ptr(io_context->caller_context);\n'
                '    return Status::IOError;')
    # The native data-log format uses RocksDB CRC32C only; it does not require
    # the RocksDB WriteThread/WAL adapter. Keeping these switches independent
    # lets the main F2 build use direct single-request commits.
    shutil.copyfile(here / 'f2_prefix_experiment.h', destination / 'core/f2_prefix_experiment.h')
    shutil.copyfile(here / 'f2_prefix_members.inc', destination / 'core/f2_prefix_members.inc')
    shutil.copyfile(here / 'f2_prefix_index.inc', destination / 'core/f2_prefix_index.inc')
    for name in ('f2_gc_trace.h', 'f2_gc_file.inc', 'f2_gc_members.inc'):
        shutil.copyfile(here / name, destination / 'core' / name)
    replace('core/f2.h', '#include "f2_sync_members.inc"',
            '#include "f2_sync_members.inc"\n#include "f2_prefix_members.inc"\n#ifdef F2_DURABLE_COMPACTION\n#include "f2_gc_members.inc"\n#endif')
    replace('device/file_system_disk.h', '#include "../core/gc_state.h"',
            '#include "../core/gc_state.h"\n#ifdef F2_DURABLE_COMPACTION\n#include "../core/f2_gc_trace.h"\n#endif')
    replace('device/file_system_disk.h',
            '    uint64_t new_begin_segment = new_begin_offset / kSegmentSize;',
            '    uint64_t new_begin_segment = new_begin_offset / kSegmentSize;\n'
            '#ifdef F2_DURABLE_COMPACTION\n'
            '    if(prefix_defer_truncation_) {\n'
            '      prefix_deferred_begin_=std::max(prefix_deferred_begin_,new_begin_segment);\n'
            '      gc_state.IssueTruncateCallback(new_begin_segment*kSegmentSize);return;\n'
            '    }\n#endif')
    replace('device/file_system_disk.h', '  std::atomic<uint64_t> begin_segment_;',
            '#ifdef F2_DURABLE_COMPACTION\n#include "../core/f2_gc_file.inc"\n#endif\n  std::atomic<uint64_t> begin_segment_;')
    replace('device/file_system_disk.h', '        file.Close();\n        file.Delete();',
            '#ifdef F2_DURABLE_COMPACTION\n'
            '        const auto path=context->files->filename_+std::to_string(idx);\n'
            '        const auto bytes=std::filesystem::file_size(path);\n'
            '        if(file.Close()!=core::Status::Ok || file.Delete()!=core::Status::Ok)\n'
            '          throw std::runtime_error("native segment retirement failed");\n'
            '        core::experimental::GcDeleted(path,bytes);\n'
            '#else\n        file.Close();\n        file.Delete();\n#endif')
    # Trace the actual segment deletion without exposing the bundle's filename.
    replace('device/file_system_disk.h', '  const uint64_t begin_segment;',
            '  const std::string& PrefixFilename() const {return filename_;}\n  const uint64_t begin_segment;')
    replace('device/file_system_disk.h', 'context->files->filename_+std::to_string(idx)',
            'context->files->PrefixFilename()+std::to_string(idx)')
    replace('device/file_system_disk.h', '    : filename_{ std::move(other.filename_) }',
            '    : filename_{ other.filename_ }')
    replace('index/cold_index.h', '  void SetRefreshCallback(void* faster, RefreshCallback callback) {',
            '#ifdef F2_DURABLE_COMPACTION\n  store_t& PrefixIndexStore() {return *store_;}\n#endif\n'
            '  void SetRefreshCallback(void* faster, RefreshCallback callback) {')
    replace('core/faster.h', '  RecoveryStatus recovery_status{ start_page, end_page };\n\n  uint32_t num_pages',
            '#ifdef F2_DURABLE_COMPACTION\n'
            '  start_page=std::max(start_page,checkpoint_.index_metadata.log_begin_address.page());\n'
            '  disk.log().begin_segment_.store(checkpoint_.index_metadata.log_begin_address.control()/disk_t::log_file_t::kSegmentSize);\n'
            '#endif\n  RecoveryStatus recovery_status{ start_page, end_page };\n\n  uint32_t num_pages')
    # Prefix commits place zero padding and invalid commit-marker records
    # between ordinary records. They are not the end of a physical page.
    replace('core/log_scan.h',
            '    while(offset <= until_offset) {\n'
            '      record_t* record = reinterpret_cast<record_t*>(&page_[offset]);\n'
            '      if (RecordInfo{ record->header }.IsNull()) {\n'
            '        break;\n'
            '      }\n'
            '      record_offsets_.push_back(offset);\n\n'
            '      if (offset + record->size() > Address::kMaxAddress) {\n'
            '        break;\n'
            '      }\n'
            '      offset += record->size();\n'
            '    }',
            '    const uint32_t finish=(start_address.page()==until.page())?until.offset():page_size_;\n'
            '    while(offset+sizeof(RecordInfo)<=finish) {\n'
            '      record_t* record=reinterpret_cast<record_t*>(&page_[offset]);\n'
            '      if(record->header.IsNull()) {offset+=sizeof(RecordInfo);continue;}\n'
            '      const auto bytes=record->size();\n'
            '      if(bytes<sizeof(RecordInfo) || offset+uint64_t(bytes)>finish)\n'
            '        throw std::runtime_error("invalid record in native compaction scan");\n'
            '      if(!record->header.invalid)record_offsets_.push_back(offset);\n'
            '      offset+=bytes;\n'
            '    }')
    from fix_iterator_lifetime import apply as fix_iterator_lifetime
    fix_iterator_lifetime(destination / 'core/log_scan.h')
    replace('core/faster.h', 'bool committed_prefix = false);',
            'bool committed_prefix = false, Address committed_until = Address::kInvalidAddress);')
    replace('core/faster.h', 'bool committed_prefix) {\n',
            'bool committed_prefix, Address committed_until) {\n')
    replace('core/faster.h', '    BREAK_NOT_OK(ReadCprMetadata(hybrid_log_token));',
            '    BREAK_NOT_OK(ReadCprMetadata(hybrid_log_token));\n'
            '    if(committed_prefix && committed_until != Address::kInvalidAddress) {\n'
            '      if(committed_until < checkpoint_.log_metadata.final_address) { status = Status::Corruption; break; }\n'
            '      checkpoint_.log_metadata.final_address = committed_until;\n'
            '    }')
    replace('core/faster.h', '  bool committed_prefix_recovery_ = false;',
            '#include "f2_prefix_index.inc"\n  bool committed_prefix_recovery_ = false;')
    path=destination/'core/faster.h'
    source=path.read_text()
    for method in ('InternalUpsert','InternalDelete'):
        start=source.index('inline OperationStatus FasterKv<K, V, D, H, OH>::'+method+'(')
        finish=source.index('\ntemplate <',start+10)
        segment=source[start:finish]
        needle='  const HashBucketEntry expected_entry{ pending_context.entry };'
        assert segment.count(needle)==1,method
        segment=segment.replace(needle,'  PrefixOverrideEntry(pending_context);\n'+needle)
        needle='index_status = hash_index_.TryUpdateEntry(thread_context, pending_context, new_address);'
        assert segment.count(needle)==1,method
        segment=segment.replace(needle,'index_status = PrefixStageEntry(thread_context, pending_context, new_address);')
        source=source[:start]+segment+source[finish:]
    path.write_text(source)
    replace('core/persistent_memory_malloc.h', '  inline Address GetTailAddress() const {',
            '  bool PrefixEnsureOpenTail() {\n'
            '    auto tail=tail_page_offset_.load();\n'
            '    return tail.offset() < kPageSize || NewPage(tail.page());\n'
            '  }\n\n  inline Address GetTailAddress() const {')
    replace('core/persistent_memory_malloc.h',
            '  safe_read_only_address.store(tail_address);\n\n  uint32_t end_page',
            '  safe_read_only_address.store(tail_address);\n'
            '  PageStatus(tail_address.page()).LastFlushedUntilAddress.store(tail_address);\n\n  uint32_t end_page')
    path=destination/'core/faster.h'
    text=path.read_text()
    needle='    // OS thread flushes current page and issues a read request if necessary\n    if(page + capacity < end_page) {'
    assert text.count(needle)==2
    replacement=('    // Index-only committed-prefix recovery does not modify records.\n'
            '    // Never rewrite previously acknowledged data while rebuilding an index.\n'
            '    if(committed_prefix_recovery_) {\n'
            '      recovery_status.page_status(page).store(PageRecoveryStatus::FlushDone);\n'
            '      if(page + capacity < end_page)\n'
            '        RETURN_NOT_OK(hlog.AsyncReadPagesFromLog(page + capacity, 1, recovery_status));\n'
            '      continue;\n'
            '    }\n'
            '    // OS thread flushes current page and issues a read request if necessary\n'
            '    if(page + capacity < end_page) {')
    path.write_text(text.replace(needle,replacement,1))
    replace('core/persistent_memory_malloc.h',
            '    Context context{ this, flush_page, std::min(page_end_address, until_address) };',
            '    Context context{ this, flush_page, std::min(page_end_address, until_address) };\n'
            '    const Address prior_flush=PageStatus(flush_page).LastFlushedUntilAddress.load();\n'
            '    uint64_t prefix_from=page_start_address.control();\n'
            '    if(prior_flush >= page_start_address && prior_flush <= page_end_address)\n'
            '      prefix_from=prior_flush.control() & ~uint64_t(F2_PREFIX_BLOCK_BYTES-1);\n'
            '    const uint64_t prefix_until=(std::min(page_end_address,until_address).control()+F2_PREFIX_BLOCK_BYTES-1) & ~uint64_t(F2_PREFIX_BLOCK_BYTES-1);\n'
            '    if(prefix_from >= prefix_until) continue;')
    replace('core/persistent_memory_malloc.h',
            '    RETURN_NOT_OK(file->WriteAsync(Page(flush_page), kPageSize * flush_page, kPageSize, callback,\n'
            '                                   context));',
            '    RETURN_NOT_OK(file->WriteAsync(Page(flush_page)+(prefix_from-page_start_address.control()),\n'
            '      prefix_from, uint32_t(prefix_until-prefix_from), callback, context));')
diff = []
for path in sorted(upstream.rglob('*')):
    if path.is_file():
        relative = path.relative_to(upstream)
        copied = destination / relative
        old, new = path.read_text(), copied.read_text()
        if old != new:
            diff.extend(difflib.unified_diff(
                old.splitlines(True), new.splitlines(True),
                fromfile='upstream/' + str(relative), tofile='build-copy/' + str(relative)))
(destination.parent / 'upstream-changes.patch').write_text(''.join(diff))
