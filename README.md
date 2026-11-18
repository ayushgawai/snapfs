# snapfs

A copy-on-write, write-anywhere block file system in C++20 (standard library + POSIX only),
inspired by WAFL-style storage designs: live blocks are never overwritten, a consistency
point (CP) atomically flips a checksummed double-buffered superblock, an intent log covers
the window between CPs, and snapshots are O(1) retained roots sharing blocks with the live tree.

## Design

```
  snapfsctl / FUSE (optional)          CLI + optional mount, flat namespace
        |
  FileSystem API                       write_file / pwrite_file / read_file / snap_*
        |        per-inode shared_mutex (RW)  +  global RW (writers shared, CP exclusive)
        v
  Intent log  ---- SPSC lock-free ring ----> log thread -> 1024-block log area (fdatasync per op)
        |        data blocks are written BEFORE the log record that references them
        v
  Write-anywhere tree                  inode file (COW blocks) -> inodes -> direct/indirect/
                                       double-indirect -> data blocks; every write allocates
                                       fresh blocks, old blocks stay untouched
        |
  Consistency point (background thread / explicit) 
        |  1. COW-write inode-file blocks   2. write new root block   3. write bitmap
        |  4. fdatasync   5. flip inactive superblock (CRC32, higher cp_id wins)  6. reset log
        v
  Block device                         file-backed, pread/pwrite, O_DIRECT where available
                                       (F_NOCACHE on macOS), 4 KiB blocks

  Disk layout: [SB0][SB1][log hdr][intent log x1024][free-space bitmap][data blocks ...]
  Superblocks are double-buffered: a torn CP leaves the older valid SB, mount picks it and
  replays the intent log on top.
```

Snapshot block sharing: a snapshot is just a retained root block. The root points at
inode-file blocks, which point at inodes, which point at data — retaining the root retains
the whole tree with zero data copying, and shared blocks are exactly the blocks reachable
from both roots:

```
        live root (cp=42)          snapshot root (cp=40)
            |                            |
        inode blocks v42  ------shared-- inode blocks v40
            |                            |
        data: [A][B'][C]           data: [A][B][C]     B' = COW rewrite of B; A, C shared
```

Per-block snapshot reference tracking is reachability: at each CP the allocator bitmap is
rebuilt from the union of blocks reachable from the live root and every snapshot root
("per-block snapshot bitmaps" are represented by this reachable-set computation rather than
a stored refcount array — see Limitations). Deleting a snapshot drops its root; blocks no
longer reachable from any root are freed at the next CP. `snap diff A B` walks both trees
and lists `(file, logical block, old block, new block)` for every pointer that differs.

## Build / run / test

```sh
make            # clang++ -std=c++20 -O2 -pthread -> build/snapfsctl + test binaries
make test       # unit, crash (50 SIGKILL runs), concurrency
make tsan       # ThreadSanitizer build + run of the concurrency test
make bench      # build/bench.img + `snapfsctl bench`

# CMake is kept for CI:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # unit, crash (50 SIGKILL runs), concurrency

./build/snapfsctl fs.img mkfs 256
echo hello > /tmp/hello.txt
./build/snapfsctl fs.img write greeting /tmp/hello.txt
./build/snapfsctl fs.img snap create v1
echo world > /tmp/hello.txt
./build/snapfsctl fs.img write greeting /tmp/hello.txt
./build/snapfsctl fs.img snap diff v1 live
./build/snapfsctl fs.img snap read v1 greeting   # -> hello
./build/snapfsctl fs.img read greeting           # -> world
./build/snapfsctl fs.img ls
./build/snapfsctl fs.img fsck
./build/snapfsctl fs.img bench
```

Optional FUSE mount (Linux, needs libfuse3): `cmake -DSNAPFS_FUSE=ON ..` then
`snapfs-mount fs.img /mnt/snapfs -f`.

ThreadSanitizer concurrency run:

```sh
cmake -S . -B build-tsan -DSNAPFS_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan --target test_concurrency_tsan && ./build-tsan/test_concurrency_tsan
```

## Benchmark

Measured with `snapfsctl bench` (32 MiB sequential, 256 random 4 KiB writes, on the
machine used for development — your numbers will differ):

```
sequential_write: 12.71 MB/s
random_write: 0.08 MB/s
snapshot_create_latency: 5150322.4 us
diff: 2002.399 ms (256 changed blocks)
```

## Limitations (honest)

- Flat namespace: no directories, renames, hard/symlinks, xattrs, or permissions beyond the image file itself. Names ≤127 chars, one image = one volume.
- Ceilings: ≤512 inode-file blocks (8192 files), ≤16 snapshots, file size bounded by the indirect tree (~1 GiB via double-indirect), image must be created at final size (no grow/shrink).
- A consistency point rewrites all inode-file blocks — O(#files), not O(dirty files). Fine at this scale; dirty inode-block tracking is the obvious upgrade.
- Snapshot block sharing is tracked by reachability recomputation at CP instead of a persisted per-block refcount/bitmap array; equivalent results, O(volume metadata) per CP.
- The intent log is fixed at 1024 blocks (8192 ops); a CP is forced when it fills. Log appends are fdatasync'd per operation, so small synchronous writes are latency-bound by the storage.
- The SPSC ring has many producers serialized by a producer mutex feeding a single consumer (log) thread; it is not a multi-producer lock-free queue.
- fsck validates superblock/root CRCs, block ranges, and bitmap-vs-reachability, but does not checksum file data blocks.
- FUSE support is a thin optional mapping and is not exercised by CI.
- Crash tests verify that post-crash files are always a complete old or new version and snapshots are intact — not that the newest acknowledged write always survives beyond the intent log's guarantees.

## License

MIT — see [LICENSE](LICENSE).
