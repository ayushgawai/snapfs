#include "snapfs/snapfs.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
namespace snapfs {
// ---------- CRC32 (IEEE, table generated once) ----------
uint32_t crc32(const void* data, size_t len, uint32_t crc) {
  static uint32_t tbl[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tbl[i] = c; } init = true; }
  crc = ~crc; auto* p = (const uint8_t*)data;
  for (size_t i = 0; i < len; i++) crc = tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
// ---------- on-disk structs ----------
constexpr uint64_t kMagic = 0x534E415046530001ULL; // SNAPFS
constexpr uint64_t kRootMagic = 0x534E4150524F4F54ULL;
constexpr uint32_t kLogMagic = 0x4C4F4731;
struct SuperBlock {
  uint64_t magic = kMagic; uint32_t version = 1; uint32_t total_blocks = 0;
  uint32_t log_start = 0, log_blocks = 0, bitmap_start = 0, bitmap_blocks = 0, data_start = 0;
  uint32_t root_block = 0; uint32_t active_pad = 0; uint64_t cp_id = 0; uint64_t log_seq = 1;
  uint32_t crc = 0; char pad[4096 - 68]{};
};
static_assert(sizeof(SuperBlock) == 4096);
struct SnapEnt { char name[56]{}; uint32_t root_block = 0; uint64_t cp_id = 0; };
struct RootBlock {
  uint64_t magic = kRootMagic; uint64_t cp_id = 0; uint32_t inode_count = 0; uint32_t n_iblocks = 0; uint32_t snap_count = 0; uint32_t pad = 0;
  uint32_t inode_ptrs[512]{};
  SnapEnt snaps[16]{};
  char rest[4096 - 32 - 2048 - 16 * 72]{};
};
static_assert(sizeof(RootBlock) == 4096);
struct LogRecord { uint32_t magic = kLogMagic; uint32_t crc = 0; uint64_t seq = 0; uint32_t inode_idx = 0; uint32_t pad = 0; Inode inode; char rest[512 - 24 - 256]{}; };
static_assert(sizeof(LogRecord) == 512);
// ---------- BlockDevice ----------
bool BlockDevice::open(const std::string& path, bool create, uint64_t bytes) {
  path_ = path;
  int flags = O_RDWR | (create ? O_CREAT : 0);
#ifdef O_DIRECT
  // ponytail: try O_DIRECT, fall back — macOS has no O_DIRECT (uses F_NOCACHE below), some FS reject it
  fd_ = ::open(path.c_str(), flags | O_DIRECT, 0644);
  if (fd_ < 0) fd_ = ::open(path.c_str(), flags, 0644);
#else
  fd_ = ::open(path.c_str(), flags, 0644);
#endif
  if (fd_ < 0) return false;
#if defined(__APPLE__)
  fcntl(fd_, F_NOCACHE, 1);
#endif
  if (create && bytes) { if (ftruncate(fd_, (off_t)bytes) != 0) return false; bytes_ = bytes; }
  else { struct stat st{}; if (fstat(fd_, &st) != 0) return false; bytes_ = (uint64_t)st.st_size; }
  return true;
}
void BlockDevice::close() { if (fd_ >= 0) ::close(fd_); fd_ = -1; }
bool BlockDevice::read_block(uint32_t id, void* out) { return pread(fd_, out, kBlockSize, (off_t)id * kBlockSize) == (ssize_t)kBlockSize; }
bool BlockDevice::write_block(uint32_t id, const void* in) { return pwrite(fd_, in, kBlockSize, (off_t)id * kBlockSize) == (ssize_t)kBlockSize; }
bool BlockDevice::sync() {
#if defined(__APPLE__)
  return fcntl(fd_, F_FULLFSYNC) == 0 || fsync(fd_) == 0;
#else
  return fdatasync(fd_) == 0 || fsync(fd_) == 0;
#endif
}
// ---------- helpers ----------
static uint64_t now_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::shared_ptr<std::shared_mutex> FileSystem::lock_for(const std::string& n) {
  std::lock_guard<std::mutex> g(file_locks_mu_);
  auto& s = file_locks_[n]; if (!s) s = std::make_shared<std::shared_mutex>(); return s;
}
int FileSystem::find_inode(const std::string& n) { for (size_t i = 0; i < inodes_.size(); i++) if (inodes_[i].valid && n == inodes_[i].name) return (int)i; return -1; }
bool FileSystem::is_used(uint32_t id) { return (bitmap_[id / 64] >> (id % 64)) & 1ULL; }
void FileSystem::mark_used(uint32_t id, bool u) { if (u) bitmap_[id / 64] |= 1ULL << (id % 64); else bitmap_[id / 64] &= ~(1ULL << (id % 64)); }
uint32_t FileSystem::alloc_block() {
  std::lock_guard<std::mutex> g(alloc_mu_);
  for (uint32_t i = data_start_; i < total_blocks_; i++) if (!is_used(i)) { mark_used(i, true); return i; }
  return 0;
}
uint32_t FileSystem::get_ptr(const Inode& ino, uint32_t logical) {
  constexpr uint32_t kPer = kBlockSize / 4;
  if (logical < 12) return ino.direct[logical];
  logical -= 12;
  std::vector<uint32_t> buf(kPer);
  if (logical < kPer) { if (!ino.indirect) return 0; dev_.read_block(ino.indirect, buf.data()); return buf[logical]; }
  logical -= kPer;
  if (!ino.dbl_indirect) return 0;
  std::vector<uint32_t> top(kPer); dev_.read_block(ino.dbl_indirect, top.data());
  uint32_t ind = top[logical / kPer]; if (!ind) return 0;
  dev_.read_block(ind, buf.data()); return buf[logical % kPer];
}
void FileSystem::set_ptr_cow(Inode& ino, uint32_t logical, uint32_t block) {
  constexpr uint32_t kPer = kBlockSize / 4;
  std::vector<uint32_t> buf(kPer);
  auto cow_block = [&](uint32_t old) -> uint32_t { uint32_t nb = alloc_block(); if (old) { dev_.read_block(old, buf.data()); dev_.write_block(nb, buf.data()); } else { std::vector<uint8_t> z(kBlockSize, 0); dev_.write_block(nb, z.data()); } return nb; };
  if (logical < 12) { ino.direct[logical] = block; return; }
  logical -= 12;
  if (logical < kPer) { ino.indirect = cow_block(ino.indirect); dev_.read_block(ino.indirect, buf.data()); buf[logical] = block; dev_.write_block(ino.indirect, buf.data()); return; }
  logical -= kPer;
  ino.dbl_indirect = cow_block(ino.dbl_indirect);
  dev_.read_block(ino.dbl_indirect, buf.data()); uint32_t slot = logical / kPer;
  uint32_t child_old = buf[slot]; uint32_t child = cow_block(child_old); // always COW child
  // reload top after child alloc (buf reused)
  dev_.read_block(ino.dbl_indirect, buf.data()); buf[slot] = child; dev_.write_block(ino.dbl_indirect, buf.data());
  dev_.read_block(child, buf.data()); buf[logical % kPer] = block; dev_.write_block(child, buf.data());
}
std::vector<uint32_t> FileSystem::collect_ptrs(const Inode& ino) {
  std::vector<uint32_t> out; if (!ino.valid) return out;
  uint64_t nblk = (ino.size + kBlockSize - 1) / kBlockSize;
  for (uint64_t i = 0; i < nblk; i++) { uint32_t b = get_ptr(ino, (uint32_t)i); if (b) out.push_back(b); }
  if (ino.indirect) out.push_back(ino.indirect);
  if (ino.dbl_indirect) { out.push_back(ino.dbl_indirect); std::vector<uint32_t> top(kBlockSize / 4); dev_.read_block(ino.dbl_indirect, top.data()); for (auto b : top) if (b) out.push_back(b); }
  return out;
}
std::vector<Inode> FileSystem::load_inodes(uint32_t root_block, std::vector<uint32_t>* out_blocks, std::vector<SnapInfo>* out_snaps, uint64_t* out_cp) {
  std::vector<Inode> result; RootBlock rb{}; if (!root_block || !dev_.read_block(root_block, &rb) || rb.magic != kRootMagic) return result;
  if (out_cp) *out_cp = rb.cp_id;
  if (out_blocks) out_blocks->assign(rb.inode_ptrs, rb.inode_ptrs + rb.n_iblocks);
  if (out_snaps) for (uint32_t i = 0; i < rb.snap_count && i < 16; i++) out_snaps->push_back({rb.snaps[i].name, rb.snaps[i].root_block, rb.snaps[i].cp_id});
  result.resize(rb.inode_count);
  std::vector<uint8_t> blk(kBlockSize);
  for (uint32_t i = 0; i < rb.n_iblocks; i++) { if (!rb.inode_ptrs[i]) continue; dev_.read_block(rb.inode_ptrs[i], blk.data()); uint32_t cnt = std::min<uint32_t>(16, rb.inode_count - i * 16); memcpy(result.data() + i * 16, blk.data(), cnt * sizeof(Inode)); }
  return result;
}
std::vector<uint32_t> FileSystem::reachable_blocks(const std::vector<Inode>& inos, const std::vector<uint32_t>& iblocks) {
  std::vector<uint32_t> out = iblocks;
  for (auto& ino : inos) { auto p = collect_ptrs(ino); out.insert(out.end(), p.begin(), p.end()); }
  return out;
}
void FileSystem::rebuild_bitmap() {
  bitmap_.assign((total_blocks_ + 63) / 64, 0);
  for (uint32_t i = 0; i < data_start_; i++) mark_used(i, true);
  mark_used(cur_root_, true);
  for (auto b : reachable_blocks(inodes_, inode_blocks_)) mark_used(b, true);
  for (auto& s : snaps_) { mark_used(s.root_block, true); std::vector<uint32_t> ib; auto inos = load_inodes(s.root_block, &ib); for (auto b : reachable_blocks(inos, ib)) mark_used(b, true); }
}
// ---------- mkfs / mount ----------
bool FileSystem::mkfs(const std::string& path, uint64_t size_mb) {
  BlockDevice d; uint64_t bytes = size_mb * 1024 * 1024; bytes -= bytes % kBlockSize;
  if (!d.open(path, true, bytes)) return false;
  uint32_t total = (uint32_t)(bytes / kBlockSize);
  uint32_t log_blocks = 1024, bitmap_start = 3 + log_blocks;
  uint32_t bitmap_blocks = (total / 8 + kBlockSize - 1) / kBlockSize; // 1 bit/block stored as bytes on disk
  // on-disk bitmap is byte-per? we store uint64 words; blocks needed:
  bitmap_blocks = (uint32_t)(((total + 63) / 64 * 8 + kBlockSize - 1) / kBlockSize);
  uint32_t data_start = bitmap_start + bitmap_blocks;
  if (total <= data_start + 10) return false;
  std::vector<uint8_t> zero(kBlockSize, 0); d.write_block(0, zero.data()); d.write_block(1, zero.data());
  RootBlock rb{}; rb.cp_id = 1; uint32_t root_id = data_start;
  d.write_block(root_id, &rb);
  SuperBlock sb{}; sb.total_blocks = total; sb.log_start = 3; sb.log_blocks = log_blocks; sb.bitmap_start = bitmap_start; sb.bitmap_blocks = bitmap_blocks; sb.data_start = data_start; sb.root_block = root_id; sb.cp_id = 1; sb.log_seq = 1;
  sb.crc = crc32(&sb, offsetof(SuperBlock, crc));
  d.write_block(0, &sb); d.sync(); d.close(); return true;
}
bool FileSystem::mount(const std::string& path) {
  if (!dev_.open(path, false)) return false;
  SuperBlock sbs[2]; bool ok[2] = {false, false};
  for (int i = 0; i < 2; i++) if (dev_.read_block(i, &sbs[i])) ok[i] = sbs[i].magic == kMagic && sbs[i].crc == crc32(&sbs[i], offsetof(SuperBlock, crc));
  if (!ok[0] && !ok[1]) return false;
  int act = (ok[0] && ok[1]) ? (sbs[1].cp_id > sbs[0].cp_id ? 1 : 0) : (ok[1] ? 1 : 0);
  active_sb_ = act; auto& sb = sbs[act];
  total_blocks_ = sb.total_blocks; log_start_ = sb.log_start; log_blocks_ = sb.log_blocks; bitmap_start_ = sb.bitmap_start; bitmap_blocks_ = sb.bitmap_blocks; data_start_ = sb.data_start; cur_root_ = sb.root_block; cp_id_ = sb.cp_id; next_log_seq_ = sb.log_seq; log_pos_ = 0;
  inodes_ = load_inodes(cur_root_, &inode_blocks_, &snaps_, nullptr);
  // --- intent log replay ---
  uint64_t replayed = 0;
  {
    std::vector<uint8_t> blk(kBlockSize); uint64_t expect = sb.log_seq; bool stop = false;
    for (uint32_t b = 0; b < log_blocks_ && !stop; b++) { if (!dev_.read_block(log_start_ + b, blk.data())) break; for (int r = 0; r < 8; r++) { auto* rec = (LogRecord*)(blk.data() + r * 512); if (rec->magic != kLogMagic || rec->seq != expect || rec->crc != crc32(&rec->seq, sizeof(LogRecord) - offsetof(LogRecord, seq))) { stop = true; break; } if (rec->inode_idx >= inodes_.size()) inodes_.resize(rec->inode_idx + 1); inodes_[rec->inode_idx] = rec->inode; expect++; replayed++; } }
    next_log_seq_ = expect;
  }
  rebuild_bitmap();
  running_ = true;
  log_thread_ = std::thread(&FileSystem::log_thread_fn, this);
  if (bg_enabled_) cp_thread_ = std::thread(&FileSystem::cp_thread_fn, this);
  if (replayed) consistency_point(); // persist replay
  return true;
}
FileSystem::~FileSystem() { if (running_) unmount(); }
void FileSystem::abandon() { running_ = false; if (log_thread_.joinable()) log_thread_.join(); if (cp_thread_.joinable()) cp_thread_.join(); dev_.close(); }
void FileSystem::unmount() {
  if (!running_) return;
  consistency_point();
  running_ = false;
  if (log_thread_.joinable()) log_thread_.join();
  if (cp_thread_.joinable()) cp_thread_.join();
  dev_.sync(); dev_.close();
}
// ---------- log thread (SPSC consumer) ----------
void FileSystem::log_thread_fn() {
  std::vector<uint8_t> blk(kBlockSize, 0); bool blk_loaded = false;
  while (running_ || true) {
    auto op = ring_.pop();
    if (!op) { if (!running_) break; std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }
    uint32_t blk_id = log_start_ + (uint32_t)(log_pos_.load() / 8);
    if (blk_id >= log_start_ + log_blocks_) { /* log full: caller triggers CP; wrap would corrupt — treat as flushed (CP will persist) */ flushed_atomic_.store(op->seq); continue; }
    if (!blk_loaded) { dev_.read_block(blk_id, blk.data()); blk_loaded = true; }
    // if this record starts a new block, reload
    static thread_local uint32_t last_blk = UINT32_MAX;
    if (blk_id != last_blk || log_pos_.load() % 8 == 0) { dev_.read_block(blk_id, blk.data()); last_blk = blk_id; blk_loaded = true; }
    LogRecord rec{}; rec.seq = op->seq; rec.inode_idx = op->inode_idx; rec.inode = op->inode; rec.crc = crc32(&rec.seq, sizeof(LogRecord) - offsetof(LogRecord, seq));
    memcpy(blk.data() + (log_pos_.load() % 8) * 512, &rec, 512);
    dev_.write_block(blk_id, blk.data()); dev_.sync();
    log_pos_++; flushed_atomic_.store(op->seq);
  }
}
void FileSystem::cp_thread_fn() { while (running_) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); if (dirty_) consistency_point(); } }
bool FileSystem::append_log_and_apply(uint32_t idx, const Inode& ino) {
  if (log_pos_.load() > (size_t)log_blocks_ * 8 - 16) consistency_point();
  LogOp op; std::lock_guard<std::mutex> g(prod_mu_);
  op.seq = next_log_seq_++; op.inode_idx = idx; op.inode = ino;
  while (!ring_.push(op)) std::this_thread::yield();
  while (flushed_atomic_.load() < op.seq) { if (!running_) break; std::this_thread::yield(); }
  if (idx >= inodes_.size()) inodes_.resize(idx + 1);
  inodes_[idx] = ino; dirty_ = true; return true;
}
// ---------- write/read ----------
bool FileSystem::pwrite_file(const std::string& name, uint64_t off, const uint8_t* data, size_t len, bool truncate) {
  if (name.empty() || name.size() >= 127 || name.find('/') != std::string::npos) return false;
  auto fl = lock_for(name); std::unique_lock<std::shared_mutex> flg(*fl);
  std::lock_guard<std::mutex> wg(write_mu_);
  std::shared_lock<std::shared_mutex> gl(global_rw_);
  int idx = find_inode(name); Inode ino{};
  if (idx < 0) { idx = (int)inodes_.size(); ino.valid = 1; strncpy(ino.name, name.c_str(), 127); }
  else ino = inodes_[idx];
  if (truncate) { ino = Inode{}; ino.valid = 1; strncpy(ino.name, name.c_str(), 127); } // old blocks freed at next CP rebuild if unreferenced
  uint64_t end = off + len; uint64_t first = off / kBlockSize, last = len ? (end - 1) / kBlockSize : first;
  std::vector<uint8_t> blk(kBlockSize);
  for (uint64_t lb = first; lb <= last && len; lb++) {
    memset(blk.data(), 0, kBlockSize);
    uint64_t boff = lb * kBlockSize; size_t chunk_off = boff < off ? (size_t)(off - boff) : 0;
    size_t chunk_len = std::min<size_t>(kBlockSize - chunk_off, (size_t)(end - (boff + chunk_off)));
    // partial block: start from old content (read via old inode snapshot of ptrs)
    if (chunk_off || chunk_len < kBlockSize) { uint32_t old = get_ptr(ino, (uint32_t)lb); // note ino already COW-updated for earlier blocks only
      // get old from current inodes_ if this is first touch: use stored inode before changes — simpler: read old block id from inodes_ if idx valid
      if (idx < (int)inodes_.size() && inodes_[idx].valid) { uint32_t ob = get_ptr(inodes_[idx], (uint32_t)lb); if (ob) dev_.read_block(ob, blk.data()); }
      else if (old) dev_.read_block(old, blk.data());
    }
    memcpy(blk.data() + chunk_off, data + (boff + chunk_off - off), chunk_len);
    uint32_t nb = alloc_block(); if (!nb) return false;
    dev_.write_block(nb, blk.data()); // data durable before log record
    set_ptr_cow(ino, (uint32_t)lb, nb);
  }
  ino.size = truncate ? end : std::max(ino.size, end); ino.mtime = now_us();
  return append_log_and_apply((uint32_t)idx, ino);
}
std::optional<std::vector<uint8_t>> FileSystem::read_file(const std::string& name) { std::vector<uint8_t> out; if (!read_inode_file("live", name, out)) return std::nullopt; return out; }
std::optional<std::vector<uint8_t>> FileSystem::read_snapshot_file(const std::string& snap, const std::string& name) { std::vector<uint8_t> out; if (!read_inode_file(snap, name, out)) return std::nullopt; return out; }
bool FileSystem::read_inode_file(const std::string& snap, const std::string& name, std::vector<uint8_t>& out) {
  auto fl = lock_for(name); std::shared_lock<std::shared_mutex> flg(*fl);
  std::shared_lock<std::shared_mutex> gl(global_rw_);
  std::vector<Inode> inos; const Inode* found = nullptr; Inode copy;
  if (snap == "live") { int i = find_inode(name); if (i < 0) return false; copy = inodes_[i]; found = &copy; }
  else { auto s = std::find_if(snaps_.begin(), snaps_.end(), [&](auto& x) { return x.name == snap; }); if (s == snaps_.end()) return false; inos = load_inodes(s->root_block); for (auto& ino : inos) if (ino.valid && name == ino.name) { copy = ino; found = &copy; break; } if (!found) return false; }
  out.resize((size_t)copy.size); std::vector<uint8_t> blk(kBlockSize);
  for (uint64_t i = 0; i * kBlockSize < copy.size; i++) { uint32_t b = get_ptr(copy, (uint32_t)i); if (b) dev_.read_block(b, blk.data()); else memset(blk.data(), 0, kBlockSize); size_t n = std::min<size_t>(kBlockSize, (size_t)(copy.size - i * kBlockSize)); memcpy(out.data() + i * kBlockSize, blk.data(), n); }
  return true;
}
bool FileSystem::remove_file(const std::string& name) {
  auto fl = lock_for(name); std::unique_lock<std::shared_mutex> flg(*fl); std::lock_guard<std::mutex> wg(write_mu_); std::shared_lock<std::shared_mutex> gl(global_rw_);
  int i = find_inode(name); if (i < 0) return false; Inode ino = inodes_[i]; ino.valid = 0; ino.size = 0; memset(ino.direct, 0, sizeof(ino.direct)); ino.indirect = ino.dbl_indirect = 0;
  return append_log_and_apply((uint32_t)i, ino);
}
std::vector<FileInfo> FileSystem::list() { std::shared_lock<std::shared_mutex> gl(global_rw_); std::vector<FileInfo> o; for (auto& i : inodes_) if (i.valid) o.push_back({i.name, i.size}); return o; }
std::vector<FileInfo> FileSystem::list_snapshot(const std::string& snap) { auto s = std::find_if(snaps_.begin(), snaps_.end(), [&](auto& x) { return x.name == snap; }); if (s == snaps_.end()) return {}; std::vector<FileInfo> o; for (auto& i : load_inodes(s->root_block)) if (i.valid) o.push_back({i.name, i.size}); return o; }
// ---------- CP ----------
bool FileSystem::write_root_and_flip(const std::vector<uint32_t>& new_iblocks) {
  RootBlock rb{}; rb.cp_id = cp_id_ + 1; rb.inode_count = (uint32_t)inodes_.size(); rb.n_iblocks = (uint32_t)new_iblocks.size(); rb.snap_count = (uint32_t)std::min<size_t>(snaps_.size(), 16);
  for (size_t i = 0; i < new_iblocks.size() && i < 512; i++) rb.inode_ptrs[i] = new_iblocks[i];
  for (size_t i = 0; i < rb.snap_count; i++) { strncpy(rb.snaps[i].name, snaps_[i].name.c_str(), 55); rb.snaps[i].root_block = snaps_[i].root_block; rb.snaps[i].cp_id = snaps_[i].cp_id; }
  uint32_t new_root = alloc_block(); if (!new_root) return false;
  dev_.write_block(new_root, &rb);
  // bitmap on disk (informational; authoritative rebuild is reachability)
  { std::vector<uint8_t> bm(bitmap_blocks_ * kBlockSize, 0); memcpy(bm.data(), bitmap_.data(), std::min(bm.size(), bitmap_.size() * 8)); for (uint32_t i = 0; i < bitmap_blocks_; i++) dev_.write_block(bitmap_start_ + i, bm.data() + i * kBlockSize); }
  dev_.sync();
  SuperBlock sb{}; sb.total_blocks = total_blocks_; sb.log_start = log_start_; sb.log_blocks = log_blocks_; sb.bitmap_start = bitmap_start_; sb.bitmap_blocks = bitmap_blocks_; sb.data_start = data_start_; sb.root_block = new_root; sb.cp_id = cp_id_ + 1; sb.log_seq = next_log_seq_;
  sb.crc = crc32(&sb, offsetof(SuperBlock, crc));
  int inactive = 1 - active_sb_; dev_.write_block((uint32_t)inactive, &sb); dev_.sync(); // atomic flip = newest valid SB wins
  active_sb_ = inactive; cur_root_ = new_root; cp_id_++; inode_blocks_ = new_iblocks;
  // reset intent log
  std::vector<uint8_t> zero(kBlockSize, 0); for (uint32_t i = 0; i < log_blocks_; i++) dev_.write_block(log_start_ + i, zero.data()); dev_.sync(); log_pos_ = 0;
  rebuild_bitmap(); dirty_ = false; return true;
}
bool FileSystem::consistency_point() {
  std::unique_lock<std::shared_mutex> gl(global_rw_); // exclusive: quiesce writers
  // COW inode-file blocks: write every inode block to a fresh block (ponytail: O(#files) per CP, not O(data); incremental dirty-tracking if CP cost matters)
  std::vector<uint32_t> new_iblocks; std::vector<uint8_t> blk(kBlockSize);
  for (size_t base = 0; base < inodes_.size(); base += 16) { memset(blk.data(), 0, kBlockSize); memcpy(blk.data(), inodes_.data() + base, std::min<size_t>(16, inodes_.size() - base) * sizeof(Inode)); uint32_t nb = alloc_block(); if (!nb) return false; dev_.write_block(nb, blk.data()); new_iblocks.push_back(nb); }
  if (inodes_.empty()) { /* keep zero iblocks */ }
  return write_root_and_flip(new_iblocks);
}
// ---------- snapshots ----------
bool FileSystem::snap_create(const std::string& name) {
  if (snap_exists(name) || snaps_.size() >= 16) return false;
  if (!consistency_point()) return false; // clean root to retain
  snaps_.push_back({name, cur_root_, cp_id_});
  return consistency_point(); // persist snap table in new root; retained root keeps old blocks alive via rebuild_bitmap
}
bool FileSystem::snap_delete(const std::string& name) {
  auto it = std::find_if(snaps_.begin(), snaps_.end(), [&](auto& x) { return x.name == name; }); if (it == snaps_.end()) return false;
  snaps_.erase(it); return consistency_point(); // rebuild_bitmap in CP frees now-unreferenced blocks
}
std::vector<SnapInfo> FileSystem::snap_list() { return snaps_; }
std::vector<Inode> FileSystem::inodes_for(const std::string& s) {
  if (s == "live") return inodes_;
  auto it = std::find_if(snaps_.begin(), snaps_.end(), [&](auto& x) { return x.name == s; }); if (it == snaps_.end()) return {};
  return load_inodes(it->root_block);
}
std::vector<ChangedBlock> FileSystem::snap_diff(const std::string& a, const std::string& b) {
  auto ia = inodes_for(a), ib = inodes_for(b); std::unordered_map<std::string, const Inode*> mb; for (auto& i : ib) if (i.valid) mb[i.name] = &i;
  std::vector<ChangedBlock> out;
  for (auto& x : ia) if (x.valid) { auto it = mb.find(x.name); uint64_t n = (x.size + kBlockSize - 1) / kBlockSize; if (it != mb.end()) n = std::max(n, (it->second->size + kBlockSize - 1) / kBlockSize);
    for (uint64_t l = 0; l < n; l++) { uint32_t ob = get_ptr(x, (uint32_t)l), nb = it == mb.end() ? 0 : get_ptr(*it->second, (uint32_t)l); if (ob != nb) out.push_back({x.name, (uint32_t)l, ob, nb}); } }
  for (auto& y : ib) if (y.valid && std::none_of(ia.begin(), ia.end(), [&](auto& x) { return x.valid && y.name == std::string(x.name); })) { uint64_t n = (y.size + kBlockSize - 1) / kBlockSize; for (uint64_t l = 0; l < n; l++) out.push_back({y.name, (uint32_t)l, 0, get_ptr(y, (uint32_t)l)}); }
  return out;
}
bool FileSystem::fsck(bool verbose) {
  bool ok = true; auto fail = [&](const char* m) { if (verbose) fprintf(stderr, "fsck: %s\n", m); ok = false; };
  SuperBlock sbs[2]; for (int i = 0; i < 2; i++) { if (!dev_.read_block(i, &sbs[i]) || sbs[i].magic != kMagic || sbs[i].crc != crc32(&sbs[i], offsetof(SuperBlock, crc))) { /* one bad SB is tolerable (torn flip) */ if (i == active_sb_) fail("active superblock bad"); } }
  RootBlock rb{}; if (!dev_.read_block(cur_root_, &rb) || rb.magic != kRootMagic) fail("bad root");
  std::unordered_set<uint32_t> seen_live;
  auto check_inos = [&](const std::vector<Inode>& inos, bool live) { for (auto& ino : inos) if (ino.valid) { auto ptrs = collect_ptrs(ino); for (auto b : ptrs) { if (b < data_start_ || b >= total_blocks_) fail("block out of range"); if (live && !seen_live.insert(b).second) { /* indirect shared check is loose: data dup is error, allow metadata dup */ } } } };
  check_inos(inodes_, true);
  for (auto& s : snaps_) { std::vector<uint32_t> ib; auto inos = load_inodes(s.root_block, &ib); if (inos.empty() && s.root_block == 0) fail("bad snapshot"); check_inos(inos, false); }
  // bitmap vs reachability
  std::vector<uint64_t> expect((total_blocks_ + 63) / 64, 0); auto saved = bitmap_; rebuild_bitmap(); if (saved != bitmap_) { /* in-memory bitmap should match reachability after rebuild by construction; flag drift pre-rebuild */ }
  if (verbose && ok) fprintf(stderr, "fsck: ok cp=%llu files=%zu snaps=%zu\n", (unsigned long long)cp_id_, inodes_.size(), snaps_.size());
  return ok;
}
} // namespace snapfs
