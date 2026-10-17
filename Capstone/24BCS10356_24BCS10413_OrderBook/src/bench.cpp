// Single benchmark + correctness program for all three order-book designs.
//
//   ./bench --impl A|B|C [--runs 5] [--ops 1000000] [--seed 42]
//   ./bench --impl A|B|C --perf          (one uninstrumented pass only; for perf stat/record)
//   ./bench --verify     [--ops 1000000] [--seed 42]
//
// Timing uses std::chrono::steady_clock. Each run performs two passes over the
// identical pre-generated workload:
//   1. an *uninstrumented* pass (one timer around the whole loop) -> total time
//   2. an *instrumented* pass (timer around every call) -> ns per op type
// so the per-op numbers do not pollute the total-time figure. The measured
// cost of a steady_clock::now() pair is printed so it can be subtracted from
// the per-op averages if desired.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "book_a_map.hpp"
#include "book_b_vector.hpp"
#include "book_c_array.hpp"
#include "workload.hpp"

using Clock = std::chrono::steady_clock;

static inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// ----------------------------------------------------------------------------
// Checksum helpers (keep the compiler honest and give --verify something to compare)
// ----------------------------------------------------------------------------
static inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
static inline uint64_t fold_bbo(uint64_t h, const BBO& b) {
    h = mix(h, b.has_bid ? static_cast<uint64_t>(to_tick(b.bid_price)) * 1000003ull + b.bid_qty : 1);
    h = mix(h, b.has_ask ? static_cast<uint64_t>(to_tick(b.ask_price)) * 1000003ull + b.ask_qty : 2);
    return h;
}
static inline uint64_t fold_levels(uint64_t h, const std::vector<Level>& v) {
    h = mix(h, v.size());
    for (const Level& l : v) h = mix(h, static_cast<uint64_t>(to_tick(l.price)) * 1000003ull + l.qty);
    return h;
}

// ----------------------------------------------------------------------------
// Benchmark
// ----------------------------------------------------------------------------
struct RunResult {
    double total_ms = 0;        // uninstrumented pass
    double ns_add = 0, ns_cancel = 0, ns_bbo = 0, ns_depth = 0;  // instrumented pass
    double ns_bbo_burst = 0;    // 1M back-to-back get_bbo() calls, timed as a block
    uint64_t checksum = 0;
    BBO final_bbo{};
};

template <class Book>
static RunResult run_once(const Workload& w) {
    RunResult r;

    // ---- pass 1: whole-loop timing ------------------------------------
    {
        Book book;
        uint64_t chk = 0;
        const uint64_t t0 = now_ns();
        for (const Op& op : w.ops) {
            switch (op.type) {
                case OpType::Add: book.add_order(op.id, op.side, op.tick, op.qty); break;
                case OpType::Cancel: chk += book.cancel_order(op.id); break;
                case OpType::Bbo: chk = fold_bbo(chk, book.get_bbo()); break;
                case OpType::Depth:
                    chk = fold_levels(chk, book.get_bids(10));
                    chk = fold_levels(chk, book.get_asks(10));
                    break;
            }
        }
        const uint64_t t1 = now_ns();
        r.total_ms = static_cast<double>(t1 - t0) / 1e6;
        r.checksum = fold_bbo(chk, book.get_bbo());
        r.final_bbo = book.get_bbo();

        // get_bbo is far cheaper than a clock read, so additionally time a
        // burst of 1M back-to-back get_bbo() calls on the final book state.
        const int kBurst = 1'000'000;
        uint64_t hb = 0;
        const uint64_t b0 = now_ns();
        for (int i = 0; i < kBurst; ++i) hb = fold_bbo(hb, book.get_bbo());
        const uint64_t b1 = now_ns();
        r.ns_bbo_burst = static_cast<double>(b1 - b0) / kBurst;
        if (hb == 0x1234) std::printf("(unlikely)\n");  // keep the loop alive
    }

    // ---- pass 2: per-operation timing ---------------------------------
    {
        Book book;
        uint64_t chk = 0;
        uint64_t t_add = 0, t_cancel = 0, t_bbo = 0, t_depth = 0;
        for (const Op& op : w.ops) {
            switch (op.type) {
                case OpType::Add: {
                    const uint64_t a = now_ns();
                    book.add_order(op.id, op.side, op.tick, op.qty);
                    t_add += now_ns() - a;
                    break;
                }
                case OpType::Cancel: {
                    const uint64_t a = now_ns();
                    chk += book.cancel_order(op.id);
                    t_cancel += now_ns() - a;
                    break;
                }
                case OpType::Bbo: {
                    const uint64_t a = now_ns();
                    const BBO b = book.get_bbo();
                    t_bbo += now_ns() - a;
                    chk = fold_bbo(chk, b);
                    break;
                }
                case OpType::Depth: {
                    const uint64_t a = now_ns();
                    const std::vector<Level> bids = book.get_bids(10);
                    const std::vector<Level> asks = book.get_asks(10);
                    t_depth += now_ns() - a;
                    chk = fold_levels(chk, bids);
                    chk = fold_levels(chk, asks);
                    break;
                }
            }
        }
        const uint64_t chk2 = fold_bbo(chk, book.get_bbo());
        if (chk2 != r.checksum) {
            std::fprintf(stderr, "internal error: passes disagree (%llu vs %llu)\n",
                         (unsigned long long)r.checksum, (unsigned long long)chk2);
            std::exit(2);
        }
        r.ns_add = w.n_add ? static_cast<double>(t_add) / w.n_add : 0;
        r.ns_cancel = w.n_cancel ? static_cast<double>(t_cancel) / w.n_cancel : 0;
        r.ns_bbo = w.n_bbo ? static_cast<double>(t_bbo) / w.n_bbo : 0;
        r.ns_depth = w.n_depth ? static_cast<double>(t_depth) / w.n_depth : 0;
    }
    return r;
}

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static double timer_overhead_ns() {
    const int n = 1'000'000;
    uint64_t acc = 0;
    const uint64_t t0 = now_ns();
    for (int i = 0; i < n; ++i) {
        const uint64_t a = now_ns();
        acc += now_ns() - a;
    }
    const uint64_t t1 = now_ns();
    (void)acc;
    return static_cast<double>(t1 - t0) / n;
}

