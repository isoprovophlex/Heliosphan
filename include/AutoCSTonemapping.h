#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::AutoCSTonemapping
{
    struct Settings
    {
        bool enabled = false;
        std::vector<std::string> plugins;
    };

    void ClearProfiles();
    void AddProfile(
        std::string a_id,
        Settings a_settings,
        std::filesystem::path a_sourcePath);
    bool GetProfileEnabled(std::string_view a_profile);
    bool SetProfileEnabled(std::string_view a_profile, bool a_enabled);
    void ApplyStartup();
}
