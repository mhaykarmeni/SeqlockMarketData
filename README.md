# SeqlockMarketData

A single-writer / N-reader market data cache built on a hand-rolled seqlock.

Seqlocks are a core primitive in HFT infrastructure: a single publisher (e.g. the feed handler) can blast out price updates at ~10–100 MHz while any number of strategy threads read the latest snapshot without ever blocking or taking a lock.

---

## How a seqlock works

A seqlock uses a single `uint64_t` sequence counter shared between writer and readers:

- **Even** value → no write in progress; data is consistent.
- **Odd** value → write in progress; data may be mid-update.

**Writer** protocol:
1. Increment counter (even → odd) — signals write has started.
2. Copy data into the shared slot.
3. Increment counter (odd → even) — signals write is complete.

**Reader** protocol:
1. Load counter into `s1`. If odd, spin (writer is active).
2. Copy data out of the shared slot.
3. Load counter into `s2`. If `s1 != s2`, a write raced the copy — retry from step 1.
4. Data is consistent.

Readers never block the writer. Multiple readers never block each other.

---

## Project structure

```
SeqlockMarketData/
├── src/
│   ├── seqlock.h        # Seqlock<T> template
│   ├── market_data.h    # Quote struct, MarketDataCacheLF, MarketDataCacheMtx
│   └── main.cpp         # Demo: 1 writer + N reader threads
├── tests/
│   └── seqlock_test.cpp # Correctness + torn-read detection tests
├── bench/
│   └── seqlock_bench.cpp # Latency/throughput vs shared_mutex
└── CMakeLists.txt
```

---

## API

### `Seqlock<T>` — `src/seqlock.h`

```cpp
template<typename T>
class Seqlock {
public:
    // Publish a new value. Only one thread may call this at a time.
    void write(const T& val) noexcept;

    // Read the latest consistent snapshot. Safe to call from any thread.
    [[nodiscard]] T read() const noexcept;
};
```

**Requirements on `T`**: trivially copyable (POD structs, scalars). The implementation copies `T` with `std::memcpy`.

**Memory ordering**: The writer must bracket the data copy with release fences so the data stores cannot straddle the counter increments. The reader must use an acquire load on `s1` and an acquire fence before reading `s2` so neither counter load can slip past the data copy.

**Spin hint**: On x86 use `_mm_pause()` inside the spin loop; on AArch64 use `yield`. Both reduce power consumption and branch mispredictions in tight spin loops.

**Cache layout**: Separate `counter` and `data` onto different cache lines to prevent false sharing between the writer updating `counter` and readers reading `data`.

---

### `Quote` — `src/market_data.h`

```cpp
struct Quote {
    double   bid;
    double   ask;
    double   last;
    double   volume;
    int64_t  exchange_ts_ns;  // exchange timestamp (nanoseconds since epoch)
};
```

---

### `MarketDataCacheLF` — `src/market_data.h`

Lock-free market data cache backed by `Seqlock<Quote>`.

```cpp
class MarketDataCacheLF {
public:
    // Publish a new quote. Single-writer only.
    void publish(const Quote& q) noexcept;

    // Return the latest consistent snapshot. Thread-safe, wait-free under no contention.
    [[nodiscard]] Quote latest() const noexcept;
};
```

---

### `MarketDataCacheMtx` — `src/market_data.h`

`std::shared_mutex`-backed alternative. Used as a benchmark baseline only.

```cpp
class MarketDataCacheMtx {
public:
    void  publish(const Quote& q);
    [[nodiscard]] Quote latest() const;
};
```

---

## Key correctness invariant for tests

The writer always publishes `bid < ask`. A reader that ever observes `bid >= ask` has suffered a torn read — the seqlock failed.

---

## Build

```bash
# Tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Demo (1 writer + N readers, asserts bid < ask)
cmake --build build --target seqlock_demo
./build/seqlock_demo

# Benchmark (seqlock vs shared_mutex)
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DSEQLOCK_BUILD_BENCH=ON
cmake --build build-release -j$(nproc)
./build-release/seqlock_bench
```

---

## Measured results

Numbers below are from `seqlock_bench` on a 7-core machine (`-O2`, Release).
Run `./build-release/seqlock_bench` to reproduce on your own hardware.

### Read latency

| Scenario | Seqlock | `shared_mutex` | Speedup |
|---|---|---|---|
| Uncontended read (1 reader) | **1.46 ns/op** | 14.59 ns/op | ~10× |
| Read under active writer (6 readers + 1 writer) | **77.5 Mops/s** | 8.8 Mops/s | ~9× |

### Reader scaling (no writer, aggregate throughput)

| Readers | Seqlock | `shared_mutex` |
|---|---|---|
| 1 | 673 Mops/s | 58 Mops/s |
| 2 | 1213 Mops/s | 15 Mops/s |
| 4 | 2027 Mops/s | 10 Mops/s |

The seqlock scales **~linearly** with readers (no reader↔reader contention),
while `shared_mutex` throughput *degrades* as readers are added — every reader
still mutates the lock's shared reader-count, so they contend on that cache line.

### Qualitative properties

| Property | Seqlock | `shared_mutex` |
|---|---|---|
| Readers scale with N | Yes — no reader↔reader contention | No — shared lock degrades |
| Writer blocks on readers | Never | Yes |
