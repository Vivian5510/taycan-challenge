#include "kv_engine.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <unistd.h>
#ifdef F2_DATA_PREFIX_TEST
extern "C" uint64_t F2PrefixHashFingerprint(const char*,size_t,uint64_t);
#endif

std::string Hex(const std::string& s) {
  static const char chars[]="0123456789abcdef";std::string result;
  for(unsigned char c:s){result+=chars[c>>4];result+=chars[c&15];}return result;
}
void Check(const Status& s) {if(!s.IsOk())throw std::runtime_error(s.ToString());}
int main(int argc,char** argv) {
  try {
    if(argc<3)throw std::runtime_error("phase directory [expected]");
    uint64_t budget=4ULL<<30;
    if(const char* bytes=std::getenv("F2_PROBE_BUDGET_BYTES"))budget=std::stoull(bytes);
    KVEngine* raw=nullptr;Check(KVEngine::Open(argv[2],budget,&raw));std::unique_ptr<KVEngine> engine(raw);
    const std::string key(32,'k');
    const std::string phase=argv[1];
    if(phase=="embedded-marker-write-kill" || phase=="embedded-marker-verify") {
      std::string value(100*1024-1,'x');
      auto encode=[&](size_t offset,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i)value.at(offset+i)=char(n>>(8*i));};
      // Actual data is a 32-byte key, hence 52 bytes before value bytes in F2.
      for(size_t marker=512-88-52;marker+88<value.size();marker+=512) {
        encode(marker,1ULL|(1ULL<<61)|(1ULL<<63),8);encode(marker+8,0,4);
        encode(marker+12,64,4);encode(marker+16,0x31465850,4);
        encode(marker+20,0x31474f4c32464350ULL,8);encode(marker+20+56,0x454e443246435031ULL,8);
      }
      const std::string fake_key(32,'f');
      if(phase=="embedded-marker-write-kill") {
        Check(engine->Put(fake_key,value));std::cout<<"EMBEDDED_MARKER_ACK"<<std::endl;::kill(::getpid(),SIGKILL);
      } else {
        std::string actual;Check(engine->Get(fake_key,&actual));
        if(actual!=value)throw std::runtime_error("embedded marker damaged value or recovery");
        std::cout<<"EMBEDDED_MARKER_RECOVERY PASS"<<std::endl;
      }
#ifdef F2_DATA_PREFIX_TEST
    } else if(phase=="hash-collision") {
      auto collision_key=[](uint64_t id){std::string k(32,'c');for(unsigned i=0;i<8;++i)k[i]=char(id>>(8*i));return k;};
      uint64_t entries=4096;while(entries<budget/4096)entries*=2;
      std::unordered_map<uint64_t,uint64_t> seen;uint64_t left=0,right=0;
      for(uint64_t id=1;id<=1000000;++id) {
        auto k=collision_key(id);auto fp=F2PrefixHashFingerprint(k.data(),k.size(),entries);
        auto found=seen.emplace(fp,id);if(!found.second){left=found.first->second;right=id;break;}
      }
      if(!right)throw std::runtime_error("no real index collision found");
      const std::string keys[2]={collision_key(left),collision_key(right)};
      auto data=[](unsigned which,unsigned round){std::string v(256,char(round));v[0]=char(which);return v;};
      std::atomic<unsigned> ready{0},go{0},done{0};std::vector<std::thread> writers;
      for(unsigned thread=0;thread<16;++thread)writers.emplace_back([&,thread]{
        ++ready;for(unsigned round=1;round<=32;++round){while(go<round)std::this_thread::yield();Check(engine->Put(keys[thread%2],data(thread%2,round)));++done;}
      });
      while(ready!=16)std::this_thread::yield();
      for(unsigned round=1;round<=32;++round) {
        go=round;while(done<round*16)std::this_thread::yield();
        for(unsigned which=0;which<2;++which){std::string v;Check(engine->Get(keys[which],&v));if(v!=data(which,round))throw std::runtime_error("colliding hash chain lost value");}
      }
      for(auto& thread:writers)thread.join();
      for(unsigned reopen=0;reopen<2;++reopen){
        engine.reset();raw=nullptr;Check(KVEngine::Open(argv[2],budget,&raw));engine.reset(raw);
        for(unsigned which=0;which<2;++which){std::string v;Check(engine->Get(keys[which],&v));if(v!=data(which,32))throw std::runtime_error("colliding chain recovery lost value");}
      }
      std::cout<<"REAL_HASH_COLLISION PASS ids="<<left<<","<<right<<std::endl;
#endif
    } else if(phase=="group-serve" || phase=="group-verify") {
      auto group_key=[](unsigned id){std::string k(32,'g');for(unsigned i=0;i<4;++i)k[i]=char(id>>(8*i));return k;};
      auto group_value=[](unsigned id,unsigned version){
        std::string value(256,'\0');uint64_t state=uint64_t(id)*977+version;
        for(auto& byte:value){state=state*6364136223846793005ULL+1;byte=char(state>>56);}
        for(unsigned i=0;i<4;++i)value[i]=char(version>>(8*i));return value;
      };
      if(phase=="group-serve") {
        unsigned count=argc>3?std::stoul(argv[3]):16;
        std::atomic<unsigned> ready{0};std::atomic<bool> start{false};std::vector<std::thread> writers;
        for(unsigned id=0;id<count;++id)writers.emplace_back([&,id]{
          ++ready;while(!start.load())std::this_thread::yield();
          for(unsigned version=1;version<=256;++version){
            Check(version%5?engine->Put(group_key(id),group_value(id,version)):engine->Delete(group_key(id)));
            const auto ack="ACK "+std::to_string(id)+" "+std::to_string(version)+"\n";
            ssize_t bytes;do{bytes=::write(STDOUT_FILENO,ack.data(),ack.size());}while(bytes<0&&errno==EINTR);
            if(bytes!=ssize_t(ack.size()))throw std::runtime_error("group ACK failed");
          }
        });
        while(ready!=count)std::this_thread::yield();start=true;
        for(auto& writer:writers)writer.join();
      } else {
        if(argc<4)throw std::runtime_error("missing acknowledged vector");
        std::vector<unsigned> acknowledged;std::stringstream input(argv[3]);std::string part;
        while(std::getline(input,part,','))acknowledged.push_back(std::stoul(part));
        const bool allow_next=argc>4&&std::string(argv[4])=="allow-next";
        std::cout<<"{\"recovered\":[";
        for(unsigned id=0;id<acknowledged.size();++id){
          unsigned version=acknowledged[id];std::string value;auto status=engine->Get(group_key(id),&value);
          if(status.IsNotFound()) {
            if(version%5==0){}
            else if(allow_next&&(version+1)%5==0)++version;
            else throw std::runtime_error("acknowledged group value lost");
          } else {
            Check(status);if(value.size()!=256)throw std::runtime_error("short group value");
            version=0;for(unsigned i=0;i<4;++i)version|=unsigned(uint8_t(value[i]))<<(8*i);
            if(version==0||version%5==0||value!=group_value(id,version))throw std::runtime_error("torn group value");
          }
          if(version!=acknowledged[id]&&(!allow_next||version!=acknowledged[id]+1))throw std::runtime_error("group prefix outside acknowledged/pending interval");
          if(id)std::cout<<",";std::cout<<version;
        }
        std::cout<<"]}"<<std::endl;
      }
    } else if(phase=="span-write-kill" || phase=="span-verify") {
      auto span_key=[](unsigned id){std::string k(32,'k');for(unsigned i=0;i<4;++i)k[i]=char(id>>(8*i));return k;};
      auto span_value=[](unsigned id,unsigned round){
        std::string v(64*1024,'\0');uint64_t state=id+uint64_t(round)*977;
        for(auto& byte:v){state=state*6364136223846793005ULL+1442695040888963407ULL;byte=char(state>>56);}return v;
      };
      if(phase=="span-write-kill") {
        unsigned acknowledged=0;
        for(unsigned round=0;round<2;++round)for(unsigned id=0;id<256;++id){Check(engine->Put(span_key(id),span_value(id,round)));++acknowledged;}
        for(unsigned id=0;id<256;id+=3){Check(engine->Delete(span_key(id)));++acknowledged;}
        for(unsigned id=1;id<256;id+=5){Check(engine->Put(span_key(id),"short"));++acknowledged;}
        std::cout<<"SPANNED_PREFIX_ACK "<<acknowledged<<std::endl;::kill(::getpid(),SIGKILL);
      } else {
        for(unsigned id=0;id<256;++id){
          std::string value;auto status=engine->Get(span_key(id),&value);
          if(id%5==1){Check(status);if(value!="short")throw std::runtime_error("short overwrite lost");}
          else if(id%3==0){if(!status.IsNotFound())throw std::runtime_error("span delete lost");}
          else {Check(status);if(value!=span_value(id,1))throw std::runtime_error("span value lost");}
        }
        std::cout<<"SPANNED_RECOVERY PASS"<<std::endl;
      }
    } else if(phase=="sync-error" || phase=="io-error" || phase=="apply-error") {
      const char* injection=phase=="apply-error"?"F2_SYNC_FAIL_APPLY":phase=="io-error"?"F2_SYNC_FAIL_WRITE":"F2_SYNC_FAIL_SYNC";
      ::setenv(injection,"1",1);
      std::atomic<unsigned> errors{0};std::vector<std::thread> writers;
      for(unsigned t=0;t<4;++t)writers.emplace_back([&]{if(!engine->Put(key,std::string(256,'x')).IsOk())++errors;});
      for(auto& t:writers)t.join();::unsetenv(injection);
      std::string output;
      if(errors!=4 || engine->Put(key,"later").IsOk() || engine->Get(key,&output).IsOk())
        throw std::runtime_error("failed sync did not poison all pending requests");
      std::cout<<"SYNC_ERROR_FAIL_CLOSED PASS"<<std::endl;
    } else if(std::string(argv[1])=="write-kill") {
      std::atomic<bool> failed{false};std::vector<std::thread> threads;
      for(unsigned t=0;t<4;++t)threads.emplace_back([&,t]{
        for(unsigned i=0;i<16;++i){
          std::string value(256,char(t*31+i));value[0]=char(t);value[1]=char(i);
          auto s=i%5==0?engine->Delete(key):engine->Put(key,value);
          if(!s.IsOk())failed=true;
        }
      });
      for(auto& thread:threads)thread.join();
      if(failed)throw std::runtime_error("concurrent mutation failed");
      // Cross-session happens-before: last completed operation must win.
      std::thread first([&]{Check(engine->Put(key,std::string(256,'A')));});first.join();
      std::thread second([&]{Check(engine->Delete(key));});second.join();
      std::thread third([&]{Check(engine->Put(key,std::string(256,'B')));});third.join();
      std::string value;Check(engine->Get(key,&value));
      if(value!=std::string(256,'B'))throw std::runtime_error("happens-before value lost");
      std::cout<<"EXPECTED "<<Hex(value)<<std::endl;
      ::kill(::getpid(),SIGKILL);
    } else {
      std::string value;Check(engine->Get(key,&value));
      if(argc!=4||Hex(value)!=argv[3])throw std::runtime_error("same-key recovered value differs");
      std::cout<<"SAME_KEY_RECOVERY PASS"<<std::endl;
    }
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
}
