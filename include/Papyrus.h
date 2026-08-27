#pragma once

#include <AutoCSTonemapping.h>
#include <Heliosphan.h>
#include <WeatherRuntime.h>

namespace MPL::Papyrus
{
    inline bool LogNotificationFailure(
        const bool a_succeeded,
        const std::string_view a_message)
    {
        if (!a_succeeded)
        {
            logger::error("[Heliosphan] [Notification] {}", a_message);
        }
        return a_succeeded;
    }

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

    inline bool GetWeatherSyncSpeedLogging(
        RE::StaticFunctionTag*,
        std::string a_profile)
    {
        return Heliosphan::GetProfileSpeedLogging(a_profile);
    }

    inline bool SetWeatherSyncDetailedLogging(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return LogNotificationFailure(
            Heliosphan::SetProfileDetailedLogging(
                a_profile,
                a_enabled),
            "Helios: Could not update HeliosphanSettings.json");
    }

    inline bool SetWeatherSyncNotifications(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return LogNotificationFailure(
            Heliosphan::SetProfileNotifications(
                a_profile,
                a_enabled),
            "Helios: Could not update HeliosphanSettings.json");
    }

    inline bool SetWeatherSyncSpeedLogging(
        RE::StaticFunctionTag*,
        std::string a_profile,
        const bool a_enabled)
    {
        return LogNotificationFailure(
            Heliosphan::SetProfileSpeedLogging(
                a_profile,
                a_enabled),
            "Helios: Could not update HeliosphanSettings.json");
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
        return LogNotificationFailure(
            AutoCSTonemapping::SetProfileEnabled(
                a_profile,
                a_enabled),
            "Helios: Could not update Helios.json");
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
            "GetWeatherSyncSpeedLogging",
            "Heliosphan",
            GetWeatherSyncSpeedLogging);
        a_vm->RegisterFunction(
            "SetWeatherSyncDetailedLogging",
            "Heliosphan",
            SetWeatherSyncDetailedLogging);
        a_vm->RegisterFunction(
            "SetWeatherSyncNotifications",
            "Heliosphan",
            SetWeatherSyncNotifications);
        a_vm->RegisterFunction(
            "SetWeatherSyncSpeedLogging",
            "Heliosphan",
            SetWeatherSyncSpeedLogging);
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
