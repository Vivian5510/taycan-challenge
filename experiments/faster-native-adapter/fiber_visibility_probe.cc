#include "kv_engine.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

static void Check(const Status& s) {if(!s.IsOk())throw std::runtime_error(s.ToString());}
static std::string Key(unsigned id) {auto s=std::string(32,'k');s[0]=char(id);return s;}
int main(int argc,char** argv) {
  try {
    if(argc!=2)throw std::runtime_error("data directory required");
    KVEngine* raw=nullptr;Check(KVEngine::Open(argv[1],4ULL<<30,&raw));std::unique_ptr<KVEngine> engine(raw);
    const std::string old(256,'o'),updated(256,'n');
    for(unsigned id=0;id<64;++id)Check(engine->Put(Key(id),old));
    int release[2],notice[2];if(::pipe(release)||::pipe(notice))throw std::runtime_error("pipe failed");
    ::setenv("F2_FIBER_TEST_RELEASE_FD",std::to_string(release[0]).c_str(),1);
    ::setenv("F2_FIBER_TEST_NOTICE_FD",std::to_string(notice[1]).c_str(),1);
    std::atomic<unsigned> ready{0},done{0},errors{0};std::atomic<bool> go{false};
    std::vector<std::thread> writers;
    for(unsigned id=0;id<32;++id)writers.emplace_back([&,id]{
      ++ready;while(!go.load())std::this_thread::yield();
      const auto s=id%3?engine->Put(Key(id),updated):engine->Delete(Key(id));
      if(!s.IsOk())++errors;++done;
    });
    while(ready!=32)std::this_thread::yield();go=true;
    char byte=0;if(::read(notice[0],&byte,1)!=1||byte!='R')throw std::runtime_error("missing sync fence");
    // These reads must complete while the writer fiber is held before fsync.
    for(unsigned id=0;id<64;++id){std::string v;Check(engine->Get(Key(id),&v));if(v!=old)throw std::runtime_error("uncommitted value visible");}
    if(done.load())throw std::runtime_error("write ACK before durable sync");
    byte='G';if(::write(release[1],&byte,1)!=1)throw std::runtime_error("release failed");
    for(auto& writer:writers)writer.join();if(errors.load())throw std::runtime_error("write failed");
    ::unsetenv("F2_FIBER_TEST_RELEASE_FD");::unsetenv("F2_FIBER_TEST_NOTICE_FD");
    for(int fd:{release[0],release[1],notice[0],notice[1]})::close(fd);
    for(unsigned reopen=0;reopen<3;++reopen) {
      for(unsigned id=0;id<64;++id) {
        std::string v;auto s=engine->Get(Key(id),&v);
        if(id<32 && id%3==0){if(!s.IsNotFound())throw std::runtime_error("delete lost");}
        else {Check(s);if(v!=(id<32?updated:old))throw std::runtime_error("update lost");}
      }
      if(reopen!=2){engine.reset();raw=nullptr;Check(KVEngine::Open(argv[1],4ULL<<30,&raw));engine.reset(raw);}
    }
    std::cout<<"FIBER_VISIBILITY PASS held_reads=64 uncommitted_acks=0 writers=32 reopen=2"<<std::endl;
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
