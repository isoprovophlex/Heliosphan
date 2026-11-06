#pragma once

#include <atomic>
#include <optional>
#include <utility>

namespace RE
{
    class TESWeather;
}

namespace MPL::Heliosphan
{
    constexpr bool ShouldRecheckLoadFallback(bool a_loadedSave, bool a_usedFallback,
        bool a_pendingRecheck, bool a_sameCell)
    {
        return a_usedFallback && (a_loadedSave || (a_pendingRecheck && a_sameCell));
    }

    struct WeatherLoadState
    {
        std::atomic_bool awaitingPostLoad{ false };
        std::optional<RE::TESWeather*> source;
        bool dispatchReady = false;

        bool DeferCellChange() const
        {
            return awaitingPostLoad.load(std::memory_order_acquire) || (source.has_value() && !dispatchReady);
        }

        bool DeferCellTiming() const
        {
            return awaitingPostLoad.load(std::memory_order_acquire) || source.has_value();
        }

        void ResetPending()
        {
            // Serialization revert must retain the PostLoadGame capture gate.
            source.reset();
            dispatchReady = false;
        }

        std::optional<RE::TESWeather*> Consume()
        {
            if (!dispatchReady) return std::nullopt;
            dispatchReady = false;
            return std::exchange(source, std::nullopt);
        }
    };
}
