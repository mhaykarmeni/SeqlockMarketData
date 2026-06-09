// Throughput benchmark: MarketDataCacheLF (seqlock) vs MarketDataCacheMtx
// (std::shared_mutex), across three scenarios:
//
//   1. Uncontended read   - 1 reader, no writer (pure latest() cost).
//   2. Read under writer   - N readers + 1 active writer (retry vs queuing).
//   3. Reader scaling      - 1..N readers, no writer (reader<->reader contention).
//
// Methodology notes:
//   * Each read folds a field into an accumulator that is then passed through
//     do_not_optimize(), so the compiler cannot delete the read loop.
//   * A warm-up pass runs before every timed measurement.
//   * We measure wall time for a fixed duration and derive ns/op and ops/sec.
//     ns/op is averaged across reader threads: (threads * duration) / total_reads.

#include "market_data.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Prevent the optimizer from discarding a computed value.
template <typename T>
inline void do_not_optimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

Quote make_quote(uint64_t i) {
    // Cheap, branch-light generator that keeps bid < ask.
    const double mid = 100.0 + static_cast<double>(i % 1000) * 0.01;
    return Quote{mid - 0.01, mid + 0.01, mid, static_cast<double>(i % 100), 0};
}

// Result of one timed run.
struct Result {
    uint64_t total_reads;
    double seconds;

    double ns_per_op(unsigned readers) const {
        return (static_cast<double>(readers) * seconds * 1e9) /
               static_cast<double>(total_reads);
    }
    double mops_per_sec() const {
        return static_cast<double>(total_reads) / seconds / 1e6;
    }
};

// Run `num_readers` reader threads (optionally with one writer) against `cache`
// for `dur`, after a short warm-up. Returns aggregate reads and elapsed time.
template <typename Cache>
Result run(Cache& cache, unsigned num_readers, bool with_writer,
           std::chrono::milliseconds dur) {
    cache.publish(make_quote(0));  // seed so first reads are valid

    std::atomic<bool> stop{false};
    std::atomic<bool> go{false};
    std::atomic<uint64_t> total{0};

    std::thread writer;
    if (with_writer) {
        writer = std::thread([&] {
            uint64_t i = 1;
            while (!go.load(std::memory_order_relaxed)) { /* wait for start */ }
            while (!stop.load(std::memory_order_relaxed)) {
                cache.publish(make_quote(i++));
            }
        });
    }

    std::vector<std::thread> readers;
    readers.reserve(num_readers);
    for (unsigned r = 0; r < num_readers; ++r) {
        readers.emplace_back([&] {
            // Warm-up: untimed reads to settle caches/branch predictor.
            double warm = 0.0;
            for (int k = 0; k < 100000; ++k) {
                warm += cache.latest().bid;
            }
            do_not_optimize(warm);

            while (!go.load(std::memory_order_relaxed)) { /* wait for start */ }

            uint64_t reads = 0;
            double acc = 0.0;
            while (!stop.load(std::memory_order_relaxed)) {
                acc += cache.latest().bid;
                ++reads;
            }
            do_not_optimize(acc);
            total.fetch_add(reads, std::memory_order_relaxed);
        });
    }

    const auto t0 = Clock::now();
    go.store(true, std::memory_order_relaxed);
    std::this_thread::sleep_for(dur);
    stop.store(true, std::memory_order_relaxed);
    const auto t1 = Clock::now();

    for (auto& r : readers) {
        r.join();
    }
    if (writer.joinable()) {
        writer.join();
    }

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return Result{total.load(), secs};
}

void scenario_uncontended() {
    const auto dur = std::chrono::milliseconds(500);
    std::printf("\n== Uncontended read (1 reader, no writer) ==\n");

    MarketDataCacheLF lf;
    MarketDataCacheMtx mtx;
    const Result rlf = run(lf, 1, false, dur);
    const Result rmtx = run(mtx, 1, false, dur);

    std::printf("  Seqlock: %7.2f ns/op   (%.1f Mops/s)\n",
                rlf.ns_per_op(1), rlf.mops_per_sec());
    std::printf("  Mutex:   %7.2f ns/op   (%.1f Mops/s)\n",
                rmtx.ns_per_op(1), rmtx.mops_per_sec());
}

void scenario_under_writer(unsigned readers) {
    const auto dur = std::chrono::milliseconds(500);
    std::printf("\n== Read under active writer (%u readers + 1 writer) ==\n", readers);

    MarketDataCacheLF lf;
    MarketDataCacheMtx mtx;
    const Result rlf = run(lf, readers, true, dur);
    const Result rmtx = run(mtx, readers, true, dur);

    std::printf("  Seqlock: %7.2f ns/op   (%.1f Mops/s aggregate)\n",
                rlf.ns_per_op(readers), rlf.mops_per_sec());
    std::printf("  Mutex:   %7.2f ns/op   (%.1f Mops/s aggregate)\n",
                rmtx.ns_per_op(readers), rmtx.mops_per_sec());
}

void scenario_scaling(unsigned max_readers) {
    const auto dur = std::chrono::milliseconds(300);
    std::printf("\n== Reader scaling (no writer) — aggregate Mops/s ==\n");
    std::printf("  %-8s %12s %12s\n", "readers", "Seqlock", "Mutex");

    for (unsigned n = 1; n <= max_readers; n *= 2) {
        MarketDataCacheLF lf;
        MarketDataCacheMtx mtx;
        const Result rlf = run(lf, n, false, dur);
        const Result rmtx = run(mtx, n, false, dur);
        std::printf("  %-8u %12.1f %12.1f\n", n, rlf.mops_per_sec(), rmtx.mops_per_sec());
    }
}

} // namespace

int main() {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned readers = hw > 2 ? hw - 1 : 3;

    std::printf("seqlock_bench — hardware_concurrency = %u\n", hw);

    scenario_uncontended();
    scenario_under_writer(readers);
    scenario_scaling(readers);

    std::printf("\nDone.\n");
    return 0;
}
