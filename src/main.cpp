#include "snapfs/snapfs.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <chrono>
#include <random>
using namespace snapfs;
static std::vector<uint8_t> slurp(const std::string& p){std::ifstream f(p,std::ios::binary);return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());}
static bool open_fs(FileSystem& fs,const char* img){ if(!fs.mount(img)){fprintf(stderr,"cannot mount %s\n",img); return false;} return true; }
int main(int argc,char**argv){
 if(argc<3){fprintf(stderr,"usage: snapfsctl <img> <cmd> [args]\n mkfs <sizeMB> | write <name> <hostfile> | read <name> [hostfile] | ls | rm <name> | snap create|list|delete|diff ... | fsck | bench\n");return 1;}
 std::string img=argv[1], cmd=argv[2];
 if(cmd=="mkfs"){ return FileSystem::mkfs(img, std::stoull(argv[3]))?0:1; }
 FileSystem fs; if(!open_fs(fs,img.c_str())) return 1;
 if(cmd=="write"){ auto d=slurp(argv[4]); bool ok=fs.write_file(argv[3],d); fs.consistency_point(); return ok?0:1; }
 if(cmd=="read"){ auto d=fs.read_file(argv[3]); if(!d) return 1; if(argc>4){std::ofstream o(argv[4],std::ios::binary); o.write((char*)d->data(),d->size());} else fwrite(d->data(),1,d->size(),stdout); return 0; }
 if(cmd=="ls"){ for(auto&f:fs.list()) printf("%s %llu\n",f.name.c_str(),(unsigned long long)f.size); return 0; }
 if(cmd=="rm"){ bool ok=fs.remove_file(argv[3]); fs.consistency_point(); return ok?0:1; }
 if(cmd=="cp"){ return fs.consistency_point()?0:1; }
 if(cmd=="snap"&&argc>=4){ std::string s=argv[3];
   if(s=="create") return fs.snap_create(argv[4])?0:1;
   if(s=="list"){ for(auto&x:fs.snap_list()) printf("%s cp=%llu root=%u\n",x.name.c_str(),(unsigned long long)x.cp_id,x.root_block); return 0; }
   if(s=="delete") return fs.snap_delete(argv[4])?0:1;
   if(s=="read"){ auto d=fs.read_snapshot_file(argv[4],argv[5]); if(!d) return 1; fwrite(d->data(),1,d->size(),stdout); return 0; }
   if(s=="ls"){ for(auto&f:fs.list_snapshot(argv[4])) printf("%s %llu\n",f.name.c_str(),(unsigned long long)f.size); return 0; }
   if(s=="diff"){ auto ch=fs.snap_diff(argv[4],argv[5]); for(auto&c:ch) printf("%s %u %u->%u\n",c.file.c_str(),c.logical,c.old_block,c.new_block); printf("changed_blocks=%zu\n",ch.size()); return 0; }
 }
 if(cmd=="fsck"){ bool ok=fs.fsck(true); printf("fsck: %s\n",ok?"OK":"FAIL"); return ok?0:1; }
 if(cmd=="bench"){
   using clk=std::chrono::steady_clock; auto ms=[&](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
   std::vector<uint8_t> chunk(1024*1024); std::mt19937 rng(42); for(auto&b:chunk) b=(uint8_t)rng();
   auto t0=clk::now(); for(int i=0;i<32;i++) fs.pwrite_file("bench_seq", (uint64_t)i*chunk.size(), chunk.data(), chunk.size());
   auto t1=clk::now(); double seq=(32.0)/(ms(t0,t1)/1000.0);
   t0=clk::now(); for(int i=0;i<256;i++){ uint64_t off=(rng()%32)*1024*1024; fs.pwrite_file("bench_seq", off, chunk.data(), 4096); } t1=clk::now(); double rnd=(256*4096/1048576.0)/(ms(t0,t1)/1000.0);
   fs.consistency_point();
   t0=clk::now(); fs.snap_create("bench_snap"); t1=clk::now(); double snap_us=std::chrono::duration<double,std::micro>(t1-t0).count();
   fs.pwrite_file("bench_seq",0,chunk.data(),chunk.size()); fs.consistency_point();
   t0=clk::now(); auto ch=fs.snap_diff("bench_snap","live"); t1=clk::now(); double diff_ms=ms(t0,t1);
   printf("sequential_write: %.2f MB/s\nrandom_write: %.2f MB/s\nsnapshot_create_latency: %.1f us\ndiff: %.3f ms (%zu changed blocks)\n",seq,rnd,snap_us,diff_ms,ch.size());
   fs.snap_delete("bench_snap"); return 0;
 }
 fprintf(stderr,"unknown command\n"); return 1;
}
