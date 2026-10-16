# Low-Latency Order Book — Capstone (Intermediate C++)

**Team**

| Name | Roll number | SST email |
|---|---|---|
| Shambhu Yadav | 24bcs10356 | shambhu.24bcs10356@sst.scaler.com |
| Ratnesh Vaibhav | 24bcs10413 | ratnesh.24bcs10413@sst.scaler.com |

Three genuinely different single-instrument resting order books behind the same
interface, one benchmark program that selects the implementation on the command
line, a lock-step correctness test (`--verify`), and a perf-based analysis
(`REPORT.md`).

```
include/order_book_common.hpp   Side, Level, BBO, tick helpers (shared)
include/book_a_map.hpp          Implementation A : std::map levels + std::unordered_map orders
include/book_b_vector.hpp       Implementation B : sorted std::vector of levels, orders stored in the level, std::map id lookup
include/book_c_array.hpp        Implementation C : price-indexed arrays + bitmap + custom open-addressing hash
src/workload.hpp                deterministic workload generator (mt19937(42))
src/bench.cpp                   benchmark + --verify + --perf
scripts/perf_all.sh             runs perf stat / perf record for A, B, C (Linux)
results/                        Linux (EC2 c7g.large) verify, bench and perf outputs; results/macos/ has Mac runs
REPORT.md                       results table + performance analysis
```

## Build

Single compiler command (C++17, `-O2`):

```sh
g++ -std=c++17 -O2 -Wall -Wextra -g -fno-omit-frame-pointer -Iinclude -Isrc src/bench.cpp -o bench
```

or simply

```sh
make            # same command as above
# or
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build   # produces build/bench
```

`-g` and `-fno-omit-frame-pointer` only add symbols / keep frame pointers so
that `perf record -g` gives readable call graphs. Code generation is plain `-O2`.
No external libraries; the standard library only.

## Run the correctness test

```sh
./bench --verify             # 1,000,000 ops, seed 42
./bench --verify --ops 200000 --seed 7
```

The test (1) runs hand-written edge cases on every implementation (empty-book
BBO, cancel of an unknown id, double cancel, `get_bids(10)` on a 3-level book,
BBO moving to the next level when the best level empties, re-using an emptied
level, zero / negative / huge depth requests, rejection of out-of-band prices,
qty <= 0 and duplicate ids) and (2) feeds the identical random workload to A, B
and C in lock-step,
comparing after **every** operation the `cancel_order` return value, the BBO,
`get_bids(10)` and `get_asks(10)` (price and quantity of every level). It also
checks that the benchmark path produces the same checksum for all three.

Captured output (`results/verify.txt`):

```
Running correctness test...

Edge cases (empty book, unknown/double cancel, insufficient depth, invalid input):
  A: PASS   B: PASS   C: PASS
Lock-step workload comparison (1010000 ops, BBO + 10-level depth compared after every op):
Benchmark checksums: A=6207609776843994971 B=6207609776843994971 C=6207609776843994971

Implementation A: PASS
Implementation B: PASS
Implementation C: PASS

All implementations produce identical results.
```

## Run the benchmarks

```sh
./bench --impl A         # 5 runs, median reported
./bench --impl B
./bench --impl C
./bench --impl C --runs 3 --ops 2000000 --seed 1
```

One program, one workload (`src/workload.hpp`), implementation chosen with
`--impl`. The workload is generated **before** timing starts, so RNG cost and
the live-order bookkeeping needed to pick cancel targets are never timed.

Workload (seed 42): 1,000,000 operations, ~70 % add / 20 % cancel / 10 %
`get_bbo`, side 50/50, qty 1–100, uniformly random valid tick in the band, every
cancel targets a currently-live order (if nothing is live the slot becomes an
add — this happens 6 times at the very start). After every 100 operations one
extra "depth" operation calls `get_bids(10)` + `get_asks(10)` (10,000 of them,
on top of the 1,000,000).

The checksum depends on the C++ standard library. `std::mt19937` itself is fully
specified, but `std::uniform_int_distribution` is implemented differently by
libstdc++ (GCC, Linux) and libc++ (Apple clang), so the same seed produces a
different op stream on each. All three implementations always run the identical
stream on a given machine, so the comparison is unaffected. For reference, seed 42
gives checksum `9406903409647291425` with GCC 13 / libstdc++ and
`6207609776843994971` with Apple clang 17 / libc++.

Each run does two passes over the same op stream:

1. **uninstrumented** — one `steady_clock` read before and after the loop →
   *Total time*;
2. **instrumented** — a `steady_clock` pair around every single call →
   ns / add, ns / cancel, ns / bbo, ns / depth. The measured cost of a clock
   pair (≈63 ns on the Graviton3 test machine, ≈25 ns on an Apple M3) is printed and a "net" column subtracts it.
   Because a `get_bbo` is far cheaper than a clock read, a third number is
   reported: 1,000,000 back-to-back `get_bbo()` calls timed as a block.

