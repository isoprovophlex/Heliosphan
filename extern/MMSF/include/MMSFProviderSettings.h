#pragma once

#include <MMSF_API.h>
#include <SKSE/Logger.h>

namespace MPL::MMSFProviderSettings
{
    inline API::MMSF::ICompatService* Query(
        API::MMSF::Interface* a_registry,
        const char* a_provider,
        const char* a_operation)
    {
        const char* failure = nullptr;
        API::MMSF::IPluginService* service = nullptr;
        if (!a_provider || !*a_provider)
        {
            failure = "provider-ID-empty";
        }
        else if (!a_registry)
        {
            failure = "MMSF-unavailable";
        }
        else
        {
            const auto features = a_registry->GetVersion();
            if (API::MMSF::GetVersion(features) != 2)
                failure = "MMSF-version-mismatch";
            else if ((features & API::MMSF::MMSFAPIFeatures::kCoreService) ==
                     API::MMSF::MMSFAPIFeatures{})
                failure = "service-registry-unavailable";
            else if (!(service = a_registry->QueryService("Compat")))
                failure = "Compat-unavailable";
            else if (service->GetVersion() != 1)
                failure = "Compat-version-mismatch";
        }
        if (failure)
        {
            SKSE::log::warn(
                "[MMSF Settings] provider={} | operation={} | status=failed | reason={}",
                a_provider ? a_provider : "<none>", a_operation, failure);
            return nullptr;
        }
        return static_cast<API::MMSF::ICompatService*>(service);
    }

    inline bool ReadDetailedLogging(
        API::MMSF::Interface* a_registry,
        const char* a_provider,
        bool& a_enabled)
    {
        auto* service = Query(a_registry, a_provider, "read");
        if (!service) return false;
        bool enabled = false;
        if (!service->GetProviderSettings(a_provider, &enabled, nullptr))
        {
            SKSE::log::warn(
                "[MMSF Settings] provider={} | operation=read | status=failed | reason=provider-lookup-failed",
                a_provider);
            return false;
        }
        a_enabled = enabled;
        SKSE::log::info(
            "[MMSF Settings] provider={} | operation=read | detailedLogging={} | source=Compat",
            a_provider, enabled);
        return true;
    }

    inline bool WriteDetailedLogging(
        API::MMSF::Interface* a_registry,
        const char* a_provider,
        const bool a_enabled)
    {
        auto* service = Query(a_registry, a_provider, "write");
        if (!service) return false;
        if (!service->UpdateProviderSettings(
                a_provider, a_enabled ? std::int8_t{ 1 } : std::int8_t{ 0 }, std::int8_t{ -1 }))
        {
            SKSE::log::warn(
                "[MMSF Settings] provider={} | operation=write | status=failed | reason=provider-update-failed",
                a_provider);
            return false;
        }
        SKSE::log::info(
            "[MMSF Settings] provider={} | operation=write | detailedLogging={} | source=Compat",
            a_provider, a_enabled);
        return true;
    }
}
