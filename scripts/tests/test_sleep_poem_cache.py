"""Run the firmware's actual streaming helper against failed/partial HTTP and SD writes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class SleepPoemCacheTest(unittest.TestCase):
    def test_streaming_is_bounded_atomic_optional_and_authenticated(self):
        source = (ROOT / 'src/activities/apps/standby/SmartStandby.cpp').read_text()
        helper = source[source.index('void sleepRasterPath('):source.index('bool cacheRasters(')]
        with tempfile.TemporaryDirectory(prefix='sleep-poem-cache-') as tmp:
            path = Path(tmp)
            harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
constexpr int O_WRONLY=1,O_CREAT=2,O_TRUNC=4;
#define LOG_INF(...)
uint32_t elapsed=0;
uint32_t millis(){return elapsed;}
struct File {
 std::string* bytes=nullptr; bool shortWrite=false;
 explicit operator bool()const{return bytes;}
 size_t size()const{return bytes->size();}
 size_t write(const uint8_t* p,size_t n){if(shortWrite)return 0;bytes->append((const char*)p,n);return n;}
};
struct Store {
 std::map<std::string,std::string> files;bool shortWrite=false,failReplace=false;
 File open(const char* p,int flags=0){if(flags){files[p].clear();return {&files[p],shortWrite};}
 auto it=files.find(p);return {it==files.end()?nullptr:&it->second,false};}
 bool replaceFile(const char* part,const char* target){if(failReplace)return false;files[target]=files[part];files.erase(part);return true;}
 void remove(const char* p){files.erase(p);}
} Storage;
enum class BmpReaderError {Ok,Invalid};
struct Bitmap {
 File& file;Bitmap(File& f,bool):file(f){}
 BmpReaderError parseHeaders(){return file.bytes->substr(0,2)=="BM"?BmpReaderError::Ok:BmpReaderError::Invalid;}
 int getWidth(){return 416;}int getHeight(){return 256;}bool is1Bit(){return true;}
};
struct Settings {char endpoint[257]="https://same.host/standby/v1/snapshot",token[257]="secret";};
struct Poem {char rasterId[25]="aaaaaaaaaaaaaaaaaaaaaaaa";};
struct Snapshot {Poem poems[1];int poemCount=1;};
namespace freeink {
struct SecureHttpClient {
 std::string url,authorization,payload="BM"+std::string(13372,'x');
 int status=200,requests=0;bool complete=true;uint32_t timeout=0;
 bool begin(const std::string& s){url=s;authorization.clear();return true;}
 void setTimeout(uint32_t t){timeout=t;}
 void addHeader(const std::string& k,const std::string& v){assert(k=="Authorization");authorization=v;}
 template<class Data,class Abort>int sendRequest(const char*,void*,size_t,Data data,Abort abort){
 ++requests;for(size_t pos=0;pos<payload.size();pos+=1024){if(abort()||!data((const uint8_t*)payload.data()+pos,std::min(size_t(1024),payload.size()-pos)))return -1;}return status;
 }
 bool responseComplete(){return complete;}
};
}
'''
            tests = r'''
int main(){
 Settings settings;Snapshot snapshot;std::atomic<bool> cancel{false};
 constexpr const char* target="/.crosspoint/standby/sleep1-aaaaaaaaaaaaaaaaaaaaaaaa.bmp";
 const std::string part=std::string(target)+".part";
 freeink::SecureHttpClient client;
 cacheSleepTiles(client,settings,snapshot,cancel,0);
 assert(Storage.files[target].size()==13374 && !Storage.files.count(part));
 assert(client.url=="https://same.host/standby/v1/poem/aaaaaaaaaaaaaaaaaaaaaaaa/sleep.bmp");
 assert(client.authorization=="Bearer secret" && client.timeout<=5000);
 cacheSleepTiles(client,settings,snapshot,cancel,0);assert(client.requests==1);
 for(int scenario=0;scenario<6;++scenario){
  Storage.files[target]="old corrupt cache";Storage.shortWrite=false;Storage.failReplace=false;
  client.status=200;client.complete=true;client.payload="BM"+std::string(13372,'x');
  if(scenario==0)client.payload.pop_back();
  if(scenario==1)client.payload.push_back('x');
  if(scenario==2)client.complete=false;
  if(scenario==3)client.status=404;
  if(scenario==4)Storage.shortWrite=true;
  if(scenario==5)Storage.failReplace=true;
  cacheSleepTiles(client,settings,snapshot,cancel,0);
  assert(Storage.files[target]=="old corrupt cache" && !Storage.files.count(part));
 }
 Storage.failReplace=false;Storage.files.clear();int previous=client.requests;
 cancel=true;cacheSleepTiles(client,settings,snapshot,cancel,0);assert(client.requests==previous && Storage.files.empty());
 cancel=false;elapsed=30000;cacheSleepTiles(client,settings,snapshot,cancel,0);assert(client.requests==previous && Storage.files.empty());
 elapsed=0;strcpy(settings.endpoint,"https://other.host/custom");cacheSleepTiles(client,settings,snapshot,cancel,0);assert(client.requests==previous);
}
'''
            cpp = path / 'test.cpp'
            cpp.write_text(harness + helper + tests)
            binary = path / 'test'
            subprocess.run(['c++', '-std=c++17', str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
