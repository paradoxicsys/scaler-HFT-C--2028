# Performance Report — Low-Latency Order Book

**Team:** Shambhu Yadav (24bcs10356, shambhu.24bcs10356@sst.scaler.com) · Ratnesh Vaibhav (24bcs10413, ratnesh.24bcs10413@sst.scaler.com)

Design details for A, B and C are in `README.md`; this report covers results and analysis.

## 1. Setup

| | |
|---|---|
| Machine | AWS EC2 `c7g.large`: 2 vCPU AWS Graviton3 (Arm Neoverse-V1), L1d 64 KiB/core, L2 1 MiB/core, L3 32 MiB |
| OS / kernel | Ubuntu 24.04, Linux 7.0.0-1014-aws |
| Compiler | g++ 13.3.0, `-std=c++17 -O2 -g -fno-omit-frame-pointer` |
| perf | 7.0.14, `kernel.perf_event_paranoid=1` |
| Timing | `./bench --impl X`: 5 runs, median (`results/bench_*.txt`) |
| Counters | `perf stat -r 5 -e … ./bench --impl X --perf` (`results/perf_stat_*.txt`) |

Workload (seed 42): 1,000,000 ops (≈70 % add / 20 % cancel / 10 % `get_bbo`) plus 10,000
depth queries (`get_bids(10)` + `get_asks(10)` every 100 ops). Full machine details:
`results/machine.txt`. Earlier numbers from the development Mac are kept in `results/macos/`.

## 2. Correctness

`./bench --verify` feeds the same workload to A, B and C in lock-step and compares, after
**every** operation, the cancel return value, the BBO, and every level of `get_bids(10)` /
`get_asks(10)`. It also runs hand-written edge cases (empty book, unknown and double cancel,
3-level book asked for 10, BBO falling to the next level, zero/negative/huge depth, invalid
price or quantity). On the EC2 machine: **A PASS, B PASS, C PASS — all implementations produce
identical results** (`results/verify.txt`). The verify test also passes under UBSan on macOS.

## 3. Results table

| Metric | A (map + hash) | B (sorted vector + id tree) | C (arrays + bitmap + own hash) |
|---|---|---|---|
| Total time (ms) | 204.1 | 427.7 | **45.3** |
| ns / add | 131.8 | 264.0 | **93.0** |
| ns / cancel | 415.6 | 1156.0 | **82.8** |
| ns / BBO | 4.62 | **4.35** | 5.01 |
| ns / depth (10+10) | 137.8 | **66.5** | 88.5 |
| IPC | 1.07 | 1.07 | **1.64** |
| Branch misses (%) | 5.45 % (8.54 M) | 5.57 % (15.97 M) | **2.64 % (1.84 M)** |
| L1-dcache-load-misses | 10.92 M | 25.22 M | **3.31 M** |
| Cache misses | 10.93 M | 25.23 M | **3.27 M** |
| dTLB-load-misses | 17.55 M | 40.44 M | **2.34 M** |
| Page faults | **15,103** | 17,700 | 15,223 |

**How to read the table**
- *Total time* is the uninstrumented pass. It is the most reliable number.
- *ns / add* and *ns / cancel* time each call with a `steady_clock` pair, minus the measured
  pair cost (63 ns on this machine). C's operations are cheaper than the timer itself, so its
  per-op numbers carry a large error. Its total (45 ns per op on average) is the better guide.
- *ns / BBO* is 1 M back-to-back calls timed as one block. A single call is below timer resolution.
- *IPC* = instructions ÷ cycles. *Branch misses %* = branch-misses ÷ branches.
- **Counter availability.** All nine requested events were available. The core has fewer
  hardware counters than events, so perf multiplexed them (each was counted about 28–43 % of the
  time and scaled up). Run-to-run variation was under ±1.2 %.
- On Arm, perf's generic `cache-misses` event maps to the same hardware event as
  `L1-dcache-load-misses` (L1D refill), so those two rows match. Last-level-cache misses were
  not measured separately.
- perf counts the whole process, including generating the 1 M-op workload, which is identical
  for all three. `perf record` shows generation and RNG are about 30 % of C's samples and under
  10 % of A's and B's. The differences between columns therefore come from the books.

## 4. Analysis

### 4.1 Which is fastest?
**C is fastest for add and cancel. All three tie on `get_bbo`. B is fastest for depth.**
C runs the workload 4.5× faster than A and 9.4× faster than B.
- **C** adds with one hash probe into a flat table, one array add and one bit-set. It cancels
  with one probe and two stores. Neither touches the allocator.
