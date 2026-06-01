// Correctness + torn-read detection for Seqlock<T> under concurrency.
//
// Strategy: the payload is several words wide and the writer sets EVERY word
// to the same monotonically increasing sequence number, publishing them as a
// single memcpy. Any consistent snapshot must therefore have all words equal.
// If a reader ever observes words that disagree, the seqlock allowed a torn
// read. We additionally assert that a single reader never sees the sequence go
// backwards (the writer only ever increases it).

#include "../src/seqlock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

// Wide enough that a half-completed copy is easy to catch, and not lock-free
// as a single atomic — exactly the case a seqlock exists for.
struct Payload {
    uint64_t word[8];
};

// Shared instance under test.
Seqlock<Payload> g_lock;

std::atomic<bool> g_stop{false};

// Per-failure diagnostics.
std::atomic<uint64_t> g_torn_reads{0};
std::atomic<uint64_t> g_backwards{0};
std::atomic<uint64_t> g_total_reads{0};

void writer_thread() {
    for (uint64_t seq = 1; !g_stop.load(std::memory_order_relaxed); ++seq) {
        Payload p;
        for (auto& w : p.word) {
            w = seq;
        }
        g_lock.write(p);
    }
}

void reader_thread() {
    uint64_t local_reads = 0;
    uint64_t last_seen = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        const Payload p = g_lock.read();

        // Torn-read check: all words must come from the same write.
        for (const auto& w : p.word) {
            if (w != p.word[0]) {
                g_torn_reads.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }

        // Monotonicity check: this reader must never see the sequence regress.
        if (p.word[0] < last_seen) {
            g_backwards.fetch_add(1, std::memory_order_relaxed);
        }
        last_seen = p.word[0];

        ++local_reads;
    }

    g_total_reads.fetch_add(local_reads, std::memory_order_relaxed);
}

bool single_threaded_sanity() {
    Payload in;
    for (uint64_t i = 0; i < 8; ++i) {
        in.word[i] = 1000 + i;
    }
    g_lock.write(in);

    const Payload out = g_lock.read();
    for (uint64_t i = 0; i < 8; ++i) {
        if (out.word[i] != 1000 + i) {
            std::printf("[FAIL] sanity: word[%llu] = %llu, expected %llu\n",
                        (unsigned long long)i,
                        (unsigned long long)out.word[i],
                        (unsigned long long)(1000 + i));
            return false;
        }
    }
    std::printf("[ OK ] single-threaded write/read round-trip\n");
    return true;
}

} // namespace

int main() {
    bool ok = single_threaded_sanity();

    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned num_readers = hw > 2 ? hw - 1 : 3;
    const auto duration = std::chrono::seconds(3);

    std::printf("Running concurrent test: 1 writer + %u readers for %llds...\n",
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

    const uint64_t torn = g_torn_reads.load();
    const uint64_t back = g_backwards.load();
    const uint64_t total = g_total_reads.load();

    std::printf("Total reads: %llu\n", (unsigned long long)total);
    std::printf("Torn reads:  %llu\n", (unsigned long long)torn);
    std::printf("Backwards:   %llu\n", (unsigned long long)back);

    if (total == 0) {
        std::printf("[FAIL] readers made no progress\n");
        ok = false;
    }
    if (torn != 0) {
        std::printf("[FAIL] %llu torn reads detected\n", (unsigned long long)torn);
        ok = false;
    }
    if (back != 0) {
        std::printf("[FAIL] %llu non-monotonic reads detected\n", (unsigned long long)back);
        ok = false;
    }

    if (ok) {
        std::printf("[ OK ] all concurrent checks passed\n");
        return 0;
    }
    std::printf("[FAIL] seqlock test failed\n");
    return 1;
}
