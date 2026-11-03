// Private to FasterKv in the native data-log build. Prepare append records
// without publishing their hash heads; publish only after the data is stable.
 private:
  struct PrefixIndexUpdate {HashBucketEntry before,after;};
  using PrefixPreparedIndex=std::unordered_map<AtomicHashBucketEntry*,PrefixIndexUpdate>;
  bool prefix_defer_index_=false;
  uint64_t prefix_cross_key_links_=0;
  std::unordered_map<AtomicHashBucketEntry*,PrefixIndexUpdate> prefix_index_updates_;
#ifdef F2_PREFIX_PIPELINE
  std::unordered_map<AtomicHashBucketEntry*,HashBucketEntry> prefix_pending_heads_;
#endif
  void PrefixBeginIndex() {
    if(prefix_defer_index_ || !prefix_index_updates_.empty())throw std::runtime_error("nested prefix index preparation");
    prefix_defer_index_=true;
  }
  template<class C> void PrefixOverrideEntry(C& context) {
    if(!prefix_defer_index_)return;
#ifdef F2_PREFIX_PIPELINE
    auto found=prefix_pending_heads_.find(context.atomic_entry);
    if(found!=prefix_pending_heads_.end())context.entry=found->second;
#else
    auto found=prefix_index_updates_.find(context.atomic_entry);
    if(found!=prefix_index_updates_.end())context.entry=found->second.after;
#endif
  }
  template<class C> Status PrefixStageEntry(ExecutionContext& execution,C& context,Address address) {
    if(!prefix_defer_index_)return hash_index_.TryUpdateEntry(execution,context,address);
    if(!hash_index_.IsSync() || !context.atomic_entry)throw std::runtime_error("prefix requires stable memory hash entry");
    HashBucketEntry after((context.entry.control_ & ~Address::kMaxAddress)|address.control());
    HashBucketEntry before=context.atomic_entry->load();
#ifdef F2_PREFIX_PIPELINE
    auto pending=prefix_pending_heads_.find(context.atomic_entry);
    if(pending!=prefix_pending_heads_.end())before=pending->second;
#endif
    auto inserted=prefix_index_updates_.emplace(context.atomic_entry,PrefixIndexUpdate{before,after});
    if(!inserted.second) {
      const auto prior=inserted.first->second.after.address();
      if(prior>=hlog.head_address.load() && !context.is_key_equal(reinterpret_cast<record_t*>(hlog.Get(prior))->key()))
        ++prefix_cross_key_links_;
    }
    inserted.first->second.after=after;
#ifdef F2_PREFIX_PIPELINE
    prefix_pending_heads_.insert_or_assign(context.atomic_entry,after);
#endif
    return Status::Ok;
  }
  void PrefixPublishIndex() {
    for(auto& entry:prefix_index_updates_) {
      auto expected=entry.second.before;
      if(!entry.first->compare_exchange_strong(expected,entry.second.after))
        throw std::runtime_error("prefix index changed outside commit queue");
    }
    prefix_index_updates_.clear();prefix_defer_index_=false;
  }
#ifdef F2_PREFIX_PIPELINE
  PrefixPreparedIndex PrefixDetachIndex() {
    PrefixPreparedIndex prepared;prepared.swap(prefix_index_updates_);prefix_defer_index_=false;return prepared;
  }
  void PrefixPublishPreparedIndex(PrefixPreparedIndex& prepared) {
    for(auto& entry:prepared) {
      auto expected=entry.second.before;
      if(!entry.first->compare_exchange_strong(expected,entry.second.after))
        throw std::runtime_error("prepared prefix publication order changed");
      auto pending=prefix_pending_heads_.find(entry.first);
      if(pending!=prefix_pending_heads_.end() && pending->second.control_==entry.second.after.control_)
        prefix_pending_heads_.erase(pending);
    }
    prepared.clear();
  }
#endif
  void PrefixAbortIndex() {
    prefix_index_updates_.clear();prefix_defer_index_=false;
#ifdef F2_PREFIX_PIPELINE
    prefix_pending_heads_.clear();
#endif
  }
