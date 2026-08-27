#pragma once

#include <AutoCSTonemapping.h>
#include <WeatherRuntime.h>
#include <WeatherSync.h>

namespace MPL::Papyrus
{
    inline void SetWeatherInstant(
        RE::StaticFunctionTag*,
        RE::TESWeather* a_weather,
        const bool a_override)
    {
        if (!a_weather)
        {
            return;
        }
        const auto result =
            WeatherRuntime::SetWeatherInstant(a_weather, a_override);
        if (WeatherSync::IsDetailedLoggingEnabled())
        {
            logger::info(
                "[Weather Sync] SetWeatherInstant: weather {:08X} "
                "override={} status={} nativeCellLightEntriesProcessed={}",
                a_weather->GetFormID(),
                a_override,
                static_cast<std::uint32_t>(result.status),
                result.lightCount);
        }
    }

    inline bool GetWeatherSyncDetailedLogging(
        RE::StaticFunctionTag*,
        std::string a_profile)
    {
        return WeatherSync::GetProfileDetailedLogging(a_profile);
    }

    inline bool GetWeatherSyncNotifications(
        RE::StaticFunctionTag*,
        std::string a_profile)
    {
        return WeatherSync::GetProfileNotifications(a_profile);
    }

    inline bool SetWeatherSyncDetailedLogging(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return WeatherSync::SetProfileDetailedLogging(
            a_profile,
            a_enabled);
    }

    inline bool SetWeatherSyncNotifications(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return WeatherSync::SetProfileNotifications(
            a_profile,
            a_enabled);
    }

    inline bool GetAutoCSTonemapping(
        RE::StaticFunctionTag*,
        std::string a_profile)
    {
        return AutoCSTonemapping::GetProfileEnabled(a_profile);
    }

    inline bool SetAutoCSTonemapping(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return AutoCSTonemapping::SetProfileEnabled(
            a_profile,
            a_enabled);
    }

    inline bool Bind(RE::BSScript::IVirtualMachine* a_vm)
    {
        a_vm->RegisterFunction(
            "SetWeatherInstant",
            "WeatherSync",
            SetWeatherInstant);
        a_vm->RegisterFunction(
            "GetWeatherSyncDetailedLogging",
            "WeatherSync",
            GetWeatherSyncDetailedLogging);
        a_vm->RegisterFunction(
            "GetWeatherSyncNotifications",
            "WeatherSync",
            GetWeatherSyncNotifications);
        a_vm->RegisterFunction(
            "SetWeatherSyncDetailedLogging",
            "WeatherSync",
            SetWeatherSyncDetailedLogging);
        a_vm->RegisterFunction(
            "SetWeatherSyncNotifications",
            "WeatherSync",
            SetWeatherSyncNotifications);
        a_vm->RegisterFunction(
            "GetAutoCSTonemapping",
            "WeatherSync",
            GetAutoCSTonemapping);
        a_vm->RegisterFunction(
            "SetAutoCSTonemapping",
            "WeatherSync",
            SetAutoCSTonemapping);
        return true;
    }
}  // namespace MPL::Papyrus
