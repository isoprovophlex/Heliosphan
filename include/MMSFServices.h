#pragma once

#include <MMSF_API.h>
#include <string>

namespace MPL::MMSFServices
{
    struct Result
    {
        API::MMSF::IPluginService* service = nullptr;
        const char* failure = nullptr;
    };

    inline Result Query(
        API::MMSF::Interface* a_registry,
        const std::string& a_name,
        const std::uint8_t a_version,
        const bool a_requiresCaching = false)
    {
        using API::MMSF::MMSFAPIFeatures;
        if (!a_registry)
        {
            return { nullptr, "MMSF-unavailable" };
        }
        const auto features = a_registry->GetVersion();
        if (API::MMSF::GetVersion(features) != 2)
        {
            return { nullptr, "MMSF-version-mismatch" };
        }
        if ((features & MMSFAPIFeatures::kCoreService) == MMSFAPIFeatures{})
        {
            return { nullptr, "service-registry-unavailable" };
        }
        if (a_requiresCaching &&
            (features & MMSFAPIFeatures::kCaching) == MMSFAPIFeatures{})
        {
            return { nullptr, "caching-unavailable" };
        }
        auto* service = a_registry->QueryService(a_name);
        if (!service)
        {
            return { nullptr, "service-unavailable" };
        }
        return { service, service->GetVersion() == a_version ? nullptr : "service-version-mismatch" };
    }
}  // namespace MPL::MMSFServices