The final BBO and a checksum (folded from every BBO/depth result and every
cancel return value) are printed so the compiler cannot discard the work.

For `perf`, use `--perf`: one uninstrumented pass, no timers, no bbo burst, so
the counters are dominated by the book itself:

```sh
perf stat -r 5 -e task-clock,cycles,instructions,branches,branch-misses,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults ./bench --impl A --perf
perf record -g ./bench --impl A --perf && perf report
./scripts/perf_all.sh      # all of the above for A, B and C -> results/perf_*.txt
```

## Interface changes

The required API is kept, with these additions / notes:

* **Integer ticks.** Internally every implementation stores prices as `int`
  ticks (1 tick = 0.01, `100.01 → 10001`). `add_order(id, side, double price,
  qty)` is still provided and converts with `lround(price * 100)`; each class
  also has `add_order(id, side, int tick, qty)` which is what the benchmark
  calls, so no float conversion sits inside the timed region. `Level.price`
  and `BBO.*_price` are still `double` (`tick / 100.0`).
* `Side`, `Level`, `BBO` are exactly as specified (`order_book_common.hpp`).
* The three classes are `OrderBookA`, `OrderBookB`, `OrderBookC`; they are
  used through templates (not a virtual base) so the benchmark pays no
  virtual-call cost that would blur the comparison.
* `add_order` returns `bool` instead of `void`. It returns `false` and leaves
  the book unchanged when the price is outside its side's band (bids
  50.00–99.99, asks 100.01–150.00), when `qty <= 0` or `qty > INT32_MAX`, or
  when the id is already live. This keeps C's price arrays from being indexed
  out of bounds and keeps all three books in agreement on bad input.
* Implementation C stores the quantity of an order as `int32_t` inside its
  16-byte hash slot (quantities are 1–100 here); level totals are `long`.

## Data-structure choices

| | A — tree | B — sorted vector, level-centric | C — price-indexed arrays (own design) |
|---|---|---|---|
| Orders | `std::unordered_map<id, {tick, qty, side}>` | `std::vector<{id, qty}>` **inside each price level**; `std::map<id, {tick, side}>` (tree) only says which level | custom open-addressing hash table, linear probing, backward-shift delete, 16-byte slots `{id, tick, qty}` |
| Price levels | `std::map<tick, qty>` per side (red-black tree) | `std::vector<PriceLevel>` per side, kept sorted, best at the back | `long qty[5000]` per side indexed by tick rank + 5000-bit occupancy bitmap |
| Best price | `map.begin()` | `vector.back()` | cached `best` rank; on emptying, `ctz` scan of the bitmap |
| Cancel lookup | hash → tick → `map.find` | id tree → tick → binary search → linear scan in level | probe → tick → direct array index |
| add | O(1) avg + O(log L) | O(log N) + O(log L), + O(L) memmove for a new level | O(1) |
| cancel | O(1) avg + O(log L) | O(log N) + O(log L) + O(k) | O(1) (worst O(L/64)) |
| get_bbo | O(1) | O(1) | O(1) |
| get_bids/asks(d) | O(d) | O(d) | O(d + L/64) |

L = non-empty price levels on the side (≤ 5000), N = live orders, k = orders at one level.

### Design details

**A — ordered tree.** Orders live in `std::unordered_map<id, {tick, qty, side}>`. Levels are
`std::map<int, long>` per side; bids use `std::greater` so `begin()` is the best bid on both
sides. Cancel: hash find → `map.find(tick)` → subtract → erase the node if it reaches zero.

**B — sorted vector, level-centric.** Each side is a `std::vector<PriceLevel>` sorted so the
best price is at the back (bids ascending, asks descending). Each `PriceLevel` holds its tick,
its total, and a `std::vector<{id, qty}>` of the orders resting there. A `std::map<id, {tick, side}>`
(red-black tree keyed by id) records which level an order is in. Cancel: tree find → binary
search → linear scan of the level → swap-remove → erase the level if empty.

**C — price-indexed arrays (own design).** Each side is `long qty[5000]` indexed by rank
(0 = best possible price: bids `9999 - tick`, asks `tick - 10001`) plus a 79-word bitmap of
non-empty levels and a cached `best` rank. When the best level empties, the next one is found
with `__builtin_ctzll` over the bitmap. Orders live in a custom open-addressing hash table:
16-byte slots `{id, tick, qty}`, Fibonacci hashing, linear probing, load factor ≤ 0.5 and
backward-shift deletion (no tombstones). The side is implied by the tick. The table is
allocated and filled once at construction, so the hot path never allocates.

## Collecting perf data on Linux

```sh
make
./bench --verify
for i in A B C; do ./bench --impl $i | tee results/bench_$i.txt; done
./scripts/perf_all.sh       # writes results/perf_stat_{A,B,C}.txt and results/perf_report_{A,B,C}.txt
```

If `perf` reports permission errors, run `sudo sysctl kernel.perf_event_paranoid=1` (or ask the
lab admin). Events shown as `<not supported>` should be listed as unavailable in REPORT.md.
