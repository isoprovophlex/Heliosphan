#pragma once

#include <MMSF_API.h>
#include <cstddef>
#include <cstdint>

namespace RE
{
    class TESImageSpace;
    class TESObjectREFR;
    class TESWeather;
}

namespace MPL::API::Heliosphan
{
    inline constexpr std::uint8_t kVersion = 1;
    inline constexpr auto kServiceName = "HELIOSPHAN";

    inline constexpr std::uint32_t kLoggingAPIVersion = 1;
    inline constexpr auto kLoggingAPIExport = "Heliosphan_GetLoggingAPI";

    enum class LogLevel : std::uint8_t
    {
        trace,
        debug,
        info,
        warning,
        error,
        critical,
    };

    // Message data is borrowed only for the duration of this synchronous call.
    using LogMessage = bool (*)(LogLevel, const char*, std::size_t) noexcept;
    using GetLoggingAPI = LogMessage (*)(std::uint32_t) noexcept;

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

    class IHeliosphanService : public MMSF::IPluginService
    {
    public:
        virtual SetWeatherResult SetWeatherInstant(RE::TESWeather*, bool) = 0;
        virtual bool RegisterLightPlacerTransformer(const LightPlacerTransformer*) = 0;
        virtual bool RequestLightPlacerReload() = 0;
        virtual bool RegisterReferenceClient(const ReferenceCallbacks*) = 0;
        virtual bool GetAutoCSTonemapping(const char*) = 0;
        virtual bool SetAutoCSTonemapping(const char*, bool) = 0;
        virtual bool IsAutoCSTonemappingApplied(const char*) = 0;
        virtual bool SetAutoCSTonemappingSuppressed(const char*, bool) = 0;
        // The target array is copied; automatic and forced ownership are combined.
        virtual bool SetCSTonemappingForcedTargets(RE::TESImageSpace* const*, std::size_t) = 0;
    };

    constexpr bool HasFlag(
        const SetWeatherResult& a_result,
        const SetWeatherFlag a_flag)
    {
        return (a_result.flags & ToMask(a_flag)) != 0;
    }
}  // namespace MPL::API::Heliosphan