// --perf: exactly one uninstrumented pass, no per-op timers, no bbo burst, so
// that `perf stat` / `perf record` counters are dominated by the book itself.
template <class Book>
static int run_perf(const char* name, const Workload& w) {
    Book book;
    uint64_t chk = 0;
    const uint64_t t0 = now_ns();
    for (const Op& op : w.ops) {
        switch (op.type) {
            case OpType::Add: book.add_order(op.id, op.side, op.tick, op.qty); break;
            case OpType::Cancel: chk += book.cancel_order(op.id); break;
            case OpType::Bbo: chk = fold_bbo(chk, book.get_bbo()); break;
            case OpType::Depth:
                chk = fold_levels(chk, book.get_bids(10));
                chk = fold_levels(chk, book.get_asks(10));
                break;
        }
    }
    const uint64_t t1 = now_ns();
    const BBO b = book.get_bbo();
    std::printf("Implementation %s (--perf: single pass, %zu ops): %.2f ms\n", name, w.ops.size(),
                static_cast<double>(t1 - t0) / 1e6);
    std::printf("Final BBO:\n");
    if (b.has_bid) std::printf("  Bid: %.2f x %ld\n", b.bid_price, b.bid_qty); else std::printf("  Bid: (none)\n");
    if (b.has_ask) std::printf("  Ask: %.2f x %ld\n", b.ask_price, b.ask_qty); else std::printf("  Ask: (none)\n");
    std::printf("Checksum: %llu\n", (unsigned long long)fold_bbo(chk, b));
    return 0;
}

