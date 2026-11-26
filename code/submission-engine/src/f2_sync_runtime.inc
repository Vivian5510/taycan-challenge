using Disk=FASTER::device::FileSystemDisk<FASTER::environment::QueueIoHandler,
#ifdef F2_DURABLE_COMPACTION
  1ULL<<F2_DATA_SEGMENT_BITS
#else
  1ULL<<30
#endif
>;
using ColdIndex=fi::ColdIndex<Disk,fi::ColdLogHashIndexDefinition<8>>;
using Store=fc::F2Kv<Key,Value,Disk,fi::MemHashIndex<Disk>,ColdIndex>;
namespace ex=fc::experimental;
template<class Context>
void SyncComplete(fc::IAsyncContext* pointer,FStatus status) {
  fc::CallbackContext<Context> context{pointer};
  context->result->status.store(status);context->result->done.store(true);
}
F2Result Convert(FStatus s) {
  if(s==FStatus::Ok) return {F2ResultCode::ok,{}};
  if(s==FStatus::NotFound) return {F2ResultCode::not_found,{}};
  return {F2ResultCode::error,fc::StatusStr(s)};
}
struct SyncDirectoryLock {
  int fd=-1;
  explicit SyncDirectoryLock(const fs::path& path) {
    fd=::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(fd<0)throw std::runtime_error("open database lock failed");
    if(::flock(fd,LOCK_EX|LOCK_NB)){::close(fd);fd=-1;throw std::runtime_error("database locked");}
  }
  ~SyncDirectoryLock(){if(fd>=0)::close(fd);}
};
class SyncBackend final:public FasterBackend {
 public:
  SyncBackend(const std::string& directory,uint64_t budget)
      :directory_(fs::absolute(directory).lexically_normal()),lock_(directory_/"LOCK") {
    api_gate_.set_atomic_readers(true);
    std::string format=std::string("f2-sync-v1 ")+F2_SYNC_STRATEGY+" "+std::to_string(F2_LOG_PAGE_BITS)+" "+std::to_string(F2_OVERFLOW_PAGE_BITS)+" "+std::to_string(budget)
#ifdef F2_DATA_PREFIX
      +" prefix-block="+std::to_string(F2_PREFIX_BLOCK_BYTES)
#endif
#ifdef F2_DURABLE_COMPACTION
      +" durable-gc-v1 segment-bits="+std::to_string(F2_DATA_SEGMENT_BITS)+" index-budget-divisor=65536"
#endif
      ;
    std::string token;uint64_t cutoff=0,prefix_base_tail=0;
    const bool use_journal=std::string(F2_SYNC_STRATEGY)=="commit-log"
                           ||std::string(F2_SYNC_STRATEGY)=="hybridlog";
#ifdef __linux__
    const std::string prewritten_format = format + " wal-prewritten-v2";
    const std::string pipeline_format = format + " wal-pipeline-v3";
#endif
    bool prewritten = false;
    bool recover=fs::exists(directory_/"F2-SYNC-CURRENT");
    if(recover) {
      std::ifstream input(directory_/"F2-SYNC-CURRENT");std::string n,existing,c;
      std::getline(input,token);std::getline(input,n);std::getline(input,existing);std::getline(input,c);
#ifdef F2_DATA_PREFIX
      const std::string native_tail=c;prefix_base_tail=std::stoull(c);std::getline(input,c);
#endif
      auto payload=token+"\n"+n+"\n"+existing+"\n";
#ifdef F2_DATA_PREFIX
      payload+=native_tail+"\n";
#endif
#ifdef __linux__
      prewritten = use_journal && (existing == prewritten_format||existing==pipeline_format);
      if (prewritten) format = existing;
#endif
      if(existing!=format || token.size()!=36 || std::stoull(c)!=ex::CRC(payload.data(),payload.size()))
        throw std::runtime_error("incompatible/corrupt sync root");
      cutoff=std::stoull(n);
    } else for(const auto& item:fs::directory_iterator(directory_))
      if(item.path().filename()!="LOCK") throw std::runtime_error("missing sync recovery root");
#ifdef __linux__
    if (!recover && use_journal) {
      const char* mode = std::getenv("F2_WAL_PREWRITTEN");
      prewritten = mode && std::strcmp(mode, "1") == 0;
#ifdef F2_WAL_OPTIMIZED
      if(!mode)prewritten=true;
#endif
      if (prewritten) format = prewritten_format;
      if(const char* depth=std::getenv("F2_WAL_PIPELINE_DEPTH");depth&&std::strcmp(depth,"0")!=0) {
        if(!prewritten||std::string(F2_SYNC_STRATEGY)!="commit-log")
          throw std::runtime_error("WAL pipeline requires prewritten commit-log");
        format=pipeline_format;
      }
    }
#endif
    if(recover && use_journal && !fs::exists(directory_/"F2-COMMIT-0"))
      throw std::runtime_error("missing commit log");
#if defined(F2_DATA_PREFIX) && !defined(F2_DURABLE_COMPACTION)
    if(recover && !fs::is_regular_file(directory_/"hot"/"log.log0"))
      throw std::runtime_error("native recovery data log missing");
#endif
    uint64_t entries=4096;while(entries<budget/
#ifdef F2_DURABLE_COMPACTION
      65536
#else
      4096
#endif
      ) entries*=2;
    const uint64_t page=1ULL<<F2_LOG_PAGE_BITS;
    uint64_t memory=std::max<uint64_t>(6*page,budget/8/page*page);
    Store::HotIndexConfig hot{entries};Store::ColdIndexConfig cold{entries,memory,1.0/3.0};
    fc::ReadCacheConfig rc{};rc.enabled=false;
    auto compaction=fc::DEFAULT_F2_COMPACTION_CONFIG;
    // The WAL-aware background coordinator below invokes F2's native compact
    // operations and delays physical retirement until the adapter root commits.
    compaction.hot_store.enabled=compaction.cold_store.enabled=false;
    store_=std::make_unique<Store>(hot,memory,(directory_/"hot").string()+"/",cold,memory,(directory_/"cold").string()+"/",1.0/3.0,0,rc,compaction);
    uint64_t reclaim=std::max<uint64_t>(8ULL<<20,budget/64);
    if(const char* override_bytes=std::getenv("F2_SYNC_RECLAIM_BYTES")) reclaim=std::stoull(override_bytes);
    if(!reclaim) throw std::runtime_error("zero reclaim threshold");
    store_->SyncConfigure(directory_.string(),format,use_journal,prewritten,cutoff,[this]{Touch();},reclaim);
#ifdef F2_DURABLE_COMPACTION
    store_->PrefixGcConfigure();
#endif
    std::vector<fc::Guid> sessions;
    if(recover) {uint32_t version=0;
#ifdef F2_DATA_PREFIX
      store_->PrefixFindCommittedTail(prefix_base_tail,cutoff);
      Check(store_->PrefixRecover(fc::Guid::Parse(token),version));
#else
      Check(store_->Recover(fc::Guid::Parse(token),version,sessions));
#endif
    }
    if(sessions.empty()) store_->StartSession();else store_->ContinueSession(sessions.front());
    if(recover) store_->SyncReplay([this,&cutoff](uint32_t op,const std::string& key,const std::string& value){Check(Apply(op,key,value,++cutoff));});
    Touch();store_->SyncCheckpointAndReclaim();store_->StopSession();
    store_->SyncStartWalRuntime();
#ifdef F2_LINUX_DATA_PREFIX
    if(const char* enabled=std::getenv("F2_PREFIX_ASYNC_WRITER");
       enabled && enabled[0]=='1' && !enabled[1]) store_->PrefixStartDurableWriter();
#endif
#ifdef F2_DURABLE_COMPACTION
    store_->PrefixGcStart();
    std::fprintf(stderr,"F2_GC_SCHEDULER enabled=1 gc_reclaim_bytes=%llu wal_checkpoint_bytes=%llu\n",
      (unsigned long long)store_->SyncGcReclaimBytes(),(unsigned long long)reclaim);
#endif
  }
  ~SyncBackend() override {
    if(store_) {try{store_->SyncClose();}catch(const std::exception& e){std::fprintf(stderr,"SYNC_CLOSE_FAIL %s\n",e.what());}
      store_->SyncPrintWalProfile();
      std::fprintf(stderr,
#if defined(F2_DATA_PREFIX) && !defined(F2_LINUX_DATA_PREFIX)
        "F2_COMMIT_COUNTS single_operations=%llu full_index=%llu\n",
#elif defined(F2_LINUX_DATA_PREFIX)
        "F2_COMMIT_COUNTS data_log_groups=%llu full_index=%llu\n",
#else
        "F2_COMMIT_COUNTS log_only=%llu full_index=%llu\n",
#endif
        (unsigned long long)store_->SyncLogCommits(),(unsigned long long)store_->SyncFullCheckpoints());
#ifdef F2_LINUX_DATA_PREFIX
      store_->SyncPrintBatchProfile();
      std::fprintf(stderr,"F2_GC_BATCH_MODE notify=%d takeover=%d direct_owners=%llu fallback_owners=%llu\n",
        int(store_->SyncBatchNotifyEnabled()),int(store_->SyncBatchTakeoverEnabled()),
        (unsigned long long)store_->SyncBatchDirectOwners(),
        (unsigned long long)store_->SyncBatchFallbackOwners());
#endif
#ifdef F2_DATA_PREFIX
      store_->PrefixPrintCounters();
#endif
      if(store_->SyncReadProfileEnabled()) {
        std::fprintf(stderr,"F2_READ_PROFILE reuse_guid=%d reuse_write_guid=%d sample_every=64 samples=%llu gate_wait_ns=%llu session_start_ns=%llu read_await_ns=%llu complete_pending_ns=%llu session_stop_ns=%llu get_total_ns=%llu\n",
          store_->SyncReadReuseGuidEnabled()?1:0,
          store_->SyncWriteReuseGuidEnabled()?1:0,
          (unsigned long long)store_->SyncReadProfileSamples(),
          (unsigned long long)store_->SyncReadProfileGateWaitNs(),
          (unsigned long long)store_->SyncReadProfileSessionStartNs(),
          (unsigned long long)store_->SyncReadProfileReadAwaitNs(),
          (unsigned long long)store_->SyncReadProfileCompletePendingNs(),
          (unsigned long long)store_->SyncReadProfileSessionStopNs(),
          (unsigned long long)store_->SyncReadProfileGetTotalNs());
      }
      store_.reset();}
    std::fprintf(stderr,"F2_SYNC_IO sync_calls=%llu sync_ns=%llu write_calls=%llu write_bytes=%llu\n",
      (unsigned long long)ex::sync_calls.load(),(unsigned long long)ex::sync_ns.load(),
      (unsigned long long)ex::write_calls.load(),(unsigned long long)ex::write_bytes.load());
  }
  F2Result Put(const std::string& key,const std::string& value) override {
    std::shared_lock<ex::ReadWriteGate> api(api_gate_);
    return Mutate(1,key,value);
  }
  F2Result Delete(const std::string& key) override {
    std::shared_lock<ex::ReadWriteGate> api(api_gate_);
    return Mutate(2,key,{});
  }
  F2Result CreateCheckpoint(const std::string& checkpoint_directory) override;
  F2Result Get(const std::string& key,std::string* value) override {
    std::shared_lock<ex::ReadWriteGate> api(api_gate_);
    if(key.empty()||!value)return {F2ResultCode::error,"invalid read"};
    const bool profile=store_->SyncReadProfileNextSample();
    const auto started=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    auto record_total=[&] {
      if(profile)store_->SyncReadProfileAddGetTotal(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-started).count());
    };
    try{
      std::string copied=key;auto done=std::make_shared<Completion>();ReadContext c{copied,done};value->clear();
      auto status=store_->SyncRead([&]{return Await(store_->Read(c,SyncComplete<ReadContext>,1),done);},profile);
      if(status==FStatus::Ok)*value=std::move(done->value);record_total();return Convert(status);
    }catch(const std::exception& e){record_total();return {F2ResultCode::error,e.what()};}
  }
 private:
  void Check(FStatus s){if(s!=FStatus::Ok)throw std::runtime_error(fc::StatusStr(s));}
  FStatus Await(FStatus s,const std::shared_ptr<Completion>& done){
    while(s==FStatus::Pending&&!done->done.load())store_->CompletePending(false);
    return s==FStatus::Pending?done->status.load():s;
  }
  FStatus Apply(uint32_t op,const std::string& key,const std::string& value,uint64_t serial){
    auto done=std::make_shared<Completion>();
    if(op==1){UpsertContext c{key,value,done};return Await(store_->Upsert(c,SyncComplete<UpsertContext>,serial),done);}
    DeleteContext c{key,done};auto s=Await(store_->Delete(c,SyncComplete<DeleteContext>,serial),done);
    return s==FStatus::NotFound?FStatus::Ok:s;
  }
  void Touch(){
    auto done=std::make_shared<Completion>();ReadContext c{"sync-session-touch",done};
    auto s=Await(store_->Read(c,SyncComplete<ReadContext>,std::max<uint64_t>(1,store_->SyncSerial())),done);
    if(s!=FStatus::NotFound)Check(s);store_->CompletePending(true);
  }
  F2Result Mutate(uint32_t op,const std::string& key,const std::string& value){
#ifdef F2_DATA_PREFIX
    if(key.size()+value.size()+128>(256ULL<<10))return {F2ResultCode::error,"native prefix prototype record exceeds 256 KiB"};
#endif
    if(key.empty()||key.size()+value.size()+128>(1ULL<<F2_LOG_PAGE_BITS))return {F2ResultCode::error,"invalid record size"};
    try{return Convert(store_->SyncMutate(op,key,value,[&](uint64_t serial){return Apply(op,key,value,serial);}));}
    catch(const std::exception& e){return {F2ResultCode::error,e.what()};}
  }
  fs::path directory_;SyncDirectoryLock lock_;std::unique_ptr<Store> store_;
  ex::ReadWriteGate api_gate_;
};

