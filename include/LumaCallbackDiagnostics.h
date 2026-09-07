#pragma once

#include <atomic>
#include <cstdint>

namespace MPL::LumaClient::Diagnostics
{
    struct CallbackSnapshot
    {
        std::uint64_t references;
        std::uint64_t changing;
        std::uint64_t changed;
        std::uint64_t patched;
    };

    class CallbackCounters
    {
    public:
        void ReferenceInitialized()
        {
            references.fetch_add(1, std::memory_order_relaxed);
        }

        std::uint64_t CellChanging()
        {
            changing.fetch_add(1, std::memory_order_relaxed);
            return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        std::uint64_t CellChanged()
        {
            changed.fetch_add(1, std::memory_order_relaxed);
            return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        void CellPatched()
        {
            patched.fetch_add(1, std::memory_order_relaxed);
        }

        // Cumulative, nonblocking snapshots can straddle concurrent callbacks.
        CallbackSnapshot Snapshot() const
        {
            return {
                references.load(std::memory_order_relaxed),
                changing.load(std::memory_order_relaxed),
                changed.load(std::memory_order_relaxed),
                patched.load(std::memory_order_relaxed)
            };
        }

    private:
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        std::atomic<std::uint64_t> references{ 0 };
        std::atomic<std::uint64_t> changing{ 0 };
        std::atomic<std::uint64_t> changed{ 0 };
        std::atomic<std::uint64_t> patched{ 0 };
        std::atomic<std::uint64_t> sequence{ 0 };
    };
}  // namespace MPL::LumaClient::Diagnostics
