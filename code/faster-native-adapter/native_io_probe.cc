// Native I/O semantics probe: execution before polling, stable buffers/FDs,
// bounded outstanding requests, exactly-once callbacks and short EOF reads.
#include "core/async.h"
#include "environment/native_file.h"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace core=FASTER::core;
namespace env=FASTER::environment;
struct Results {std::atomic<unsigned> callbacks{0},errors{0};std::atomic<size_t> bytes{0};};
struct Context:core::IAsyncContext {
  explicit Context(Results& results):results(&results){}
  core::Status DeepCopy_Internal(core::IAsyncContext*& copy) override {
    return core::IAsyncContext::DeepCopy_Internal(*this,copy);
  }
  Results* results;
};
void Complete(core::IAsyncContext* pointer,core::Status status,size_t bytes) {
  core::CallbackContext<Context> context(pointer);
  if(status!=core::Status::Ok)++context->results->errors;
  context->results->bytes+=bytes;++context->results->callbacks;
}
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv) {
  try {
    Require(argc==2,"directory required");std::filesystem::create_directories(argv[1]);
    const auto path=std::string(argv[1])+"/data";
    env::QueueIoHandler handler;env::QueueFile file(path);
    Require(file.Open(env::FileCreateDisposition::CreateOrTruncate,{false,false},&handler)==core::Status::Ok,"open");
    Results writes;std::vector<std::array<uint8_t,4096>> buffers(64);
    for(unsigned i=0;i<buffers.size();++i){buffers[i].fill(uint8_t(i+1));Context context(writes);
      Require(file.Write(i*4096,4096,buffers[i].data(),context,Complete)==core::Status::Ok,"schedule write");}
    Require(writes.callbacks==0,"callback ran outside completion poller");
    int raw=::open(path.c_str(),O_RDONLY);Require(raw>=0,"verify open");
    bool observed=false;std::array<uint8_t,4096> actual{};
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {
      observed=::pread(raw,actual.data(),actual.size(),0)==ssize_t(actual.size()) && actual==buffers[0];
      if(observed)break;
#ifdef F2_NATIVE_POSIX_AIO
      std::this_thread::yield();
#else
      break;
#endif
    } while(std::chrono::steady_clock::now()<deadline);
#ifdef F2_NATIVE_POSIX_AIO
    Require(observed,"OS I/O did not progress before polling");
    Require(file.Close()==core::Status::Ok,"close original descriptor");
    // Reuse the original descriptor number while requests remain outstanding.
    int replacement=::open("/dev/null",O_RDWR);Require(replacement>=0,"reuse descriptor");
#else
    Require(!observed,"inline backend unexpectedly wrote before polling");
#endif
    handler.Drain();
    Require(writes.callbacks==buffers.size() && writes.errors==0 && writes.bytes==buffers.size()*4096,"write completion counts");
    for(unsigned i=0;i<buffers.size();++i)
      Require(::pread(raw,actual.data(),actual.size(),i*4096)==ssize_t(actual.size()) && actual==buffers[i],"buffer or descriptor lifetime");
#ifdef F2_NATIVE_POSIX_AIO
    ::close(replacement);
#else
    file.Close();
#endif
    env::QueueFile reader(path);Require(reader.Open(env::FileCreateDisposition::OpenExisting,{false,false},&handler)==core::Status::Ok,"read open");
    Results reads;Context context(reads);std::array<uint8_t,4096> tail{};
    Require(reader.Read(buffers.size()*4096-517,4096,tail.data(),context,Complete)==core::Status::Ok,"schedule short read");
    handler.Drain();
    Require(reads.callbacks==1 && reads.errors==0 && reads.bytes==517,"short EOF completion");
    for(unsigned i=0;i<517;++i)Require(tail[i]==64,"short read bytes");
    reader.Close();::close(raw);
    std::cout<<"NATIVE_IO_SEMANTICS PASS writes=64 short_read=517 callbacks_once=1 before_poll="<<observed<<std::endl;
    return 0;
  } catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<std::endl;return 1;}
}