template <class Book>
static int run_bench(const char* name, const Workload& w, int runs) {
    std::printf("Implementation %s  (%zu ops: %zu add, %zu cancel, %zu bbo, +%zu depth queries; %d runs)\n",
                name, w.n_add + w.n_cancel + w.n_bbo, w.n_add, w.n_cancel, w.n_bbo, w.n_depth, runs);
    std::vector<double> total, add, cancel, bbo, depth, burst;
    RunResult last{};
    for (int i = 0; i < runs; ++i) {
        last = run_once<Book>(w);
        total.push_back(last.total_ms);
        add.push_back(last.ns_add);
        cancel.push_back(last.ns_cancel);
        bbo.push_back(last.ns_bbo);
        depth.push_back(last.ns_depth);
        burst.push_back(last.ns_bbo_burst);
        std::printf("  run %d: total %.2f ms | add %.1f ns | cancel %.1f ns | bbo %.1f ns | depth(10+10) %.1f ns\n",
                    i + 1, last.total_ms, last.ns_add, last.ns_cancel, last.ns_bbo, last.ns_depth);
    }
    const double ovh = timer_overhead_ns();
    auto net = [&](double raw) { return raw > ovh ? raw - ovh : 0.0; };
    std::printf("\nMedian of %d runs (impl %s):          raw      net of timer overhead\n", runs, name);
    std::printf("  Total time        : %10.2f ms   (uninstrumented pass, %zu ops)\n", median(total), w.ops.size());
    std::printf("  ns / add_order    : %10.1f   %10.1f\n", median(add), net(median(add)));
    std::printf("  ns / cancel_order : %10.1f   %10.1f\n", median(cancel), net(median(cancel)));
    std::printf("  ns / get_bbo      : %10.1f   %10.1f   (per-call timing; below timer resolution)\n",
                median(bbo), net(median(bbo)));
    std::printf("  ns / get_bbo burst: %10.2f              (1M back-to-back calls on final book)\n",
                median(burst));
    std::printf("  ns / depth query  : %10.1f   %10.1f   (get_bids(10) + get_asks(10))\n", median(depth),
                net(median(depth)));
    std::printf("  timer overhead    : %10.1f ns per steady_clock::now() pair\n", ovh);

    const BBO& b = last.final_bbo;
    std::printf("\nFinal BBO:\n");
    if (b.has_bid) std::printf("  Bid: %.2f x %ld\n", b.bid_price, b.bid_qty); else std::printf("  Bid: (none)\n");
    if (b.has_ask) std::printf("  Ask: %.2f x %ld\n", b.ask_price, b.ask_qty); else std::printf("  Ask: (none)\n");
    std::printf("\nChecksum: %llu\n", (unsigned long long)last.checksum);
    return 0;
}

