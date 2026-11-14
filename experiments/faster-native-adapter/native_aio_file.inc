// Inside FASTER::environment. OS asynchronous I/O; callbacks stay on F2 pollers.
void QueueIoHandler::SubmitQueuedLocked() {
  for(auto& pointer:aio_jobs_) {
    auto& job=*pointer;
    if(aio_inflight_>=8)break;
    if(job.submitted || job.inline_completed || job.error)continue;
    job.control={};job.control.aio_fildes=job.fd;
    job.control.aio_offset=off_t(job.offset+job.done);
    job.control.aio_buf=job.buffer+job.done;
    job.control.aio_nbytes=job.length-job.done;
    job.control.aio_sigevent.sigev_notify=SIGEV_NONE;
    int result=job.read ? ::aio_read(&job.control) : ::aio_write(&job.control);
    if(result==0){job.submitted=true;++aio_inflight_;++aio_submissions_;aio_peak_=std::max(aio_peak_,aio_inflight_);}
    else if(errno==EAGAIN || errno==EINTR){++aio_retries_;break;}
    else job.error=errno?errno:EIO;
  }
}
core::Status QueueIoHandler::Schedule(bool read,int fd,uint64_t offset,uint32_t length,
    uint8_t* buffer,core::IAsyncContext& context,core::AsyncIOCallback callback,const std::string& path) {
  auto job=std::make_unique<AioJob>();
  // Stable descriptor and aiocb storage until aio_return. F2 keeps its buffer
  // alive until our callback; no per-request payload copy is introduced.
  job->fd=::fcntl(fd,F_DUPFD_CLOEXEC,0);if(job->fd<0)return Status::IOError;
  auto result=context.DeepCopy(job->context);if(result!=Status::Ok)return result;
  job->read=read;job->offset=offset;job->length=length;job->buffer=buffer;job->callback=callback;job->path=path;
#ifdef F2_BATCHED_FILE_SYNC
  if(!read && std::getenv("F2_SYNC_FAIL_WRITE"))job->error=EIO;
#endif
#ifdef F2_NATIVE_INLINE_HOT_WRITES
  // The caller immediately waits for these small commit writes. Complete the
  // pwrite here instead of submitting to another OS worker, but keep the F2
  // callback deferred to TryComplete: Schedule must not re-enter the allocator.
  // Large flushes, reads, cold data and disk-index I/O keep the AIO path.
  if(!read && !job->error && length<=4096 && path.find("/hot/log.log")!=std::string::npos) {
    while(job->done<length) {
      const ssize_t bytes=::pwrite(job->fd,buffer+job->done,length-job->done,off_t(offset+job->done));
      if(bytes<0 && errno==EINTR)continue;
      if(bytes<=0){job->error=bytes<0?errno:EIO;break;}
      job->done+=uint32_t(bytes);
    }
    job->inline_completed=true;
  }
#endif
  std::lock_guard<std::mutex> lock(mutex_);
  aio_jobs_.push_back(std::move(job));SubmitQueuedLocked();return Status::Ok;
}
#if F2_AIO_COMPLETION_BATCH > 1
#include "native_aio_batch.inc"
#else
bool QueueIoHandler::TryComplete() {
  std::unique_ptr<AioJob> completed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    SubmitQueuedLocked();
    for(auto it=aio_jobs_.begin();it!=aio_jobs_.end();++it) {
      auto& job=**it;
      if(!job.error && !job.submitted && !job.inline_completed)continue;
      if(job.submitted) {
        const int error=::aio_error(&job.control);if(error==EINPROGRESS)continue;
        const ssize_t bytes=::aio_return(&job.control);
        job.submitted=false;--aio_inflight_;
        if(error || bytes<0)job.error=error?error:EIO;
        else {
          job.done+=uint32_t(bytes);
          if(bytes>0 && job.done<job.length){SubmitQueuedLocked();return true;}
          if(!job.read && job.done!=job.length)job.error=EIO;
        }
      }
      completed=std::move(*it);aio_jobs_.erase(it);SubmitQueuedLocked();break;
    }
  }
  if(!completed)return false;
  auto& job=*completed;
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
  job.callback(job.context,status,job.done);return true;
}
#endif
