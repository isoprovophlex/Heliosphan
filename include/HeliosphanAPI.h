#pragma once

#include <cstddef>
#include <cstdint>

namespace RE
{
    class TESObjectREFR;
    class TESWeather;
}

namespace MPL::HeliosphanAPI
{
    inline constexpr std::uint32_t kVersion = 4;

    struct ReferenceCallbacks
    {
        // The ID is copied during registration.
        const char* id = nullptr;
        void (*OnReferenceEmittanceChanged)(RE::TESObjectREFR*) = nullptr;
    };

    using LightPlacerOutput =
        bool (*)(void*, const char*, std::size_t);
    using LightPlacerTransform =
        bool (*)(
            const char*,
            const char*,
            std::size_t,
            LightPlacerOutput,
            void*);

    struct LightPlacerTransformer
    {
        // The ID is copied during registration.
        const char* id = nullptr;
        // Input and output pointers are valid only during this synchronous call.
        // A successful transform must invoke the output callback exactly once.
        LightPlacerTransform TransformJson = nullptr;
        // Reports whether the combined transform, reload, and restoration succeeded.
        void (*OnReloadComplete)(bool) = nullptr;
    };

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
        bool (*RegisterLightPlacerTransformer)(
            const LightPlacerTransformer*) = nullptr;
        bool (*RequestLightPlacerReload)() = nullptr;
        bool (*RegisterReferenceClient)(
            const ReferenceCallbacks*) = nullptr;
    };

    constexpr bool HasFlag(
        const SetWeatherResult& a_result,
        const SetWeatherFlag a_flag)
    {
        return (a_result.flags & ToMask(a_flag)) != 0;
    }

    using RequestInterface = const Interface* (*) (std::uint32_t);
}  // namespace MPL::HeliosphanAPI