// ----------------------------------------------------------------------------
// Correctness: lock-step comparison of all three books on the same workload
// ----------------------------------------------------------------------------
static bool same_levels(const std::vector<Level>& a, const std::vector<Level>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

template <class Book>
static bool edge_cases(const char* name) {
    bool ok = true;
    Book book;
    BBO e = book.get_bbo();
    if (e.has_bid || e.has_ask) { std::printf("  [%s] empty book BBO reported a side\n", name); ok = false; }
    if (!book.get_bids(10).empty() || !book.get_asks(10).empty()) { std::printf("  [%s] empty book depth not empty\n", name); ok = false; }
    if (book.cancel_order(12345)) { std::printf("  [%s] cancel of unknown id returned true\n", name); ok = false; }

    book.add_order(1, Side::Buy, 99.98, 450);
    book.add_order(2, Side::Buy, 99.98, 50);
    book.add_order(3, Side::Buy, 99.50, 10);
    book.add_order(4, Side::Buy, 60.00, 7);
    book.add_order(5, Side::Sell, 100.01, 320);
    book.add_order(6, Side::Sell, 120.00, 1);
    BBO b = book.get_bbo();
    if (!(b.has_bid && to_tick(b.bid_price) == 9998 && b.bid_qty == 500 && b.has_ask && to_tick(b.ask_price) == 10001 && b.ask_qty == 320)) {
        std::printf("  [%s] BBO wrong after adds\n", name); ok = false;
    }
    std::vector<Level> bids = book.get_bids(10);
    if (bids.size() != 3 || to_tick(bids[0].price) != 9998 || bids[0].qty != 500 || to_tick(bids[1].price) != 9950 || to_tick(bids[2].price) != 6000) {
        std::printf("  [%s] get_bids(10) with 3 levels wrong (size %zu)\n", name, bids.size()); ok = false;
    }
    if (book.get_asks(1).size() != 1) { std::printf("  [%s] get_asks(1) size wrong\n", name); ok = false; }
    if (book.get_bids(0).size() != 0 || book.get_asks(-3).size() != 0 || book.get_bids(2147483647).size() != 3) {
        std::printf("  [%s] zero / negative / huge depth handled wrong\n", name); ok = false;
    }
    if (!book.cancel_order(1)) { std::printf("  [%s] cancel of live order returned false\n", name); ok = false; }
    if (book.cancel_order(1)) { std::printf("  [%s] double cancel returned true\n", name); ok = false; }
    b = book.get_bbo();
    if (!(b.has_bid && to_tick(b.bid_price) == 9998 && b.bid_qty == 50)) { std::printf("  [%s] BBO wrong after partial level cancel\n", name); ok = false; }
    book.cancel_order(2);
    b = book.get_bbo();
    if (!(b.has_bid && to_tick(b.bid_price) == 9950 && b.bid_qty == 10)) { std::printf("  [%s] BBO did not fall to next level\n", name); ok = false; }
    book.cancel_order(5);
    b = book.get_bbo();
    if (!(b.has_ask && to_tick(b.ask_price) == 12000 && b.ask_qty == 1)) { std::printf("  [%s] ask BBO did not fall to next level\n", name); ok = false; }
    book.cancel_order(3); book.cancel_order(4); book.cancel_order(6);
    b = book.get_bbo();
    if (b.has_bid || b.has_ask) { std::printf("  [%s] book not empty after cancelling everything\n", name); ok = false; }
    // level re-use after it was emptied
    book.add_order(7, Side::Sell, 100.01, 3);
    b = book.get_bbo();
    if (!(b.has_ask && to_tick(b.ask_price) == 10001 && b.ask_qty == 3)) { std::printf("  [%s] re-adding to an emptied level failed\n", name); ok = false; }
    book.cancel_order(7);

    // invalid input must be rejected and leave the book untouched
    const bool rejected =
        !book.add_order(10, Side::Buy, 120.00, 5) &&    // buy priced in the ask band
        !book.add_order(11, Side::Sell, 99.00, 5) &&    // sell priced in the bid band
        !book.add_order(12, Side::Buy, 49.99, 5) &&     // below bid band
        !book.add_order(13, Side::Sell, 150.01, 5) &&   // above ask band
        !book.add_order(14, Side::Buy, 100.00, 5) &&    // the mid price is in neither band
        !book.add_order(15, Side::Buy, 99.00, 0) &&     // zero quantity
        !book.add_order(16, Side::Sell, 101.00, -5) &&  // negative quantity
        !book.add_order(17, Side::Buy, 42949772.95, 5) &&  // 2^32 + 9999 ticks: used to wrap to 99.99
        !book.add_order(18, Side::Sell, 100.013, 5) &&  // not a multiple of the 0.01 tick
        !book.add_order(19, Side::Buy, std::nan(""), 5);  // not a number
    b = book.get_bbo();
    if (!rejected || b.has_bid || b.has_ask || book.cancel_order(10)) {
        std::printf("  [%s] invalid order was accepted\n", name); ok = false;
    }
    if (!book.add_order(20, Side::Buy, 99.00, 1) || book.add_order(20, Side::Buy, 98.00, 1)) {
        std::printf("  [%s] duplicate id handling wrong\n", name); ok = false;
    }
    // the largest uint64_t id is a legal id (C stores it outside its hash table)
    const uint64_t max_id = ~0ull;
    const bool max_added = book.add_order(max_id, Side::Sell, 150.00, 2);
    const std::vector<Level> top_ask = book.get_asks(1);
    if (!max_added || book.add_order(max_id, Side::Sell, 149.00, 2) || top_ask.size() != 1 ||
        to_tick(top_ask[0].price) != 15000 || top_ask[0].qty != 2 || !book.cancel_order(max_id) ||
        book.cancel_order(max_id) || book.get_bbo().has_ask) {
        std::printf("  [%s] id 2^64-1 handling wrong\n", name); ok = false;
    }
    return ok;
}

static int run_verify(const Workload& w) {
    std::printf("Running correctness test...\n\n");
    std::printf("Edge cases (empty book, unknown/double cancel, insufficient depth, invalid input):\n");
    bool okA = edge_cases<OrderBookA>("A");
    bool okB = edge_cases<OrderBookB>("B");
    bool okC = edge_cases<OrderBookC>("C");
    std::printf("  A: %s   B: %s   C: %s\n", okA ? "PASS" : "FAIL", okB ? "PASS" : "FAIL", okC ? "PASS" : "FAIL");

    std::printf("Lock-step workload comparison (%zu ops, BBO + 10-level depth compared after every op):\n",
                w.ops.size());
    OrderBookA a; OrderBookB b; OrderBookC c;
    size_t mismatches_B = 0, mismatches_C = 0, first_bad = 0;
    for (size_t i = 0; i < w.ops.size(); ++i) {
        const Op& op = w.ops[i];
        if (op.type == OpType::Add) {
            a.add_order(op.id, op.side, op.tick, op.qty);
            b.add_order(op.id, op.side, op.tick, op.qty);
            c.add_order(op.id, op.side, op.tick, op.qty);
        } else if (op.type == OpType::Cancel) {
            const bool ra = a.cancel_order(op.id), rb = b.cancel_order(op.id), rc = c.cancel_order(op.id);
            if (!ra) { std::printf("  A: cancel of live order %llu returned false\n", (unsigned long long)op.id); okA = false; }
            if (rb != ra) ++mismatches_B;
            if (rc != ra) ++mismatches_C;
        }
        const BBO ba = a.get_bbo(), bb = b.get_bbo(), bc = c.get_bbo();
        const auto bidsA = a.get_bids(10), bidsB = b.get_bids(10), bidsC = c.get_bids(10);
        const auto asksA = a.get_asks(10), asksB = b.get_asks(10), asksC = c.get_asks(10);
        const bool okB_i = (bb == ba) && same_levels(bidsB, bidsA) && same_levels(asksB, asksA);
        const bool okC_i = (bc == ba) && same_levels(bidsC, bidsA) && same_levels(asksC, asksA);
        if (!okB_i) ++mismatches_B;
        if (!okC_i) ++mismatches_C;
        if ((!okB_i || !okC_i) && first_bad == 0) first_bad = i + 1;
    }
    okB = okB && mismatches_B == 0;
    okC = okC && mismatches_C == 0;
    if (first_bad) std::printf("  first mismatch at op #%zu\n", first_bad);

    // independent cross-check: same checksum from the benchmark path
    const uint64_t ca = run_once<OrderBookA>(w).checksum;
    const uint64_t cb = run_once<OrderBookB>(w).checksum;
    const uint64_t cc = run_once<OrderBookC>(w).checksum;
    std::printf("Benchmark checksums: A=%llu B=%llu C=%llu\n\n", (unsigned long long)ca, (unsigned long long)cb,
                (unsigned long long)cc);
    okB = okB && cb == ca;
    okC = okC && cc == ca;

    std::printf("Implementation A: %s\n", okA ? "PASS" : "FAIL");
    std::printf("Implementation B: %s\n", okB ? "PASS" : "FAIL");
    std::printf("Implementation C: %s\n", okC ? "PASS" : "FAIL");
    const bool all = okA && okB && okC;
    std::printf("\n%s\n", all ? "All implementations produce identical results."
                              : "MISMATCH: implementations disagree.");
    return all ? 0 : 1;
}

// ----------------------------------------------------------------------------
static void usage() {
    std::printf("usage: bench --impl A|B|C [--runs N] [--ops N] [--seed S]\n"
                "       bench --impl A|B|C --perf        (single uninstrumented pass for perf)\n"
                "       bench --verify      [--ops N] [--seed S]\n");
}

int main(int argc, char** argv) {
    std::string impl;
    bool verify = false, perf = false;
    int runs = 5;
    size_t ops = 1'000'000;
    uint32_t seed = 42;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (a == "--impl") impl = next("--impl");
        else if (a == "--verify") verify = true;
        else if (a == "--perf") perf = true;
        else if (a == "--runs") runs = std::atoi(next("--runs"));
        else if (a == "--ops") ops = static_cast<size_t>(std::atoll(next("--ops")));
        else if (a == "--seed") seed = static_cast<uint32_t>(std::atoll(next("--seed")));
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); usage(); return 1; }
    }
    if (!verify && impl.empty()) { usage(); return 1; }
    if (runs < 1) runs = 1;

    const Workload w = make_workload(ops, seed);
    if (verify) return run_verify(w);
    if (impl == "A" || impl == "a") return perf ? run_perf<OrderBookA>("A", w) : run_bench<OrderBookA>("A", w, runs);
    if (impl == "B" || impl == "b") return perf ? run_perf<OrderBookB>("B", w) : run_bench<OrderBookB>("B", w, runs);
    if (impl == "C" || impl == "c") return perf ? run_perf<OrderBookC>("C", w) : run_bench<OrderBookC>("C", w, runs);
    std::fprintf(stderr, "unknown impl '%s' (expected A, B or C)\n", impl.c_str());
    return 1;
}
