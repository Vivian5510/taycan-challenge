// Included in F2Kv, only for the native data-log experiment. No compaction or
// read cache is permitted between a full base and the committed hot-log tail.
 public:
  using prefix_record_t=Record<K,V>;
  uint32_t PrefixMarkerSize() const {
    return prefix_record_t::size(sizeof(K),sizeof(V)+sizeof(experimental::PrefixFooter));
  }
  uint32_t PrefixFooterOffset() const {
    static_assert(sizeof(K)==4 && alignof(K)==4 && sizeof(V)==8 && alignof(V)==4,
      "native data-log prototype expects this adapter's byte-record headers");
    return sizeof(RecordInfo)+sizeof(K)+sizeof(V);
  }
  uint64_t PrefixOpenTail() {
    while(!hot_store.hlog.PrefixEnsureOpenTail()) {CompletePending(false);Refresh();std::this_thread::yield();}
    return hot_store.hlog.GetTailAddress().control();
  }
  void PrefixPadTo(uint64_t until) {
    for(uint64_t at=PrefixOpenTail();at<until;at=PrefixOpenTail()) {
      const uint64_t page=hot_store.hlog.kPageSize;
      uint32_t bytes=uint32_t(std::min(until-at,page-at%page));
      Address address=hot_store.BlockAllocate(bytes);
      if(address.control()!=at)throw std::runtime_error("prefix padding address changed");
      std::memset(hot_store.hlog.Get(address),0,bytes);
    }
    if(PrefixOpenTail()!=until)throw std::runtime_error("prefix padding exceeded boundary");
  }
  void PrefixSealBase() {
    const auto at=PrefixOpenTail();
    PrefixPadTo((at+experimental::kPrefixBlock-1)&~(experimental::kPrefixBlock-1));
    sync_state_->prefix.stable_tail=PrefixOpenTail();
    sync_state_->prefix.prepared_tail=sync_state_->prefix.stable_tail;
  }
  void PrefixSyncDataFiles() {
    auto& s=*sync_state_;std::set<std::string> files;
    const std::string prefix=(s.root/"hot"/"log.log").string();
    {std::lock_guard<std::mutex> lock(experimental::dirty_mutex);
      for(const auto& path:experimental::failed_paths)
        if(path.compare(0,prefix.size(),prefix)==0)throw std::runtime_error("native data-log I/O failure");
      for(auto it=experimental::dirty_paths.begin();it!=experimental::dirty_paths.end();) {
        if(it->compare(0,prefix.size(),prefix)==0) {files.insert(*it);it=experimental::dirty_paths.erase(it);}
        else ++it;
      }
    }
    if(files.empty())throw std::runtime_error("native commit produced no data writes");
    for(const auto& file:files) {
      experimental::SyncPath(file);++s.prefix.data_syncs;
      if(s.prefix.synced_files.insert(file).second) {
        experimental::SyncDirectory(std::filesystem::path(file).parent_path());++s.prefix.directory_syncs;
      }
    }
  }
  void PrefixCommit(uint64_t first,uint64_t last,uint64_t count) {
    auto& s=*sync_state_;
    if(compaction_config_.hot_store.enabled || compaction_config_.cold_store.enabled || hot_store.UseReadCache())
      throw std::runtime_error("native prefix requires compaction/read-cache disabled");
    if(experimental::kPrefixBlock%hot_store.disk.sector_size())
      throw std::runtime_error("native seal unit is incompatible with device sector size");
    const uint64_t from=
      s.prefix.stable_tail;
    const uint64_t at=PrefixOpenTail();
    const uint32_t marker_size=PrefixMarkerSize();
    const uint64_t end=(at+marker_size+experimental::kPrefixBlock-1)&~(experimental::kPrefixBlock-1);
    const uint64_t marker_at=end-marker_size;
    PrefixPadTo(marker_at);
    if(from<hot_store.hlog.head_address.load().control())throw std::runtime_error("uncommitted prefix left memory ring");
    uint32_t crc=0;
    for(uint64_t offset=from;offset<marker_at;) {
      const uint64_t bytes=std::min(marker_at-offset,hot_store.hlog.kPageSize-offset%hot_store.hlog.kPageSize);
      crc=rocksdb::crc32c::Extend(crc,reinterpret_cast<const char*>(hot_store.hlog.Get(Address(offset))),size_t(bytes));
      offset+=bytes;
    }
    Address address=hot_store.BlockAllocate(marker_size);
    if(address.control()!=marker_at)throw std::runtime_error("native marker address changed");
    auto* record=reinterpret_cast<prefix_record_t*>(hot_store.hlog.Get(address));
    std::memset(record,0,marker_size);
    record->header=RecordInfo(uint16_t(hot_store.system_state_.version()),true,false,true,Address::kInvalidAddress);
    const_cast<K&>(record->key()).length=0;
    record->value().length=sizeof(experimental::PrefixFooter);record->value().reserved=experimental::kPrefixTag;
    experimental::PrefixFooter footer{experimental::kPrefixMagic,from,end,first,last,count,crc,0,experimental::kPrefixEndMagic};
    footer.footer_crc=experimental::PrefixFooterCRC(footer);
    std::memcpy(reinterpret_cast<uint8_t*>(&record->value())+sizeof(V),&footer,sizeof(footer));
    PrefixPadTo(end);
    const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    const bool inject=minimum && last>=std::stoull(minimum);
    if(inject)experimental::Fault("before-data-flush");
    Address boundary=hot_store.hlog.ShiftReadOnlyToTail();
    if(boundary.control()!=end)throw std::runtime_error("native flush tail differs");
    const auto flush_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(hot_store.hlog.flushed_until_address.load()<boundary) {
      CompletePending(false);Refresh();std::this_thread::yield();
      if(std::chrono::steady_clock::now()>flush_deadline)throw std::runtime_error("native data-log flush frontier stalled");
    }
    CompletePending(true);
    if(inject)experimental::Fault("before-data-sync");
    PrefixSyncDataFiles();
    if(inject)experimental::Fault("after-data-sync");
    s.prefix.stable_tail=end;++s.prefix.commits;
  }
  uint64_t PrefixFileEnd() const {
    uint64_t end=0;const auto directory=sync_state_->root/"hot";
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
      const std::string name=entry.path().filename().string();
      if(name.rfind("log.log",0)!=0)continue;
      const auto suffix=name.substr(7);
      if(suffix.empty()||suffix.find_first_not_of("0123456789")!=std::string::npos)continue;
      const uint64_t bytes=entry.file_size();
      if(bytes)end=std::max(end,std::stoull(suffix)*experimental::kPrefixSegment+bytes);
    }
    return end;
  }
  void PrefixFindCommittedTail(uint64_t base_tail,uint64_t base_serial) {
    auto& s=*sync_state_;
    if(!base_tail || base_tail%experimental::kPrefixBlock)throw std::runtime_error("unaligned native base");
    const auto end=PrefixFileEnd();if(end<base_tail)throw std::runtime_error("native base log truncated");
    uint64_t stable=base_tail,serial=base_serial,record_at=base_tail;uint32_t crc=0;
    bool malformed_record=false;
    const uint64_t page_size=hot_store.hlog.kPageSize;
    std::vector<uint8_t> buffer(page_size);
    auto read_absolute=[&](uint64_t address,uint8_t* destination,size_t bytes) {
      size_t copied=0;
      while(copied<bytes) {
        const uint64_t segment=address/experimental::kPrefixSegment;
        const auto path=s.root/"hot"/("log.log"+std::to_string(segment));
        int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)throw std::runtime_error("native data segment missing");
        const size_t in_segment=size_t(std::min<uint64_t>(bytes-copied,experimental::kPrefixSegment-address%experimental::kPrefixSegment));
        size_t done=0;
        while(done<in_segment) {
          auto n=::pread(fd,destination+copied+done,in_segment-done,
                         off_t(address%experimental::kPrefixSegment+done));
          if(n<0&&errno==EINTR)continue;
          if(n<=0){::close(fd);throw std::runtime_error("short native data segment");}
          done+=size_t(n);
        }
        ::close(fd);copied+=in_segment;address+=in_segment;
      }
    };
    for(uint64_t page=base_tail/page_size;page*page_size<end;++page) {
      const uint64_t page_at=page*page_size;
      const auto path=s.root/"hot"/("log.log"+std::to_string(page_at/experimental::kPrefixSegment));
      int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)throw std::runtime_error("native data segment missing");
      size_t done=0;const size_t want=size_t(std::min(page_size,end-page_at));
      while(done<want) {
        auto n=::pread(fd,buffer.data()+done,want-done,off_t(page_at%experimental::kPrefixSegment+done));
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;done+=size_t(n);
      }
      ::close(fd);
      auto read_piece=[&](uint64_t address,uint8_t* destination,size_t bytes) {
        if(address>=page_at && address+bytes<=page_at+done)
          std::memcpy(destination,buffer.data()+address-page_at,bytes);
        else read_absolute(address,destination,bytes);
      };
      for(uint64_t offset=page_at<base_tail?base_tail-page_at:0;offset+experimental::kPrefixBlock<=done;offset+=experimental::kPrefixBlock) {
        const uint64_t sector_end=page_at+offset+experimental::kPrefixBlock;
        const uint64_t marker_at=sector_end-PrefixMarkerSize();
        // Walk real record boundaries. Arbitrary user values may contain an
        // exact copy of a commit signature at a sector-relative offset.
        while(!malformed_record && record_at<marker_at) {
          std::array<uint8_t,sizeof(RecordInfo)+sizeof(K)> header{};
          read_piece(record_at,header.data(),header.size());
          auto* candidate=reinterpret_cast<prefix_record_t*>(header.data());
          if(candidate->header.IsNull()) {record_at+=sizeof(RecordInfo);continue;}
          const uint64_t key_length=candidate->key().length;
          const uint64_t value_offset=(sizeof(RecordInfo)+sizeof(K)+key_length+alignof(V)-1)&~uint64_t(alignof(V)-1);
          const uint64_t record_page_end=(record_at/page_size+1)*page_size;
          if(key_length>256*1024 || record_at+value_offset+sizeof(V)>std::min(end,record_page_end)) {malformed_record=true;break;}
          std::array<uint8_t,sizeof(V)> value_header{};
          read_piece(record_at+value_offset,value_header.data(),value_header.size());
          auto* value=reinterpret_cast<V*>(value_header.data());
          const uint64_t value_length=value->length;
          const uint64_t size=prefix_record_t::size(uint32_t(sizeof(K)+key_length),uint32_t(sizeof(V)+value_length));
          if(value_length>256*1024 || !size || record_at+size>std::min(end,record_page_end)) {malformed_record=true;break;}
          record_at+=size;
        }
        auto* marker=buffer.data()+offset+experimental::kPrefixBlock-PrefixMarkerSize();
        experimental::PrefixFooter footer;std::memcpy(&footer,marker+PrefixFooterOffset(),sizeof(footer));
        auto* record=reinterpret_cast<prefix_record_t*>(marker);
        const bool framed=record_at==marker_at && record->key().length==0 && record->header.invalid &&
          record->value().length==sizeof(footer) && record->value().reserved==experimental::kPrefixTag;
        const bool recognized=framed || (malformed_record && footer.magic==experimental::kPrefixMagic &&
          footer.end_magic==experimental::kPrefixEndMagic && footer.footer_crc==experimental::PrefixFooterCRC(footer));
        if(!recognized) {crc=rocksdb::crc32c::Extend(crc,reinterpret_cast<const char*>(buffer.data()+offset),experimental::kPrefixBlock);continue;}
        if(malformed_record)throw std::runtime_error("malformed data record inside committed prefix: record="+std::to_string(record_at)+
          " page="+std::to_string(page_at)+" bytes="+std::to_string(done)+" marker="+std::to_string(marker_at));
        if(footer.magic!=experimental::kPrefixMagic||footer.end_magic!=experimental::kPrefixEndMagic||
           footer.footer_crc!=experimental::PrefixFooterCRC(footer)||footer.from!=stable||footer.end!=sector_end||
           footer.first!=serial+1||footer.last<footer.first||footer.operations!=footer.last-footer.first+1||
           record->key().length!=0||!record->header.invalid||record->value().length!=sizeof(footer)||record->value().reserved!=experimental::kPrefixTag)
          throw std::runtime_error("corrupt native commit footer/chain");
        crc=rocksdb::crc32c::Extend(crc,reinterpret_cast<const char*>(buffer.data()+offset),experimental::kPrefixBlock-PrefixMarkerSize());
        if(crc!=footer.data_crc)throw std::runtime_error("native committed data checksum mismatch");
        stable=sector_end;serial=footer.last;record_at=sector_end;crc=0;++s.prefix.recovered_commits;
      }
    }
    s.prefix.base_tail=base_tail;s.prefix.stable_tail=stable;s.serial=serial;
    s.prefix.prepared_tail=stable;
  }
  Status PrefixRecover(const Guid& base,uint32_t& version) {
    auto& s=*sync_state_;std::vector<Guid> sessions;uint32_t cold_version=0;
    Status result=hot_store.Recover(base,base,version,sessions,true,Address(s.prefix.stable_tail));
    if(result!=Status::Ok)return result;
    result=cold_store.Recover(base,base,cold_version,sessions);
    if(result!=Status::Ok)return result;
    // Remove only the uncommitted suffix. Keep empty segment names intact:
    // the F2 file bundle may already hold descriptors for them.
    for(const auto& entry:std::filesystem::directory_iterator(s.root/"hot")) {
      const auto name=entry.path().filename().string();if(name.rfind("log.log",0)!=0)continue;
      const auto suffix=name.substr(7);if(suffix.empty()||suffix.find_first_not_of("0123456789")!=std::string::npos)continue;
      const uint64_t start=std::stoull(suffix)*experimental::kPrefixSegment;
      const uint64_t keep=s.prefix.stable_tail>start?std::min(experimental::kPrefixSegment,s.prefix.stable_tail-start):0;
      if(entry.file_size()>keep) {
        int fd=::open(entry.path().c_str(),O_RDWR|O_CLOEXEC);if(fd<0)throw std::runtime_error("open native tail for repair failed");
        if(::ftruncate(fd,off_t(keep))) {::close(fd);throw std::runtime_error("truncate native uncommitted suffix failed");}
        try{experimental::Sync(fd);}catch(...){::close(fd);throw;}::close(fd);
      }
      s.prefix.synced_files.insert(entry.path().string());
    }
    return Status::Ok;
  }
  void PrefixPrintCounters() const {
    const auto& p=sync_state_->prefix;
    std::fprintf(stderr,"F2_DATA_PREFIX commits=%llu data_syncs=%llu directory_syncs=%llu write_calls=%llu write_bytes=%llu stable_tail=%llu base_tail=%llu recovered_commits=%llu\n",
      (unsigned long long)p.commits,(unsigned long long)p.data_syncs,(unsigned long long)p.directory_syncs,
      (unsigned long long)experimental::prefix_write_calls.load(),(unsigned long long)experimental::prefix_write_bytes.load(),
      (unsigned long long)p.stable_tail,(unsigned long long)p.base_tail,(unsigned long long)p.recovered_commits);
    std::fprintf(stderr,"F2_PREFIX_INDEX cross_key_links=%llu\n",(unsigned long long)hot_store.prefix_cross_key_links_);
  }