constexpr unsigned kMaxF2Shards=64;
const char* kF2ShardFormat="f2-shards-v1";
const char* kF2ShardHash="fnv1a64-v1";

std::optional<unsigned> ReadShardCount(const fs::path& directory) {
  const auto manifest=directory/"F2-SHARDS";
  if(!fs::exists(manifest))return std::nullopt;
  std::ifstream input(manifest);
  std::string version,count,hash,crc;
  if(!std::getline(input,version)||!std::getline(input,count)||
     !std::getline(input,hash)||!std::getline(input,crc)||
     version!=kF2ShardFormat||hash!=kF2ShardHash)
    throw std::runtime_error("invalid F2 shard manifest");
  const std::string payload=version+"\n"+count+"\n"+hash+"\n";
  if(std::stoull(crc)!=ex::CRC(payload.data(),payload.size()))
    throw std::runtime_error("corrupt F2 shard manifest");
  const auto shards=std::stoul(count);
  if(shards<2||shards>kMaxF2Shards)
    throw std::runtime_error("unsupported F2 shard count");
  return static_cast<unsigned>(shards);
}

void WriteShardManifest(const fs::path& directory,unsigned shards) {
  const std::string payload=std::string(kF2ShardFormat)+"\n"+std::to_string(shards)+"\n"+
                            kF2ShardHash+"\n";
  const std::string content=payload+std::to_string(ex::CRC(payload.data(),payload.size()))+"\n";
  const auto temporary=directory/"F2-SHARDS.tmp";
  const auto manifest=directory/"F2-SHARDS";
  const int fd=::open(temporary.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
  if(fd<0)throw std::runtime_error("create F2 shard manifest failed");
  try {
    size_t written=0;
    while(written<content.size()) {
      const auto result=::write(fd,content.data()+written,content.size()-written);
      if(result<0&&errno==EINTR)continue;
      if(result<=0)throw std::runtime_error("write F2 shard manifest failed");
      written+=size_t(result);
    }
    ex::Sync(fd);
  } catch(...) {
    ::close(fd);
    throw;
  }
  if(::close(fd)<0)throw std::runtime_error("close F2 shard manifest failed");
  if(::rename(temporary.c_str(),manifest.c_str()))
    throw std::runtime_error("publish F2 shard manifest failed");
  ex::SyncDirectory(directory);
}

bool PathContains(const fs::path& parent,const fs::path& child) {
  auto p=parent.begin(),c=child.begin();
  for(;p!=parent.end();++p,++c)if(c==child.end()||*p!=*c)return false;
  return true;
}

void WriteSyncedFile(const fs::path& path,const std::string& contents) {
  const int fd=::open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
  if(fd<0)throw std::runtime_error("create checkpoint marker failed");
  try {
    size_t written=0;
    while(written<contents.size()) {
      const auto result=::write(fd,contents.data()+written,contents.size()-written);
      if(result<0&&errno==EINTR)continue;
      if(result<=0)throw std::runtime_error("write checkpoint marker failed");
      written+=size_t(result);
    }
    ex::Sync(fd);
  } catch(...) {::close(fd);throw;}
  if(::close(fd)<0)throw std::runtime_error("close checkpoint marker failed");
}

void SyncRegularFile(const fs::path& path) {
  const int fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
  if(fd<0)throw std::runtime_error("open checkpoint file for sync failed: "+path.string());
  try {ex::Sync(fd);} catch(...) {::close(fd);throw;}
  if(::close(fd)<0)throw std::runtime_error("close checkpoint file failed: "+path.string());
}

class CheckpointDirectory {
 public:
  CheckpointDirectory(const fs::path& source,const fs::path& requested) {
    if(requested.empty())throw std::runtime_error("empty checkpoint directory");
    const auto source_path=fs::canonical(source);
    auto destination=fs::absolute(requested).lexically_normal();
    if(destination.filename().empty())throw std::runtime_error("invalid checkpoint directory");
    const auto lexical_candidate=fs::weakly_canonical(destination.parent_path())/
                                 destination.filename();
    if(PathContains(source_path,lexical_candidate)||PathContains(lexical_candidate,source_path))
      throw std::runtime_error("checkpoint directory overlaps the source database");
    fs::create_directories(destination.parent_path());
    destination_=fs::canonical(destination.parent_path())/destination.filename();
    if(fs::exists(fs::symlink_status(destination_)))
      throw std::runtime_error("checkpoint directory already exists");
    if(PathContains(source_path,destination_)||PathContains(destination_,source_path))
      throw std::runtime_error("checkpoint directory overlaps the source database");

    if(!fs::create_directory(destination_))
      throw std::runtime_error("checkpoint directory already exists");
    try {
      WriteSyncedFile(destination_/"F2-CHECKPOINT-INCOMPLETE",
                      "f2-checkpoint-incomplete-v1\n");
      ex::SyncDirectory(destination_);
      ex::SyncDirectory(destination_.parent_path());
    } catch(...) {
      std::error_code ignored;
      fs::remove_all(destination_,ignored);
      throw;
    }
  }

  const fs::path& path() const {return destination_;}

  void CopySnapshotFiles(const fs::path& source) {
    if(!fs::is_regular_file(source/"F2-SYNC-CURRENT"))
      throw std::runtime_error("F2 checkpoint root is missing");
    std::vector<std::pair<fs::path,fs::perms>> directories;
    for(const auto& item:fs::recursive_directory_iterator(source)) {
      const auto relative=item.path().lexically_relative(source);
      if(relative.empty())throw std::runtime_error("invalid F2 checkpoint path");
      if(relative.parent_path().empty()&&IsRootMetadata(relative.filename()))continue;
      const auto status=item.symlink_status();
      if(fs::is_symlink(status))throw std::runtime_error("symlink in F2 checkpoint source");
      const auto output=destination_/relative;
      if(fs::is_directory(status)) {
        fs::create_directories(output);
        directories.emplace_back(output,status.permissions());
      } else if(fs::is_regular_file(status)) {
        fs::create_directories(output.parent_path());
        fs::copy_file(item.path(),output);
        fs::permissions(output,status.permissions());
        SyncRegularFile(output);
      } else {
        throw std::runtime_error("unsupported file in F2 checkpoint source");
      }
    }
    std::sort(directories.begin(),directories.end(),[](const auto& left,const auto& right) {
      return std::distance(left.first.begin(),left.first.end())>
             std::distance(right.first.begin(),right.first.end());
    });
    for(const auto& [path,permissions]:directories)fs::permissions(path,permissions);
    SyncDirectoryTree();
  }

  void PublishSingle(const fs::path& source) {
    PublishRootFile(source/"F2-SYNC-CURRENT");
  }

  void PublishSharded(unsigned shards) {
    SyncDirectoryTree();
    WriteShardManifest(destination_,shards);
    RemoveIncompleteMarker();
  }

 private:
  static bool IsRootMetadata(const fs::path& name) {
    return name=="LOCK"||name=="F2-SYNC-CURRENT"||name=="F2-SYNC-CURRENT.tmp"||
           name=="F2-SHARDS"||name=="F2-SHARDS.tmp"||
           name=="F2-CHECKPOINT-INCOMPLETE";
  }

  void SyncDirectoryTree() const {
    std::vector<fs::path> directories{destination_};
    for(const auto& item:fs::recursive_directory_iterator(destination_))
      if(item.is_directory())directories.push_back(item.path());
    std::sort(directories.begin(),directories.end(),[](const auto& left,const auto& right) {
      return std::distance(left.begin(),left.end())>
             std::distance(right.begin(),right.end());
    });
    for(const auto& directory:directories)ex::SyncDirectory(directory);
  }

  void RemoveIncompleteMarker() {
    if(!fs::remove(destination_/"F2-CHECKPOINT-INCOMPLETE"))
      throw std::runtime_error("checkpoint incomplete marker disappeared");
    ex::SyncDirectory(destination_);
  }

  void PublishRootFile(const fs::path& source_root) {
    if(!fs::is_regular_file(source_root))
      throw std::runtime_error("F2 checkpoint root disappeared");
    SyncDirectoryTree();
    const auto temporary=destination_/"F2-SYNC-CURRENT.tmp";
    fs::copy_file(source_root,temporary);
    SyncRegularFile(temporary);
    fs::rename(temporary,destination_/"F2-SYNC-CURRENT");
    ex::SyncDirectory(destination_);
    RemoveIncompleteMarker();
  }

  fs::path destination_;
};

F2Result SyncBackend::CreateCheckpoint(const std::string& checkpoint_directory) {
  std::unique_lock<ex::ReadWriteGate> api(api_gate_);
  try {
    CheckpointDirectory checkpoint(directory_,checkpoint_directory);
    store_->SyncCreateCheckpoint([&](const fs::path& source) {
      checkpoint.CopySnapshotFiles(source);
      checkpoint.PublishSingle(source);
    });
    return {F2ResultCode::ok,{}};
  } catch(const std::exception& error) {
    return {F2ResultCode::error,error.what()};
  }
}

uint64_t StableKeyHash(const std::string& key) {
  uint64_t hash=14695981039346656037ULL;
  for(unsigned char byte:key) {
    hash^=byte;
    hash*=1099511628211ULL;
  }
  return hash;
}

class ShardedSyncBackend final:public FasterBackend {
 public:
  ShardedSyncBackend(const std::string& directory,uint64_t budget,unsigned shards)
      :directory_(fs::absolute(directory).lexically_normal()),lock_(directory_/"LOCK"),shards_(shards) {
    api_gate_.set_atomic_readers(true);
    if(shards_<2||shards_>kMaxF2Shards)throw std::runtime_error("unsupported F2 shard count");
    const auto existing=ReadShardCount(directory_);
    if(existing) {
      if(*existing!=shards_)throw std::runtime_error("F2 shard count differs from persisted layout");
    } else {
      for(const auto& entry:fs::directory_iterator(directory_))
        if(entry.path().filename()!="LOCK"&&entry.path().filename()!="F2-SHARDS.tmp")
          throw std::runtime_error("cannot shard a non-empty F2 database");
      const auto temporary=directory_/"F2-SHARDS.tmp";
      if(fs::exists(temporary))fs::remove(temporary);
      WriteShardManifest(directory_,shards_);
    }

    const uint64_t shard_budget=budget/shards_;
    if(shard_budget<(16ULL<<20))throw std::runtime_error("F2 shard budget is below 16 MiB");
    backends_.reserve(shards_);
    for(unsigned shard=0;shard<shards_;++shard) {
      const auto shard_directory=directory_/("shard-"+std::to_string(shard));
      if(existing) {
        if(!fs::is_directory(shard_directory)||!fs::is_regular_file(shard_directory/"F2-SYNC-CURRENT"))
          throw std::runtime_error("missing persisted F2 shard");
      } else {
        fs::create_directories(shard_directory);
      }
      backends_.push_back(std::make_unique<SyncBackend>(shard_directory.string(),shard_budget));
    }
    if(!existing)ex::SyncDirectory(directory_);
  }

  F2Result Put(const std::string& key,const std::string& value) override {
    std::shared_lock<ex::ReadWriteGate> api(api_gate_);
    return Owner(key).Put(key,value);
  }
  F2Result Get(const std::string& key,std::string* value) override {
    std::shared_lock<ex::ReadWriteGate> api(api_gate_);
    return Owner(key).Get(key,value);
  }
  F2Result Delete(const std::string& key) override {
    std::shared_lock<ex::ReadWriteGate> api(api_gate_);
    return Owner(key).Delete(key);
  }
  F2Result CreateCheckpoint(const std::string& checkpoint_directory) override;

 private:
  FasterBackend& Owner(const std::string& key) {
    return *backends_[StableKeyHash(key)%shards_];
  }

  fs::path directory_;
  SyncDirectoryLock lock_;
  unsigned shards_;
  std::vector<std::unique_ptr<FasterBackend>> backends_;
  ex::ReadWriteGate api_gate_;
};

F2Result ShardedSyncBackend::CreateCheckpoint(const std::string& checkpoint_directory) {
  std::unique_lock<ex::ReadWriteGate> api(api_gate_);
  try {
    CheckpointDirectory checkpoint(directory_,checkpoint_directory);
    for(unsigned shard=0;shard<shards_;++shard) {
      const auto destination=checkpoint.path()/("shard-"+std::to_string(shard));
      const auto result=backends_[shard]->CreateCheckpoint(destination.string());
      if(result.code!=F2ResultCode::ok)
        throw std::runtime_error("F2 shard checkpoint failed: "+result.message);
    }
    checkpoint.PublishSharded(shards_);
    return {F2ResultCode::ok,{}};
  } catch(const std::exception& error) {
    return {F2ResultCode::error,error.what()};
  }
}
}
std::unique_ptr<FasterBackend> CreateFasterBackend(const std::string& directory,uint64_t budget) {
  const auto root=fs::absolute(directory).lexically_normal();
  const auto persisted=ReadShardCount(root);
  unsigned requested=0;
  if(const char* value=std::getenv("F2_SHARD_COUNT")) {
    size_t consumed=0;
    const auto parsed=std::stoul(value,&consumed);
    if(consumed!=std::strlen(value)||parsed<1||parsed>kMaxF2Shards)
      throw std::runtime_error("invalid F2_SHARD_COUNT");
    requested=static_cast<unsigned>(parsed);
  }
  if(persisted) {
    if(requested&&requested!=*persisted)
      throw std::runtime_error("F2_SHARD_COUNT differs from persisted layout");
    std::fprintf(stderr,"F2_SHARD_LAYOUT count=%u routing=%s\n",*persisted,kF2ShardHash);
    return std::make_unique<ShardedSyncBackend>(root.string(),budget,*persisted);
  }
  if(requested<2) {
    std::fprintf(stderr,"F2_SHARD_LAYOUT count=1 routing=single\n");
    return std::make_unique<SyncBackend>(root.string(),budget);
  }
  std::fprintf(stderr,"F2_SHARD_LAYOUT count=%u routing=%s\n",requested,kF2ShardHash);
  return std::make_unique<ShardedSyncBackend>(root.string(),budget,requested);
}
