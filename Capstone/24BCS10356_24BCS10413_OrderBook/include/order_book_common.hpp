// Common types shared by all three order-book implementations.
//
// Interface change vs. the assignment: prices are handled internally as
// integer *ticks* (1 tick = 0.01). 100.01 -> 10001. The public API still
// accepts/returns doubles (add_order(double), Level.price, BBO.*_price), but
// every implementation also exposes a tick-based add_order overload, which is
// what the benchmark uses so that all three designs are measured on the same
// footing (no double->int conversion inside the timed region).
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

enum class Side { Buy, Sell };

struct Level {
    double price;
    long qty;
};

struct BBO {
    bool has_bid;
    double bid_price;
    long bid_qty;

    bool has_ask;
    double ask_price;
    long ask_qty;
};

// Price band in ticks.
constexpr int kBidMinTick = 5000;   // 50.00
constexpr int kBidMaxTick = 9999;   // 99.99
constexpr int kAskMinTick = 10001;  // 100.01
constexpr int kAskMaxTick = 15000;  // 150.00
constexpr int kLevelsPerSide = 5000;

inline int to_tick(double price) { return static_cast<int>(std::lround(price * 100.0)); }
inline double to_price(int tick) { return static_cast<double>(tick) / 100.0; }

inline bool valid_tick(Side side, int tick) {
    return side == Side::Buy ? (tick >= kBidMinTick && tick <= kBidMaxTick)
                             : (tick >= kAskMinTick && tick <= kAskMaxTick);
}

// Number of levels to reserve for a depth query: never more than one side can
// hold, so get_bids(INT_MAX) does not try to allocate gigabytes.
inline size_t depth_capacity(int depth) {
    if (depth <= 0) return 0;
    return static_cast<size_t>(depth < kLevelsPerSide ? depth : kLevelsPerSide);
}

// An order is accepted only if its price lies inside its own side's band and
// its quantity is positive (and fits the 32-bit per-order field used by C).
inline bool valid_order(Side side, int tick, long qty) {
    return valid_tick(side, tick) && qty > 0 && qty <= 2147483647L;
}

inline bool operator==(const Level& a, const Level& b) { return a.price == b.price && a.qty == b.qty; }

inline bool operator==(const BBO& a, const BBO& b) {
    if (a.has_bid != b.has_bid || a.has_ask != b.has_ask) return false;
    if (a.has_bid && (a.bid_price != b.bid_price || a.bid_qty != b.bid_qty)) return false;
    if (a.has_ask && (a.ask_price != b.ask_price || a.ask_qty != b.ask_qty)) return false;
    return true;
}
