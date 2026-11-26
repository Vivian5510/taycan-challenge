// Private to FasterKv in synchronous durable builds. Prepare append records
// without publishing their hash heads; publish after data-log or WAL is stable.
 private:
  struct PrefixIndexUpdate {
    AtomicHashBucketEntry* key;
    HashBucketEntry before,after;
  };
  bool prefix_defer_index_=false;
  uint64_t prefix_cross_key_links_=0;
  // A commit group is small and bounded by the caller count. Reuse the
  // backing array and a fixed open-addressing table between groups so every
  // durable Put does not allocate or linearly scan temporary index state.
  std::vector<PrefixIndexUpdate> prefix_index_updates_;
  std::vector<uint16_t> prefix_index_slots_=std::vector<uint16_t>(256,0xffff);
  static size_t PrefixIndexHash(AtomicHashBucketEntry* key) {
    return (reinterpret_cast<uintptr_t>(key)>>4)&255;
  }
  uint16_t PrefixIndexSlot(AtomicHashBucketEntry* key) const {
    size_t slot=PrefixIndexHash(key);
    for(size_t probes=0;probes<prefix_index_slots_.size();++probes) {
      const uint16_t index=prefix_index_slots_[slot];
      if(index==0xffff || prefix_index_updates_[index].key==key)return index;
      slot=(slot+1)&255;
    }
    throw std::runtime_error("prefix index update table exhausted");
  }
  void PrefixBeginIndex() {
    if(prefix_defer_index_ || !prefix_index_updates_.empty())throw std::runtime_error("nested prefix index preparation");
    if(prefix_index_updates_.capacity()<64)prefix_index_updates_.reserve(64);
    std::fill(prefix_index_slots_.begin(),prefix_index_slots_.end(),uint16_t{0xffff});
    prefix_defer_index_=true;
  }
  template<class C> void PrefixOverrideEntry(C& context) {
    if(!prefix_defer_index_)return;
    const uint16_t index=PrefixIndexSlot(context.atomic_entry);
    if(index!=0xffff)context.entry=prefix_index_updates_[index].after;
  }
  template<class C> Status PrefixStageEntry(ExecutionContext& execution,C& context,Address address) {
    if(!prefix_defer_index_)return hash_index_.TryUpdateEntry(execution,context,address);
    if(!hash_index_.IsSync() || !context.atomic_entry)throw std::runtime_error("prefix requires stable memory hash entry");
    HashBucketEntry after((context.entry.control_ & ~Address::kMaxAddress)|address.control());
    HashBucketEntry before=context.atomic_entry->load();
    const size_t hash=PrefixIndexHash(context.atomic_entry);
    size_t slot=hash;
    for(size_t probes=0;probes<prefix_index_slots_.size();++probes) {
      const uint16_t index=prefix_index_slots_[slot];
      if(index==0xffff) {
        if(prefix_index_updates_.size()>=0xffff)throw std::runtime_error("prefix index update table exhausted");
        prefix_index_slots_[slot]=uint16_t(prefix_index_updates_.size());
        prefix_index_updates_.push_back(PrefixIndexUpdate{context.atomic_entry,before,after});
        return Status::Ok;
      }
      if(prefix_index_updates_[index].key==context.atomic_entry) {
        auto& found=prefix_index_updates_[index];
        const auto prior=found.after.address();
        if(prior>=hlog.head_address.load() && !context.is_key_equal(reinterpret_cast<record_t*>(hlog.Get(prior))->key()))
          ++prefix_cross_key_links_;
        found.after=after;
        return Status::Ok;
      }
      slot=(slot+1)&255;
    }
    throw std::runtime_error("prefix index update table exhausted");
  }
  void PrefixPublishIndex() {
    for(auto& entry:prefix_index_updates_) {
      auto expected=entry.before;
      if(!entry.key->compare_exchange_strong(expected,entry.after))
        throw std::runtime_error("prefix index changed outside single-request commit");
    }
    prefix_index_updates_.clear();prefix_defer_index_=false;
  }
  void PrefixAbortIndex() {
    prefix_index_updates_.clear();prefix_defer_index_=false;
  }
