#pragma once

#include <WeatherSync.h>
#include <WeatherSyncAPI.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace MPL::WeatherRuntime
{
    namespace detail
    {
        using UpdateCellEmittance = void (*)(RE::TESObjectCELL*);

        inline REL::Relocation<UpdateCellEmittance> updateCellEmittance{
            REL::VariantID(18464, 18895, 0x272820)
        };
    }

    inline WeatherSyncAPI::SetWeatherResult SetWeatherInstant(
        RE::TESWeather* a_weather,
        const bool a_override)
    {
        WeatherSyncAPI::SetWeatherResult result{};
        if (!a_weather)
        {
            return result;
        }
        auto* sky = RE::Sky::GetSingleton();
        if (!sky)
        {
            result.status =
                WeatherSyncAPI::SetWeatherStatus::kSkyUnavailable;
            return result;
        }
        const auto operationStart = std::chrono::steady_clock::now();
        const auto forceStart = std::chrono::steady_clock::now();
        sky->ForceWeather(a_weather, a_override);
        result.flags |= WeatherSyncAPI::ToMask(
            WeatherSyncAPI::SetWeatherFlag::kWeatherApplied);
        const auto forceDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - forceStart);

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell)
        {
            result.status = WeatherSyncAPI::SetWeatherStatus::
                kAppliedWithoutPlayerCell;
            return result;
        }
        auto* loadedData = cell->GetRuntimeData().loadedData;
        if (!loadedData)
        {
            result.status = WeatherSyncAPI::SetWeatherStatus::
                kAppliedWithoutLoadedData;
            if (WeatherSync::IsDetailedLoggingEnabled())
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

        const auto sourceEntries =
            loadedData->emittanceSourceRefMap.size();
        const auto lightEntries =
            loadedData->emittanceLightRefMap.size();
        const auto refreshStart = std::chrono::steady_clock::now();
        result.flags |= WeatherSyncAPI::ToMask(
            WeatherSyncAPI::SetWeatherFlag::kEmittanceRefreshAttempted);
        detail::updateCellEmittance(cell);
        result.flags |= WeatherSyncAPI::ToMask(
            WeatherSyncAPI::SetWeatherFlag::kEmittanceRefreshCompleted);
        result.status =
            WeatherSyncAPI::SetWeatherStatus::kAppliedAndRefreshed;
        result.lightCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                lightEntries,
                std::numeric_limits<std::uint32_t>::max()));
        const auto refreshDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - refreshStart);

        if (WeatherSync::IsDetailedLoggingEnabled())
        {
            logger::info(
                "[Weather Sync] [Emittance Refresh] "
                "Cell {:08X}: target={:08X}, override={}, "
                "source entries={}, region sources={}, region fallbacks={}, "
                "light entries={}; ForceWeather={} us, region caches={} us, "
                "native cell refresh={} us, total={} us",
                cell->GetFormID(),
                a_weather->GetFormID(),
                a_override,
                sourceEntries,
                regionSources,
                regionFallbacks,
                lightEntries,
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
