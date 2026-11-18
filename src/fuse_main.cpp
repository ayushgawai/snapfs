#define FUSE_USE_VERSION 31
#include <fuse.h>
#include "snapfs/snapfs.hpp"
#include <cstring>
#include <string>
// ponytail: thin mapping of flat snapfs names to FUSE paths; directories/nesting unsupported (flat namespace)
static snapfs::FileSystem* FS;
static std::string base(const char* p){ std::string s=p; return s.size()>1?s.substr(1):s; }
static int getattr(const char* p, struct stat* st, struct fuse_file_info*){ memset(st,0,sizeof(*st)); if(!strcmp(p,"/")){st->st_mode=S_IFDIR|0755;st->st_nlink=2;return 0;} for(auto&f:FS->list()) if(f.name==base(p)){st->st_mode=S_IFREG|0644;st->st_size=f.size;return 0;} return -ENOENT; }
static int readdir(const char* p, void* buf, fuse_fill_dir_t fill, off_t, struct fuse_file_info*, enum fuse_readdir_flags){ if(strcmp(p,"/")) return -ENOENT; fill(buf,".",nullptr,0,(enum fuse_fill_dir_flags)0); fill(buf,"..",nullptr,0,(enum fuse_fill_dir_flags)0); for(auto&f:FS->list()) fill(buf,f.name.c_str(),nullptr,0,(enum fuse_fill_dir_flags)0); return 0; }
static int open_cb(const char* p, struct fuse_file_info*){ for(auto&f:FS->list()) if(f.name==base(p)) return 0; return -ENOENT; }
static int create_cb(const char* p, mode_t, struct fuse_file_info*){ FS->write_file(base(p),{}); FS->consistency_point(); return 0; }
static int read_cb(const char* p, char* buf, size_t n, off_t off, struct fuse_file_info*){ auto d=FS->read_file(base(p)); if(!d) return -ENOENT; if(off>=(off_t)d->size()) return 0; n=std::min(n,(size_t)(d->size()-off)); memcpy(buf,d->data()+off,n); return (int)n; }
static int write_cb(const char* p, const char* buf, size_t n, off_t off, struct fuse_file_info*){ return FS->pwrite_file(base(p),off,(const uint8_t*)buf,n)?(int)n:-EIO; }
static int truncate_cb(const char* p, off_t n, struct fuse_file_info*){ auto d=FS->read_file(base(p)); std::vector<uint8_t> v(n,0); if(d) memcpy(v.data(),d->data(),std::min((size_t)n,d->size())); return FS->write_file(base(p),v)?0:-EIO; }
static int unlink_cb(const char* p){ FS->remove_file(base(p)); FS->consistency_point(); return 0; }
static struct fuse_operations ops{};
int main(int argc,char**argv){ if(argc<3){fprintf(stderr,"usage: snapfs-mount <img> <mountpoint> [fuse opts]\n");return 1;} static snapfs::FileSystem fs; if(!fs.mount(argv[1])) return 1; FS=&fs;
 ops.getattr=getattr; ops.readdir=readdir; ops.open=open_cb; ops.create=create_cb; ops.read=read_cb; ops.write=write_cb; ops.truncate=truncate_cb; ops.unlink=unlink_cb;
 // hand remaining args to fuse
 char* fargv[]={argv[0],argv[2],argc>3?argv[3]:(char*)"-f",nullptr}; int ac=argc>3?3:3;
 int r=fuse_main(ac,fargv,&ops,nullptr); fs.unmount(); return r; }
