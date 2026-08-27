#pragma once

#include <MMSF_API.h>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::RegionWeatherPatcher
{
    struct Settings
    {
        bool enabled = false;
        std::vector<std::string> plugins;
    };

    void Apply(
        const Settings& a_settings,
        std::string_view a_profile,
        std::string_view a_weatherPrefix,
        std::string_view a_regionPrefix,
        API::MMSF::Interface* a_mmsf,
        bool a_detailedLogging);
}  // namespace MPL::RegionWeatherPatcher
