#pragma once
#include "seqlock.h"

struct Quote {
    double   bid;
    double   ask;
    double   last;
    double   volume;
    int64_t  exchange_ts_ns;  // exchange timestamp (nanoseconds since epoch)
};

class MarketDataCacheLF {
public:
    // Publish a new quote. Single-writer only.
    void publish(const Quote& q) noexcept {
        m_seqlock.write(q);
    }

    // Return the latest consistent snapshot. Thread-safe, wait-free under no contention.
    [[nodiscard]] Quote latest() const noexcept {
        return m_seqlock.read();
    }
private:
    Seqlock<Quote> m_seqlock;
};

class MarketDataCacheMtx {
public:
    void  publish(const Quote& q);
    [[nodiscard]] Quote latest() const;
};