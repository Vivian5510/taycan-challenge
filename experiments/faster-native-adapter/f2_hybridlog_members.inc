// Quiescent committed-prefix protocol. SyncMutate admits one request
// at a time. No new mutation enters until its hot HybridLog prefix is
// stable and its recovery descriptor is published. No additional WAL.
 public:
  Guid HybridLogCommit(uint64_t required) {
    if(compaction_config_.hot_store.enabled || compaction_config_.cold_store.enabled || hot_store.UseReadCache())
      throw std::runtime_error("hybrid prefix requires compaction/read-cache disabled");
    experimental::waiter={};Guid token;
    if(!hot_store.CheckpointHybridLog(experimental::CheckpointDone,token))
      throw std::runtime_error("hot HybridLog commit rejected");
    while(!experimental::waiter.done || hot_store.system_state_.phase()!=Phase::REST) {
      hot_store.CompletePending(false);cold_store.CompletePending(false);
      std::this_thread::yield();
    }
    if(experimental::waiter.status!=Status::Ok || experimental::waiter.serial<required)
      throw std::runtime_error("hot HybridLog did not persist required serial");
    return token;
  }
  Status HybridLogRecover(const Guid& base,const Guid& hot_token,uint32_t& version) {
    if(compaction_config_.hot_store.enabled || compaction_config_.cold_store.enabled)
      return Status::Aborted;
    std::vector<Guid> sessions;uint32_t cold_version=0;
    Status result=hot_store.Recover(base,hot_token,version,sessions,true);
    if(result!=Status::Ok)return result;
    // No migration is permitted between base and hot_token, so the cold store
    // and its disk index are exactly those of the durable full base.
    result=cold_store.Recover(base,base,cold_version,sessions);
    return result;
  }
