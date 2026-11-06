#pragma once

#include <ExternalEmittance.h>
#include <Heliosphan.h>
#include <HeliosphanServiceAPI.h>
#include <WeatherSoundFix.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace MPL::WeatherRuntime
{
    namespace detail
    {
        inline void TraceStage(const char* a_stage, const RE::TESWeather* a_weather, const bool a_override)
        {
            if (!Heliosphan::IsDetailedLoggingEnabled()) return;
            logger::info("[Weather Runtime] stage={} | target={:08X} | override={}",
                a_stage, a_weather ? a_weather->GetFormID() : 0, a_override);
            spdlog::default_logger()->flush();
        }

        using UpdateCellEmittance = void (*)(RE::TESObjectCELL*);

        inline REL::Relocation<UpdateCellEmittance> updateCellEmittance{
            REL::VariantID(18464, 18895, 0x272820)
        };
    }

    inline API::Heliosphan::SetWeatherResult SetWeatherInstant(
        RE::TESWeather* a_weather,
        const bool a_override)
    {
        API::Heliosphan::SetWeatherResult result{};
        if (!a_weather)
        {
            return result;
        }
        auto* sky = RE::Sky::GetSingleton();
        if (!sky)
        {
            result.status =
                API::Heliosphan::SetWeatherStatus::kSkyUnavailable;
            return result;
        }
        const auto operationStart = std::chrono::steady_clock::now();
        const auto forceStart = std::chrono::steady_clock::now();
        detail::TraceStage("ForceWeather-begin", a_weather, a_override);
        WeatherSoundFix::BeginWeatherChange(a_weather);
        sky->ForceWeather(a_weather, a_override);
        WeatherSoundFix::Poll();
        detail::TraceStage("ForceWeather-end", a_weather, a_override);
        result.flags |= API::Heliosphan::ToMask(
            API::Heliosphan::SetWeatherFlag::kWeatherApplied);
        const auto forceDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - forceStart);

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell)
        {
            result.status = API::Heliosphan::SetWeatherStatus::
                kAppliedWithoutPlayerCell;
            return result;
        }
        auto* loadedData = cell->GetRuntimeData().loadedData;
        if (!loadedData)
        {
            result.status = API::Heliosphan::SetWeatherStatus::
                kAppliedWithoutLoadedData;
            if (Heliosphan::IsDetailedLoggingEnabled())
            {
                logger::info(
                    "[Weather Sync] [Emittance Refresh] "
                    "Forced weather {:08X} in {} us, but the player cell "
                    "has no loaded emittance data",
                    a_weather->GetFormID(),
                    forceDuration.count());
            }
            return result;
        }

        const auto regionStart = std::chrono::steady_clock::now();
        detail::TraceStage("region-cache-begin", a_weather, a_override);
        std::uint32_t regionSources = 0;
        std::uint32_t regionFallbacks = 0;
        for (const auto& entry : loadedData->emittanceSourceRefMap)
        {
            auto* source = entry.first;
            if (!source || !source->Is(RE::FormType::Region))
            {
                continue;
            }

            auto* region = static_cast<RE::TESRegion*>(source);
            region->SetCurrentWeather(a_weather);
            ++regionSources;
            if (region->currentWeather != a_weather)
            {
                ++regionFallbacks;
            }
        }
        const auto regionDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - regionStart);
        detail::TraceStage("region-cache-end", a_weather, a_override);

        const auto sourceEntries =
            loadedData->emittanceSourceRefMap.size();
        const auto lightEntries =
            loadedData->emittanceLightRefMap.size();
        const auto refreshStart = std::chrono::steady_clock::now();
        result.flags |= API::Heliosphan::ToMask(
            API::Heliosphan::SetWeatherFlag::kEmittanceRefreshAttempted);
        detail::TraceStage("cell-emittance-begin", a_weather, a_override);
        detail::updateCellEmittance(cell);
        detail::TraceStage("cell-emittance-end", a_weather, a_override);
        result.flags |= API::Heliosphan::ToMask(
            API::Heliosphan::SetWeatherFlag::kEmittanceRefreshCompleted);
        detail::TraceStage("client-notification-begin", a_weather, a_override);
        const auto clientReferences =
            ExternalEmittance::NotifyCellEmittanceRefreshed(cell);
        detail::TraceStage("client-notification-end", a_weather, a_override);
        result.status =
            API::Heliosphan::SetWeatherStatus::kAppliedAndRefreshed;
        result.lightCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                lightEntries,
                std::numeric_limits<std::uint32_t>::max()));
        const auto refreshDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - refreshStart);

        if (Heliosphan::IsDetailedLoggingEnabled())
        {
            logger::info(
                "[Weather Sync] [Emittance Refresh] "
                "Cell {:08X}: target={:08X}, override={}, "
                "source entries={}, region sources={}, region fallbacks={}, "
                "light entries={}, client references={}; ForceWeather={} us, "
                "region caches={} us, native cell refresh={} us, total={} us",
                cell->GetFormID(),
                a_weather->GetFormID(),
                a_override,
                sourceEntries,
                regionSources,
                regionFallbacks,
                lightEntries,
                clientReferences,
                forceDuration.count(),
                regionDuration.count(),
                refreshDuration.count(),
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - operationStart)
                    .count());
        }
        return result;
    }
}  // namespace MPL::WeatherRuntime