- **A** allocates a hash node and descends about 13 red-black-tree nodes per add. perf record
  puts 60 % of its samples in `add_order`/`cancel_order`, plus libstdc++ hash-table internals.
- **B** walks the id tree (`std::map`, up to ~500k entries, ~19 levels deep) on every add and cancel, then
  binary-searches the level vector. A cancel also scans about 50 orders in the level, so cancel
  is B's worst operation (33 % of samples). perf record also shows 7.4 % in
  `vector::_M_insert_aux`: inserting a new level moves every later `PriceLevel` one slot.
  Each holds its own vector, so this is a move per element, not a single memmove. Another 5.9 %
  is `_Rb_tree_rebalance_for_erase` from the id tree.
- **`get_bbo`** is O(1) in all three (`map.begin()`, `vector.back()`, cached `best` rank). It
  costs about 4–5 ns, mostly the checksum fold, so the differences are noise.
- **Depth:** B wins because its top 10 levels are the last 10 contiguous elements. C computes the
  next set bit for each level. A follows tree pointers.

### 4.2 Branch prediction
**B has the most branch misses (16.0 M), then A (8.5 M), then C (1.8 M).** A and B have a similar
miss *rate* (≈5.5 %), but B executes almost twice as many branches. Each tree descent compares
`key < node->key` with a random id or price, and the predictor can't learn a coin flip. A does
one tree descent per op. B does two (id tree and binary search), plus the scan loop in
`remove_order`, whose exit point is random and mispredicts once per cancel. C's only
data-dependent branches are the short probe loop (about 1.5 probes at load ≤ 0.5) and "did this
level just become empty/non-empty", which is rare once about 50 orders rest per level. That is why
C's rate is half the others' and its total is 4.6× below A's.

### 4.3 Cache behaviour
**B has the most L1 misses (25.2 M), then A (10.9 M), then C (3.3 M). dTLB misses follow the same
order (40.4 M / 17.6 M / 2.3 M).**
- **A follows pointers and allocates many small objects.** Every order is a heap-allocated hash
  node, and every level is a heap-allocated tree node. A lookup jumps across about 13 scattered
  nodes plus a hash node. Each jump can be a miss, and the next load can't start until the
  previous one returns.
- **B is contiguous for levels and scattered for everything else.** The level vector and its
  binary search are cache-friendly. But the id tree has 500k scattered nodes, and every level's
  order list is a separate heap block. A cancel touches both, which is why B misses most and
  why its dTLB misses are highest: the accesses span many pages.
- **C accesses contiguous memory.** Its price arrays total 80 KB, so they stay in L1/L2. The
  order table is one 32 MB array, and a probe is usually one cache line. That line is random,
  because ids arrive in random order, so it is the one miss C cannot avoid. C's 3.3 M total also
  includes the misses from generating the workload.

These misses explain the IPC. A and B both sit at 1.07 instructions per cycle on a core that can
retire several per cycle, so they spend most cycles waiting for memory. C reaches 1.64 while
executing 1.8× fewer instructions than A.

### 4.4 Page faults
**B causes the most page faults (17,700). A (15,103) and C (15,223) are close.** Roughly 6,000
faults in every column (24 MB ÷ 4 KB pages) come from the pre-generated workload, which is
identical for all three.
- **B** keeps requesting fresh memory throughout the run: 10,000 per-level order vectors that
  grow by doubling (each growth allocates a new block and frees the old one) and 500k id-tree nodes.
- **A** faults steadily as it allocates about 700k hash nodes.
- **C** takes almost all its faults *at construction*, when it writes the empty marker into each
  of its 2²¹ slots (32 MB = 8,192 pages). After that the hot loop never allocates.
C's total is similar to A's, but its faults all happen at start-up instead of while trading.

### 4.5 Production choice
I would deploy **C**.
- **Latency:** lowest total time, and the lowest add and cancel latency.
- **Predictability:** no allocation after start-up, no rebalancing, no reallocation. Its worst
  cases are bounded: a scan of 79 bitmap words when the best level empties, and a probe loop
  capped by the load factor. A and B call the allocator on every add and cancel and rebalance
  trees, which is where tail-latency spikes come from.
- **Memory:** a fixed 80 KB for prices plus 16 B per table slot. It is reserved up front, which
  costs 32 MB even for an empty book. That is a deliberate trade for no runtime allocation.
- **Cache behaviour:** 3.3–7.6× fewer L1 misses and 7.5–17× fewer dTLB misses than A and B.
- **Complexity and maintainability:** C is the most code, and its hash-table deletion needs a
  careful reviewer. The lock-step `--verify` test makes that acceptable. C also depends on the
  fixed tick size and 10,000-level band. If prices were unbounded, A would be the safer choice.
  B's layout of orders inside levels suits FIFO matching, which this book doesn't need.

