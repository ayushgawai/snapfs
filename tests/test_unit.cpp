#include "snapfs/snapfs.hpp"
#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
using namespace snapfs;
static std::vector<uint8_t> pat(size_t n, uint8_t s){ std::vector<uint8_t> v(n); for(size_t i=0;i<n;i++) v[i]=(uint8_t)(s+i*7); return v; }
int main(){
 char img[]="/tmp/snapfs_unit_XXXXXX"; close(mkstemp(img));
 assert(FileSystem::mkfs(img, 64));
 { FileSystem fs(false); assert(fs.mount(img));
   // allocator + tree: small, multi-block, overwrite (COW), sparse-ish offset write
   assert(fs.write_file("a", pat(100,1))); assert(fs.read_file("a")==pat(100,1));
   assert(fs.write_file("big", pat(200000,3))); assert(fs.read_file("big")==pat(200000,3));
   assert(fs.write_file("a", pat(5000,9))); assert(fs.read_file("a")==pat(5000,9)); // overwrite never in place
   assert(fs.consistency_point()); assert(fs.fsck(true));
   // snapshot isolation
   assert(fs.snap_create("s1")); assert(fs.write_file("a", pat(5000,42)));
   assert(fs.read_snapshot_file("s1","a")==pat(5000,9)); assert(fs.read_file("a")==pat(5000,42));
   auto d=fs.snap_diff("s1","live"); assert(!d.empty());
   assert(fs.snap_delete("s1")); assert(fs.fsck(true));
   fs.unmount();
 }
 { FileSystem fs(false); assert(fs.mount(img)); assert(fs.read_file("a")==pat(5000,42)); assert(fs.fsck(true)); fs.unmount(); }
 // log replay: child writes then _exit without CP/unmount; parent must replay intent log
 char img2[]="/tmp/snapfs_log_XXXXXX"; close(mkstemp(img2)); assert(FileSystem::mkfs(img2,64));
 pid_t p=fork(); assert(p>=0);
 if(p==0){ FileSystem fs(false); if(!fs.mount(img2)) _exit(2); fs.write_file("logged", pat(8192,77)); _exit(0); } // no CP, no unmount
 int st; waitpid(p,&st,0);
 { FileSystem fs(false); assert(fs.mount(img2)); auto v=fs.read_file("logged"); assert(v==pat(8192,77)); assert(fs.fsck(true)); fs.unmount(); }
 unlink(img); unlink(img2); printf("unit ok\n"); return 0;
}
