// Demo: 1 writer (feed handler) + N readers (strategies) over MarketDataCacheLF.
//
// The writer random-walks a mid price and always publishes a quote with
// bid < ask. Each reader continuously snapshots the cache and asserts that
// invariant: observing bid >= ask would mean the seqlock let a torn read
// through. After a bounded run we join everyone and print a summary.

#include "market_data.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace {

MarketDataCacheLF g_cache;
std::atomic<bool> g_stop{false};

// Aggregated reader stats.
std::atomic<uint64_t> g_total_reads{0};
std::atomic<uint64_t> g_invariant_violations{0};

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Feed handler: publish a fresh, internally-consistent quote as fast as it can.
void writer_thread() {
    std::mt19937_64 rng{std::random_device{}()};
    std::normal_distribution<double> step{0.0, 0.01};  // price tick noise

    double mid = 100.0;
    uint64_t published = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        mid += step(rng);
        if (mid < 1.0) {
            mid = 1.0;  // keep prices sane
        }
        const double spread = 0.02;

        Quote q;
        q.bid = mid - spread / 2.0;
        q.ask = mid + spread / 2.0;
        q.last = mid;
        q.volume = static_cast<double>(published % 1000);
        q.exchange_ts_ns = now_ns();

        g_cache.publish(q);
        ++published;
    }

    std::printf("[writer] published %llu quotes\n", (unsigned long long)published);
}

// Strategy: snapshot the latest quote and verify it is internally consistent.
void reader_thread() {
    uint64_t reads = 0;
    uint64_t violations = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        const Quote q = g_cache.latest();
        if (q.bid >= q.ask) {
            ++violations;  // torn read: bid and ask came from different writes
        }
        ++reads;
    }

    g_total_reads.fetch_add(reads, std::memory_order_relaxed);
    g_invariant_violations.fetch_add(violations, std::memory_order_relaxed);
}

} // namespace

int main() {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned num_readers = hw > 2 ? hw - 1 : 3;
    const auto duration = std::chrono::seconds(3);

    // Seed the cache so the very first reads see a valid quote.
    g_cache.publish(Quote{99.99, 100.01, 100.0, 0.0, now_ns()});

    std::printf("Demo: 1 writer + %u readers for %llds...\n",
                num_readers, (long long)duration.count());

    std::thread writer(writer_thread);
    std::vector<std::thread> readers;
    readers.reserve(num_readers);
    for (unsigned i = 0; i < num_readers; ++i) {
        readers.emplace_back(reader_thread);
    }

    std::this_thread::sleep_for(duration);
    g_stop.store(true, std::memory_order_relaxed);

    writer.join();
    for (auto& r : readers) {
        r.join();
    }

    const uint64_t reads = g_total_reads.load();
    const uint64_t violations = g_invariant_violations.load();
    const Quote last = g_cache.latest();

    std::printf("[readers] total reads: %llu\n", (unsigned long long)reads);
    std::printf("[readers] invariant violations (bid >= ask): %llu\n",
                (unsigned long long)violations);
    std::printf("[final] bid=%.4f ask=%.4f last=%.4f\n",
                last.bid, last.ask, last.last);

    if (violations != 0) {
        std::printf("FAIL: torn reads detected\n");
        return 1;
    }
    std::printf("OK: all reads internally consistent\n");
    return 0;
}
