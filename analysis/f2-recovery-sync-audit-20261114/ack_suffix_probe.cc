#include "kv_engine.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
std::string Key(unsigned id){std::string s(32,'k');for(unsigned i=0;i<4;++i)s[i]=char(id>>(8*i));return s;}
std::string Value(unsigned id){std::string s(256,'v');s[0]=char(id);return s;}
void Check(const Status& s){if(!s.IsOk())throw std::runtime_error(s.ToString());}
int main(int argc,char** argv){try{
 if(argc<3)return 2;KVEngine* raw=nullptr;Check(KVEngine::Open(argv[2],4ULL<<30,&raw));std::unique_ptr<KVEngine> db(raw);
 if(std::string(argv[1])=="write"){
  for(unsigned i=0;i<64;++i)Check(db->Put(Key(i),Value(i)));
  for(unsigned i=0;i<64;i+=7)Check(db->Delete(Key(i)));
  std::cout<<"ACK_PREFIX 74"<<std::endl;
  if(const char* arm=std::getenv("SYNC_AUDIT_ARM_PATH")){
    char signal;if(!std::cin.get(signal))throw std::runtime_error("missing controller release");
    FILE* f=std::fopen(arm,"w");if(!f)throw std::runtime_error("arm create");std::fclose(f);
  }
  if(!std::getenv("SYNC_AUDIT_ARM_PATH"))::setenv("F2_SYNC_FAULT","before-data-sync",1);::setenv("F2_SYNC_FAULT_MIN_SERIAL","75",1);
  Check(db->Put(Key(1),std::string(100*1024-1,'n')));return 3;
 }
 bool pending=false;
 for(unsigned i=0;i<64;++i){std::string v;auto s=db->Get(Key(i),&v);
  if(i%7==0){if(!s.IsNotFound())throw std::runtime_error("deleted ACK resurrected");}
  else{Check(s);if(i==1&&v==std::string(100*1024-1,'n'))pending=true;else if(v!=Value(i))throw std::runtime_error("acknowledged suffix value lost");}}
 std::cout<<"VERIFIED pending="<<pending<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
