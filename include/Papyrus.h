#pragma once

#include <AutoCSTonemapping.h>
#include <WeatherRuntime.h>
#include <Heliosphan.h>

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
        if (Heliosphan::IsDetailedLoggingEnabled())
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
        return Heliosphan::GetProfileDetailedLogging(a_profile);
    }

    inline bool GetWeatherSyncNotifications(
        RE::StaticFunctionTag*,
        std::string a_profile)
    {
        return Heliosphan::GetProfileNotifications(a_profile);
    }

    inline bool SetWeatherSyncDetailedLogging(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return Heliosphan::SetProfileDetailedLogging(
            a_profile,
            a_enabled);
    }

    inline bool SetWeatherSyncNotifications(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return Heliosphan::SetProfileNotifications(
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
            "Heliosphan",
            SetWeatherInstant);
        a_vm->RegisterFunction(
            "GetWeatherSyncDetailedLogging",
            "Heliosphan",
            GetWeatherSyncDetailedLogging);
        a_vm->RegisterFunction(
            "GetWeatherSyncNotifications",
            "Heliosphan",
            GetWeatherSyncNotifications);
        a_vm->RegisterFunction(
            "SetWeatherSyncDetailedLogging",
            "Heliosphan",
            SetWeatherSyncDetailedLogging);
        a_vm->RegisterFunction(
            "SetWeatherSyncNotifications",
            "Heliosphan",
            SetWeatherSyncNotifications);
        a_vm->RegisterFunction(
            "GetAutoCSTonemapping",
            "Heliosphan",
            GetAutoCSTonemapping);
        a_vm->RegisterFunction(
            "SetAutoCSTonemapping",
            "Heliosphan",
            SetAutoCSTonemapping);
        return true;
    }
}  // namespace MPL::Papyrus
