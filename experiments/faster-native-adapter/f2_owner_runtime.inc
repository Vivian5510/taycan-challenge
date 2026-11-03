// Opt-in topology/ownership experiment, not a Dragonfly runtime port. No new
// WAL: every owner uses the same F2 HybridLog commit/recovery implementation.
// Reads are fences between consecutive mutation groups in each owner's queue.
class OwnerBackend final : public FasterBackend {
  struct Request {
    uint32_t op;
    const std::string *key,*value;
    std::string* output;
    reference::CommitWait::Writer waiter;
    F2Result result{F2ResultCode::error,"owner request unfinished"};
  };
  struct Owner {
    std::mutex mutex;
    std::condition_variable work,space,initialized;
    std::deque<Request*> queue;
    std::thread thread;
    bool closing=false,ready=false;
    std::string error;
    uint64_t batches=0,requests=0,writes=0,peak_queue=0;
  };
 public:
  OwnerBackend(const std::string& directory,uint64_t budget)
      :root_(fs::absolute(directory).lexically_normal()),lock_(root_/"OWNER-LOCK") {
    count_=1;
    if(const char* n=std::getenv("F2_OWNER_SHARDS"))count_=std::stoul(n);
    if(count_!=1 && count_!=2 && count_!=4)throw std::runtime_error("owner count must be 1/2/4");
    if(budget/count_<(32ULL<<20))throw std::runtime_error("owner budget below prototype floor");
    const auto format="f2-owner-v1 fnv1a-mix64 "+std::to_string(count_)+" "+std::to_string(budget)+"\n";
    const auto content=format+std::to_string(ex::CRC(format.data(),format.size()))+"\n";
    const auto manifest=root_/"OWNER-LAYOUT";
    const bool existing=fs::exists(manifest);
    if(existing) {
      std::ifstream input(manifest);std::string found((std::istreambuf_iterator<char>(input)),{});
      if(found!=content)throw std::runtime_error("owner layout/budget mismatch or corrupt");
      for(unsigned id=0;id<count_;++id)
        if(!fs::is_regular_file(root_/std::to_string(id)/"F2-SYNC-CURRENT"))
          throw std::runtime_error("missing owner recovery root");
    } else {
      for(const auto& item:fs::directory_iterator(root_))
        if(item.path().filename()!="OWNER-LOCK")throw std::runtime_error("partial owner initialization; no layout");
    }
    try {
      for(unsigned id=0;id<count_;++id) {
        auto owner=std::make_unique<Owner>();auto* p=owner.get();owners_.push_back(std::move(owner));
        fs::create_directories(root_/std::to_string(id));
        p->thread=std::thread([this,p,id,budget]{Run(p,root_/std::to_string(id),budget/count_);});
        std::unique_lock<std::mutex> guard(p->mutex);p->initialized.wait(guard,[p]{return p->ready;});
        if(!p->error.empty())throw std::runtime_error(p->error);
      }
      if(!existing) {
        auto tmp=root_/"OWNER-LAYOUT.tmp";
        int fd=::open(tmp.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
        if(fd<0)throw std::runtime_error("create owner layout");
        try{ex::WriteAll(fd,content.data(),content.size());ex::Sync(fd);}catch(...){::close(fd);throw;}
        ::close(fd);fs::rename(tmp,manifest);ex::SyncDirectory(root_);
      }
    }catch(...){Stop();throw;}
  }
  ~OwnerBackend() override {Stop();}
  F2Result Put(const std::string& k,const std::string& v) override {return Submit(1,k,&v,nullptr);}
  F2Result Delete(const std::string& k) override {return Submit(2,k,nullptr,nullptr);}
  F2Result Get(const std::string& k,std::string* v) override {
    if(!v)return {F2ResultCode::error,"null output"};return Submit(0,k,nullptr,v);
  }
 private:
  static uint64_t Hash(const std::string& key) {
    uint64_t h=14695981039346656037ULL;
    for(unsigned char c:key){h^=c;h*=1099511628211ULL;}
    h^=h>>30;h*=0xbf58476d1ce4e5b9ULL;h^=h>>27;h*=0x94d049bb133111ebULL;return h^(h>>31);
  }
  void Finish(Request* request,F2Result result) {
    request->result=std::move(result);
    wait_.Complete(&request->waiter); // Last access: request can be destroyed now.
  }
  F2Result Submit(uint32_t op,const std::string& k,const std::string* v,std::string* output) {
    if(k.empty() || k.size()+(v?v->size():0)+128>(256ULL<<10))return {F2ResultCode::error,"invalid record"};
    Request request{op,&k,v,output};
    auto& owner=*owners_[Hash(k)%count_];
    {
      std::unique_lock<std::mutex> guard(owner.mutex);
      owner.space.wait(guard,[&]{return owner.queue.size()<256 || owner.closing;});
      if(owner.closing)return {F2ResultCode::error,"owner closed"};
      const bool wake=owner.queue.empty();owner.queue.push_back(&request);
      owner.peak_queue=std::max<uint64_t>(owner.peak_queue,owner.queue.size());
      if(wake)owner.work.notify_one();
    }
    wait_.Wait(&request.waiter);return request.result;
  }
  void Run(Owner* owner,const fs::path& path,uint64_t budget) {
    std::unique_ptr<SyncBackend> backend;
    try{backend=std::make_unique<SyncBackend>(path.string(),budget);}
    catch(const std::exception& e){owner->error=e.what();}
    {std::lock_guard<std::mutex> guard(owner->mutex);owner->ready=true;owner->initialized.notify_one();}
    if(!backend)return;
    bool failed=false;
    for(;;) {
      std::vector<Request*> group;
      {
        std::unique_lock<std::mutex> guard(owner->mutex);
        owner->work.wait(guard,[&]{return !owner->queue.empty()||owner->closing;});
        if(owner->queue.empty()&&owner->closing)break;
        size_t bytes=0;
        do {
          auto* req=owner->queue.front();
          const auto n=req->key->size()+(req->value?req->value->size():0)+128;
          if(!group.empty() && (req->op==0 || group.front()->op==0 || bytes+n>(256ULL<<10)))break;
          group.push_back(req);owner->queue.pop_front();bytes+=n;
        }while(!owner->queue.empty()&&group.size()<64);
        owner->space.notify_all();
      }
      ++owner->batches;owner->requests+=group.size();
      if(failed){for(auto* req:group)Finish(req,{F2ResultCode::error,"owner poisoned"});continue;}
      try {
        if(group.front()->op==0) {
          auto* req=group.front();Finish(req,backend->Get(*req->key,req->output));
        } else {
          rocksdb::WriteBatch batch;
          for(auto* req:group) {
            auto s=req->op==1?batch.Put(*req->key,*req->value):batch.Delete(*req->key);
            reference::Check(s);
          }
          auto result=backend->OwnerBatch(batch);owner->writes+=group.size();
          failed=result.code==F2ResultCode::error;
          for(auto* req:group)Finish(req,result);
        }
      }catch(const std::exception& e){failed=true;for(auto* req:group)Finish(req,{F2ResultCode::error,e.what()});}
    }
    backend.reset();
  }
  void Stop() {
    for(auto& owner:owners_) {
      {std::lock_guard<std::mutex> guard(owner->mutex);owner->closing=true;owner->work.notify_one();owner->space.notify_all();}
      if(owner->thread.joinable())owner->thread.join();
      std::fprintf(stderr,"F2_OWNER batches=%llu requests=%llu writes=%llu peak_queue=%llu shards=%u\n",
        (unsigned long long)owner->batches,(unsigned long long)owner->requests,
        (unsigned long long)owner->writes,(unsigned long long)owner->peak_queue,count_);
    }
    owners_.clear();
  }
  fs::path root_;SyncDirectoryLock lock_;
  unsigned count_=0;
  std::vector<std::unique_ptr<Owner>> owners_;
  reference::CommitWait wait_;
};
