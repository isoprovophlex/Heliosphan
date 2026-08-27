#pragma once

#include <cstdint>

namespace RE
{
    class TESWeather;
}

namespace MPL::WeatherSyncAPI
{
    inline constexpr std::uint32_t kVersion = 2;

    enum class SetWeatherStatus : std::uint32_t
    {
        kInvalidWeather,
        kSkyUnavailable,
        kAppliedWithoutPlayerCell,
        kAppliedWithoutLoadedData,
        kAppliedAndRefreshed,
    };

    enum class SetWeatherFlag : std::uint32_t
    {
        kWeatherApplied = 1U << 0,
        kEmittanceRefreshAttempted = 1U << 1,
        kEmittanceRefreshCompleted = 1U << 2,
    };

    constexpr std::uint32_t ToMask(const SetWeatherFlag a_flag)
    {
        return static_cast<std::uint32_t>(a_flag);
    }

    struct SetWeatherResult
    {
        SetWeatherStatus status = SetWeatherStatus::kInvalidWeather;
        std::uint32_t flags = 0;
        std::uint32_t lightCount = 0;
    };

    struct Interface
    {
        std::uint32_t version = kVersion;
        SetWeatherResult (*SetWeatherInstant)(
            RE::TESWeather*,
            bool) = nullptr;
    };

    constexpr bool HasFlag(
        const SetWeatherResult& a_result,
        const SetWeatherFlag a_flag)
    {
        return (a_result.flags & ToMask(a_flag)) != 0;
    }

    using RequestInterface = const Interface* (*) (std::uint32_t);
}  // namespace MPL::WeatherSyncAPI
