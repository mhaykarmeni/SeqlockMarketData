# SeqlockMarketData

A single-writer / N-reader market data cache built on a hand-rolled seqlock.

Seqlocks are a core primitive in HFT infrastructure: a single publisher (e.g. the feed handler) can blast out price updates at ~10–100 MHz while any number of strategy threads read the latest snapshot without ever blocking or taking a lock.

---

## How a seqlock works

A seqlock uses a single `uint64_t` sequence counter shared between writer and readers:

- **Even** value → no write in progress; data is consistent.
- **Odd** value → write in progress; data may be mid-update.

**Writer** protocol:
1. Increment seq (even → odd) — signals write has started.
2. Copy data into the shared slot.
3. Increment seq (odd → even) — signals write is complete.

**Reader** protocol:
1. Load seq into `s1`. If odd, spin (writer is active).
2. Copy data out of the shared slot.
3. Load seq into `s2`. If `s1 != s2`, a write raced the copy — retry from step 1.
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
    requires std::is_trivially_copyable_v<T>
class Seqlock {
public:
    // Publish a new value. Only one thread may call this at a time.
    void write(const T& val) noexcept;

    // Read the latest consistent snapshot. Safe to call from any thread.
    [[nodiscard]] T read() const noexcept;
};
```

**Requirements on `T`**: trivially copyable (POD structs, scalars). The implementation copies `T` with `std::memcpy`.

**Memory ordering**: The writer must bracket the data copy with release fences so the data stores cannot straddle the seq increments. The reader must use an acquire load on `s1` and an acquire fence before reading `s2` so neither seq load can slip past the data copy.

**Spin hint**: On x86 use `_mm_pause()` inside the spin loop; on AArch64 use `yield`. Both reduce power consumption and branch mispredictions in tight spin loops.

**Cache layout**: Separate `seq` and `data` onto different cache lines to prevent false sharing between the writer updating `seq` and readers reading `data`.

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
# Tests only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# With benchmarks
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DSEQLOCK_BUILD_BENCH=ON
cmake --build build-release -j$(nproc)
./build-release/seqlock_bench
```

---

## Expected characteristics

| Scenario | Seqlock | `shared_mutex` |
|---|---|---|
| Uncontended read | ~5 ns | ~20–40 ns |
| Read under active writer | ~5–15 ns (retry) | ~20–40 ns (queued) |
| Readers scale with N | Yes — no reader↔reader contention | No — shared lock degrades |
| Writer blocks on readers | Never | Yes |
