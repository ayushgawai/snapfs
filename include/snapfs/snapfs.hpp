#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <array>
#include <optional>
#include <functional>

namespace snapfs {
constexpr uint32_t kBlockSize = 4096;
uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

// Lock-free SPSC ring (single producer serialized by caller if many writers, single consumer = log thread)
template <typename T, size_t N = 1024>
struct SpscRing {
  std::array<std::optional<T>, N> buf;
  std::atomic<size_t> head{0}, tail{0}; // head consumer, tail producer
  bool push(T v) {
    size_t t = tail.load(std::memory_order_relaxed);
    if (t - head.load(std::memory_order_acquire) >= N) return false;
    buf[t % N] = std::move(v);
    tail.store(t + 1, std::memory_order_release);
    return true;
  }
  std::optional<T> pop() {
    size_t h = head.load(std::memory_order_relaxed);
    if (h == tail.load(std::memory_order_acquire)) return std::nullopt;
    auto v = std::move(buf[h % N]);
    buf[h % N].reset();
    head.store(h + 1, std::memory_order_release);
    return v;
  }
};

struct Inode {
  char name[128]{};
  uint64_t size = 0;
  uint32_t direct[12]{};
  uint32_t indirect = 0;
  uint32_t dbl_indirect = 0;
  uint32_t valid = 0;
  uint32_t pad = 0;
  uint64_t mtime = 0;
  char reserved[48]{};
};
static_assert(sizeof(Inode) == 256);

struct FileInfo { std::string name; uint64_t size; };
struct SnapInfo { std::string name; uint32_t root_block = 0; uint64_t cp_id = 0; };
struct ChangedBlock { std::string file; uint32_t logical = 0; uint32_t old_block = 0; uint32_t new_block = 0; };

class BlockDevice {
 public:
  bool open(const std::string& path, bool create, uint64_t bytes = 0);
  void close();
  bool read_block(uint32_t id, void* out);
  bool write_block(uint32_t id, const void* in);
  bool sync();
  int fd() const { return fd_; }
  uint64_t bytes() const { return bytes_; }
 private:
  int fd_ = -1; uint64_t bytes_ = 0; std::string path_;
};

class FileSystem {
 public:
  explicit FileSystem(bool background_cp = true) : bg_enabled_(background_cp) {}
  ~FileSystem();
  FileSystem(const FileSystem&) = delete; FileSystem& operator=(const FileSystem&) = delete;

  static bool mkfs(const std::string& path, uint64_t size_mb);
  bool mount(const std::string& path);
  void unmount(); // clean CP + shutdown threads
  void abandon(); // simulate crash: stop threads without CP (for tests via _exit mostly)

  bool write_file(const std::string& name, const std::vector<uint8_t>& data) { return pwrite_file(name, 0, data.data(), data.size(), true); }
  bool pwrite_file(const std::string& name, uint64_t off, const uint8_t* data, size_t len, bool truncate = false);
  std::optional<std::vector<uint8_t>> read_file(const std::string& name);
  std::optional<std::vector<uint8_t>> read_snapshot_file(const std::string& snap, const std::string& name);
  bool remove_file(const std::string& name);
  std::vector<FileInfo> list();
  std::vector<FileInfo> list_snapshot(const std::string& snap);

  bool consistency_point();
  bool snap_create(const std::string& name);
  bool snap_delete(const std::string& name);
  std::vector<SnapInfo> snap_list();
  std::vector<ChangedBlock> snap_diff(const std::string& a, const std::string& b); // b may be "live"
  bool fsck(bool verbose = false);
  uint64_t cp_id() const { return cp_id_; }
  uint32_t total_blocks() const { return total_blocks_; }

 private:
  struct LogOp { uint64_t seq = 0; uint32_t inode_idx = 0; Inode inode; };
  BlockDevice dev_;
  uint32_t total_blocks_ = 0, log_start_ = 0, log_blocks_ = 1024, bitmap_start_ = 0, bitmap_blocks_ = 0, data_start_ = 0;
  uint32_t cur_root_ = 0; uint64_t cp_id_ = 0; int active_sb_ = 0;
  uint64_t next_log_seq_ = 1, flushed_seq_ = 0;
  std::vector<Inode> inodes_;
  std::vector<uint32_t> inode_blocks_; // live inode-file blocks
  std::vector<SnapInfo> snaps_;
  std::vector<uint64_t> bitmap_;
  std::mutex alloc_mu_;
  std::shared_mutex global_rw_; // writers shared, CP exclusive
  std::mutex prod_mu_; // serialize SPSC producers
  std::mutex write_mu_; // ponytail: serializes writers' inode-table updates; per-inode shard if write throughput matters
  std::unordered_map<std::string, std::shared_ptr<std::shared_mutex>> file_locks_;
  std::mutex file_locks_mu_;
  SpscRing<LogOp> ring_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> flushed_atomic_{0};
  std::thread log_thread_, cp_thread_;
  bool bg_enabled_;
  std::atomic<bool> dirty_{false};
  std::atomic<size_t> log_pos_{0}; // records written since CP
  std::shared_ptr<std::shared_mutex> lock_for(const std::string& name);
  uint32_t alloc_block();
  void mark_used(uint32_t id, bool used);
  bool is_used(uint32_t id);
  std::vector<Inode> load_inodes(uint32_t root_block, std::vector<uint32_t>* out_blocks = nullptr, std::vector<SnapInfo>* out_snaps = nullptr, uint64_t* out_cp = nullptr);
  std::vector<uint32_t> collect_ptrs(const Inode& ino);
  std::vector<uint32_t> reachable_blocks(const std::vector<Inode>& inos, const std::vector<uint32_t>& iblocks);
  void rebuild_bitmap();
  uint32_t get_ptr(const Inode& ino, uint32_t logical);
  // COW helpers operating on a copied inode; allocate new indirect blocks as needed
  void set_ptr_cow(Inode& ino, uint32_t logical, uint32_t block);
  bool write_root_and_flip(const std::vector<uint32_t>& new_iblocks);
  void log_thread_fn();
  void cp_thread_fn();
  bool append_log_and_apply(uint32_t idx, const Inode& ino);
  int find_inode(const std::string& name);
  bool read_inode_file(const std::string& snap, const std::string& name, std::vector<uint8_t>& out);
  std::vector<Inode> inodes_for(const std::string& snap_or_live); // "live" or snap name; empty on error w/ flag
  bool snap_exists(const std::string& n) { for (auto& s : snaps_) if (s.name == n) return true; return false; }
};
} // namespace snapfs
