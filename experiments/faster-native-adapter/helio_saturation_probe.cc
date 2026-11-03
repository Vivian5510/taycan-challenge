#include "kv_engine.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
static void Check(const Status& s){if(!s.IsOk())throw std::runtime_error(s.ToString());}
static std::string Key(unsigned id){std::string s(32,'s');for(unsigned i=0;i<4;++i)s[i]=char(id>>(8*i));return s;}
int main(int argc,char** argv){try{
 if(argc!=2)throw std::runtime_error("data directory required");
 int release[2],full[2];if(::pipe(release)||::pipe(full))throw std::runtime_error("pipe failed");
 ::setenv("F2_HELIO_RELEASE_FD",std::to_string(release[0]).c_str(),1);
 ::setenv("F2_HELIO_FULL_FD",std::to_string(full[1]).c_str(),1);
 KVEngine* raw=nullptr;Check(KVEngine::Open(argv[1],4ULL<<30,&raw));std::unique_ptr<KVEngine> engine(raw);
 std::atomic<unsigned> ready{0},done{0},errors{0};std::atomic<bool> go{false};std::vector<std::thread> callers;
 for(unsigned id=0;id<384;++id)callers.emplace_back([&,id]{++ready;while(!go.load())std::this_thread::yield();
  if(!engine->Put(Key(id),std::string(256,char(id))).IsOk())++errors;++done;});
 while(ready.load()!=384)std::this_thread::yield();go=true;
 char byte=0;if(::read(full[0],&byte,1)!=1||byte!='F')throw std::runtime_error("full queue path not reached");
 if(done.load())throw std::runtime_error("consumer completed while test hold active");
 byte='G';if(::write(release[1],&byte,1)!=1)throw std::runtime_error("release failed");
 for(auto& caller:callers)caller.join();if(errors.load()||done.load()!=384)throw std::runtime_error("lost/failed caller");
 ::unsetenv("F2_HELIO_RELEASE_FD");::unsetenv("F2_HELIO_FULL_FD");
 for(int fd:{release[0],release[1],full[0],full[1]})::close(fd);
 for(unsigned pass=0;pass<3;++pass){
  for(unsigned id=0;id<384;++id){std::string v;Check(engine->Get(Key(id),&v));if(v!=std::string(256,char(id)))throw std::runtime_error("bad value");}
  if(pass!=2){engine.reset();raw=nullptr;Check(KVEngine::Open(argv[1],4ULL<<30,&raw));engine.reset(raw);}
 }
 std::cout<<"SATURATION PASS full_path=observed callers=384 held_acks=0 reopen=2"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
