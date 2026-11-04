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
    const std::string format=std::string("f2-sync-v1 ")+F2_SYNC_STRATEGY+" "+std::to_string(F2_LOG_PAGE_BITS)+" "+std::to_string(F2_OVERFLOW_PAGE_BITS)+" "+std::to_string(budget)
#ifdef F2_DATA_PREFIX
      +" prefix-block="+std::to_string(F2_PREFIX_BLOCK_BYTES)
#endif
#ifdef F2_DURABLE_COMPACTION
      +" durable-gc-v1 segment-bits="+std::to_string(F2_DATA_SEGMENT_BITS)+" index-budget-divisor=65536"
#endif
      ;
    std::string token,base_token;uint64_t cutoff=0,prefix_base_tail=0;
    const bool hybrid=std::string(F2_SYNC_STRATEGY)=="hybridlog";
    bool recover=fs::exists(directory_/"F2-SYNC-CURRENT");
    if(recover) {
      std::ifstream input(directory_/"F2-SYNC-CURRENT");std::string n,existing,c;
      std::getline(input,token);std::getline(input,n);std::getline(input,existing);std::getline(input,c);
#ifdef F2_DATA_PREFIX
      const std::string native_tail=c;prefix_base_tail=std::stoull(c);std::getline(input,c);
#endif
      if(hybrid){base_token=c;std::getline(input,c);}
      auto payload=token+"\n"+n+"\n"+existing+"\n";
      if(hybrid)payload+=base_token+"\n";
#ifdef F2_DATA_PREFIX
      payload+=native_tail+"\n";
#endif
      if(existing!=format || token.size()!=36 || std::stoull(c)!=ex::CRC(payload.data(),payload.size()))
        throw std::runtime_error("incompatible/corrupt sync root");
      cutoff=std::stoull(n);
    } else for(const auto& item:fs::directory_iterator(directory_))
      if(item.path().filename()!="LOCK") throw std::runtime_error("missing sync recovery root");
    if(recover && std::string(F2_SYNC_STRATEGY)=="commit-log" && !fs::exists(directory_/"F2-COMMIT-0"))
      throw std::runtime_error("missing commit log");
#if defined(F2_ROCKS_WAL) && !defined(F2_DATA_PREFIX)
    if(recover && !fs::is_regular_file(directory_/"wal"/(token+".log")))
      throw std::runtime_error("missing WAL generation referenced by recovery root");
#endif
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
    auto compaction=fc::DEFAULT_F2_COMPACTION_CONFIG;compaction.hot_store.enabled=compaction.cold_store.enabled=false;
    store_=std::make_unique<Store>(hot,memory,(directory_/"hot").string()+"/",cold,memory,(directory_/"cold").string()+"/",1.0/3.0,0,rc,compaction);
    uint64_t reclaim=std::max<uint64_t>(8ULL<<20,budget/64);
    if(const char* override_bytes=std::getenv("F2_SYNC_RECLAIM_BYTES")) reclaim=std::stoull(override_bytes);
    if(!reclaim) throw std::runtime_error("zero reclaim threshold");
    store_->SyncConfigure(directory_.string(),format,std::string(F2_SYNC_STRATEGY)=="commit-log",cutoff,[this]{Touch();},reclaim,hybrid,
#ifdef F2_ROCKS_WAL
      token);
    store_->SyncSetApply([this](uint32_t op,const std::string& key,const std::string& value,uint64_t serial){return Apply(op,key,value,serial);});
#else
      base_token);
#endif
#ifdef F2_DURABLE_COMPACTION
    store_->PrefixGcConfigure();
#endif
    std::vector<fc::Guid> sessions;
    if(recover) {uint32_t version=0;
#ifdef F2_DATA_PREFIX
      store_->PrefixFindCommittedTail(prefix_base_tail,cutoff);
      Check(store_->PrefixRecover(fc::Guid::Parse(token),version));
#else
#ifdef F2_HYBRID_EXPERIMENT
      if(hybrid)Check(store_->HybridLogRecover(fc::Guid::Parse(base_token),fc::Guid::Parse(token),version));else
#endif
      Check(store_->Recover(fc::Guid::Parse(token),version,sessions));
#endif
    }
    if(sessions.empty()) store_->StartSession();else store_->ContinueSession(sessions.front());
    if(recover) store_->SyncReplay([this,&cutoff](uint32_t op,const std::string& key,const std::string& value){Check(Apply(op,key,value,++cutoff));});
    Touch();store_->SyncCheckpointAndReclaim();store_->StopSession();
#ifdef F2_DURABLE_COMPACTION
    store_->PrefixGcStart();
#endif
#ifdef F2_PREFIX_PIPELINE
    store_->PrefixStartPipeline();
#endif
  }
  ~SyncBackend() override {
    if(store_) {try{store_->SyncClose();}catch(const std::exception& e){std::fprintf(stderr,"SYNC_CLOSE_FAIL %s\n",e.what());}
      std::fprintf(stderr,
#ifdef F2_ROCKS_WAL
        "F2_COMMIT_COUNTS wal_groups=%llu full_index=%llu\n",
#else
        "F2_COMMIT_COUNTS log_only=%llu full_index=%llu\n",
#endif
        (unsigned long long)store_->SyncLogCommits(),(unsigned long long)store_->SyncFullCheckpoints());
#ifdef F2_ROCKS_WAL
      store_->SyncPrintRocksCounters();
#endif
#ifdef F2_DATA_PREFIX
      store_->PrefixPrintCounters();
#endif
#ifdef F2_PREFIX_PIPELINE
      store_->PrefixPrintPipelineCounters();
#endif
      store_.reset();}
    std::fprintf(stderr,"F2_SYNC_IO sync_calls=%llu write_calls=%llu write_bytes=%llu\n",
      (unsigned long long)ex::sync_calls.load(),(unsigned long long)ex::write_calls.load(),(unsigned long long)ex::write_bytes.load());
  }
  F2Result Put(const std::string& key,const std::string& value) override {return Mutate(1,key,value);}
  F2Result Delete(const std::string& key) override {return Mutate(2,key,{});}
  F2Result Get(const std::string& key,std::string* value) override {
    if(key.empty()||!value)return {F2ResultCode::error,"invalid read"};
    try{
      std::string copied=key;auto done=std::make_shared<Completion>();ReadContext c{copied,done};value->clear();
      auto status=store_->SyncRead([&]{return Await(store_->Read(c,SyncComplete<ReadContext>,1),done);});
      if(status==FStatus::Ok)*value=std::move(done->value);return Convert(status);
    }catch(const std::exception& e){return {F2ResultCode::error,e.what()};}
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
};
}
std::unique_ptr<FasterBackend> CreateFasterBackend(const std::string& directory,uint64_t budget){return std::make_unique<SyncBackend>(directory,budget);}
