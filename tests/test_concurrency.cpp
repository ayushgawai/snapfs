#include "snapfs/snapfs.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <unistd.h>
using namespace snapfs;
int main(){
 char img[]="/tmp/snapfs_conc_XXXXXX"; close(mkstemp(img)); assert(FileSystem::mkfs(img,128));
 FileSystem fs; assert(fs.mount(img)); // background CP thread on
 std::vector<std::thread> ts;
 for(int t=0;t<8;t++) ts.emplace_back([&,t]{
   std::vector<uint8_t> d(4096,(uint8_t)t);
   for(int i=0;i<50;i++){ fs.pwrite_file("w"+std::to_string(t), (uint64_t)(i%4)*4096, d.data(), d.size()); fs.pwrite_file("shared", (uint64_t)t*4096, d.data(), d.size()); }
 });
 for(auto&th:ts) th.join();
 assert(fs.consistency_point()); assert(fs.fsck(true));
 for(int t=0;t<8;t++){ auto v=fs.read_file("w"+std::to_string(t)); assert(v); for(size_t i=0;i<v->size();i++) assert((*v)[i]==(uint8_t)t); }
 auto s=fs.read_file("shared"); assert(s && s->size()==32768); for(int t=0;t<8;t++) for(int i=0;i<4096;i++) assert((*s)[t*4096+i]==(uint8_t)t);
 fs.unmount(); unlink(img); printf("concurrency ok\n"); return 0;
}
