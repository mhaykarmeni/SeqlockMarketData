#pragma once
#include <atomic>
#include <cstring>
#include <cstdint>
#include <type_traits>

template<typename T>
    requires std::is_trivially_copyable_v<T>
class Seqlock {
public:
    // Publish a new value. Only one thread may call this at a time.
    void write(const T& val) noexcept {
        //used load + store approach instead of heavier fetch_add...valid since we have only one writer
        const uint64_t curr = m_counter.load(std::memory_order_relaxed);
        m_counter.store(curr + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        //this memcpy of non-atomic is theoretically open door to data-race/UB, but it works practically
        std::memcpy(static_cast<void*>(&m_value), static_cast<const void*>(&val), sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        m_counter.store(curr + 2, std::memory_order_relaxed);
    }

    // Read the latest consistent snapshot. Safe to call from any thread.
    [[nodiscard]] T read() const noexcept {
        while(true) {
            const uint64_t currState1 = m_counter.load(std::memory_order_relaxed);
            if(currState1 & 1) {
                //writer is active
                continue;
            }
            T val{};
            std::atomic_thread_fence(std::memory_order_acquire);
            std::memcpy(static_cast<void*>(&val), static_cast<const void*>(&m_value), sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t currState2 = m_counter.load(std::memory_order_acquire);
            if(currState1 != currState2) {
                continue;
            }
            return val;
        }
    }
private:
    alignas(64) T m_value;
    alignas(64) std::atomic<uint64_t> m_counter{};
};