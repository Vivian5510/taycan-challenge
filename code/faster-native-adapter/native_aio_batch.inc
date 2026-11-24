// Experimental batch completion drain. Same submission and error semantics as
// native_aio_file.inc; callbacks always execute after releasing handler mutex.
//
// Reentry contract: collect up to F2_AIO_COMPLETION_BATCH ready jobs in one
// locked pass (amortising poll overhead), but only execute one callback per
// outer TryComplete() invocation.  Jobs [1..N-1] are pushed back to the front
// of aio_jobs_ — marked inline_completed so the dequeue check sees them as
// ready — before the lock is released.  A callback that re-enters TryComplete()
// while waiting for a sibling will find those siblings in aio_jobs_ and can
// dispatch them.  Executing the whole batch before any re-entry would hide
// siblings from a waiting callback, forming a dependency cycle (confirmed by
// completion_reentry_probe, 2026-11-14).
bool QueueIoHandler::TryComplete() {
  std::array<std::unique_ptr<AioJob>,F2_AIO_COMPLETION_BATCH> completed;
  size_t count=0;
  bool progressed=false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++aio_completion_polls_;
    SubmitQueuedLocked();
    for(auto it=aio_jobs_.begin();it!=aio_jobs_.end() && count<completed.size();) {
      auto& job=**it;
      if(!job.error && !job.submitted && !job.inline_completed){++it;continue;}
      if(job.submitted) {
        const int error=::aio_error(&job.control);
        if(error==EINPROGRESS){++it;continue;}
        const ssize_t bytes=::aio_return(&job.control);
        job.submitted=false;--aio_inflight_;progressed=true;
        if(error || bytes<0)job.error=error?error:EIO;
        else {
          job.done+=uint32_t(bytes);
          if(bytes>0 && job.done<job.length){++it;continue;}
          if(!job.read && job.done!=job.length)job.error=EIO;
        }
      }
      completed[count++]=std::move(*it);
      it=aio_jobs_.erase(it);
    }
    // Return completed[1..count-1] to the front of aio_jobs_ so that re-entrant
    // TryComplete() calls can find and dispatch them.  Set inline_completed so
    // the dequeue check above treats them as immediately ready (their I/O is
    // done; only the callback is deferred).  Insertion order: push in reverse
    // so completed[1] ends up at the front.
    for(size_t i=count;i>1;--i){
      completed[i-1]->inline_completed=true;
      aio_jobs_.push_front(std::move(completed[i-1]));
    }
    SubmitQueuedLocked();
    if(count>0)++aio_completion_callbacks_;
    if(count>1)++aio_multi_completion_polls_;
  }
  if(count==0)return progressed;
  {
    auto& job=*completed[0];
    auto status=job.error?Status::IOError:Status::Ok;
#ifdef F2_COMMIT_EXPERIMENT
    if(!job.read){++core::experimental::write_calls;core::experimental::write_bytes+=job.done;
#ifdef F2_DATA_PREFIX
      if(job.path.find("/hot/log.log")!=std::string::npos){++core::experimental::prefix_write_calls;core::experimental::prefix_write_bytes+=job.done;}
#endif
    }
#endif
    if(!job.read && status==Status::Ok) {
#ifdef F2_BATCHED_FILE_SYNC
#ifdef F2_DATA_PREFIX
      if(!core::experimental::MarkDirtyDataDescriptor(job.path,job.fd))status=Status::IOError;
#endif
      core::experimental::MarkDirty(job.path);
#else
#ifdef F2_COMMIT_EXPERIMENT
      try{core::experimental::Sync(job.fd);}catch(...){status=Status::IOError;}
#else
#ifdef F2_MAC_STANDARD_SYNC
      if(::fsync(job.fd)<0)status=Status::IOError;
#else
      if(::fcntl(job.fd,F_FULLFSYNC)<0)status=Status::IOError;
#endif
#endif
#endif
    }
#ifdef F2_BATCHED_FILE_SYNC
    if(status!=Status::Ok)core::experimental::MarkIoFailure(job.path);
#endif
#ifdef F2_NATIVE_IO_PROFILE
    if(!job.read)++write_completions_;
#endif
    job.callback(job.context,status,job.done);
  }
  return count || progressed;
}
