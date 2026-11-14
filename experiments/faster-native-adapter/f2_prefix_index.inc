// Private to FasterKv in the native data-log build. Prepare append records
// without publishing their hash heads; publish only after the data is stable.
 private:
  struct PrefixIndexUpdate {HashBucketEntry before,after;};
  bool prefix_defer_index_=false;
  uint64_t prefix_cross_key_links_=0;
  std::unordered_map<AtomicHashBucketEntry*,PrefixIndexUpdate> prefix_index_updates_;
  void PrefixBeginIndex() {
    if(prefix_defer_index_ || !prefix_index_updates_.empty())throw std::runtime_error("nested prefix index preparation");
    prefix_defer_index_=true;
  }
  template<class C> void PrefixOverrideEntry(C& context) {
    if(!prefix_defer_index_)return;
    auto found=prefix_index_updates_.find(context.atomic_entry);
    if(found!=prefix_index_updates_.end())context.entry=found->second.after;
  }
  template<class C> Status PrefixStageEntry(ExecutionContext& execution,C& context,Address address) {
    if(!prefix_defer_index_)return hash_index_.TryUpdateEntry(execution,context,address);
    if(!hash_index_.IsSync() || !context.atomic_entry)throw std::runtime_error("prefix requires stable memory hash entry");
    HashBucketEntry after((context.entry.control_ & ~Address::kMaxAddress)|address.control());
    HashBucketEntry before=context.atomic_entry->load();
    auto inserted=prefix_index_updates_.emplace(context.atomic_entry,PrefixIndexUpdate{before,after});
    if(!inserted.second) {
      const auto prior=inserted.first->second.after.address();
      if(prior>=hlog.head_address.load() && !context.is_key_equal(reinterpret_cast<record_t*>(hlog.Get(prior))->key()))
        ++prefix_cross_key_links_;
    }
    inserted.first->second.after=after;
    return Status::Ok;
  }
  void PrefixPublishIndex() {
    for(auto& entry:prefix_index_updates_) {
      auto expected=entry.second.before;
      if(!entry.first->compare_exchange_strong(expected,entry.second.after))
        throw std::runtime_error("prefix index changed outside single-request commit");
    }
    prefix_index_updates_.clear();prefix_defer_index_=false;
  }
  void PrefixAbortIndex() {
    prefix_index_updates_.clear();prefix_defer_index_=false;
  }