### 4.6 What we would try next
1. Index orders by id in a plain array when ids are dense sequence numbers, removing hashing entirely.
2. Pack each order slot into 8 bytes to halve the table's cache and TLB footprint.
3. Use huge pages for the 32 MB table. C's dTLB misses (2.3 M) are close to its L1 misses (3.3 M).
4. Use a two-level bitmap so the worst-case "next level" search is 2 loads, not 79.
5. Index the two sides as `side_[int(side)]` to remove the buy/sell branch.
6. Give A and B pool allocators, to measure how much of their gap is allocator cost and how much
   is pointer chasing.
7. Time C's operations in batches rather than per call, since the clock costs more than the operation.

## 5. perf output

### 5.1 `perf stat -r 5` — Implementation A
```
 Performance counter stats for './bench --impl A --perf' (5 runs):
            302.47 msec task-clock                                                              ( +-  0.48% )
         742851072      cycles                                                                  ( +-  0.47% )  (42.54%)
         795296019      instructions                                                            ( +-  0.48% )  (42.59%)
         156818194      branches                                                                ( +-  0.23% )  (42.92%)
           8543272      branch-misses                                                           ( +-  0.50% )  (28.74%)
          10929271      cache-misses                                                            ( +-  0.36% )  (28.73%)
          10923235      L1-dcache-load-misses                                                   ( +-  0.23% )  (28.67%)
          17554571      dTLB-load-misses                                                        ( +-  0.80% )  (28.35%)
             15103      page-faults                                                             ( +-  0.00% )
       0.305069301 +- 0.001471233 seconds time elapsed  ( +-  0.48% )
```
### 5.2 `perf stat -r 5` — Implementation B
```
 Performance counter stats for './bench --impl B --perf' (5 runs):
            573.06 msec task-clock                                                              ( +-  0.75% )
        1403229455      cycles                                                                  ( +-  0.72% )  (42.61%)
        1507807960      instructions                                                            ( +-  0.21% )  (42.79%)
         286597959      branches                                                                ( +-  0.12% )  (42.97%)
          15969673      branch-misses                                                           ( +-  0.22% )  (28.75%)
          25233190      cache-misses                                                            ( +-  0.50% )  (28.63%)
          25218384      L1-dcache-load-misses                                                   ( +-  0.71% )  (28.45%)
          40442071      dTLB-load-misses                                                        ( +-  0.73% )  (28.40%)
             17700      page-faults                                                             ( +-  0.00% )
       0.577182758 +- 0.004275806 seconds time elapsed  ( +-  0.74% )
```
### 5.3 `perf stat -r 5` — Implementation C
```
 Performance counter stats for './bench --impl C --perf' (5 runs):
            108.81 msec task-clock                                                              ( +-  0.28% )
         267452857      cycles                                                                  ( +-  0.26% )  (42.42%)
         438755509      instructions                                                            ( +-  0.28% )  (43.32%)
          69684402      branches                                                                ( +-  0.15% )  (44.23%)
           1841457      branch-misses                                                           ( +-  0.91% )  (28.96%)
           3265732      cache-misses                                                            ( +-  0.85% )  (28.02%)
           3309602      L1-dcache-load-misses                                                   ( +-  0.49% )  (27.72%)
           2336504      dTLB-load-misses                                                        ( +-  1.14% )  (27.75%)
             15223      page-faults                                                             ( +-  0.00% )
       0.109596579 +- 0.000313743 seconds time elapsed  ( +-  0.29% )
```
### 5.4 `perf record -g` — top functions (self time)
| A | B | C |
|---|---|---|
| 35.0 % `OrderBookA::add_order` | 33.3 % `OrderBookB::cancel_order` | 34.8 % `OrderBookC::add_order` |
| 25.3 % `OrderBookA::cancel_order` | 22.2 % `OrderBookB::add_order` | 15.6 % `make_workload` (not timed) |
| 6.3 % `make_workload` (not timed) | 7.4 % `vector<PriceLevel>::_M_insert_aux` | 7.5 % RNG distribution (not timed) |
| 4.3 % + 4.0 % `std::_Hashtable` internals | 5.9 % `_Rb_tree_rebalance_for_erase` | 7.3 % `OrderBookC::cancel_order` |
| 3.0 % `unordered_map::emplace` | 4.7 % `std::_Rb_tree` (id map) | ≈10 % kernel code (unresolved `[k]` symbols) |

Full reports: `results/perf_report_{A,B,C}.txt`.
